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
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
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
static int ohos_diag_quiet;
#ifdef __aarch64__
int ohos_call_pe_fault( void *fn, void *teb, int sig, int si_code, void *addr,
                        uint64_t *pc, uint64_t *xrip, uint32_t *prot, int *kind );
#endif
#endif


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
 */
int ohos_mprotect_exec( void *base, size_t size, int unix_prot )
{
    /* 2026-09-20 诊断 (ROUND3 §9.12): OHOS 的 RWX 到底是被"prctl 未生效"挡的还是被
     * "内核直接拒 RWX"挡的 —— 前者可以修(JIT 常开/正确的时机), 后者只能改方案(双映射)。
     * 记录: prctl 返回值、首次 mprotect 结果与 errno、是否走 W^X 安全回退。每进程 32 条。 */
    static int wx_log_count;
    const int jit_rc = ohos_jit_enable_rc();
    const int want_rwx = (unix_prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC);
    errno = 0;
    if (!mprotect( base, size, unix_prot ))
    {
        if (want_rwx && wx_log_count < 32)
        {
            char buf[200];
            int n = snprintf( buf, sizeof(buf),
                              "[WX-MPROTECT] pid=%d req=0x%x rwx=1 jit_rc=%d first=OK base=%p size=%zu\n",
                              getpid(), unix_prot, jit_rc, base, size );
            wx_log_count++;
            if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
        }
        return 0;
    }
    const int first_errno = errno;

    /* 文件映射页在 noexec 文件系统上加 PROT_EXEC 会被拒 (EACCES/EPERM) ——
     * 加壳程序把整个模块 (含 .data/.edata/.rdata) 改 RWX 解密时命中, 且壳不
     * 检查返回值继续写 → 只读页写入 → Access Violation。逐页换匿名页能保留
     * 请求的保护位 (含 EXEC), 比下面的 RW 降级更完整, 所以先试它; 只有换页
     * 也失败 (非 EACCES/EPERM 的硬错误) 才落到降级分支。 */
    if ((first_errno == EACCES || first_errno == EPERM) &&
        !ohos_anon_replace_range( base, size, unix_prot ))
    {
        if (wx_log_count < 32)
        {
            char buf[208];
            int n = snprintf( buf, sizeof(buf),
                              "[WX-MPROTECT] pid=%d req=0x%x rwx=%d jit_rc=%d first=FAIL errno=%d "
                              "anon_replace=OK base=%p size=%zu\n",
                              getpid(), unix_prot, want_rwx, jit_rc, first_errno, base, size );
            wx_log_count++;
            if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
        }
        return 0;
    }

    /* Anonymous RWX mmap is EPERM on OHOS. mprotect(RWX) is often rejected
     * too (W^X). Dynarec unprotect only needs the guest page writable —
     * native execution is in a separate JIT mapping. Drop EXEC, keep WRITE. */
    if ((unix_prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
    {
        int rw = unix_prot & ~PROT_EXEC;
        WARN( "mprotect(%p,%lu,%x) failed, retry W^X-safe %x\n",
              base, (unsigned long)size, unix_prot, rw );
        errno = 0;
        const int rc = mprotect( base, size, rw );
        if (wx_log_count < 32)
        {
            char buf[232];
            int n = snprintf( buf, sizeof(buf),
                              "[WX-MPROTECT] pid=%d req=0x%x rwx=1 jit_rc=%d first=FAIL errno=%d "
                              "fallback_rw=%d fallback_errno=%d base=%p size=%zu\n",
                              getpid(), unix_prot, jit_rc, first_errno, rc, errno, base, size );
            wx_log_count++;
            if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
        }
        return rc;
    }
    if (wx_log_count < 32)
    {
        char buf[200];
        int n = snprintf( buf, sizeof(buf),
                          "[WX-MPROTECT] pid=%d req=0x%x rwx=0 jit_rc=%d first=FAIL errno=%d base=%p size=%zu\n",
                          getpid(), unix_prot, jit_rc, first_errno, base, size );
        wx_log_count++;
        if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
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

static void (*wine_segv_handler)(int, siginfo_t *, void *);
static void (*wine_bus_handler)(int, siginfo_t *, void *);
static void (*wine_trap_handler)(int, siginfo_t *, void *);
/* thread.c 里实现: 读 ARM64EC 线程的 guest(x64) Rip/Rsp + 栈上的返回地址 */
extern void ohos_prof_dump_guest_context( void );
static ohos_wowbox64_fault_fn volatile p_wowbox64_host_fault;
static volatile sig_atomic_t ohos_smc_log_count;
static volatile sig_atomic_t ohos_trap_log_count;

struct ohos_signal_log
{
    char buf[512];
    size_t len;
};

static void ohos_signal_log_char( struct ohos_signal_log *log, char ch )
{
    if (log->len < sizeof(log->buf)) log->buf[log->len++] = ch;
}

static void ohos_signal_log_str( struct ohos_signal_log *log, const char *str )
{
    while (*str && log->len < sizeof(log->buf)) log->buf[log->len++] = *str++;
}

static void ohos_signal_log_dec( struct ohos_signal_log *log, int64_t value )
{
    char tmp[24];
    size_t len = 0;
    uint64_t number;

    if (value < 0)
    {
        ohos_signal_log_char( log, '-' );
        number = (uint64_t)(-(value + 1)) + 1;
    }
    else number = value;

    do
    {
        tmp[len++] = '0' + number % 10;
        number /= 10;
    } while (number && len < sizeof(tmp));
    while (len) ohos_signal_log_char( log, tmp[--len] );
}

static void ohos_signal_log_hex( struct ohos_signal_log *log, uint64_t value )
{
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    size_t len = 0;

    ohos_signal_log_str( log, "0x" );
    do
    {
        tmp[len++] = digits[value & 15];
        value >>= 4;
    } while (value && len < sizeof(tmp));
    while (len) ohos_signal_log_char( log, tmp[--len] );
}

static void ohos_signal_log_field_dec( struct ohos_signal_log *log, const char *name, int64_t value )
{
    ohos_signal_log_char( log, ' ' );
    ohos_signal_log_str( log, name );
    ohos_signal_log_char( log, '=' );
    ohos_signal_log_dec( log, value );
}

static void ohos_signal_log_field_hex( struct ohos_signal_log *log, const char *name, uint64_t value )
{
    ohos_signal_log_char( log, ' ' );
    ohos_signal_log_str( log, name );
    ohos_signal_log_char( log, '=' );
    ohos_signal_log_hex( log, value );
}

static void ohos_signal_log_emit( struct ohos_signal_log *log )
{
    ohos_signal_log_char( log, '\n' );
    write( 2, log->buf, log->len );
}

/* A saved FEX SP can point just past the emulator stack, in its guard page.
 * Never dereference it from the SIGSEGV handler: a nested synchronous fault
 * terminates the process before Wine/FEX can handle the original exception.
 * A kernel-mediated copy fails safely for guard/unmapped pages and concurrent
 * protection changes. OHOS may deny process_vm_readv; omit the dump then. */
static size_t ohos_smc_read_stack( uint64_t sp, uintptr_t *values, size_t count )
{
    const int saved_errno = errno;
    ssize_t bytes = -1;

#ifdef SYS_process_vm_readv
    struct iovec local = { values, count * sizeof(*values) };
    struct iovec remote = { (void *)(uintptr_t)(sp & ~(uint64_t)7), count * sizeof(*values) };
    if (sp) bytes = syscall( SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL );
#endif
    errno = saved_errno;
    return bytes > 0 ? (size_t)bytes / sizeof(*values) : 0;
}

/* TEMP-DIAG(GUEST-BT): dump the raw stack around a "call to NULL" (guest RIP 0)
 * so the caller can be resolved offline against the module map (+module). */
static void ohos_smc_dump_stack( uint64_t sp )
{
    uintptr_t values[40];
    const size_t count = ohos_smc_read_stack( sp, values, ARRAY_SIZE(values) );
    char buf[256];
    int i, n;

    for (i = 0; i < count; i++)
    {
        uintptr_t v = values[i];
        if (!v || (v >> 32) == 0) continue;   /* skip obviously non-code values */
        n = snprintf( buf, sizeof(buf), "[SMC-stack] sp+%03d = %p\n", i * 8, (void *)v );
        if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
    }
}

/* TEMP-DIAG(FAULT-MAP): 故障地址归属查询 (2026-09-18)
 * CEF renderer 在 win64 下每 7~8 秒吃一颗 SIGSEGV(SEGV_ACCERR) 之后被 NS 回调
 * 报 signal=11 退出, 但 [SMC] 行只有裸地址 —— 是 guest PE、FEX 代码缓存还是
 * Wine 堆, 光看地址判断不了, 根因定位就缺这一环。
 * handler 里按需读一次 /proc/self/maps (上限 256KB, open/read/write 都是
 * async-signal-safe), 给 addr / x27(guest rip) / pc 各打一行 [SMC-MAP]。
 * 每个进程最多打 24 组, 避免刷屏。 */
static char ohos_maps_cache[256 * 1024];
static size_t ohos_maps_cache_len;
static int ohos_maps_cache_ready;
/* Signal handlers must never wait for an interrupted reader of this buffer. */
static unsigned char ohos_maps_busy;
static int ohos_map_dump_count;
/* 2026-09-20: 归属配额 (按信号类型分开计数, 见 ohos_smc_log_enter)。 */
static int ohos_map_dump_quota;
/* 2026-09-20: 致命现场 (SIGSEGV + guest RIP==0) 的独立配额, 不受限流影响。 */
static int ohos_fatal_dump_count;

static void ohos_smc_load_maps( void )
{
    ssize_t n, total = 0;
    int fd;

    if (ohos_maps_cache_ready) return;
    ohos_maps_cache_ready = 1;
    fd = open( "/proc/self/maps", O_RDONLY );
    if (fd < 0) return;
    while (total < (ssize_t)sizeof(ohos_maps_cache) - 1)
    {
        n = read( fd, ohos_maps_cache + total, sizeof(ohos_maps_cache) - 1 - total );
        if (n <= 0) break;
        total += n;
    }
    close( fd );
    ohos_maps_cache[total] = 0;
    ohos_maps_cache_len = (size_t)total;
}

static void ohos_smc_map_lookup( const char *what, uint64_t v );
static int ohos_smc_parse_hex( const char **pp, uint64_t *out );

/* 2026-09-20: 致命故障时强制刷新 maps。
 * 原实现每进程只读一次 /proc/self/maps (用首次故障时的快照), 到真正致命的那次
 * (例如 renderer 崩在 0x37bb7bc830) 早已过期 —— 现场全部显示 <unmapped>, 没法把
 * 野地址落到模块。刷新代价 = 一次 open/read (async-signal-safe)。 */
static void ohos_smc_reload_maps( void )
{
    ohos_maps_cache_ready = 0;
    ohos_maps_cache_len = 0;
    ohos_smc_load_maps();
}

/* 2026-09-20: 栈顶若干字逐个做模块归属 —— 找 return address, 定位"野指针是谁给的"。
 * 只查前 12 个字, 且受 ohos_map_dump_count 全局配额约束 (上限 256 行)。 */
static void ohos_smc_attribute_stack( uint64_t sp )
{
    uintptr_t values[12];
    const size_t count = ohos_smc_read_stack( sp, values, ARRAY_SIZE(values) );
    char what[16];
    int i;

    if (!sp) return;
    for (i = 0; i < count; i++)
    {
        uintptr_t v = values[i];
        if (!v || v < 0x10000) continue;
        snprintf( what, sizeof(what), "stk+%d", i * 8 );
        ohos_smc_map_lookup( what, (uint64_t)v );
    }
}

/* 2026-09-20: 取地址所在 VMA 的"名字"字段 (maps 行末), 用于把故障按区域归类。
 * 目前最关心 FEX 的 per-thread call-ret 栈 (FEXMem_CallRetStacks):
 * 它两侧是 guard page, X17 是 callret SP; 失衡时 X17 会跑出窗口 —— 那类故障必须单独可见。 */
static int ohos_smc_region_name( uint64_t v, char *out, size_t out_n )
{
    char *line, *end;

    if (!v || !out || out_n < 8) return 0;
    out[0] = 0;
    ohos_smc_load_maps();
    if (!ohos_maps_cache_len) return 0;

    for (line = ohos_maps_cache; line < ohos_maps_cache + ohos_maps_cache_len; line = end + 1)
    {
        const char *p = line, *q, *name;
        uint64_t lo, hi;

        end = memchr( line, '\n', (size_t)(ohos_maps_cache + ohos_maps_cache_len - line) );
        if (!end) break;
        if (!ohos_smc_parse_hex( &p, &lo ) || *p != '-') continue;
        p++;
        if (!ohos_smc_parse_hex( &p, &hi )) continue;
        if (v < lo || v >= hi) continue;

        *end = 0;
        q = end;
        while (q > line && q[-1] == ' ') q--;
        name = q;
        while (name > line && name[-1] != ' ') name--;
        { size_t len = (size_t)(q - name);
          size_t i;
          if (len > out_n - 1) len = out_n - 1;
          for (i = 0; i < len; i++) out[i] = name[i];
          out[len] = 0; }
        *end = '\n';
        return 1;
    }
    return 0;
}

/* 2026-09-20: call-ret 栈相关故障单独计数/打印 (每进程 32 次), 不受限流影响。 */
static int ohos_callret_dump_count;

/* 2026-09-20: 取地址所在 VMA 的 perms(4 字符) 与名字。 */
static int ohos_smc_region_info( uint64_t v, char *perms, size_t perms_n,
                                char *name, size_t name_n )
{
    char *line, *end;

    if (!v) return 0;
    if (perms && perms_n) perms[0] = 0;
    if (name && name_n) name[0] = 0;
    ohos_smc_load_maps();
    if (!ohos_maps_cache_len) return 0;

    for (line = ohos_maps_cache; line < ohos_maps_cache + ohos_maps_cache_len; line = end + 1)
    {
        const char *p = line, *q, *nm;
        uint64_t lo, hi;
        size_t i;

        end = memchr( line, '\n', (size_t)(ohos_maps_cache + ohos_maps_cache_len - line) );
        if (!end) break;
        if (!ohos_smc_parse_hex( &p, &lo ) || *p != '-') continue;
        p++;
        if (!ohos_smc_parse_hex( &p, &hi )) continue;
        if (v < lo || v >= hi) continue;

        while (*p == ' ') p++;
        if (perms && perms_n)
        {
            for (i = 0; i + 1 < perms_n && p[i] && p[i] != ' '; i++) perms[i] = p[i];
            perms[i] = 0;
        }
        if (name && name_n)
        {
            *end = 0;
            q = end;
            while (q > line && q[-1] == ' ') q--;
            nm = q;
            while (nm > line && nm[-1] != ' ') nm--;
            { size_t len = (size_t)(q - nm);
              if (len > name_n - 1) len = name_n - 1;
              for (i = 0; i < len; i++) name[i] = nm[i];
              name[len] = 0; }
            *end = '\n';
        }
        return 1;
    }
    return 0;
}

/* 2026-09-20 只读对照: guest 往"可执行但不可写"(r-xp) 页写 → SEGV_ACCERR。
 * 这是 OHOS 无 RWX 时最可能的崩溃形态 (见 ROUND3 §9.10):
 * OHOS 拒 RWX → 代码页被留在 R+X → guest 自修改写失败 → 若无人恢复该页为可写就进程死。
 * 本函数**只打日志不改行为**, 每进程 64 次; 用来量出分布、以及是否与致命故障重合。 */
static int ohos_codewrite_dump_count;

static void ohos_smc_check_code_write( int sig, const siginfo_t *info, uint64_t addr,
                                      uint64_t xrip, uint64_t pc_in, uint64_t sp )
{
    char perms[8], name[96];
    char buf[288];
    int n;

    if (sig != SIGSEGV || !info || info->si_code != SEGV_ACCERR) return;
    if (ohos_codewrite_dump_count >= 64) return;
    if (!ohos_smc_region_info( addr, perms, sizeof(perms), name, sizeof(name) )) return;
    /* 只关心可执行且不可写的页 (r-x / rwx 中的 r-x 段) —— 这类页允许读/执行, 只拒绝写。 */
    if (!(perms[0] == 'r' && perms[1] == '-')) return;
    ohos_codewrite_dump_count++;
    n = snprintf( buf, sizeof(buf),
                  "[SMC-CODEWRITE] pid=%d tid=%ld addr=%p perms=%s region=%s xrip=%p pc=%p sp=%p\n",
                  getpid(), syscall( SYS_gettid ), (void *)(uintptr_t)addr,
                  perms, name[0] ? name : "<anon>", (void *)(uintptr_t)xrip,
                  (void *)(uintptr_t)pc_in, (void *)(uintptr_t)sp );
    if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
}

static void ohos_smc_check_callret( int sig, uint64_t addr, uint64_t xrip, uint64_t x17,
                                   uint64_t sp, uint64_t pc_in )
{
    char region[96];
    char buf[288];
    int n;

    if (ohos_callret_dump_count >= 32) return;
    if (!ohos_smc_region_name( addr, region, sizeof(region) )) region[0] = 0;

    if (strstr( region, "CallRetStacks" ) || (x17 && addr == x17))
    {
        ohos_callret_dump_count++;
        n = snprintf( buf, sizeof(buf),
                      "[CALLRET-FAULT] pid=%d tid=%ld sig=%d addr=%p x17=%p xrip=%p pc=%p sp=%p region=%s%s\n",
                      getpid(), syscall( SYS_gettid ), sig, (void *)(uintptr_t)addr,
                      (void *)(uintptr_t)x17, (void *)(uintptr_t)xrip, (void *)(uintptr_t)pc_in,
                      (void *)(uintptr_t)sp, region[0] ? region : "<none>",
                      (x17 && addr == x17) ? " addr==x17" : "" );
        if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
    }
}

static int ohos_smc_parse_hex( const char **pp, uint64_t *out )
{
    const char *p = *pp;
    uint64_t v = 0;
    int digits = 0;

    while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))
    {
        uint64_t d;
        if (*p <= '9') d = (uint64_t)(*p - '0');
        else if (*p >= 'a') d = (uint64_t)(*p - 'a' + 10);
        else d = (uint64_t)(*p - 'A' + 10);
        v = (v << 4) | d;
        p++;
        digits++;
        if (digits > 16) break;
    }
    *pp = p;
    *out = v;
    return digits;
}

static void ohos_smc_map_lookup( const char *what, uint64_t v )
{
    char *line, *end;
    char buf[320];
    int n;

    if (!v) return;
    if (ohos_map_dump_count++ >= 256) return;
    ohos_smc_load_maps();
    if (!ohos_maps_cache_len) return;

    for (line = ohos_maps_cache; line < ohos_maps_cache + ohos_maps_cache_len; line = end + 1)
    {
        const char *p;
        uint64_t lo, hi;

        end = memchr( line, '\n', (size_t)(ohos_maps_cache + ohos_maps_cache_len - line) );
        if (!end) break;
        p = line;
        if (!ohos_smc_parse_hex( &p, &lo ) || *p != '-') continue;
        p++;
        if (!ohos_smc_parse_hex( &p, &hi )) continue;
        if (v < lo || v >= hi) continue;
        *end = 0;
        n = snprintf( buf, sizeof(buf), "[SMC-MAP] %s=%p %s\n", what, (void *)(uintptr_t)v, line );
        *end = '\n';
        if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
        return;
    }
    n = snprintf( buf, sizeof(buf), "[SMC-MAP] %s=%p <unmapped>\n", what, (void *)(uintptr_t)v );
    if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
}

/* WineHua: 打印 addr 所在 VMA 的前后邻居 (各 3 条)。
 * 用于判断 "sp/fault 落在 ---p 区域" 到底是线程栈的 guard page、还是别的保留区 —
 * 只看一条 [SMC-MAP] 分不清, 邻居里的 [stack]/[anon:] 名字才是关键。 */
static void ohos_smc_dump_vma_neighbors( uint64_t v )
{
    char *lines[256];
    int count = 0, hit = -1, i;
    char *line, *end;
    char buf[320];
    int n;

    if (!v) return;
    ohos_smc_load_maps();
    if (!ohos_maps_cache_len) return;

    for (line = ohos_maps_cache; line < ohos_maps_cache + ohos_maps_cache_len && count < 256; line = end + 1)
    {
        const char *p;
        uint64_t lo, hi;

        end = memchr( line, '\n', (size_t)(ohos_maps_cache + ohos_maps_cache_len - line) );
        if (!end) break;
        *end = 0;
        lines[count++] = line;
        p = line;
        if (ohos_smc_parse_hex( &p, &lo ) && *p == '-')
        {
            p++;
            if (ohos_smc_parse_hex( &p, &hi ) && v >= lo && v < hi) hit = count - 1;
        }
        *end = '\n';
        if (hit >= 0 && count > hit + 3) break;
    }
    if (hit < 0) return;
    for (i = (hit > 3 ? hit - 3 : 0); i <= hit + 3 && i < count; i++)
    {
        n = snprintf( buf, sizeof(buf), "[SMC-NEIGHBOR] %s%s\n", (i == hit) ? ">>> " : "    ", lines[i] );
        if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
    }
}

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

static void ohos_signal_log_prot( struct ohos_signal_log *log, uint32_t prot )
{
    ohos_signal_log_str( log, " prot=" );
    ohos_signal_log_char( log, (prot & 0x1) ? 'R' : '-' );
    ohos_signal_log_char( log, (prot & 0x2) ? 'W' : '-' );
    ohos_signal_log_char( log, (prot & 0x4) ? 'X' : '-' );
    if (prot & OHOS_PROT_DYNAREC) ohos_signal_log_str( log, "|PROT_DYNAREC" );
    if (prot & OHOS_PROT_DYNAREC_R) ohos_signal_log_str( log, "|PROT_DYNAREC_R" );
}

/* TEMP-DIAG(PROF-SAMPLE) ---------------------------------------------------
 * A 32 bit process that spins (state R, ~1 core, every other thread idle) is
 * either executing guest code in a tight loop or stuck in emulator/Wine host
 * code. There is no in-guest debugger here, so sample the host context with
 * ITIMER_PROF and record:
 *   pc   host program counter  (resolved to a module + offset)
 *   lr   host link register    (resolved)
 *   x27  box64 dynarec keeps the guest RIP there (see wowbox64 native_epilog)
 *   x0   box64 keeps the x64emu_t pointer there
 * Only records; the interrupted context is never modified. */
#define OHOS_PROF_MAX_MODULES 80
#define OHOS_PROF_MAX_SAMPLES 40000

struct ohos_prof_module
{
    uintptr_t start;
    uintptr_t end;
    char name[72];
};

static struct ohos_prof_module ohos_prof_modules[OHOS_PROF_MAX_MODULES];
static int ohos_prof_module_count;
static int ohos_prof_installed;
static volatile sig_atomic_t ohos_prof_samples;
static volatile sig_atomic_t ohos_prof_overflowed;

static int ohos_prof_phdr_cb( struct dl_phdr_info *info, size_t size, void *data )
{
    uintptr_t start = 0, end = 0;
    struct ohos_prof_module *mod;
    int i;

    (void)size;
    (void)data;
    if (ohos_prof_module_count >= OHOS_PROF_MAX_MODULES) return 0;
    for (i = 0; i < info->dlpi_phnum; i++)
    {
        uintptr_t s, e;
        if (info->dlpi_phdr[i].p_type != PT_LOAD) continue;
        s = (uintptr_t)info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
        e = s + info->dlpi_phdr[i].p_memsz;
        if (!start || s < start) start = s;
        if (e > end) end = e;
    }
    if (!end) return 0;

    mod = &ohos_prof_modules[ohos_prof_module_count++];
    mod->start = start;
    mod->end = end;
    snprintf( mod->name, sizeof(mod->name), "%s",
              (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "<main>" );
    return 0;
}

/* Address -> "module+offset". Returns the module name and stores the offset.
 * Pure array scan so it stays usable from a signal handler. */
static const char *ohos_prof_resolve( uintptr_t addr, uintptr_t *offset )
{
    int i;

    for (i = 0; i < ohos_prof_module_count; i++)
    {
        if (addr >= ohos_prof_modules[i].start && addr < ohos_prof_modules[i].end)
        {
            *offset = addr - ohos_prof_modules[i].start;
            return ohos_prof_modules[i].name;
        }
    }
    *offset = 0;
    return "?";
}

static void ohos_prof_emit( const char *tag, uintptr_t addr, int depth )
{
    const char *mod;
    uintptr_t off;
    char buf[320];
    int n;

    mod = ohos_prof_resolve( addr, &off );
    n = snprintf( buf, sizeof(buf), "[prof] %s d=%d addr=%p mod=%s+%#lx\n",
                  tag, depth, (void *)addr, mod, (unsigned long)off );
    if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
}

static void ohos_prof_handler( int sig, siginfo_t *info, void *uctx )
{
    ucontext_t *uc = (ucontext_t *)uctx;
    uintptr_t pc = 0, lr = 0, sp = 0, fp = 0, guest_rip = 0, emu = 0;
    long tid = syscall( SYS_gettid );
    int i;
    char buf[256];
    int n;

    (void)sig;
    (void)info;
    if (__atomic_add_fetch( &ohos_prof_samples, 1, __ATOMIC_RELAXED ) > OHOS_PROF_MAX_SAMPLES)
    {
        if (!ohos_prof_overflowed)
        {
            ohos_prof_overflowed = 1;
            n = snprintf( buf, sizeof(buf), "[prof] sample budget reached, stopping pid=%d\n", getpid() );
            if (n > 0) write( 2, buf, (size_t)n );
            setitimer( ITIMER_PROF, &(struct itimerval){{0, 0}, {0, 0}}, NULL );
        }
        return;
    }

#if defined(__aarch64__)
    if (uc)
    {
        pc = uc->uc_mcontext.pc;
        lr = uc->uc_mcontext.regs[30];
        fp = uc->uc_mcontext.regs[29];
        sp = uc->uc_mcontext.sp;
        guest_rip = uc->uc_mcontext.regs[27];
        emu = uc->uc_mcontext.regs[0];
    }
#endif

    n = snprintf( buf, sizeof(buf), "[prof] pid=%d tid=%ld pc=%p lr=%p x27=%p x0=%p sp=%p\n",
                  getpid(), tid, (void *)pc, (void *)lr, (void *)guest_rip, (void *)emu, (void *)sp );
    if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );

    /* WineHua (2026-09-19): ARM64EC/FEX 进程还要把 guest(x64) 上下文打出来 ——
     * 上面的 x27/x0 是 box64 约定, 对 FEX 无意义。 */
#if defined(__aarch64__)
    ohos_prof_dump_guest_context();
#endif

    ohos_prof_emit( "pc", pc, 0 );
    ohos_prof_emit( "lr", lr, 0 );

    /* Bounded frame-pointer walk. Only accept frames that stay inside the
     * sampled thread's stack window so a bogus fp cannot fault us. */
    for (i = 0; i < 12 && fp; i++)
    {
        uintptr_t next_fp, ret;
        if (fp < sp || fp - sp > 0x100000 || (fp & 7)) break;
        next_fp = ((const uintptr_t *)fp)[0];
        ret = ((const uintptr_t *)fp)[1];
        if (!ret) break;
        ohos_prof_emit( "ret", ret, i + 1 );
        if (next_fp <= fp) break;
        fp = next_fp;
    }
}

void ohos_prof_sampler_init( void )
{
    struct sigaction sa;
    struct itimerval it;
    const char *env = getenv( "WINEHUA_PROF_SAMPLE" );
    char buf[256];
    int i, n;

    if (ohos_prof_installed) return;
    ohos_prof_installed = 1;
    if (!env || !env[0] || env[0] == '0') return;

    dl_iterate_phdr( ohos_prof_phdr_cb, NULL );

    memset( &sa, 0, sizeof(sa) );
    sa.sa_sigaction = ohos_prof_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset( &sa.sa_mask );
    if (sigaction( SIGPROF, &sa, NULL ) != 0)
    {
        n = snprintf( buf, sizeof(buf), "[prof] sigaction failed errno=%d pid=%d\n", errno, getpid() );
        if (n > 0) write( 2, buf, (size_t)n );
        return;
    }

    memset( &it, 0, sizeof(it) );
    it.it_interval.tv_usec = 100000;
    it.it_value.tv_usec = 100000;
    setitimer( ITIMER_PROF, &it, NULL );

    n = snprintf( buf, sizeof(buf), "[prof] armed pid=%d modules=%d interval=100ms\n",
                  getpid(), ohos_prof_module_count );
    if (n > 0) write( 2, buf, (size_t)n );
    for (i = 0; i < ohos_prof_module_count; i++)
    {
        n = snprintf( buf, sizeof(buf), "[prof-mod] pid=%d %s %p-%p\n", getpid(),
                      ohos_prof_modules[i].name, (void *)ohos_prof_modules[i].start,
                      (void *)ohos_prof_modules[i].end );
        if (n > 0) write( 2, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1) );
    }
}

static void ohos_smc_log_enter( int sig, const siginfo_t *info, uint64_t pc_in,
                                uint64_t xrip_in, uint64_t emu, uint64_t lr, uint64_t sp,
                                uint64_t x16, uint64_t x17, void *teb, void *fn )
{
    struct ohos_signal_log log = {{0}, 0};
    long tid;

    if (ohos_diag_quiet) return;

    if (__atomic_test_and_set( &ohos_maps_busy, __ATOMIC_ACQUIRE )) return;

    /* 2026-09-20: 致命现场优先, 且**不受限流影响**。
     * 现场 (17:48 起 broker 16380 那轮): renderer 的致命故障是 guest RIP==0
     * (x27_in=0x0) —— 跳到了 NULL 函数指针; 而它恰好落在限流窗口里被整块丢掉,
     * 于是"谁调用 NULL"这条最有价值的证据没了。这里给每进程 6 次独立配额,
     * 打印当前 maps 归属 + 栈顶归属 (return address) + 邻接 VMA。 */
    /* 2026-09-20: **所有 SIGSEGV** 现场都提到限流之前 (每进程 16 次)。
     * 实测踩过两次: 崩溃进程在致命之前已经打了上百条 sig=7(未对齐) 故障, 限流
     * (count>64 每 32 条) 把致命的那条 SIGSEGV 整个丢掉 —— 于是 pid 归属拿到、
     * 致命现场却查不到。 */
    if (sig == SIGSEGV && ohos_fatal_dump_count < 16)
    {
        ohos_fatal_dump_count++;
        /* 归属查询自身也有 256 行上限; 致命现场要保证打得出来 → 先复位该计数。 */
        ohos_map_dump_count = 0;
        ohos_smc_reload_maps();
        ohos_smc_map_lookup( "addr", (uint64_t)(uintptr_t)(info ? info->si_addr : NULL) );
        ohos_smc_map_lookup( "x27", xrip_in );
        ohos_smc_map_lookup( "pc", pc_in );
        ohos_smc_map_lookup( "lr", lr );
        ohos_smc_dump_vma_neighbors( sp );
        ohos_smc_dump_stack( sp );
        ohos_smc_attribute_stack( sp );
    }

    /* 故障归属先打: 它与后面的 [SMC] enter 行同源, 但即使 enter 行被限流
     * (count>64 时每 32 颗才打一条) 归属信息也不会跟着丢。
     * 2026-09-20: 改成对**所有**信号的首次 N 次都打归属 —— 之前只对 SIGSEGV 打、
     * 且每进程 24 组名额会被启动期的 SIGBUS(7) 探针吃光 (实测踩过), 结果真正致命
     * 的那次既没有归属也没有邻居。现在按"每进程前 64 组"配额, SIGSEGV 额外放宽。 */
    {
        const int quota = (sig == SIGSEGV) ? 96 : 64;
        /* call-ret 栈故障 (FEXMem_CallRetStacks 区域 / addr==X17) 单独归类, 不限流。 */
        ohos_smc_check_callret( sig, (uint64_t)(uintptr_t)(info ? info->si_addr : NULL),
                                xrip_in, x17, sp, pc_in );
        /* 2026-09-20: "写 r-xp 代码页"(OHOS 无 RWX 的典型形态) 单独归类, 只读对照。 */
        ohos_smc_check_code_write( sig, info, (uint64_t)(uintptr_t)(info ? info->si_addr : NULL),
                                   xrip_in, pc_in, sp );
        if (ohos_map_dump_quota < quota)
        {
            ohos_map_dump_quota++;
            /* 2026-09-20: 致命故障现刷 maps —— 首次加载的快照到真正的致命那次
             * 早已过期 (实测 renderer 的 fault-maps 是 17:49 写的, 崩在更晚,
             * 结果 addr/x27/pc 全部 <unmapped>, 无法归属)。 */
            /* 配额内每次归属查询都用当前快照 (每进程最多 64/96 次重读, 代价可控)。 */
            ohos_smc_reload_maps();
            ohos_smc_map_lookup( "addr", (uint64_t)(uintptr_t)(info ? info->si_addr : NULL) );
            ohos_smc_map_lookup( "x27", xrip_in );
            ohos_smc_map_lookup( "pc", pc_in );
            ohos_smc_map_lookup( "lr", lr );
            /* sp 落在 ---p 区 (栈越界/guard page) 是最需要邻居上下文的一类现场。 */
            ohos_smc_dump_vma_neighbors( sp );
            /* SIGSEGV: 栈顶逐个做模块归属, 找给出野指针的 return address。 */
            if (sig == SIGSEGV) ohos_smc_attribute_stack( sp );
        }
    }

    /* Same cap as ohos_smc_log. Uncapped enter lines filled ~500MB wine_stderr
     * when winedbg looped on a JIT NULL deref after Heaven's FS-base crash. */
    __atomic_clear( &ohos_maps_busy, __ATOMIC_RELEASE );
    __atomic_add_fetch( &ohos_smc_log_count, 1, __ATOMIC_RELAXED );
    if (ohos_smc_log_count > 64 && (ohos_smc_log_count % 32) != 0) return;

    tid = syscall( SYS_gettid );
    ohos_signal_log_str( &log, "[SMC] enter" );
    ohos_signal_log_field_dec( &log, "pid", getpid() );
    ohos_signal_log_field_dec( &log, "tid", tid );
    ohos_signal_log_field_dec( &log, "sig", sig );
    ohos_signal_log_field_dec( &log, "code", info ? info->si_code : 0 );
    ohos_signal_log_field_hex( &log, "addr", (uintptr_t)(info ? info->si_addr : NULL) );
    ohos_signal_log_field_hex( &log, "pc_in", pc_in );
    ohos_signal_log_field_hex( &log, "x27_in", xrip_in );
    ohos_signal_log_field_hex( &log, "x0_in", emu );
    ohos_signal_log_field_hex( &log, "lr", lr );
    ohos_signal_log_field_hex( &log, "sp", sp );
    ohos_signal_log_field_hex( &log, "x16", x16 );
    ohos_signal_log_field_hex( &log, "x17", x17 );
    ohos_signal_log_field_hex( &log, "teb", (uintptr_t)teb );
    ohos_signal_log_field_hex( &log, "fn", (uintptr_t)fn );
    ohos_signal_log_emit( &log );

    /* Guest code jumped to NULL: the return address of that call is on the
     * stack, and the PE module list tells us which image it belongs to. */
    if (sig == SIGSEGV && xrip_in == 0)
    {
        ohos_smc_dump_stack( sp );
    }
}

static void ohos_smc_log( int sig, const siginfo_t *info, uint64_t pc_in, uint64_t pc_out,
                          uint64_t xrip_in, uint64_t xrip_out, uint32_t prot, int dynarec,
                          int kind, const char *result, int wine )
{
    struct ohos_signal_log log = {{0}, 0};
    long tid;

    if (ohos_diag_quiet) return;

    /* Count lives in ohos_smc_log_enter (called first on every fault). */
    if (ohos_smc_log_count > 64 && (ohos_smc_log_count % 32) != 0) return;
    tid = syscall( SYS_gettid );
    ohos_signal_log_str( &log, "[SMC]" );
    ohos_signal_log_field_dec( &log, "pid", getpid() );
    ohos_signal_log_field_dec( &log, "tid", tid );
    ohos_signal_log_field_dec( &log, "sig", sig );
    ohos_signal_log_field_dec( &log, "code", info ? info->si_code : 0 );
    ohos_signal_log_field_hex( &log, "addr", (uintptr_t)(info ? info->si_addr : NULL) );
    ohos_signal_log_field_hex( &log, "pc_in", pc_in );
    ohos_signal_log_field_hex( &log, "pc_out", pc_out );
    ohos_signal_log_field_hex( &log, "x27_in", xrip_in );
    ohos_signal_log_field_hex( &log, "x27_out", xrip_out );
    ohos_signal_log_prot( &log, prot );
    ohos_signal_log_field_dec( &log, "dynarec", dynarec );
    ohos_signal_log_field_dec( &log, "kind", kind );
    ohos_signal_log_str( &log, " result=" );
    ohos_signal_log_str( &log, result );
    ohos_signal_log_field_dec( &log, "wine", wine );
    ohos_signal_log_emit( &log );
}

static void ohos_trap_log( const char *phase, int sig, const siginfo_t *info, const ucontext_t *uc )
{
    struct ohos_signal_log log = {{0}, 0};
    long tid;

    if (ohos_diag_quiet) return;

    if (__atomic_add_fetch( &ohos_trap_log_count, 1, __ATOMIC_RELAXED ) > 64) return;
    tid = syscall( SYS_gettid );
    ohos_signal_log_str( &log, "[TRAP] " );
    ohos_signal_log_str( &log, phase );
    ohos_signal_log_field_dec( &log, "tid", tid );
    ohos_signal_log_field_dec( &log, "sig", sig );
    ohos_signal_log_field_dec( &log, "code", info ? info->si_code : 0 );
    ohos_signal_log_field_hex( &log, "addr", (uintptr_t)(info ? info->si_addr : NULL) );
#ifdef __aarch64__
    ohos_signal_log_field_hex( &log, "pc", uc ? uc->uc_mcontext.pc : 0 );
    ohos_signal_log_field_hex( &log, "x27", uc ? uc->uc_mcontext.regs[27] : 0 );
    ohos_signal_log_field_hex( &log, "x0", uc ? uc->uc_mcontext.regs[0] : 0 );
    ohos_signal_log_field_hex( &log, "lr", uc ? uc->uc_mcontext.regs[30] : 0 );
    ohos_signal_log_field_hex( &log, "sp", uc ? uc->uc_mcontext.sp : 0 );
    ohos_signal_log_field_hex( &log, "x16", uc ? uc->uc_mcontext.regs[16] : 0 );
    ohos_signal_log_field_hex( &log, "x17", uc ? uc->uc_mcontext.regs[17] : 0 );
#endif
    ohos_signal_log_field_hex( &log, "handler", (uintptr_t)wine_trap_handler );
    ohos_signal_log_emit( &log );
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
    uint64_t pc_in = 0, pc = 0, xrip_in = 0, xrip = 0, emu = 0;
    uint64_t lr = 0, sp = 0, x16 = 0, x17 = 0;
    uint32_t prot = 0;
    int kind = 0, handled = 0, dynarec = 0;
    ohos_wowbox64_fault_fn fn;
    const char *result = "not_mine";
    void *teb = NULL;

    if (uc)
    {
        pc_in = pc = uc->uc_mcontext.pc;
#ifdef __aarch64__
        xrip_in = xrip = uc->uc_mcontext.regs[27];
        emu = uc->uc_mcontext.regs[0];
        lr = uc->uc_mcontext.regs[30];
        sp = uc->uc_mcontext.sp;
        x16 = uc->uc_mcontext.regs[16];
        x17 = uc->uc_mcontext.regs[17];
#endif
    }

    fn = p_wowbox64_host_fault;
#ifdef __aarch64__
    teb = NtCurrentTeb();
#endif
    /* Temporary probe: saved registers only, no guest pointer dereferences,
     * libc formatting, maps parsing or allocation in this signal handler. */
    if (sig == SIGSEGV)
    {
        static unsigned int count;
        unsigned int seq = __atomic_add_fetch( &count, 1, __ATOMIC_RELAXED );
        if (seq <= 65536)
        {
            struct ohos_signal_log log = {{0}, 0};
            const int saved_errno = errno;
            ohos_signal_log_str( &log, "[FAULT-MIN]" );
            ohos_signal_log_field_dec( &log, "pid", getpid() );
            ohos_signal_log_field_dec( &log, "tid", syscall( SYS_gettid ) );
            ohos_signal_log_field_dec( &log, "seq", seq );
            ohos_signal_log_field_dec( &log, "code", info ? info->si_code : 0 );
            ohos_signal_log_field_hex( &log, "addr", (uintptr_t)(info ? info->si_addr : NULL) );
            ohos_signal_log_field_hex( &log, "pc", pc_in );
            ohos_signal_log_field_hex( &log, "lr", lr );
            ohos_signal_log_field_hex( &log, "sp", sp );
            ohos_signal_log_field_hex( &log, "teb", (uintptr_t)teb );
            ohos_signal_log_field_hex( &log, "self", (uintptr_t)ohos_route_host_fault );
            ohos_signal_log_field_hex( &log, "x9", uc ? uc->uc_mcontext.regs[9] : 0 );
            ohos_signal_log_field_hex( &log, "x18", uc ? uc->uc_mcontext.regs[18] : 0 );
            ohos_signal_log_emit( &log );
            errno = saved_errno;
        }
    }
    ohos_smc_log_enter( sig, info, pc_in, xrip_in, emu, lr, sp, x16, x17, teb, fn );

#ifdef __aarch64__
    if (fn && info && teb)
        handled = ohos_call_pe_fault( (void *)fn, teb, sig, info->si_code, info->si_addr,
                                      &pc, &xrip, &prot, &kind );
#else
    if (fn && info)
        handled = fn( sig, info->si_code, info->si_addr, &pc, &xrip, &prot, &kind );
#endif

    /* FEX handles guest SMC after Wine's SEH dispatch. Do not change guest
     * page permissions here based on native maps: v9-v11 disproved that
     * recovery, and even its disabled path raced on the diagnostic cache. */

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
        ohos_smc_log( sig, info, pc_in, pc, xrip_in, xrip, prot, dynarec, kind, result, 0 );
        return 1;
    }

    /* SIGILL that is not a dynarec CALLRET trap: let Wine ill_handler run. */
    if (sig == SIGILL)
    {
        ohos_smc_log( sig, info, pc_in, pc, xrip_in, xrip, prot, dynarec, kind, result, 1 );
        return 0;
    }

    ohos_smc_log( sig, info, pc_in, pc, xrip_in, xrip, prot, dynarec, kind, "wine_seh", 1 );
    if (sig == SIGBUS)
    {
        if (wine_bus_handler) wine_bus_handler( sig, info, uctx );
    }
    else if (wine_segv_handler) wine_segv_handler( sig, info, uctx );
    if (uc)
    {
        pc = uc->uc_mcontext.pc;
#ifdef __aarch64__
        xrip = uc->uc_mcontext.regs[27];
#endif
    }
    ohos_smc_log( sig, info, pc_in, pc, xrip_in, xrip, prot, dynarec, kind, result, 1 );
    return 1;
}

