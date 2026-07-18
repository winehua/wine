/*
 * Wayland OpenGL functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#include "waylanddrv.h"
#include "wine/debug.h"

#ifdef HAVE_LIBWAYLAND_EGL

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#include <wayland-egl.h>

#include "wine/opengl_driver.h"

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs wayland_drawable_funcs;
static const struct opengl_drawable_funcs winehua_readback_drawable_funcs;
static struct opengl_driver_funcs winehua_readback_driver_funcs;
static const struct opengl_driver_funcs *winehua_base_driver_funcs;

#define WINEHUA_PRESENT_SURFACE_MAGIC 0x57535053u
#define WINEHUA_PRESENT_SURFACE_VERSION 1u

struct winehua_present_surface_page
{
    uint32_t magic;
    uint32_t version;
    uint32_t surface_id;
    uint32_t reserved;
};

static pthread_once_t winehua_present_surface_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t winehua_present_surface_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct winehua_present_surface_page *winehua_present_surface_page;

struct wayland_gl_drawable
{
    struct opengl_drawable base;
    struct wl_egl_window *wl_egl_window;
};

static struct wayland_gl_drawable *impl_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_gl_drawable, base);
}

static void wayland_drawable_destroy(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);
    if (gl->wl_egl_window) wl_egl_window_destroy(gl->wl_egl_window);
}

static EGLConfig egl_config_for_format(int format)
{
    return egl->configs[(format - 1) % egl->config_count];
}

static void wayland_gl_drawable_sync_size(struct wayland_gl_drawable *gl)
{
    int client_width, client_height;
    RECT client_rect = {0};

    NtUserGetClientRect(gl->base.client->hwnd, &client_rect, NtUserGetDpiForWindow(gl->base.client->hwnd));
    client_width = client_rect.right - client_rect.left;
    client_height = client_rect.bottom - client_rect.top;
    if (client_width == 0 || client_height == 0) client_width = client_height = 1;

    wl_egl_window_resize(gl->wl_egl_window, client_width, client_height, 0, 0);
}

static BOOL wayland_opengl_surface_create(HWND hwnd, int format, struct opengl_drawable **drawable)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_client_surface *client;
    EGLint attribs[4], *attrib = attribs;
    struct opengl_drawable *previous;
    struct wayland_gl_drawable *gl;
    RECT rect;

    TRACE("hwnd=%p format=%d\n", hwnd, format);

    if ((previous = *drawable) && previous->format == format) return TRUE;

    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    if (rect.right == rect.left) rect.right = rect.left + 1;
    if (rect.bottom == rect.top) rect.bottom = rect.top + 1;

    if (!egl->has_EGL_EXT_present_opaque)
        WARN("Missing EGL_EXT_present_opaque extension\n");
    else
    {
        *attrib++ = EGL_PRESENT_OPAQUE_EXT;
        *attrib++ = EGL_TRUE;
    }
    *attrib++ = EGL_NONE;

    if (!(client = wayland_client_surface_create(hwnd))) return FALSE;
    gl = opengl_drawable_create(sizeof(*gl), &wayland_drawable_funcs, format, &client->client);
    client_surface_release(&client->client);
    if (!gl) return FALSE;

    opengl_drawable_map_buffer(&gl->base, GL_FRONT_LEFT, GL_BACK_LEFT);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT, GL_BACK);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT_AND_BACK, GL_BACK);
    if (gl->base.stereo) opengl_drawable_map_buffer(&gl->base, GL_FRONT_RIGHT, GL_BACK_RIGHT);

    if (!(gl->wl_egl_window = wl_egl_window_create(client->wl_surface, rect.right, rect.bottom))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->wl_egl_window, attribs))) goto err;
    set_client_surface(hwnd, client);

    TRACE("Created drawable %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);

    if (previous) opengl_drawable_release( previous );
    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static void wayland_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_WAYLAND_KHR;
    platform->native_display = process_wayland.wl_display;
    platform->force_pbuffer_formats = TRUE;
    egl = platform;
}

static BOOL winehua_env_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0");
}

static uint32_t winehua_prepare_present_surface(struct wayland_client_surface *client)
{
    return client && client->wl_surface
        ? wl_proxy_get_id((struct wl_proxy *)client->wl_surface) : 0;
}

static void winehua_init_present_surface_page(void)
{
    const char *tmp_dir = getenv("TMPDIR");
    struct winehua_present_surface_page *page;
    char path[256];
    int fd;

    if (!tmp_dir || !tmp_dir[0] ||
        snprintf(path, sizeof(path), "%s/winehua_present_surface_%u.shm",
                 tmp_dir, (uint32_t)getpid()) >= sizeof(path))
        return;
    if ((fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600)) < 0)
        return;
    if (ftruncate(fd, sizeof(*page)) ||
        (page = mmap(NULL, sizeof(*page), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        close(fd);
        return;
    }
    close(fd);

    page->version = WINEHUA_PRESENT_SURFACE_VERSION;
    page->reserved = 0;
    __atomic_store_n(&page->surface_id, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&page->magic, WINEHUA_PRESENT_SURFACE_MAGIC, __ATOMIC_RELEASE);
    winehua_present_surface_page = page;
}

static void winehua_begin_present_surface(uint32_t surface_id)
{
    pthread_once(&winehua_present_surface_once, winehua_init_present_surface_page);
    pthread_mutex_lock(&winehua_present_surface_mutex);
    if (winehua_present_surface_page)
        __atomic_store_n(&winehua_present_surface_page->surface_id,
                         surface_id, __ATOMIC_RELEASE);
}

static void winehua_finish_present_surface(void)
{
    if (winehua_present_surface_page)
        __atomic_store_n(&winehua_present_surface_page->surface_id,
                         0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&winehua_present_surface_mutex);
}

static BOOL winehua_surface_zero_copy_ready(uint32_t surface_id)
{
    const char *ready_dir = getenv("WINEHUA_ZERO_COPY_READY_DIR");
    char path[256];
    uint64_t surface_key;

    if (!surface_id || !ready_dir || !ready_dir[0]) return FALSE;
    surface_key = ((uint64_t)(uint32_t)getpid() << 32) | surface_id;
    if (snprintf(path, sizeof(path), "%s/winehua_zc_surface_%llu.ready",
                 ready_dir, (unsigned long long)surface_key) >= sizeof(path))
        return FALSE;
    return access(path, F_OK) == 0;
}

static void winehua_wayland_diag(const char *fmt, ...)
{
    va_list args;

    if (!winehua_env_enabled("WINEHUA_OPENGL_DIAG")) return;

    fprintf(stderr, "winehua_wayland_gl: ");
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

enum winehua_gl_stage
{
    WINEHUA_GL_IDLE,
    WINEHUA_GL_SWAP_ENTER,
    WINEHUA_GL_FLUSH,
    WINEHUA_GL_READBACK,
    WINEHUA_GL_CPU_COPY,
    WINEHUA_GL_SHM_COMMIT,
};

static pthread_once_t winehua_gl_diag_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t winehua_gl_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static enum winehua_gl_stage winehua_gl_current_stage;
static uint64_t winehua_gl_stage_started_ms;
static uint64_t winehua_gl_last_stall_report_ms;
static uint64_t winehua_gl_last_slow_report_ms;
static unsigned long winehua_gl_thread;
static HWND winehua_gl_hwnd;
static unsigned long winehua_gl_commits;
static unsigned long winehua_gl_releases;
static unsigned long winehua_gl_drops;
static LONG winehua_gl_in_flight;
static LONG winehua_gl_zero_copy_presents;
static LONG winehua_gl_readbacks;

static uint64_t winehua_gl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *winehua_gl_stage_name(enum winehua_gl_stage stage)
{
    switch (stage)
    {
    case WINEHUA_GL_SWAP_ENTER: return "swap-enter";
    case WINEHUA_GL_FLUSH: return "glFlush";
    case WINEHUA_GL_READBACK: return "glReadPixels";
    case WINEHUA_GL_CPU_COPY: return "cpu-copy";
    case WINEHUA_GL_SHM_COMMIT: return "shm-commit";
    default: return "idle";
    }
}

static void *winehua_gl_watchdog(void *unused)
{
    for (;;)
    {
        enum winehua_gl_stage stage;
        uint64_t now, started;
        unsigned long thread, commits, releases, drops;
        LONG in_flight;
        HWND hwnd;

        usleep(250000);
        now = winehua_gl_now_ms();
        pthread_mutex_lock(&winehua_gl_diag_mutex);
        stage = winehua_gl_current_stage;
        started = winehua_gl_stage_started_ms;
        thread = winehua_gl_thread;
        hwnd = winehua_gl_hwnd;
        commits = winehua_gl_commits;
        releases = winehua_gl_releases;
        drops = winehua_gl_drops;
        in_flight = winehua_gl_in_flight;
        if (stage != WINEHUA_GL_IDLE && now - started >= 1000 &&
            now - winehua_gl_last_stall_report_ms >= 1000)
            winehua_gl_last_stall_report_ms = now;
        else
            stage = WINEHUA_GL_IDLE;
        pthread_mutex_unlock(&winehua_gl_diag_mutex);

        if (stage != WINEHUA_GL_IDLE)
        {
            fprintf(stderr,
                    "winehua_gl_stall: stage=%s age_ms=%llu thread=%lu hwnd=%p in_flight=%d commits=%lu releases=%lu drops=%lu\n",
                    winehua_gl_stage_name(stage), (unsigned long long)(now - started),
                    thread, hwnd, (int)in_flight, commits, releases, drops);
            fflush(stderr);
        }
    }
    return NULL;
}

static void winehua_gl_diag_init(void)
{
    pthread_t thread;
    if (!winehua_env_enabled("WINEHUA_GL_STALL_DIAG")) return;
    if (!pthread_create(&thread, NULL, winehua_gl_watchdog, NULL)) pthread_detach(thread);
}

static uint64_t winehua_gl_stage_begin(enum winehua_gl_stage stage, HWND hwnd, LONG in_flight)
{
    uint64_t now;
    if (!winehua_env_enabled("WINEHUA_GL_STALL_DIAG")) return 0;
    pthread_once(&winehua_gl_diag_once, winehua_gl_diag_init);
    now = winehua_gl_now_ms();
    pthread_mutex_lock(&winehua_gl_diag_mutex);
    winehua_gl_current_stage = stage;
    winehua_gl_stage_started_ms = now;
    winehua_gl_thread = (unsigned long)pthread_self();
    winehua_gl_hwnd = hwnd;
    winehua_gl_in_flight = in_flight;
    pthread_mutex_unlock(&winehua_gl_diag_mutex);
    return now;
}

static void winehua_gl_stage_end(enum winehua_gl_stage stage, uint64_t started)
{
    uint64_t now;
    BOOL report = FALSE;
    if (!started) return;
    now = winehua_gl_now_ms();
    pthread_mutex_lock(&winehua_gl_diag_mutex);
    if (winehua_gl_current_stage == stage) winehua_gl_current_stage = WINEHUA_GL_IDLE;
    if (now - started >= 50 && now - winehua_gl_last_slow_report_ms >= 1000)
    {
        winehua_gl_last_slow_report_ms = now;
        report = TRUE;
    }
    pthread_mutex_unlock(&winehua_gl_diag_mutex);
    if (report)
    {
        fprintf(stderr, "winehua_gl_slow: stage=%s duration_ms=%llu\n",
                winehua_gl_stage_name(stage), (unsigned long long)(now - started));
        fflush(stderr);
    }
}

static void winehua_gl_diag_idle(void)
{
    if (!winehua_env_enabled("WINEHUA_GL_STALL_DIAG")) return;
    pthread_mutex_lock(&winehua_gl_diag_mutex);
    winehua_gl_current_stage = WINEHUA_GL_IDLE;
    pthread_mutex_unlock(&winehua_gl_diag_mutex);
}

static void wayland_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);

    TRACE("drawable %s, flags %#x\n", debugstr_opengl_drawable(base), flags);

    if (flags & GL_FLUSH_INTERVAL) funcs->p_eglSwapInterval(egl->display, abs(base->interval));

    /* Since context_flush is called from operations that may latch the native size,
     * perform any pending resizes before calling them. */
    if (flags & GL_FLUSH_UPDATED) wayland_gl_drawable_sync_size(gl);
}

