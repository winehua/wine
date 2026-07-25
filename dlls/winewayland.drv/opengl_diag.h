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

#ifndef __WINE_WAYLAND_OPENGL_DIAG_H
#define __WINE_WAYLAND_OPENGL_DIAG_H

#include <stdint.h>
#include <pthread.h>
#include "windef.h"
#include "winbase.h"

BOOL winehua_env_enabled(const char *name);

void winehua_wayland_diag(const char *fmt, ...);

enum winehua_gl_stage
{
    WINEHUA_GL_IDLE,
    WINEHUA_GL_SWAP_ENTER,
    WINEHUA_GL_FLUSH,
    WINEHUA_GL_READBACK,
    WINEHUA_GL_CPU_COPY,
    WINEHUA_GL_SHM_COMMIT,
};

uint64_t winehua_gl_stage_begin(enum winehua_gl_stage stage, HWND hwnd, LONG in_flight);
void winehua_gl_stage_end(enum winehua_gl_stage stage, uint64_t started);
void winehua_gl_diag_idle(void);

extern pthread_mutex_t winehua_gl_diag_mutex;
extern unsigned long winehua_gl_commits;
extern unsigned long winehua_gl_releases;
extern unsigned long winehua_gl_drops;
extern LONG winehua_gl_in_flight;
extern LONG winehua_gl_zero_copy_presents;
extern LONG winehua_gl_readbacks;

#endif /* __WINE_WAYLAND_OPENGL_DIAG_H */
