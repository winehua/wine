/*
 * WineHua virtual memory helpers (noexec filesystem workarounds)
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

#ifndef __WINE_OHOS_VIRTUAL_H
#define __WINE_OHOS_VIRTUAL_H

#ifdef __OHOS__

#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

/* Enable JIT for noexec filesystem via prctl.
 * Call before any mmap/mprotect with PROT_EXEC on noexec filesystem.
 * Box64's InternalMmap & NewBrick also need JIT, so the caller decides
 * whether to pair with ohos_jit_disable(). */
static inline void ohos_jit_enable(void)
{
    prctl( 0x6a6974, 0, 0 );
}

static inline void ohos_jit_disable(void)
{
    prctl( 0x6a6974, 0, 1 );
}

/* mprotect after enabling JIT — used when PROT_EXEC is set on noexec fs.
 * Does NOT disable JIT (Box64 still needs it). */
int ohos_mprotect_exec( void *base, size_t size, int unix_prot );

/* Map an executable PE section using anonymous memory + pread.
 * view_base: base address of the file view (view->base)
 * virtual_address: RVA of the section (sec->VirtualAddress)
 * section_name: for error messages (sec->Name formatted)
 * JIT is enabled for mmap and disabled immediately after (paired). */
int ohos_map_exec_section( void *view_base, int fd,
                           size_t virtual_address, size_t file_size, off_t file_start,
                           size_t host_page_mask,
                           const char *section_name );

/* Register Wine's SIGSEGV/SIGBUS handler as an OHOS musl sigchain special
 * handler so recoverable dynarec write faults are claimed before DFX dumps
 * and freezes the thread. wine_handler is the existing ntdll segv_handler. */
void ohos_install_sigchain_fault_handlers( void (*wine_handler)(int, siginfo_t *, void *) );

/* wowbox64.dll (PE) registers this from BTCpuProcessInit. handler is
 * wowbox64_handle_host_fault; NULL unregisters. */
void ohos_set_wowbox64_host_fault( void *handler );

#endif /* __OHOS__ */

#endif /* __WINE_OHOS_VIRTUAL_H */