struct winehua_readback_drawable
{
    struct opengl_drawable base;
    int width;
    int height;
    BYTE *rgba;
    SIZE_T rgba_size;
    struct winehua_readback_state *state;
};

struct winehua_readback_slot
{
    struct winehua_readback_state *state;
    struct wayland_shm_buffer *buffer;
    LONG busy;
};

#define WINEHUA_READBACK_MAX_IN_FLIGHT 3

struct winehua_readback_state
{
    LONG ref;
    LONG in_flight;
    int width;
    int height;
    struct winehua_readback_slot slots[WINEHUA_READBACK_MAX_IN_FLIGHT];
};

static void winehua_readback_state_ref(struct winehua_readback_state *state)
{
    InterlockedIncrement(&state->ref);
}

static void winehua_readback_state_unref(struct winehua_readback_state *state)
{
    unsigned int i;

    if (InterlockedDecrement(&state->ref)) return;
    for (i = 0; i < ARRAY_SIZE(state->slots); ++i)
        if (state->slots[i].buffer) wayland_shm_buffer_unref(state->slots[i].buffer);
    free(state);
}

static struct winehua_readback_drawable *winehua_readback_from_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct winehua_readback_drawable, base);
}

static void winehua_readback_drawable_destroy(struct opengl_drawable *base)
{
    struct winehua_readback_drawable *gl = winehua_readback_from_drawable(base);

    if (base->surface) funcs->p_eglDestroySurface(egl->display, base->surface);
    free(gl->rgba);
    if (gl->state) winehua_readback_state_unref(gl->state);
}

