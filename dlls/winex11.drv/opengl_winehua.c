/*
 * WineHua private OpenGL present for the X route.
 *
 * Copyright 2025-2026 WineHua contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Why this exists
 * ---------------
 * The X route has no GLX: DRI3/GLX need a DRM render node and a guest Mesa of
 * the host's architecture, neither of which exists for an emulated x86_64
 * guest.  Without a driver here winex11 falls back to win32u's generic EGL
 * driver (surfaceless platform, FBO drawable) whose swap is a no-op —
 * rendering works, nothing ever reaches the host (T3: frames=3028 at 447fps
 * with displayFps=-1).
 *
 * What this driver does
 * ---------------------
 * Keep the working render half (surfaceless EGL, inherited from the base
 * driver funcs) and give the drawable a *pbuffer* EGL surface so the swap has
 * a real front resource: the guest Mesa VirGL/vtest winsys then emits a
 * present command carrying the id published in the present page, and the host
 * presenter blits that texture into the window's scene node — the same channel
 * the route already uses for Vulkan/venus frames, keyed by the same id:
 * the X window id of the HWND's managed toplevel.
 *
 * The id is published on *every* swap, not only when the host has attached a
 * target: the host discovers surfaces from their presents (see
 * include/wine/winehua_present.h).  Frames presented before the host attaches
 * are dropped by the presenter (kPresentNoTarget) — the driver reports that as
 * rate-limited degradation rather than silently presenting into nothing.
 *
 * Mirrors dlls/winewayland.drv/opengl_readback.c (zero-copy branch); the
 * readback pool there is a Wayland-route fallback and has no X-route analog —
 * on this route a window that never gets a target stays unpresented, which the
 * degradation counter surfaces.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"

#include "wine/debug.h"
#include "wine/opengl_driver.h"
#include "wine/winehua_present.h"

#include "x11drv.h"

WINE_DEFAULT_DEBUG_CHANNEL(wgl);

/* 降级可见性: 未挂接时每 300 帧打一行 (约 1s@300fps), 不静默也不刷屏 */
#define WINEHUA_X11_GL_DEGRADE_INTERVAL 300

static const struct opengl_funcs *funcs;
static struct egl_platform *egl_platform;
static const struct opengl_driver_funcs *winehua_base_driver_funcs;
static struct opengl_driver_funcs winehua_x11_driver_funcs;

struct winehua_x11_gl_drawable
{
    struct opengl_drawable base;
    int width;
    int height;
    uint64_t presents;      /* 已 present 帧数 */
    uint64_t unpresented;   /* 无目标/无窗口而丢弃的帧数 */
    uint64_t lastReported;  /* 上次降级日志所在的 presents 位置 */
};

static struct winehua_x11_gl_drawable *impl_from_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct winehua_x11_gl_drawable, base);
}

static EGLConfig wx11_config_for_format(int format)
{
    if (!egl_platform || !egl_platform->config_count) return NULL;
    return egl_platform->configs[(format - 1) % egl_platform->config_count];
}

static void winehua_x11_drawable_destroy(struct opengl_drawable *base)
{
    struct winehua_x11_gl_drawable *gl = impl_from_drawable(base);

    TRACE("%s: presents=%llu unpresented=%llu\n", debugstr_opengl_drawable(base),
          (unsigned long long)gl->presents, (unsigned long long)gl->unpresented);
    if (base->surface) funcs->p_eglDestroySurface(egl_platform->display, base->surface);
}

static void winehua_x11_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    TRACE("%s, flags %#x\n", debugstr_opengl_drawable(base), flags);
}

/* 呈现: 发布 id → flush → swap → 摘 id。三步顺序不可换 —— mesa 在 swap 内部
 * 读页面 (virgl_vtest_flush_frontbuffer), id 必须在 swap 期间可见; 之后立刻
 * 清掉, 迟到的 flush_frontbuffer 不会误挂到本窗。 */
static BOOL winehua_x11_drawable_swap(struct opengl_drawable *base)
{
    struct winehua_x11_gl_drawable *gl = impl_from_drawable(base);
    Window xwindow = X11DRV_get_whole_window(base->client->hwnd);
    BOOL ok;

    if (!xwindow || xwindow == root_window)
    {
        /* 窗口还没有 managed toplevel (未 map / 桌面窗): 帧无处可去。 */
        if (!gl->unpresented++)
            WARN("hwnd %p has no managed toplevel, private present unavailable\n",
                 base->client->hwnd);
        return TRUE; /* 与上游 framebuffer_surface_swap 同语义: 换缓冲本身成功 */
    }

    winehua_present_surface_begin((uint32_t)xwindow);
    /* Pbuffer swap alone does not reliably flush the front resource (与
     * winewayland.drv/opengl_readback.c:353 同因)。 */
    funcs->p_glFlush();
    ok = funcs->p_eglSwapBuffers(egl_platform->display, base->surface);
    winehua_present_surface_end();

    if (!ok)
    {
        WARN("eglSwapBuffers failed hwnd=%p xwindow=%lx error=%#x\n",
             base->client->hwnd, xwindow, funcs->p_eglGetError());
        return FALSE;
    }

    gl->presents++;
    if (!winehua_present_surface_ready((uint32_t)xwindow))
    {
        gl->unpresented++;
        if (gl->presents - gl->lastReported >= WINEHUA_X11_GL_DEGRADE_INTERVAL)
        {
            gl->lastReported = gl->presents;
            WARN("hwnd %p xwindow %lx: host has no present target after %llu frames "
                 "(%llu dropped), check display route / window registration\n",
                 base->client->hwnd, xwindow, (unsigned long long)gl->presents,
                 (unsigned long long)gl->unpresented);
        }
    }
    return TRUE;
}

