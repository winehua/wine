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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ohos_virtual.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(virtual);


/***********************************************************************
 *           ohos_anon_replace_page
 *
 * Replace one file-backed page with an anonymous page, keeping content.
 *
 * OHOS noexec filesystems refuse PROT_EXEC on pages that come from a file
 * mapping (EACCES).  The JIT prctl does not help there — it only unlocks
 * anonymous pages.  Packed executables remap their whole image RWX to
 * decrypt, which hits any section that is still file-backed (.data, .edata,
 * .rdata ...), not just the executable ones handled by
 * ohos_map_exec_section().  noexec only forbids PROT_EXEC, RW stays
 * allowed, so the page can be copied out first, remapped anonymous, and
 * copied back — afterwards adding PROT_EXEC succeeds.
 *
 * len must not exceed one page: tmp is a single-page scratch buffer and the
 * MAP_FIXED remap rounds up to a page boundary, so a larger len would both
 * overflow tmp and clobber memory past the intended range.
 */
static int ohos_anon_replace_page( char *page, size_t len, int unix_prot, char *tmp )
{
    if (mprotect( page, len, PROT_READ | PROT_WRITE ))
    {
        fprintf( stderr, "[OHOS-VIRT] cannot make %p readable: %s\n", page, strerror( errno ) );
        return -1;
    }
    memcpy( tmp, page, len );

    if (mmap( page, len, PROT_READ | PROT_WRITE,
              MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0 ) == MAP_FAILED)
    {
        fprintf( stderr, "[OHOS-VIRT] cannot remap %p anonymous: %s\n", page, strerror( errno ) );
        return -1;
    }
    memcpy( page, tmp, len );

    if (mprotect( page, len, unix_prot ))
    {
        fprintf( stderr, "[OHOS-VIRT] cannot set prot %#x on %p: %s\n",
                 unix_prot, page, strerror( errno ) );
        return -1;
    }
    return 0;
}


/***********************************************************************
 *           ohos_anon_replace_range
 *
 * Page-by-page fallback for ohos_mprotect_exec(): every page whose
 * mprotect() still fails gets swapped to anonymous memory.  Pages that
 * succeed are left untouched, so a range mixing anon and file-backed
 * pages only pays for the file-backed ones.
 *
 * Note the page loses MAP_SHARED semantics if it had any.  PE image
 * mapping is MAP_PRIVATE (write-copy), so sections are safe; a shared
 * section remapped executable would silently become process-local.  That
 * combination has not been observed, and the alternative is the failure
 * this function exists to work around.
 */
static int ohos_anon_replace_range( char *addr, size_t size, int unix_prot )
{
    long page_size = sysconf( _SC_PAGESIZE );
    char *tmp;
    size_t done, replaced = 0;

    if (page_size <= 0) page_size = 4096;
    /* mprotect_range() always passes page-aligned ranges; a partial page
     * would make MAP_FIXED clobber memory outside the range. */
    if (size % page_size) return -1;

    tmp = mmap( NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (tmp == MAP_FAILED) return -1;

    for (done = 0; done < size; done += page_size)
    {
        char *page = addr + done;

        if (mprotect( page, page_size, unix_prot ) == 0) continue;  /* already anon */

        if (errno != EACCES && errno != EPERM)
        {
            fprintf( stderr, "[OHOS-VIRT] mprotect %p failed: %s\n", page, strerror( errno ) );
            munmap( tmp, page_size );
            return -1;
        }
        if (ohos_anon_replace_page( page, page_size, unix_prot, tmp ))
        {
            munmap( tmp, page_size );
            return -1;
        }
        replaced++;
    }

    fprintf( stderr, "[OHOS-VIRT] %p-%p: replaced %zu/%zu pages with anon\n",
             addr, addr + size, replaced, size / page_size );
    munmap( tmp, page_size );
    return 0;
}


/***********************************************************************
 *           ohos_mprotect_exec
 *
 * mprotect wrapper for OHOS noexec filesystem.
 * Enables JIT before calling mprotect with PROT_EXEC.
 * Does NOT disable JIT — Box64's InternalMmap & NewBrick also need it.
 *
 * Fast path is the plain mprotect: anonymous pages (and every normal
 * program) succeed immediately.  Only noexec file-backed pages fail, and
 * those are retried with ohos_anon_replace_range().
 */
int ohos_mprotect_exec( void *base, size_t size, int unix_prot )
{
    ohos_jit_enable();
    if (mprotect( base, size, unix_prot ) == 0) return 0;

    if (errno != EACCES && errno != EPERM) return -1;

    return ohos_anon_replace_range( base, size, unix_prot );
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
    /* OHOS 上页权限在创建时确定: mprotect 无法把已存在的 RW 页变为可执行
     * (实测返回成功但权限不变, 执行即 SEGV_ACCERR), 而新建匿名页的权限是
     * 生效的。故映射时直接带上 PROT_EXEC —— 页随后保持 RWX (wine 的
     * set_vprot(RX) 在 OHOS 上不生效), 可执行性已满足。 */
    if (mmap( sec_addr - sec_offset, sec_map_size + sec_offset,
              PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0 ) == MAP_FAILED)
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