static void winehua_readback_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    TRACE("%s, flags %#x\n", debugstr_opengl_drawable(base), flags);
}

static void winehua_readback_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct winehua_readback_slot *slot = data;
    LONG in_flight;

    slot->buffer->busy = FALSE;
    InterlockedExchange(&slot->busy, FALSE);
    in_flight = InterlockedDecrement(&slot->state->in_flight);
    if (winehua_env_enabled("WINEHUA_GL_STALL_DIAG"))
    {
        pthread_mutex_lock(&winehua_gl_diag_mutex);
        winehua_gl_releases++;
        winehua_gl_in_flight = in_flight;
        pthread_mutex_unlock(&winehua_gl_diag_mutex);
    }
    winehua_readback_state_unref(slot->state);
}

static const struct wl_buffer_listener winehua_readback_buffer_listener =
{
    winehua_readback_buffer_release,
};

static struct winehua_readback_state *winehua_readback_state_create(HWND hwnd,
                                                                    int width, int height)
{
    struct winehua_readback_state *state;
    unsigned int i;

    if (!(state = calloc(1, sizeof(*state)))) return NULL;
    state->ref = 1;
    state->width = width;
    state->height = height;

    for (i = 0; i < ARRAY_SIZE(state->slots); ++i)
    {
        struct winehua_readback_slot *slot = &state->slots[i];

        slot->state = state;
        if (!(slot->buffer = wayland_shm_buffer_create(width, height, WL_SHM_FORMAT_ARGB8888)))
        {
            winehua_wayland_diag("readback shm pool allocation failed hwnd=%p size=%dx%d slot=%u",
                                 hwnd, width, height, i);
            winehua_readback_state_unref(state);
            return NULL;
        }
        wl_proxy_set_queue((struct wl_proxy *)slot->buffer->wl_buffer,
                           process_wayland.wl_event_queue);
        if (wl_buffer_add_listener(slot->buffer->wl_buffer,
                                   &winehua_readback_buffer_listener, slot) < 0)
        {
            winehua_wayland_diag("readback buffer listener registration failed hwnd=%p slot=%u",
                                 hwnd, i);
            winehua_readback_state_unref(state);
            return NULL;
        }
    }
    return state;
}

