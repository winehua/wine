/*
 * WineHua OpenGL readback pipeline
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

#ifndef __WINE_WAYLAND_OPENGL_READBACK_H
#define __WINE_WAYLAND_OPENGL_READBACK_H

#include "wine/opengl_driver.h"

extern struct opengl_driver_funcs winehua_readback_driver_funcs;
extern const struct opengl_driver_funcs *winehua_base_driver_funcs;

BOOL winehua_readback_surface_create(HWND hwnd, int format, struct opengl_drawable **drawable);
void winehua_readback_init_egl_platform(struct egl_platform *platform);
BOOL winehua_readback_make_current(struct opengl_drawable *draw,
                                   struct opengl_drawable *read, void *context);

#endif /* __WINE_WAYLAND_OPENGL_READBACK_H */
