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

#ifdef __OHOS__

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdlib.h>

#include "waylanddrv.h"
#include "wayland_surface_ohos.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);


/**********************************************************************
 *          wayland_surface_update_min_max
 *
 * Send window resize constraints to the compositor so that
 * non-resizable windows are also prevented from being resized on
 * the HarmonyOS side.
 *
 * Logic:
 *   WS_THICKFRAME && !WS_DLGFRAME → resizable (min=small, max=0=unlimited)
 *   otherwise (dialog / message box etc.) → non-resizable (min == max == current size)
 *
 * Called from wayland_surface_reconfigure() after each xdg_toplevel
 * state change.
 */
void wayland_surface_update_min_max( struct wayland_surface *surface )
{
    LONG style;
    int cur_w, cur_h;

    if (!surface->xdg_toplevel) return;

    style = NtUserGetWindowLongW( surface->hwnd, GWL_STYLE );

    /* current window size (surface-local) */
    cur_w = surface->window.rect.right - surface->window.rect.left;
    cur_h = surface->window.rect.bottom - surface->window.rect.top;
    wayland_surface_coords_from_window( surface, cur_w, cur_h, &cur_w, &cur_h );

    /* WS_THICKFRAME without WS_DLGFRAME → resizable, otherwise lock size */
    if ((style & WS_THICKFRAME) && !(style & WS_DLGFRAME))
    {
        /* resizable: set a small min to avoid window collapse, max=0 unlimited */
        surface->min_width  = max( 1, cur_w / 4 );
        surface->min_height = max( 1, cur_h / 4 );
        surface->max_width  = 0;
        surface->max_height = 0;
    }
    else
    {
        /* non-resizable (dialog etc.): min == max, compositor blocks user resize */
        surface->min_width  = cur_w;
        surface->min_height = cur_h;
        surface->max_width  = cur_w;
        surface->max_height = cur_h;
    }

    TRACE( "hwnd=%p style=0x%x min=%dx%d max=%dx%d\n",
           surface->hwnd, (unsigned)style,
           surface->min_width, surface->min_height,
           surface->max_width, surface->max_height );

    xdg_toplevel_set_min_size( surface->xdg_toplevel, surface->min_width, surface->min_height );
    xdg_toplevel_set_max_size( surface->xdg_toplevel, surface->max_width, surface->max_height );
}

#endif /* __OHOS__ */