static struct winehua_readback_slot *winehua_readback_acquire_slot(
    struct winehua_readback_state *state)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(state->slots); ++i)
        if (!InterlockedCompareExchange(&state->slots[i].busy, TRUE, FALSE))
            return &state->slots[i];
    return NULL;
}

static BOOL winehua_readback_present(struct opengl_drawable *base)
{
    struct winehua_readback_drawable *gl = winehua_readback_from_drawable(base);
    struct winehua_readback_state *new_state;
    struct winehua_readback_slot *slot = NULL;
    struct wayland_client_surface *client;
    struct wayland_shm_buffer *buffer = NULL;
    BYTE *rgba = NULL, *src, *dst;
    RECT rect = {0};
    BOOL submitted = FALSE, ret = FALSE;
    SIZE_T rgba_size;
    uint64_t stage_started;
    uint32_t surface_id;
    GLint pack_alignment, pack_buffer, pack_row_length, pack_skip_pixels, pack_skip_rows;
    GLint read_fbo;
    BOOL swap_ok;
    LONG presents, readbacks;
    int y;

    if (!base->client || !base->client->hwnd) return FALSE;
    client = CONTAINING_RECORD(base->client, struct wayland_client_surface, client);
    if (!client->wl_surface) return FALSE;

    NtUserGetClientRect(base->client->hwnd, &rect, NtUserGetDpiForWindow(base->client->hwnd));
    gl->width = max(1, rect.right - rect.left);
    gl->height = max(1, rect.bottom - rect.top);

    /* The pbuffer is sized at drawable creation and never followed window
     * resizes (only the SHM state did). After a display mode change the
     * guest keeps rendering at pbuffer dimensions while glReadPixels(0, 0,
     * client_w, client_h) reads the bottom-left corner — frames appear
     * shifted down by (old_h - new_h) with their bottom rows lost.
     * Recreate the pbuffer and rebind the current context when the client
     * size changes; skip this frame (it was rendered at the old size), the
     * next one renders correctly. */
    {
        EGLint pb_w = 0, pb_h = 0;
        funcs->p_eglQuerySurface(egl->display, base->surface, EGL_WIDTH, &pb_w);
        funcs->p_eglQuerySurface(egl->display, base->surface, EGL_HEIGHT, &pb_h);
        if (pb_w != gl->width || pb_h != gl->height)
        {
            EGLint attribs[5];
            EGLSurface new_surface;
            EGLContext ctx = funcs->p_eglGetCurrentContext();

            attribs[0] = EGL_WIDTH;
            attribs[1] = gl->width;
            attribs[2] = EGL_HEIGHT;
            attribs[3] = gl->height;
            attribs[4] = EGL_NONE;
            new_surface = funcs->p_eglCreatePbufferSurface(egl->display,
                                                           egl_config_for_format(base->format),
                                                           attribs);
            if (!new_surface)
            {
                winehua_wayland_diag("readback pbuffer resize alloc failed hwnd=%p size=%dx%d error=%#x",
                                     base->client->hwnd, gl->width, gl->height,
                                     funcs->p_eglGetError());
            }
            else if (ctx == EGL_NO_CONTEXT ||
                     !funcs->p_eglMakeCurrent(egl->display, new_surface, new_surface, ctx))
            {
                winehua_wayland_diag("readback pbuffer rebind failed hwnd=%p error=%#x",
                                     base->client->hwnd, funcs->p_eglGetError());
                funcs->p_eglDestroySurface(egl->display, new_surface);
            }
            else
            {
                fprintf(stderr, "winehua_readback: pbuffer resized %dx%d -> %dx%d hwnd=%p\n",
                        (int)pb_w, (int)pb_h, gl->width, gl->height, base->client->hwnd);
                fflush(stderr);
                funcs->p_eglDestroySurface(egl->display, base->surface);
                base->surface = new_surface;
                return TRUE;
            }
        }
    }
    surface_id = winehua_prepare_present_surface(client);

    if (surface_id && winehua_surface_zero_copy_ready(surface_id))
    {
        winehua_begin_present_surface(surface_id);
        /* Pbuffer swap alone does not reliably flush the front resource. */
        funcs->p_glFlush();
        swap_ok = funcs->p_eglSwapBuffers(egl->display, base->surface);
        presents = InterlockedIncrement(&winehua_gl_zero_copy_presents);
        winehua_finish_present_surface();
        if (presents == 1 || !(presents % 120))
        {
            fprintf(stderr,
                    "winehua_gl_zero_copy: presents=%d readbacks=%d pid=%u surface=%u size=%dx%d\n",
                    (int)presents, (int)winehua_gl_readbacks, (uint32_t)getpid(), surface_id,
                    gl->width, gl->height);
            fflush(stderr);
        }
        if (!swap_ok)
            winehua_wayland_diag("zero-copy pbuffer swap failed hwnd=%p surface=%u error=%#x",
                                 base->client->hwnd, surface_id, funcs->p_eglGetError());
        return swap_ok;
    }

    if (gl->state->width != gl->width || gl->state->height != gl->height)
    {
        if (!(new_state = winehua_readback_state_create(base->client->hwnd,
                                                        gl->width, gl->height)))
            return FALSE;
        winehua_readback_state_unref(gl->state);
        gl->state = new_state;
    }
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_SWAP_ENTER, base->client->hwnd,
                                           gl->state->in_flight);

    rgba_size = (SIZE_T)gl->width * gl->height * 4;
    if (gl->rgba_size < rgba_size)
    {
        if (!(rgba = realloc(gl->rgba, rgba_size)))
        {
            winehua_wayland_diag("readback rgba allocation failed hwnd=%p size=%dx%d",
                                 base->client->hwnd, gl->width, gl->height);
            goto done;
        }
        gl->rgba = rgba;
        gl->rgba_size = rgba_size;
    }
    rgba = gl->rgba;

    funcs->p_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    funcs->p_glGetIntegerv(GL_PACK_ALIGNMENT, &pack_alignment);
    funcs->p_glGetIntegerv(GL_PACK_ROW_LENGTH, &pack_row_length);
    funcs->p_glGetIntegerv(GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
    funcs->p_glGetIntegerv(GL_PACK_SKIP_ROWS, &pack_skip_rows);
    funcs->p_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);

    /* SwapBuffers presents the drawable's default framebuffer. Isolate the
     * CPU readback from PBO/FBO and pixel-pack state left by the application. */
    if (pack_buffer) funcs->p_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    if (pack_alignment != 1) funcs->p_glPixelStorei(GL_PACK_ALIGNMENT, 1);
    if (pack_row_length) funcs->p_glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (pack_skip_pixels) funcs->p_glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    if (pack_skip_rows) funcs->p_glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    if (read_fbo) funcs->p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

    /* Virpipe submits queued guest commands on flush. Ensure the host has
     * completed this frame before the synchronous readback starts. */
    winehua_gl_stage_end(WINEHUA_GL_SWAP_ENTER, stage_started);
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_FLUSH, base->client->hwnd,
                                           gl->state->in_flight);
    winehua_begin_present_surface(surface_id);
    funcs->p_glFlush();
    winehua_finish_present_surface();
    winehua_gl_stage_end(WINEHUA_GL_FLUSH, stage_started);
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_READBACK, base->client->hwnd,
                                           gl->state->in_flight);
    readbacks = InterlockedIncrement(&winehua_gl_readbacks);
    /* 诊断 (#246 画面下移): 锁定 stale 高度基准在哪一环 —
     * client rect vs pbuffer 实际尺寸 vs 应用最后设置的 viewport */
    if (readbacks == 1 || !(readbacks % 300))
    {
        EGLint pb_w = 0, pb_h = 0;
        GLint vp[4] = {0};
        funcs->p_eglQuerySurface(egl->display, base->surface, EGL_WIDTH, &pb_w);
        funcs->p_eglQuerySurface(egl->display, base->surface, EGL_HEIGHT, &pb_h);
        funcs->p_glGetIntegerv(GL_VIEWPORT, vp);
        fprintf(stderr,
                "winehua_readback_diag: hwnd=%p client=%dx%d pbuf=%dx%d viewport=%d,%d %dx%d\n",
                base->client->hwnd, gl->width, gl->height, (int)pb_w, (int)pb_h,
                (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3]);
        fflush(stderr);
    }
    funcs->p_glReadPixels(0, 0, gl->width, gl->height, GL_BGRA, GL_UNSIGNED_BYTE, rgba);
    winehua_gl_stage_end(WINEHUA_GL_READBACK, stage_started);

    /* Keep the pbuffer's WSI present semantics active so the virpipe winsys
     * receives the exact front resource while readback remains the fallback. */
    winehua_begin_present_surface(surface_id);
    if (!funcs->p_eglSwapBuffers(egl->display, base->surface))
        winehua_wayland_diag("readback pbuffer swap failed hwnd=%p error=%#x",
                             base->client->hwnd, funcs->p_eglGetError());
    winehua_finish_present_surface();
    if (readbacks == 1 || !(readbacks % 120))
    {
        fprintf(stderr,
                "winehua_gl_present_bridge: readbacks=%d pid=%u surface=%u mapped=%s\n",
                (int)readbacks, (uint32_t)getpid(), surface_id,
                winehua_present_surface_page ? "yes" : "no");
        fflush(stderr);
    }

    if (read_fbo) funcs->p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    if (pack_skip_rows) funcs->p_glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
    if (pack_skip_pixels) funcs->p_glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
    if (pack_row_length) funcs->p_glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
    if (pack_alignment != 1) funcs->p_glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    if (pack_buffer) funcs->p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);

    /* The readback above is the VirGL completion point. If all display
     * buffers are busy, drop only this presentation copy; never skip the GL
     * flush/readback and never wait for Wayland while holding Wine GL state. */
    if (!(slot = winehua_readback_acquire_slot(gl->state)))
    {
        if (winehua_env_enabled("WINEHUA_GL_STALL_DIAG"))
        {
            pthread_mutex_lock(&winehua_gl_diag_mutex);
            winehua_gl_drops++;
            winehua_gl_in_flight = gl->state->in_flight;
            pthread_mutex_unlock(&winehua_gl_diag_mutex);
        }
        ret = TRUE;
        goto done;
    }
    InterlockedIncrement(&gl->state->in_flight);
    buffer = slot->buffer;

    /* OpenGL readback starts at the lower-left; Wayland SHM starts at the
     * upper-left. BGRA readback already matches little-endian ARGB8888. */
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_CPU_COPY, base->client->hwnd,
                                           gl->state->in_flight);
    for (y = 0; y < gl->height; ++y)
    {
        src = rgba + (gl->height - 1 - y) * gl->width * 4;
        dst = (BYTE *)buffer->map_data + y * gl->width * 4;
        memcpy(dst, src, gl->width * 4);
    }
    winehua_gl_stage_end(WINEHUA_GL_CPU_COPY, stage_started);

    buffer->busy = TRUE;
    winehua_readback_state_ref(gl->state);
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_SHM_COMMIT, base->client->hwnd,
                                           gl->state->in_flight);
    wl_surface_attach(client->wl_surface, buffer->wl_buffer, 0, 0);
    wl_surface_damage_buffer(client->wl_surface, 0, 0, gl->width, gl->height);
    if (client->wp_viewport) wp_viewport_set_destination(client->wp_viewport, gl->width, gl->height);
    wl_surface_commit(client->wl_surface);
    wl_display_flush(process_wayland.wl_display);
    winehua_gl_stage_end(WINEHUA_GL_SHM_COMMIT, stage_started);
    if (winehua_env_enabled("WINEHUA_GL_STALL_DIAG"))
    {
        pthread_mutex_lock(&winehua_gl_diag_mutex);
        winehua_gl_commits++;
        winehua_gl_in_flight = gl->state->in_flight;
        pthread_mutex_unlock(&winehua_gl_diag_mutex);
    }
    submitted = ret = TRUE;

