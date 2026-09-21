/*
 * WineHua OpenGL diagnostic framework
 *
 * Copyright 2025-2026 WineHua contributors
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

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "opengl_diag.h"

BOOL winehua_env_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0");
}

void winehua_wayland_diag(const char *fmt, ...)
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

static pthread_once_t winehua_gl_diag_once = PTHREAD_ONCE_INIT;
pthread_mutex_t winehua_gl_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static enum winehua_gl_stage winehua_gl_current_stage;
static uint64_t winehua_gl_stage_started_ms;
static uint64_t winehua_gl_last_stall_report_ms;
static uint64_t winehua_gl_last_slow_report_ms;
static unsigned long winehua_gl_thread;
static HWND winehua_gl_hwnd;
unsigned long winehua_gl_commits;
unsigned long winehua_gl_releases;
unsigned long winehua_gl_drops;
LONG winehua_gl_in_flight;
LONG winehua_gl_zero_copy_presents;
LONG winehua_gl_readbacks;

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

uint64_t winehua_gl_stage_begin(enum winehua_gl_stage stage, HWND hwnd, LONG in_flight)
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

void winehua_gl_stage_end(enum winehua_gl_stage stage, uint64_t started)
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

void winehua_gl_diag_idle(void)
{
    if (!winehua_env_enabled("WINEHUA_GL_STALL_DIAG")) return;
    pthread_mutex_lock(&winehua_gl_diag_mutex);
    winehua_gl_current_stage = WINEHUA_GL_IDLE;
    pthread_mutex_unlock(&winehua_gl_diag_mutex);
}
