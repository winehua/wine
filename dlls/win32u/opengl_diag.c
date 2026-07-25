/*
 * WineHua OpenGL diagnostic helpers
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

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "windef.h"
#include "opengl_diag.h"

/* WINEHUA_OPENGL_DIAG env var guard — cached for fast path */
static BOOL winehua_opengl_diag_enabled(void)
{
    static int cached = -1;
    const char *value;

    if (cached != -1) return cached;

    value = getenv( "WINEHUA_OPENGL_DIAG" );
    cached = !!(value && value[0] && strcmp( value, "0" ));
    return cached;
}


/***********************************************************************
 *           winehua_opengl_diag
 *
 * Print diagnostic output to stderr when WINEHUA_OPENGL_DIAG is set.
 */
void winehua_opengl_diag( const char *fmt, ... )
{
    va_list args;

    if (!winehua_opengl_diag_enabled()) return;

    fprintf( stderr, "winehua_opengl_diag: " );
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
    fputc( '\n', stderr );
    fflush( stderr );
}


/***********************************************************************
 *           winehua_preload_guest_egl_deps
 *
 * Preload guest Mesa shared libraries from the same directory as the
 * guest EGL/GL library.  This ensures dependencies are resolved before
 * dlopen'ing the main EGL library (needed for VirGL in-box64 setups).
 */
void winehua_preload_guest_egl_deps( const char *egl_path )
{
    static const char *deps[] =
    {
        "libffi.so.8",
        "libdrm.so.2",
        "libwayland-client.so.0",
        "libwayland-server.so.0",
        "libwayland-egl.so.1",
        "libgallium-25.0.1.so",
        "libGLESv2.so.2",
        "libGLESv1_CM.so.1",
    };
    const char *slash;
    char dir[4096], path[4096];
    size_t len;
    unsigned int i;

    if (!egl_path || !(slash = strrchr( egl_path, '/' ))) return;
    len = slash - egl_path;
    if (!len || len >= sizeof(dir)) return;
    memcpy( dir, egl_path, len );
    dir[len] = 0;

    for (i = 0; i < ARRAY_SIZE(deps); i++)
    {
        if (snprintf( path, sizeof(path), "%s/%s", dir, deps[i] ) >= sizeof(path)) continue;
        if (access( path, R_OK )) continue;
        if (!dlopen( path, RTLD_NOW | RTLD_GLOBAL ))
            winehua_opengl_diag( "preload %s failed: %s", path, dlerror() );
    }
}

#endif /* __OHOS__ */
