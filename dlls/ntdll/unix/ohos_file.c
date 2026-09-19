/*
 * WineHua file path helpers (no-symlink drive mapping)
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

#include <stdio.h>
#include <stdlib.h>

#include "ohos_file.h"


/***********************************************************************
 *           ohos_drive_unix_path
 *
 * Resolve a DOS drive letter to its default Unix path on OHOS.
 * Symlinks are not available in the NAPI sandbox, so the mapping is
 * hardcoded:
 *   'z'        → $HOME (/storage/Users/currentUser)
 *   'c' … 'y' → config_dir/drive_X
 *
 * Returns the path length (excluding null) or 0 if not mappable.
 */
int ohos_drive_unix_path( char drive, const char *config_dir,
                           char *buf, size_t size )
{
    if (drive == 'z')
    {
        const char *home = getenv( "HOME" );
        if (!home) home = "/storage/Users/currentUser";
        return snprintf( buf, size, "%s", home );
    }
    if (drive >= 'c' && drive <= 'y')
        return snprintf( buf, size, "%s/drive_%c", config_dir, drive );
    return 0;
}

#endif /* __OHOS__ */