done:
    winehua_gl_diag_idle();
    if (!submitted)
    {
        if (slot)
        {
            buffer->busy = FALSE;
            InterlockedExchange(&slot->busy, FALSE);
            InterlockedDecrement(&gl->state->in_flight);
        }
    }
    return ret;
}

static BOOL winehua_readback_drawable_swap(struct opengl_drawable *base)
{
    client_surface_present(base->client);
    return winehua_readback_present(base);
}

static const struct opengl_drawable_funcs winehua_readback_drawable_funcs =
{
    .destroy = winehua_readback_drawable_destroy,
    .flush = winehua_readback_drawable_flush,
    .swap = winehua_readback_drawable_swap,
};

static BOOL winehua_readback_surface_create(HWND hwnd, int format, struct opengl_drawable **drawable)
{
    struct winehua_readback_drawable *gl;
    struct wayland_client_surface *client;
    struct opengl_drawable *previous;
    EGLint attribs[5];
    RECT rect;

    if ((previous = *drawable) && previous->format == format) return TRUE;
    if (!(client = wayland_client_surface_create(hwnd))) return FALSE;
    pthread_once(&winehua_present_surface_once, winehua_init_present_surface_page);
    if (!(gl = opengl_drawable_create(sizeof(*gl), &winehua_readback_drawable_funcs,
                                      format, &client->client)))
    {
        client_surface_release(&client->client);
        return FALSE;
    }
    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    gl->width = max(1, rect.right - rect.left);
    gl->height = max(1, rect.bottom - rect.top);
    if (!(gl->state = winehua_readback_state_create(hwnd, gl->width, gl->height)))
    {
        winehua_wayland_diag("readback state allocation failed hwnd=%p", hwnd);
        opengl_drawable_release(&gl->base);
        client_surface_release(&client->client);
        return FALSE;
    }
    attribs[0] = EGL_WIDTH;
    attribs[1] = gl->width;
    attribs[2] = EGL_HEIGHT;
    attribs[3] = gl->height;
    attribs[4] = EGL_NONE;

    gl->base.surface = funcs->p_eglCreatePbufferSurface(egl->display,
                                                        egl_config_for_format(format), attribs);
    if (!gl->base.surface)
    {
        winehua_wayland_diag("readback pbuffer creation failed hwnd=%p format=%d size=%dx%d error=%#x",
                             hwnd, format, gl->width, gl->height, funcs->p_eglGetError());
        opengl_drawable_release(&gl->base);
        client_surface_release(&client->client);
        return FALSE;
    }

    opengl_drawable_map_buffer(&gl->base, GL_FRONT_LEFT, GL_BACK_LEFT);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT, GL_BACK);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT_AND_BACK, GL_BACK);
    if (gl->base.stereo) opengl_drawable_map_buffer(&gl->base, GL_FRONT_RIGHT, GL_BACK_RIGHT);

    set_client_surface(hwnd, client);
    client_surface_release(&client->client);
    if (previous) opengl_drawable_release(previous);
    *drawable = &gl->base;
    return TRUE;
}

