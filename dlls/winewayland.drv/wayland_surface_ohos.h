/*
 * WineHua Wayland surface helpers (desktop mode / min-max constraints)
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

#ifndef __WINE_WAYLAND_SURFACE_OHOS_H
#define __WINE_WAYLAND_SURFACE_OHOS_H

#ifdef __OHOS__

/* OHOS desktop mode: valid window position coordinate range.
 * Positions outside this range are treated as uninitialized by the
 * compositor and must not be forwarded as xdg_surface geometry offsets. */
#define WINEHUA_DESKTOP_COORD_MIN  (-32768)
#define WINEHUA_DESKTOP_COORD_MAX  32768

struct wayland_surface;

/* Update xdg_toplevel min/max size constraints based on window style.
 *
 * In desktop mode the compositor enforces resize constraints per
 * xdg_toplevel protocol.  This helper inspects the Win32 window style
 * bits so that:
 *   - Resizable windows get a small min (1/4 current size) and no max.
 *   - Non-resizable windows (dialogs, message boxes) get min == max,
 *     which prevents the user from resizing them on the compositor side.
 *
 * Must be called after each xdg_toplevel reconfigure. */
void wayland_surface_update_min_max( struct wayland_surface *surface );

#endif /* __OHOS__ */

#endif /* __WINE_WAYLAND_SURFACE_OHOS_H */
