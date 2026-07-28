/*
 * WineHua virtual memory helpers — noexec filesystem workarounds
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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ohos_virtual.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(virtual);


/***********************************************************************
 *           ohos_mprotect_exec
 *
 * mprotect wrapper for OHOS noexec filesystem.
 * Enables JIT before calling mprotect with PROT_EXEC.
 * Does NOT disable JIT — Box64's InternalMmap & NewBrick also need it.
 */
int ohos_mprotect_exec( void *base, size_t size, int unix_prot )
{
    ohos_jit_enable();
    return mprotect( base, size, unix_prot );
}


/***********************************************************************
 *           ohos_map_exec_section
 *
 * Map an executable PE section using anonymous memory + pread to work
 * around OHOS's noexec filesystem.  JIT is enabled for the anon mmap
 * call and disabled immediately after (paired).
 */
int ohos_map_exec_section( void *view_base, int fd,
                           size_t virtual_address, size_t file_size, off_t file_start,
                           size_t host_page_mask,
                           const char *section_name )
{
    char *sec_addr = (char *)view_base + virtual_address;
    size_t sec_offset = virtual_address & host_page_mask;
    size_t sec_map_size = (file_size + sec_offset + host_page_mask) & ~host_page_mask;

    ohos_jit_enable();
    if (mmap( sec_addr - sec_offset, sec_map_size + sec_offset,
              PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0 ) == MAP_FAILED)
    {
        ohos_jit_disable();
        ERR( "Could not map %s section with anon mmap\n", section_name );
        return 0;
    }
    ohos_jit_disable();

    if (pread( fd, sec_addr, file_size, file_start ) != (ssize_t)file_size)
    {
        ERR( "Could not read %s section\n", section_name );
        return 0;
    }
    return 1;
}