static void winehua_readback_init_egl_platform(struct egl_platform *platform)
{
    winehua_base_driver_funcs->p_init_egl_platform(platform);
    egl = platform;
}

static BOOL winehua_readback_make_current(struct opengl_drawable *draw,
                                          struct opengl_drawable *read, void *context)
{
    return winehua_base_driver_funcs->p_make_current(draw, read, context);
}

static BOOL wayland_drawable_swap(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);

    client_surface_present(base->client);
    funcs->p_eglSwapBuffers(egl->display, gl->base.surface);

    return TRUE;
}

struct wayland_pbuffer
{
    struct opengl_drawable base;
    struct wl_surface *surface;
    struct wl_egl_window *window;
};

static struct wayland_pbuffer *pbuffer_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_pbuffer, base);
}

static void wayland_pbuffer_destroy(struct opengl_drawable *base)
{
    struct wayland_pbuffer *gl = pbuffer_from_opengl_drawable(base);

    TRACE("%s\n", debugstr_opengl_drawable(base));

    if (gl->window)
        wl_egl_window_destroy(gl->window);
    if (gl->surface)
        wl_surface_destroy(gl->surface);
}

static const struct opengl_drawable_funcs wayland_pbuffer_funcs =
{
    .destroy = wayland_pbuffer_destroy,
};

