/* WineHua TEMP-DIAG(IPC-TRACE) 实现 — 说明见 winehua_ipc_trace.h */

#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"

#include "winehua_ipc_trace.h"

#define WINEHUA_TRACED_HANDLES 64
static HANDLE traced_handles[WINEHUA_TRACED_HANDLES];
static int traced_count;

int winehua_ipc_trace_level(void)
{
    static int cached = -1;
    char v[8];

    if (cached < 0)
    {
        cached = 0;
        if (GetEnvironmentVariableA( "WINEHUA_IPC_TRACE", v, sizeof(v) ) && v[0] >= '1' && v[0] <= '2')
            cached = v[0] - '0';
    }
    return cached;
}

int winehua_ipc_trace_match( const WCHAR *name )
{
    static const char needle[] = "SteamChrome";
    int i;

    if (!name) return 0;
    for (i = 0; name[i]; i++)
    {
        const WCHAR *n = name + i;
        const char *s = needle;
        while (*s && *n && (char)*n == *s) { s++; n++; }
        if (!*s) return 1;
    }
    return 0;
}

void winehua_ipc_remember( HANDLE h )
{
    if (!h || traced_count >= WINEHUA_TRACED_HANDLES) return;
    traced_handles[traced_count++] = h;
}

void winehua_ipc_forget( HANDLE h )
{
    int i;

    for (i = 0; i < traced_count; i++)
    {
        if (traced_handles[i] != h) continue;
        traced_handles[i] = traced_handles[--traced_count];
        return;
    }
}

int winehua_ipc_is_traced( HANDLE h )
{
    int i;

    for (i = 0; i < traced_count; i++)
        if (traced_handles[i] == h) return 1;
    return 0;
}

void winehua_ipc_trace( const char *api, const WCHAR *name, const char *fmt, ... )
{
    DWORD saved = GetLastError();
    char line[512];
    char aname[192];
    int i, n;

    if (!winehua_ipc_trace_level() || !winehua_ipc_trace_match( name ))
    {
        SetLastError( saved );
        return;
    }

    for (i = 0; name[i] && i < (int)sizeof(aname) - 1; i++) aname[i] = (char)name[i];
    aname[i] = 0;
    n = snprintf( line, sizeof(line), "[IPC-TRACE] api=%s pid=%lu tid=%lu name=\"%s\" ",
                  api, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(), aname );
    if (n > 0 && n < (int)sizeof(line) && fmt)
    {
        va_list ap;
        va_start( ap, fmt );
        vsnprintf( line + n, sizeof(line) - n, fmt, ap );
        va_end( ap );
    }
    n = strlen( line );
    if (n < (int)sizeof(line) - 40)
        snprintf( line + n, sizeof(line) - n, " last_error=%lu\n", (unsigned long)saved );
    __wine_dbg_output( line );
    SetLastError( saved );
}

void winehua_ipc_trace_handle( const char *api, HANDLE h, const char *fmt, ... )
{
    DWORD saved = GetLastError();
    char line[320];
    int n;

    if (!winehua_ipc_trace_level() ||
        (winehua_ipc_trace_level() < 2 && !winehua_ipc_is_traced( h )))
    {
        SetLastError( saved );
        return;
    }

    n = snprintf( line, sizeof(line), "[IPC-TRACE] api=%s pid=%lu tid=%lu handle=%p ",
                  api, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId(), h );
    if (n > 0 && n < (int)sizeof(line) && fmt)
    {
        va_list ap;
        va_start( ap, fmt );
        vsnprintf( line + n, sizeof(line) - n, fmt, ap );
        va_end( ap );
    }
    n = strlen( line );
    if (n < (int)sizeof(line) - 40)
        snprintf( line + n, sizeof(line) - n, " last_error=%lu\n", (unsigned long)saved );
    __wine_dbg_output( line );
    SetLastError( saved );
}
