/*
 * WineHua GL proc dispatch 探针 (2026-09-18, 只诊断)
 *
 * 背景: guest GLES3 能力打通后 CEF browser 进程在启动早期 SIGSEGV, 现场是
 * `x27_in=0` (高度怀疑 guest control-flow target 为 NULL) —— 也就是某个 GL/EGL
 * 入口解析成 NULL 后被当作必需函数调用。wglGetProcAddress 返回 NULL 对可选扩展
 * 是合法的, 所以需要"最近解析历史", 而不是孤立的 MISSING 行。
 *
 * 覆盖: Wine WGL/EGL 驱动层唯一的解析漏斗 egldrv_get_proc_address()
 *       (PE 侧 wglGetProcAddress → UNIX_CALL(wglGetProcAddress) → p_get_proc_address)
 *       + 直接走 eglGetProcAddress 的 EGL 入口。
 * 输出: 每个 (pid, name, source) 只打一次 "GL-PROC:"; 每次解析都进 ring;
 *       出现 NULL 时立刻 dump ring 最近 64 条 ("GL-PROC-HISTORY:")。
 * 开关: WINEHUA_GL_PROC_TRACE=1
 */
#ifdef __OHOS__

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

#include "winehua_gl_proc_trace.h"

#define RING_SIZE 64
#define DEDUP_SIZE 512

struct ring_entry
{
    char name[64];
    void *result;
    const char *source;
};

static struct ring_entry ring[RING_SIZE];
static unsigned int ring_pos;
static unsigned int ring_count;

struct dedup_entry
{
    char name[64];
    const char *source;
};

static struct dedup_entry dedup[DEDUP_SIZE];
static unsigned int dedup_count;

static int enabled_cached = -1;

static int trace_enabled(void)
{
    if (enabled_cached < 0)
    {
        const char *v = getenv( "WINEHUA_GL_PROC_TRACE" );
        enabled_cached = !!(v && v[0] && v[0] != '0');
    }
    return enabled_cached;
}

static void dump_history( const char *why, const char *name )
{
    unsigned int i, idx;

    fprintf( stderr, "GL-PROC-HISTORY: pid=%d why=%s trigger=%s entries=%u\n",
             getpid(), why, name ? name : "-", ring_count );
    for (i = 0; i < ring_count && i < RING_SIZE; i++)
    {
        idx = (ring_pos + RING_SIZE - 1 - i) % RING_SIZE;  /* 从最新往旧打 */
        fprintf( stderr, "GL-PROC-HISTORY: -%u %s source=%s -> %p\n",
                 i, ring[idx].name, ring[idx].source ? ring[idx].source : "?",
                 ring[idx].result );
    }
    fflush( stderr );
}

void winehua_gl_proc_trace_note( const char *name, void *result, const char *source )
{
    unsigned int i;
    int seen = 0;

    if (!trace_enabled() || !name) return;

    /* ring: 每次解析都记 */
    snprintf( ring[ring_pos].name, sizeof(ring[ring_pos].name), "%s", name );
    ring[ring_pos].result = result;
    ring[ring_pos].source = source;
    ring_pos = (ring_pos + 1) % RING_SIZE;
    if (ring_count < RING_SIZE) ring_count++;

    /* dedup 日志: 同 (name, source) 只打一次 */
    for (i = 0; i < dedup_count; i++)
    {
        if (dedup[i].source == source && !strcmp( dedup[i].name, name )) { seen = 1; break; }
    }
    if (!seen && dedup_count < DEDUP_SIZE)
    {
        snprintf( dedup[dedup_count].name, sizeof(dedup[dedup_count].name), "%s", name );
        dedup[dedup_count].source = source;
        dedup_count++;
        fprintf( stderr, "GL-PROC: pid=%d tid=%ld name=%s source=%s result=%p%s\n",
                 getpid(), (long)syscall(SYS_gettid), name, source ? source : "?",
                 result, result ? "" : " (NULL)" );
        fflush( stderr );
    }

    if (!result) dump_history( "null-result", name );
}

void winehua_gl_proc_trace_dump( const char *why )
{
    if (!trace_enabled()) return;
    dump_history( why ? why : "manual", "-" );
}

#endif /* __OHOS__ */