static BOOL wayland_pbuffer_create(HDC hdc, int format, BOOL largest, GLenum texture_format, GLenum texture_target,
                                   GLint max_level, GLsizei *width, GLsizei *height, struct opengl_drawable **surface)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_pbuffer *gl;

    TRACE("hdc %p, format %d, largest %u, texture_format %#x, texture_target %#x, max_level %#x, width %d, height %d, private %p\n",
          hdc, format, largest, texture_format, texture_target, max_level, *width, *height, surface);

    if (!(gl = opengl_drawable_create(sizeof(*gl), &wayland_pbuffer_funcs, format, NULL))) return FALSE;
    /* Wayland EGL doesn't support pixmap or pbuffer, create a dummy window surface to act as the target render surface. */
    if (!(gl->surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    if (!(gl->window = wl_egl_window_create(gl->surface, *width, *height))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->window, NULL))) goto err;

    TRACE("Created pbuffer %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);
    *surface = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static BOOL wayland_pbuffer_updated(HDC hdc, struct opengl_drawable *base, GLenum cube_face, GLint mipmap_level)
{
    return GL_TRUE;
}

static UINT wayland_pbuffer_bind(HDC hdc, struct opengl_drawable *base, GLenum buffer)
{
    return -1; /* use default implementation */
}

static struct opengl_driver_funcs wayland_driver_funcs =
{
    .p_init_egl_platform = wayland_init_egl_platform,
    .p_surface_create = wayland_opengl_surface_create,
    .p_pbuffer_create = wayland_pbuffer_create,
    .p_pbuffer_updated = wayland_pbuffer_updated,
    .p_pbuffer_bind = wayland_pbuffer_bind,
};

static const struct opengl_drawable_funcs wayland_drawable_funcs =
{
    .destroy = wayland_drawable_destroy,
    .flush = wayland_drawable_flush,
    .swap = wayland_drawable_swap,
};

/**********************************************************************
 *           WAYLAND_OpenGLInit
 */
UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR("Version mismatch, opengl32 wants %u but driver has %u\n",
            version, WINE_OPENGL_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    if (winehua_env_enabled("WINEHUA_WAYLAND_READBACK"))
    {
        winehua_base_driver_funcs = *driver_funcs;
        winehua_readback_driver_funcs = *winehua_base_driver_funcs;
        winehua_readback_driver_funcs.p_init_egl_platform = winehua_readback_init_egl_platform;
        winehua_readback_driver_funcs.p_surface_create = winehua_readback_surface_create;
        winehua_readback_driver_funcs.p_make_current = winehua_readback_make_current;
        *driver_funcs = &winehua_readback_driver_funcs;
        return STATUS_SUCCESS;
    }

    wayland_driver_funcs.p_get_proc_address = (*driver_funcs)->p_get_proc_address;
    wayland_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    wayland_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    wayland_driver_funcs.p_init_extensions = (*driver_funcs)->p_init_extensions;
    wayland_driver_funcs.p_context_create = (*driver_funcs)->p_context_create;
    wayland_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    wayland_driver_funcs.p_make_current = (*driver_funcs)->p_make_current;

    *driver_funcs = &wayland_driver_funcs;
    return STATUS_SUCCESS;
}

#else /* No GL */

UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif
