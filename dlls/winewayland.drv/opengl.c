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
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
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
                    "winehua_gl_stall: stage=%s age_ms=%llu thread=%lu hwnd=%p in_flight=%ld commits=%lu releases=%lu drops=%lu\n",
                    winehua_gl_stage_name(stage), (unsigned long long)(now - started),
                    thread, hwnd, in_flight, commits, releases, drops);
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
    struct winehua_readback_state *state;
};

struct winehua_readback_state
{
    LONG ref;
    LONG in_flight;
};

struct winehua_readback_submission
{
    struct winehua_readback_state *state;
    struct wayland_shm_buffer *buffer;
};

#define WINEHUA_READBACK_MAX_IN_FLIGHT 3

static void winehua_readback_state_ref(struct winehua_readback_state *state)
{
    InterlockedIncrement(&state->ref);
}

static void winehua_readback_state_unref(struct winehua_readback_state *state)
{
    if (!InterlockedDecrement(&state->ref)) free(state);
}

static struct winehua_readback_drawable *winehua_readback_from_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct winehua_readback_drawable, base);
}

static void winehua_readback_drawable_destroy(struct opengl_drawable *base)
{
    struct winehua_readback_drawable *gl = winehua_readback_from_drawable(base);

    if (base->surface) funcs->p_eglDestroySurface(egl->display, base->surface);
    if (gl->state) winehua_readback_state_unref(gl->state);
}

static void winehua_readback_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    TRACE("%s, flags %#x\n", debugstr_opengl_drawable(base), flags);
}

static void winehua_readback_buffer_release(void *data, struct wl_buffer *buffer)
{
    struct winehua_readback_submission *submission = data;
    LONG in_flight;

    submission->buffer->busy = FALSE;
    wayland_shm_buffer_unref(submission->buffer);
    in_flight = InterlockedDecrement(&submission->state->in_flight);
    if (winehua_env_enabled("WINEHUA_GL_STALL_DIAG"))
    {
        pthread_mutex_lock(&winehua_gl_diag_mutex);
        winehua_gl_releases++;
        winehua_gl_in_flight = in_flight;
        pthread_mutex_unlock(&winehua_gl_diag_mutex);
    }
    winehua_readback_state_unref(submission->state);
    free(submission);
}

static const struct wl_buffer_listener winehua_readback_buffer_listener =
{
    winehua_readback_buffer_release,
};

static BOOL winehua_readback_present(struct opengl_drawable *base)
{
    struct winehua_readback_drawable *gl = winehua_readback_from_drawable(base);
    struct winehua_readback_submission *submission = NULL;
    struct wayland_client_surface *client;
    struct wayland_shm_buffer *buffer = NULL;
    BYTE *rgba = NULL, *src, *dst;
    RECT rect = {0};
    BOOL slot_acquired = FALSE, submitted = FALSE, ret = FALSE;
    uint64_t stage_started;
    GLint pack_alignment, pack_buffer, pack_row_length, pack_skip_pixels, pack_skip_rows;
    GLint read_fbo;
    int x, y;

    if (!base->client || !base->client->hwnd) return FALSE;
    client = CONTAINING_RECORD(base->client, struct wayland_client_surface, client);
    if (!client->wl_surface) return FALSE;

    NtUserGetClientRect(base->client->hwnd, &rect, NtUserGetDpiForWindow(base->client->hwnd));
    gl->width = max(1, rect.right - rect.left);
    gl->height = max(1, rect.bottom - rect.top);
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_SWAP_ENTER, base->client->hwnd,
                                           gl->state->in_flight);

    if (!(rgba = malloc(gl->width * gl->height * 4)))
    {
        winehua_wayland_diag("readback rgba allocation failed hwnd=%p size=%dx%d",
                             base->client->hwnd, gl->width, gl->height);
        goto done;
    }
    if (!(submission = malloc(sizeof(*submission))))
    {
        winehua_wayland_diag("readback submission allocation failed hwnd=%p",
                             base->client->hwnd);
        goto done;
    }

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
    funcs->p_glFlush();
    winehua_gl_stage_end(WINEHUA_GL_FLUSH, stage_started);
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_READBACK, base->client->hwnd,
                                           gl->state->in_flight);
    funcs->p_glReadPixels(0, 0, gl->width, gl->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    winehua_gl_stage_end(WINEHUA_GL_READBACK, stage_started);

    if (read_fbo) funcs->p_glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
    if (pack_skip_rows) funcs->p_glPixelStorei(GL_PACK_SKIP_ROWS, pack_skip_rows);
    if (pack_skip_pixels) funcs->p_glPixelStorei(GL_PACK_SKIP_PIXELS, pack_skip_pixels);
    if (pack_row_length) funcs->p_glPixelStorei(GL_PACK_ROW_LENGTH, pack_row_length);
    if (pack_alignment != 1) funcs->p_glPixelStorei(GL_PACK_ALIGNMENT, pack_alignment);
    if (pack_buffer) funcs->p_glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);

    /* The readback above is the VirGL completion point. If all display
     * buffers are busy, drop only this presentation copy; never skip the GL
     * flush/readback and never wait for Wayland while holding Wine GL state. */
    if (InterlockedIncrement(&gl->state->in_flight) > WINEHUA_READBACK_MAX_IN_FLIGHT)
    {
        InterlockedDecrement(&gl->state->in_flight);
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
    slot_acquired = TRUE;

    if (!(buffer = wayland_shm_buffer_create(gl->width, gl->height, WL_SHM_FORMAT_ARGB8888)))
    {
        winehua_wayland_diag("readback shm allocation failed hwnd=%p size=%dx%d",
                             base->client->hwnd, gl->width, gl->height);
        goto done;
    }

    /* OpenGL readback starts at the lower-left; Wayland SHM starts at the
     * upper-left. Wayland ARGB8888 is BGRA in little-endian memory. */
    stage_started = winehua_gl_stage_begin(WINEHUA_GL_CPU_COPY, base->client->hwnd,
                                           gl->state->in_flight);
    for (y = 0; y < gl->height; ++y)
    {
        src = rgba + (gl->height - 1 - y) * gl->width * 4;
        dst = (BYTE *)buffer->map_data + y * gl->width * 4;
        for (x = 0; x < gl->width; ++x)
        {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    winehua_gl_stage_end(WINEHUA_GL_CPU_COPY, stage_started);

    submission->state = gl->state;
    submission->buffer = buffer;
    wl_proxy_set_queue((struct wl_proxy *)buffer->wl_buffer, process_wayland.wl_event_queue);
    if (wl_buffer_add_listener(buffer->wl_buffer, &winehua_readback_buffer_listener,
                               submission) < 0)
    {
        winehua_wayland_diag("readback buffer listener registration failed hwnd=%p",
                             base->client->hwnd);
        goto done;
    }
    buffer->busy = TRUE;
    wayland_shm_buffer_ref(buffer);
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
    free(rgba);
    if (buffer) wayland_shm_buffer_unref(buffer);
    if (!submitted)
    {
        free(submission);
        if (slot_acquired) InterlockedDecrement(&gl->state->in_flight);
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
    if (!(gl = opengl_drawable_create(sizeof(*gl), &winehua_readback_drawable_funcs,
                                      format, &client->client)))
    {
        client_surface_release(&client->client);
        return FALSE;
    }
    if (!(gl->state = calloc(1, sizeof(*gl->state))))
    {
        winehua_wayland_diag("readback state allocation failed hwnd=%p", hwnd);
        opengl_drawable_release(&gl->base);
        client_surface_release(&client->client);
        return FALSE;
    }
    gl->state->ref = 1;

    NtUserGetClientRect(hwnd, &rect, NtUserGetDpiForWindow(hwnd));
    gl->width = max(1, rect.right - rect.left);
    gl->height = max(1, rect.bottom - rect.top);
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
