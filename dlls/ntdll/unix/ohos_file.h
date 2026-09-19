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

#ifndef __WINE_OHOS_FILE_H
#define __WINE_OHOS_FILE_H

#ifdef __OHOS__

#include <stddef.h>

/* Resolve a DOS drive letter to its default Unix path on OHOS.
 *
 * On OHOS the dosdevices/ symlinks are unavailable; this provides the
 * hardcoded fallback mapping:
 *   'z'        → $HOME (fallback /storage/Users/currentUser)
 *   'c' … 'y' → $WINEPREFIX/drive_X
 *   everything else → empty (returns 0)
 *
 * Parameters:
 *   drive:       drive letter (lowercase, 'c' … 'z')
 *   config_dir:  WINEPREFIX path (e.g. "/data/storage/el2/base/files/.wine")
 *   buf:         output buffer
 *   size:        output buffer size
 *
 * Returns: number of bytes written (excluding null terminator),
 *          or 0 if the drive letter is not mappable. */
int ohos_drive_unix_path( char drive, const char *config_dir,
                           char *buf, size_t size );

#endif /* __OHOS__ */

#endif /* __WINE_OHOS_FILE_H */