static int ohos_sigchain_fault( int sig, siginfo_t *info, void *uctx )
{
    return ohos_route_host_fault( sig, info, uctx );
}

static int ohos_sigchain_trap( int sig, siginfo_t *info, void *uctx )
{
    ohos_trap_log( "enter", sig, info, uctx );
    if (wine_trap_handler) wine_trap_handler( sig, info, uctx );
    ohos_trap_log( "exit", sig, info, uctx );
    return 1;
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

static void ohos_rt_sigaction_trap( int sig, siginfo_t *info, void *uctx )
{
    ohos_trap_log( "enter-rt", sig, info, uctx );
    if (wine_trap_handler) wine_trap_handler( sig, info, uctx );
    ohos_trap_log( "exit-rt", sig, info, uctx );
}

void ohos_install_sigchain_fault_handlers( void (*segv_handler)(int, siginfo_t *, void *),
                                           void (*bus_handler)(int, siginfo_t *, void *),
                                           void (*trap_handler)(int, siginfo_t *, void *) )
{
    void (*add_special)(int, struct ohos_sigchain_action *);
    struct ohos_sigchain_action sca;
    const char *quiet = getenv( "WINEHUA_DIAG_QUIET" );

    /* Temporary candidate-only direct stderr: the in-process pipe reader can
     * die before it copies the final fault. Enabled by the explicit CEF probe. */
    {
        const char *direct = getenv( "WINEHUA_CEF_NO_CRASH_HANDLER" );
        if (direct && !strcmp( direct, "1" ))
        {
            char path[128];
            int fd;
            snprintf( path, sizeof(path), "/data/storage/el2/base/temp/wine-native-%d.log", getpid() );
            fd = open( path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600 );
            if (fd >= 0)
            {
                if (dup2( fd, STDERR_FILENO ) >= 0)
                    fprintf( stderr, "[NATIVE-PROBE] pid=%d direct-stderr=1\n", getpid() );
                if (fd != STDERR_FILENO) close( fd );
            }
        }
    }

    ohos_diag_quiet = quiet && quiet[0] == '1';

    wine_segv_handler = segv_handler;
    wine_bus_handler = bus_handler;
    wine_trap_handler = trap_handler;
    if (getenv( "WINEHUA_DUMP_PHDR" ))
        dl_iterate_phdr( ohos_phdr_cb, NULL );

    add_special = dlsym( RTLD_DEFAULT, "AddSpecialSignalHandlerFn" );
    if (!add_special) add_special = dlsym( RTLD_DEFAULT, "add_special_signal_handler" );
    /* Temporary candidate-only raw signal control, using the existing fallback. */
    if (getenv( "WINEHUA_CEF_NO_CRASH_HANDLER" )) add_special = NULL;
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
        sca.sca_sigaction = ohos_sigchain_trap;
        add_special( SIGTRAP, &sca );
        fprintf( stderr, "[ntdll] OHOS sigchain claimed SIGSEGV/SIGBUS/SIGILL/SIGTRAP (before DFX/Wine SEH)\n" );
        return;
    }

    /* libc hid the sigchain API: talk to the kernel, bypass DFX entirely. */
    if (!ohos_rt_sigaction( SIGSEGV, ohos_rt_sigaction_fault, SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER, NULL ) &&
        !ohos_rt_sigaction( SIGBUS,  ohos_rt_sigaction_fault, SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER, NULL ) &&
        !ohos_rt_sigaction( SIGILL,  ohos_rt_sigaction_fault, SA_SIGINFO | SA_RESTART | SA_ONSTACK | SA_NODEFER, NULL ) &&
        !ohos_rt_sigaction( SIGTRAP, ohos_rt_sigaction_trap,  SA_SIGINFO | SA_RESTART | SA_ONSTACK, NULL ))
        fprintf( stderr, "[ntdll] OHOS rt_sigaction installed SIGSEGV/SIGBUS/SIGILL/SIGTRAP (bypass DFX)\n" );
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