static const struct opengl_drawable_funcs winehua_x11_drawable_funcs =
{
    .destroy = winehua_x11_drawable_destroy,
    .flush = winehua_x11_drawable_flush,
    .swap = winehua_x11_drawable_swap,
};

/* 与 egldrv_surface_create 同形, 差别只在 pbuffer (有 front resource 才有
 * 真正的 present 语义) 与自带的 swap。 */
static BOOL winehua_x11_surface_create(HWND hwnd, int format, struct opengl_drawable **drawable)
{
    struct winehua_x11_gl_drawable *gl;
    struct client_surface *client;
    struct opengl_drawable *previous;
    EGLint attribs[5];
    RECT rect;

    if ((previous = *drawable) && previous->format == format) return TRUE;
    if (!(client = nulldrv_client_surface_create(hwnd))) return FALSE;
    /* 页面尽早绑定: mesa 侧第一次 flush_frontbuffer 就该读到有效页 */
    winehua_present_surface_init();
    if (!(gl = opengl_drawable_create(sizeof(*gl), &winehua_x11_drawable_funcs,
                                      format, client)))
    {
        client_surface_release(client);
        return FALSE;
    }

    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    gl->width = max(1, rect.right - rect.left);
    gl->height = max(1, rect.bottom - rect.top);

    attribs[0] = EGL_WIDTH;
    attribs[1] = gl->width;
    attribs[2] = EGL_HEIGHT;
    attribs[3] = gl->height;
    attribs[4] = EGL_NONE;
    gl->base.surface = funcs->p_eglCreatePbufferSurface(egl_platform->display,
                                                       wx11_config_for_format(format), attribs);
    if (!gl->base.surface)
    {
        WARN("pbuffer creation failed hwnd=%p format=%d size=%dx%d error=%#x\n",
             hwnd, format, gl->width, gl->height, funcs->p_eglGetError());
        opengl_drawable_release(&gl->base);
        client_surface_release(client);
        return FALSE;
    }

    /* 单缓冲 pbuffer: front 映射到 back, 应用画的那一份就是 present 的源
     * (与 winewayland.drv/opengl_readback.c 同一映射)。 */
    opengl_drawable_map_buffer(&gl->base, GL_FRONT_LEFT, GL_BACK_LEFT);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT, GL_BACK);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT_AND_BACK, GL_BACK);
    if (gl->base.stereo) opengl_drawable_map_buffer(&gl->base, GL_FRONT_RIGHT, GL_BACK_RIGHT);

    client_surface_release(client);
    if (previous) opengl_drawable_release(previous);
    *drawable = &gl->base;
    TRACE("created %s (pbuffer %dx%d)\n", debugstr_opengl_drawable(&gl->base),
          gl->width, gl->height);
    return TRUE;
}

static void winehua_x11_init_egl_platform(struct egl_platform *platform)
{
    /* 渲染半场原样继承 (surfaceless EGL: T3 已证可渲染) —— 本驱动只换呈现出口 */
    winehua_base_driver_funcs->p_init_egl_platform(platform);
    egl_platform = platform;
}

/* X 路线私有 present 是否可用: 宿主已武装 vtest present 通道
 * (WINEHUA_VTEST_PRESENT, 由 graphics_broker 随 guest 环境下发)。
 * WINEHUA_X11_GL_PRESENT=0 可显式关掉 (对照实验用), 关掉即回到上游行为
 * (egldrv, 不出图)。 */
static BOOL winehua_x11_gl_enabled(void)
{
    const char *opt_out = getenv("WINEHUA_X11_GL_PRESENT");
    const char *present;

    if (opt_out && opt_out[0] && !strcmp(opt_out, "0")) return FALSE;
    present = getenv("WINEHUA_VTEST_PRESENT");
    return present && present[0] && strcmp(present, "0");
}

UINT winehua_x11_gl_init(UINT version, const struct opengl_funcs *opengl_funcs,
                         const struct opengl_driver_funcs **driver_funcs)
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR("Version mismatch, opengl32 wants %u but driver has %u\n",
            version, WINE_OPENGL_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }
    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    if (!winehua_x11_gl_enabled()) return STATUS_NOT_IMPLEMENTED;

    funcs = opengl_funcs;
    winehua_base_driver_funcs = *driver_funcs;
    winehua_x11_driver_funcs = *winehua_base_driver_funcs; /* 先全量继承 */
    winehua_x11_driver_funcs.p_init_egl_platform = winehua_x11_init_egl_platform;
    winehua_x11_driver_funcs.p_surface_create = winehua_x11_surface_create;
    *driver_funcs = &winehua_x11_driver_funcs;

    TRACE("WineHua X-route GL present enabled (pbuffer drawable + X window id)\n");
    return STATUS_SUCCESS;
}
