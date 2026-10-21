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
    BOOL warned_child;      /* 子窗落位限制只提示一次 (见 swap) */
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
    /* EGLSurface 不在这里销毁: 上游约定由 win32u 的 opengl_drawable_release 统一
     * 销毁 (egldrv / winewayland 的 destroy 回调都只做自己的事)。这里再销毁一次
     * 就是二次销毁 —— 第二次传入已释放的句柄: 释放后读 + EGL 显示表 last error
     * 被污染 (驱动失败路径打出的错误码会是上一次 destroy 的残留)。 */
}

/* 窗口几何变化 (GL_FLUSH_UPDATED) ⇒ pbuffer 必须跟随 client rect: 尺寸若在
 * drawable 创建时冻结、之后永不更新, 应用改窗/最大化/DPI 变化后就会一直渲染
 * 到旧尺寸 —— 超出部分被裁、剩下的被主机按窗几何拉伸, 而 guest 侧零错误
 * (review 2026-10-01)。win32u 在窗口几何变化时给 client surface 置 updated,
 * 下一次 swap 会带 GL_FLUSH_UPDATED 调到这里; 做法与 winewayland.drv/
 * opengl_readback.c 的重建路径一致 (新建 pbuffer + 重绑当前 context, 保留
 * buffer 映射表 —— 映射与 EGLSurface 无关)。 */
static void winehua_x11_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    struct winehua_x11_gl_drawable *gl = impl_from_drawable(base);
    EGLint attribs[5];
    EGLSurface new_surface;
    EGLContext ctx;
    RECT rect;
    int width, height;

    TRACE("%s, flags %#x\n", debugstr_opengl_drawable(base), flags);
    if (!(flags & GL_FLUSH_UPDATED) || !base->surface || !base->client) return;

    NtUserGetClientRect(base->client->hwnd, &rect, NtUserGetDpiForWindow(base->client->hwnd));
    width = max(1, rect.right - rect.left);
    height = max(1, rect.bottom - rect.top);
    if (width == gl->width && height == gl->height) return;

    attribs[0] = EGL_WIDTH;
    attribs[1] = width;
    attribs[2] = EGL_HEIGHT;
    attribs[3] = height;
    attribs[4] = EGL_NONE;
    new_surface = funcs->p_eglCreatePbufferSurface(egl_platform->display,
                                                   wx11_config_for_format(base->format), attribs);
    if (!new_surface)
    {
        WARN("pbuffer resize alloc failed hwnd=%p size=%dx%d error=%#x\n",
             base->client->hwnd, width, height, funcs->p_eglGetError());
        return;
    }
    ctx = funcs->p_eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT ||
        !funcs->p_eglMakeCurrent(egl_platform->display, new_surface, new_surface, ctx))
    {
        WARN("pbuffer rebind failed hwnd=%p error=%#x\n",
             base->client->hwnd, funcs->p_eglGetError());
        funcs->p_eglDestroySurface(egl_platform->display, new_surface);
        return;
    }
    funcs->p_eglDestroySurface(egl_platform->display, base->surface);
    base->surface = new_surface;
    WARN("pbuffer resized %dx%d -> %dx%d hwnd=%p\n", gl->width, gl->height, width, height,
         base->client->hwnd);
    gl->width = width;
    gl->height = height;
}

/* 呈现: 发布 id → flush → swap → 摘 id。三步顺序不可换 —— mesa 在 swap 内部
 * 读页面 (virgl_vtest_flush_frontbuffer), id 必须在 swap 期间可见; 之后立刻
 * 清掉, 迟到的 flush_frontbuffer 不会误挂到本窗。 */
static BOOL winehua_x11_drawable_swap(struct opengl_drawable *base)
{
    struct winehua_x11_gl_drawable *gl = impl_from_drawable(base);
    HWND hwnd = base->client->hwnd;
    HWND toplevel;
    Window xwindow;
    BOOL ok;

    /* id 取**受管顶层窗**的: 子窗 (WS_CHILD) 自己没有 whole_window ——
     * is_window_managed 只对 managed toplevel 建窗 —— 直接用子窗 hwnd 会永远
     * 拿不到 id ⇒ 整条通道静默不呈现 (review 2026-10-01)。GA_ROOT 沿 parent
     * 上溯到顶层。**已知限制**: 子窗内容按顶层窗矩形落位 (子矩形偏移当前不经
     * present 通道传递, 见 known-issues §2.11), 首次出现时提示一次。 */
    toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    if (toplevel && toplevel != hwnd && !gl->warned_child)
    {
        gl->warned_child = TRUE;
        WARN("hwnd %p is a child window, presenting through its toplevel %p; "
             "sub-rect offset is not carried by the present channel\n", hwnd, toplevel);
    }
    xwindow = X11DRV_get_whole_window(toplevel ? toplevel : hwnd);

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
    /* 页建不起来 (无 TMPDIR / open/mmap 失败) 时整条通道只会"什么都不发生"
     * (id 恒 0 ⇒ 一帧都不会 present), 而降级告警说的是"宿主没挂接" —— 方向
     * 相反。这里是唯一能区分"通道断了"与"宿主没挂接"的探针 (header 的约定),
     * 所以显式探一次并留痕 (review 2026-10-01: 该函数此前无人调用)。 */
    if (!winehua_present_surface_mapped())
        ERR("present page not mapped (TMPDIR/mmap failed): frames will never reach the host\n");
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
    if (!opengl_funcs->egl_handle)
    {
        /* STATUS_NOT_SUPPORTED 的上游含义是"运行期缺 libGL"; 通道已武装但本进程
         * 没有 egl_handle 时也走这里 —— 按旧含义会被误读 (known-issues §2.11
         * 小项)。真实原因显式打出来。 */
        ERR("no EGL handle in this process: private present unavailable "
            "(channel armed, but no EGL to create the pbuffer with)\n");
        return STATUS_NOT_SUPPORTED;
    }
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
