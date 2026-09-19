/*
 * WineHua TEMP-DIAG(IPC-TRACE) — 可关闭的 SteamChrome IPC 观测 (2026-09-18)
 *
 * 只观察, 不改行为:
 *   - WINEHUA_IPC_TRACE=1 打开 (2 = 连未记过的句柄也打);
 *   - 名字过滤 = 含 "SteamChrome";
 *   - 打印前后恢复 LastError, 不污染被测 API 的错误状态。
 *
 * 诊断脚手架: 结论成立后按计划要求收敛/删除。
 */
#ifndef __WINEHUA_IPC_TRACE_H
#define __WINEHUA_IPC_TRACE_H

#include "windef.h"
#include "winbase.h"

extern int winehua_ipc_trace_level(void);
extern int winehua_ipc_trace_match(const WCHAR *name);
extern void winehua_ipc_remember(HANDLE h);
extern int winehua_ipc_is_traced(HANDLE h);
extern void winehua_ipc_trace(const char *api, const WCHAR *name, const char *fmt, ...);
extern void winehua_ipc_trace_handle(const char *api, HANDLE h, const char *fmt, ...);
extern void winehua_ipc_forget(HANDLE h);

#endif /* __WINEHUA_IPC_TRACE_H */
