/*
 * WineHua toplevel modal relation reporter (winehua_toplevel protocol)
 *
 * Copyright 2026 WineHua contributors
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
 *
 * 背景: base xdg-shell 无法表达 Win32 模态对话框关系, 合成器拿不到
 * "哪个窗口是模态、owner 是谁", 导致主窗口被 raise 到对话框上面时,
 * 主窗口因 owner-disabled 不可点、对话框被永久遮挡。本文件把判定后的
 * 模态关系经私有协议 winehua_toplevel.set_modal 上报给 WineHua 合成器。
 *
 * 模态判定 = Win32 语义 (dialog.c 模态对话框创建时序):
 *   1) EnableWindow(owner, FALSE)      — owner 被禁用
 *   2) ShowWindow(dialog, SW_SHOW)     — 对话框显示
 * 或 Messagebox (MSGBOX 模板 DS_MODALFRAME → WS_EX_DLGMODALFRAME)。
 * 因此判定 = 窗口带 WS_EX_DLGMODALFRAME + owner 存在 + owner 被禁用。
 * 触发点: WAYLAND_WindowPosChanged (显示/隐藏/移动时探测, owner 此时
 * 已被禁用) 与 wayland_surface_destroy (销毁清态)。MB_TASKMODAL (无
 * owner 的线程级模态) 不支持, 见 WM_TABLE / 设计文档限制。
 */

#ifdef __OHOS__

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include "waylanddrv.h"  /* windef/winbase 链提供 WS_* 宏 (与 window.c 同源) */
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* 调用方 (WAYLAND_WindowPosChanged / wayland_win_data 持有锁域) 保证
 * win_data_mutex 已持有: 全部访问用 nolock 变体 (与 window.c is_window_managed
 * 同先例) — get+release 常规对配对会在持锁域内重入同一把非递归互斥锁自死锁 */
static struct wl_surface *get_toplevel_surface(struct wayland_win_data *data)
{
    return data && data->wayland_surface ? data->wayland_surface->wl_surface : NULL;
}

/**********************************************************************
 *          winehua_modal_update
 *
 * 探测 hwnd (surface->hwnd) 的模态关系并上报; 状态未变时静默。
 */
void winehua_modal_update(struct wayland_surface *surface)
{
    HWND owner, owner_data_hwnd;
    LONG ex_style;
    BOOL modal;
    struct wayland_win_data *owner_data;

    if (!surface || !surface->wl_surface) return;
    if (!process_wayland.winehua_toplevel) return;
    if (!wayland_surface_is_toplevel(surface)) return;

    ex_style = NtUserGetWindowLongW(surface->hwnd, GWL_EXSTYLE);
    if (!(ex_style & WS_EX_DLGMODALFRAME))
    {
        modal = FALSE;
        owner = NULL;
    }
    else
    {
        owner = NtUserGetWindowRelative(surface->hwnd, GW_OWNER);
        if (owner && (NtUserGetWindowLongW(owner, GWL_STYLE) & WS_DISABLED))
        {
            modal = TRUE;
            /* owner 是被禁用的主窗口本身, 不是它的祖先 — 模态判定只看直系
             * owner (dialog.c 的 EnableWindow 也只禁用直接 owner)。 */
        }
        else
        {
            modal = FALSE;
            owner = NULL;
        }
    }

    if (!modal && !surface->winehua_modal_owner) return; /* 状态未变 (非模态) */

    /* owner 的 wl_surface 通过 owner 的 win_data 现取; owner 显示前 (surface
     * 未建) 则本次不发送、保持未上报状态 — 模态场景下本函数每次
     * WindowPosChanged 都会重新进入 (早退只拦"非模态且未上报"), owner 一旦
     * 显示随后任意一次状态变化即补发。不发送 NULL+modal=1: 协议里
     * owner_surface==NULL 是 clearing 语义, 与"应用关系"矛盾。 */
    owner_data = modal ? wayland_win_data_get_nolock(owner) : NULL;
    owner_data_hwnd = modal && owner_data ? owner_data->hwnd : NULL;

    if (modal && owner_data && get_toplevel_surface(owner_data))
    {
        TRACE("hwnd=%p modal=%d owner=%p owner_surface=%p",
              surface->hwnd, modal, owner_data_hwnd,
              get_toplevel_surface(owner_data));

        winehua_toplevel_set_modal(process_wayland.winehua_toplevel,
                                   surface->wl_surface,
                                   get_toplevel_surface(owner_data), 1);
        surface->winehua_modal_owner = owner;
    }
    else if (modal)
    {
        /* owner surface 未就绪 (rare): 不发送, 保留下次重试 */
        TRACE("hwnd=%p modal=1 but owner surface not ready (owner=%p), defer\n",
              surface->hwnd, owner_data_hwnd);
        surface->winehua_modal_owner = NULL;
    }
    else
    {
        winehua_toplevel_set_modal(process_wayland.winehua_toplevel,
                                   surface->wl_surface, NULL, 0);
        surface->winehua_modal_owner = NULL;
    }
}

/**********************************************************************
 *          winehua_modal_release
 *
 * surface 销毁前解除模态关系 (复合器侧同时清理组)。
 */
void winehua_modal_release(struct wayland_surface *surface)
{
    if (!surface || !surface->wl_surface) return;
    if (!surface->winehua_modal_owner) return;
    if (!process_wayland.winehua_toplevel) return;

    TRACE("hwnd=%p releasing modal\n", surface->hwnd);
    winehua_toplevel_set_modal(process_wayland.winehua_toplevel,
                               surface->wl_surface, NULL, 0);
    surface->winehua_modal_owner = NULL;
}

#endif /* __OHOS__ */
