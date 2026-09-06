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

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"

#include "ohos_virtual.h"
#include "wine/debug.h"
#ifdef __OHOS__
#ifdef __aarch64__
#include "wine/asm.h"
#endif
#endif

WINE_DEFAULT_DEBUG_CHANNEL(virtual);

#ifdef __OHOS__
#ifdef __aarch64__
int ohos_call_pe_fault( void *fn, void *teb, int sig, int si_code, void *addr,
                        uint64_t *pc, uint64_t *xrip, uint32_t *prot, int *kind );
#endif
#endif


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
    if (!mprotect( base, size, unix_prot )) return 0;

    /* Anonymous RWX mmap is EPERM on OHOS. mprotect(RWX) is often rejected
     * too (W^X). Dynarec unprotect only needs the guest page writable —
     * native execution is in a separate JIT mapping. Drop EXEC, keep WRITE. */
    if ((unix_prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
    {
        int rw = unix_prot & ~PROT_EXEC;
        WARN( "mprotect(%p,%lu,%x) failed, retry W^X-safe %x\n",
              base, (unsigned long)size, unix_prot, rw );
        return mprotect( base, size, rw );
    }
    return -1;
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


#ifdef __OHOS__

#include <elf.h>
#include <link.h>

#ifndef SYS_gettid
#define SYS_gettid 178
#endif

/* musl sigchain special-handler ABI (same as Android ART sigchain).
 * Returning non-zero claims the signal and stops later slots (DFX). */
struct ohos_sigchain_action
{
    int (*sca_sigaction)(int, siginfo_t *, void *);
    sigset_t sca_mask;
    uint64_t sca_flags;
};

struct ohos_k_sigaction
{
    void (*handler)(int, siginfo_t *, void *);
    unsigned long flags;
    void (*restorer)(void);
    unsigned long mask[2];
};

/* Keep in sync with box64 custommem.h */
#define OHOS_PROT_DYNAREC    0x80
#define OHOS_PROT_DYNAREC_R  0x40

#define WOWBOX64_FAULT_NOT_MINE 0
#define WOWBOX64_FAULT_HANDLED  1
#define WOWBOX64_FAULT_KIND_RETRY  0
#define WOWBOX64_FAULT_KIND_EPILOG 1

typedef int (*ohos_wowbox64_fault_fn)(int sig, int si_code, void *fault_addr,
                                      uint64_t *pc_inout, uint64_t *xrip_out,
                                      uint32_t *prot_out, int *kind_out);

static void (*wine_fault_handler)(int, siginfo_t *, void *);
static ohos_wowbox64_fault_fn volatile p_wowbox64_host_fault;
static unsigned ohos_smc_log_count;

void ohos_set_wowbox64_host_fault( void *handler )
{
    p_wowbox64_host_fault = (ohos_wowbox64_fault_fn)handler;
    fprintf( stderr, "[ntdll] wowbox64 host fault handler %p unix_mprotect %p\n",
             handler, ohos_mprotect_exec );
}

static void ohos_smc_write( const char *buf, size_t bufsz, int n )
{
    if (n < 0) return;
    if ((size_t)n >= bufsz) n = (int)bufsz - 1;
    write( 2, buf, (size_t)n );
}

static int ohos_phdr_cb( struct dl_phdr_info *info, size_t size, void *data )
{
    uintptr_t start = 0, end = 0;
    int i;
    char buf[384];
    int n;

    (void)size;
    (void)data;
    for (i = 0; i < info->dlpi_phnum; i++)
    {
        uintptr_t s, e;
        if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
        s = (uintptr_t)info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        e = s + info->dlpi_phdr[i].p_memsz;
        if (!start || s < start) start = s;
        if (e > end) end = e;
    }
    if (end)
    {
        n = snprintf( buf, sizeof(buf), "[ntdll] phdr %s %p-%p\n",
                      (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "<main>",
                      (void *)start, (void *)end );
        ohos_smc_write( buf, sizeof(buf), n );
    }
    return 0;
}

static void ohos_fmt_prot( char *buf, size_t n, uint32_t prot )
{
    snprintf( buf, n, "%c%c%c%s%s",
              (prot & 0x1) ? 'R' : '-',
              (prot & 0x2) ? 'W' : '-',
              (prot & 0x4) ? 'X' : '-',
              (prot & OHOS_PROT_DYNAREC) ? "|PROT_DYNAREC" : "",
              (prot & OHOS_PROT_DYNAREC_R) ? "|PROT_DYNAREC_R" : "" );
}

static void ohos_smc_log_enter( int sig, const siginfo_t *info, uint64_t pc_in, void *teb, void *fn )
{
    char buf[256];
    int n;
    long tid;

    /* Same cap as ohos_smc_log. Uncapped enter lines filled ~500MB wine_stderr
     * when winedbg looped on a JIT NULL deref after Heaven's FS-base crash. */
    ohos_smc_log_count++;
    if (ohos_smc_log_count > 64 && (ohos_smc_log_count % 32) != 0) return;

    tid = syscall( SYS_gettid );
    n = snprintf( buf, sizeof(buf),
                  "[SMC] enter tid=%ld sig=%d addr=%p pc_in=%p teb=%p fn=%p\n",
                  tid, sig, info ? info->si_addr : NULL, (void *)(uintptr_t)pc_in, teb, fn );
    ohos_smc_write( buf, sizeof(buf), n );
}

static void ohos_smc_log( int sig, const siginfo_t *info, uint64_t pc_in, uint64_t pc_out,
                          uint32_t prot, int dynarec, const char *result, int wine )
{
    char protbuf[64];
    char buf[384];
    int n;
    long tid;

    /* Count lives in ohos_smc_log_enter (called first on every fault). */
    if (ohos_smc_log_count > 64 && (ohos_smc_log_count % 32) != 0) return;
    ohos_fmt_prot( protbuf, sizeof(protbuf), prot );
    tid = syscall( SYS_gettid );
    n = snprintf( buf, sizeof(buf),
                  "[SMC] tid=%ld sig=%d addr=%p pc_in=%p prot=%s dynarec=%d result=%s pc_out=%p wine=%d\n",
                  tid, sig, info ? info->si_addr : NULL, (void *)(uintptr_t)pc_in,
                  protbuf, dynarec, result, (void *)(uintptr_t)pc_out, wine );
    ohos_smc_write( buf, sizeof(buf), n );
}

/* Route Box64 SMC internally; never mix unprotect + Wine SEH on the same fault.
 * Always return 1 so DFX does not dump.
 *
 * wowbox64 is ARM64 PE: x18 is TEB (and GS cookies). JIT uses x18 as a guest
 * GPR, so the POSIX handler must restore TEB before calling PE. Also log with
 * write(2) — fprintf in this handler deadlocks on the FILE lock. */
static int ohos_route_host_fault( int sig, siginfo_t *info, void *uctx )
{
    ucontext_t *uc = uctx;
    uint64_t pc_in = 0, pc = 0, xrip = 0;
    uint32_t prot = 0;
    int kind = 0, handled = 0, dynarec = 0;
    ohos_wowbox64_fault_fn fn;
    const char *result = "not_mine";
    void *teb = NULL;

    if (uc)
    {
        pc_in = pc = uc->uc_mcontext.pc;
#ifdef __aarch64__
        xrip = uc->uc_mcontext.regs[27];
#endif
    }

    fn = p_wowbox64_host_fault;
#ifdef __aarch64__
    teb = NtCurrentTeb();
#endif
    ohos_smc_log_enter( sig, info, pc_in, teb, fn );

#ifdef __aarch64__
    if (fn && info && teb)
        handled = ohos_call_pe_fault( (void *)fn, teb, sig, info->si_code, info->si_addr,
                                      &pc, &xrip, &prot, &kind );
#else
    if (fn && info)
        handled = fn( sig, info->si_code, info->si_addr, &pc, &xrip, &prot, &kind );
#endif

    dynarec = (prot & (OHOS_PROT_DYNAREC | OHOS_PROT_DYNAREC_R)) ? 1 : 0;
    if (handled == WOWBOX64_FAULT_HANDLED)
    {
        if (uc)
        {
            uc->uc_mcontext.pc = pc;
            if (kind == WOWBOX64_FAULT_KIND_EPILOG && xrip)
                uc->uc_mcontext.regs[27] = xrip;
        }
        result = (kind == WOWBOX64_FAULT_KIND_EPILOG) ? "epilog" : "retry";
        ohos_smc_log( sig, info, pc_in, pc, prot, dynarec, result, 0 );
        return 1;
    }

    /* SIGILL that is not a dynarec CALLRET trap: let Wine ill_handler run. */
    if (sig == SIGILL)
    {
        ohos_smc_log( sig, info, pc_in, pc, prot, dynarec, result, 1 );
        return 0;
    }

    ohos_smc_log( sig, info, pc_in, pc, prot, dynarec, "wine_seh", 1 );
    if (wine_fault_handler) wine_fault_handler( sig, info, uctx );
    if (uc) pc = uc->uc_mcontext.pc;
    ohos_smc_log( sig, info, pc_in, pc, prot, dynarec, result, 1 );
    return 1;
}

static int ohos_sigchain_fault( int sig, siginfo_t *info, void *uctx )
{
    return ohos_route_host_fault( sig, info, uctx );
}

static int ohos_rt_sigaction( int sig, void (*handler)(int, siginfo_t *, void *), int flags, const sigset_t *mask )
{
    struct ohos_k_sigaction ksa;

    memset( &ksa, 0, sizeof(ksa) );
    ksa.handler = handler;
    ksa.flags = (unsigned long)flags;
    if (mask) memcpy( ksa.mask, mask, sizeof(ksa.mask) < sizeof(*mask) ? sizeof(ksa.mask) : sizeof(*mask) );
    return syscall( SYS_rt_sigaction, sig, &ksa, NULL, 8 ) ? -1 : 0;
}

static void ohos_rt_sigaction_fault( int sig, siginfo_t *info, void *uctx )
{
    ohos_route_host_fault( sig, info, uctx );
}

void ohos_install_sigchain_fault_handlers( void (*wine_handler)(int, siginfo_t *, void *) )
{
    void (*add_special)(int, struct ohos_sigchain_action *);
    struct ohos_sigchain_action sca;

    wine_fault_handler = wine_handler;
    if (getenv( "WINEHUA_DUMP_PHDR" ))
        dl_iterate_phdr( ohos_phdr_cb, NULL );

    add_special = dlsym( RTLD_DEFAULT, "AddSpecialSignalHandlerFn" );
    if (!add_special) add_special = dlsym( RTLD_DEFAULT, "add_special_signal_handler" );
    if (add_special)
    {
        memset( &sca, 0, sizeof(sca) );
        sca.sca_sigaction = ohos_sigchain_fault;
        sigfillset( &sca.sca_mask );
        sigdelset( &sca.sca_mask, SIGSEGV );
        sigdelset( &sca.sca_mask, SIGBUS );
        sigdelset( &sca.sca_mask, SIGILL );
        add_special( SIGSEGV, &sca );
        add_special( SIGBUS, &sca );
        add_special( SIGILL, &sca );
        fprintf( stderr, "[ntdll] OHOS sigchain claimed SIGSEGV/SIGBUS/SIGILL (Box64 SMC/CALLRET before DFX/Wine SEH)\n" );
        return;
    }

    /* libc hid the sigchain API: talk to the kernel, bypass DFX entirely. */
    if (!ohos_rt_sigaction( SIGSEGV, ohos_rt_sigaction_fault, SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER, NULL ) &&
        !ohos_rt_sigaction( SIGBUS,  ohos_rt_sigaction_fault, SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER, NULL ) &&
        !ohos_rt_sigaction( SIGILL,  ohos_rt_sigaction_fault, SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER, NULL ))
        fprintf( stderr, "[ntdll] OHOS rt_sigaction installed SIGSEGV/SIGBUS/SIGILL (bypass DFX)\n" );
    else
        fprintf( stderr, "[ntdll] OHOS fault-handler install failed, DFX may still intercept SIGSEGV\n" );
}

#ifdef __aarch64__
/* Set x18=TEB then call PE wowbox64_handle_host_fault. JIT leaves x18 as a
 * guest GPR; ARM64 PE RtlEnterCriticalSection / GS cookies require TEB. */
__ASM_GLOBAL_FUNC( ohos_call_pe_fault,
                   "stp x18, x30, [sp, #-16]!\n\t"
                   "mov x8, x0\n\t"
                   "mov x18, x1\n\t"
                   "mov w0, w2\n\t"
                   "mov w1, w3\n\t"
                   "mov x2, x4\n\t"
                   "mov x3, x5\n\t"
                   "mov x4, x6\n\t"
                   "mov x5, x7\n\t"
                   "ldr x6, [sp, #16]\n\t"
                   "blr x8\n\t"
                   "ldp x18, x30, [sp], #16\n\t"
                   "ret" )
#endif

#endif /* __OHOS__ */
