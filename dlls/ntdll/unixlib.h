/*
 * Ntdll Unix interface
 *
 * Copyright (C) 2020 Alexandre Julliard
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

#ifndef __NTDLL_UNIXLIB_H
#define __NTDLL_UNIXLIB_H

#include "wine/unixlib.h"

struct _DISPATCHER_CONTEXT;

struct wine_dbg_write_params
{
    const char  *str;
    unsigned int len;
};

struct wine_server_fd_to_handle_params
{
    int          fd;
    unsigned int access;
    unsigned int attributes;
    HANDLE      *handle;
};

struct wine_server_handle_to_fd_params
{
    HANDLE        handle;
    unsigned int  access;
    int          *unix_fd;
    unsigned int *options;
};

struct wine_spawnvp_params
{
    char       **argv;
    int          wait;
};

struct load_so_dll_params
{
    UNICODE_STRING              nt_name;
    void                      **module;
};

struct unwind_builtin_dll_params
{
    ULONG                       type;
    struct _DISPATCHER_CONTEXT *dispatch;
    CONTEXT                    *context;
};

/* wowbox64.dll registers a host SIGSEGV consumer so OHOS musl sigchain can
 * digest Box64 dynarec SMC faults without entering Wine SEH.
 * p_unix_mprotect is a PE slot; unix writes ohos_mprotect_exec so unprotectDB
 * can restore kernel WRITE from a POSIX handler without NtProtectVirtualMemory. */
struct ohos_set_wowbox64_fault_params
{
    void *handler;
    void **p_unix_mprotect;
};

struct wine_get_unix_env_params
{
    const char *name;
    char *val;
    unsigned int buffer_len;
};

struct wine_set_unix_env_params
{
    const char *name;
    const char *val;
};

struct wine_dbg_ftrace_params
{
    char *str;
    unsigned int len;
    unsigned int ctx;
};


struct steamclient_setup_trampolines_params
{
    HMODULE src_mod;
    HMODULE tgt_mod;
};

struct debugstr_pc_args
{
    void *pc;
    char *buffer;
    unsigned int size;
};

struct compat_wine_nt_to_unix_file_name_params
{
    const OBJECT_ATTRIBUTES *attr;
    char *nameA;
    ULONG *size;
    unsigned int disposition;
};

enum ntdll_unix_funcs
{
    unix_load_so_dll,
    unix_unwind_builtin_dll,
    unix_wine_dbg_write,
    unix_wine_server_call,
    unix_wine_server_fd_to_handle,
    unix_wine_server_handle_to_fd,
    unix_wine_spawnvp,
    unix_system_time_precise,
    /* WineHua/OHOS: must stay at index 8 — prebuilt PE modules (wowbox64.dll)
     * shipped in the runtime were compiled against the WineHua header where this
     * entry sits right after unix_system_time_precise.  Appending it at the end
     * instead made those calls land on wine_get_unix_env (observed as
     * "wowbox64 registered host fault handler ... unix_mprotect=0"). */
    unix_ohos_set_wowbox64_fault,
    unix___wine_get_unix_env,
    unix___wine_set_unix_env,
    unix_wine_dbg_ftrace,
    unix_steamclient_setup_trampolines,
    unix_debugstr_pc,
    unix_compat_wine_nt_to_unix_file_name,
};

extern unixlib_handle_t __wine_unixlib_handle;

#define WINE_BACKTRACE_LOG_ON() WARN_ON(seh)

#define WINE_BACKTRACE_LOG(args...) do { \
        WARN_(seh)("backtrace: " args); \
    } while (0)

#endif /* __NTDLL_UNIXLIB_H */
