/*
 * Window theming support
 *
 * Copyright 2022 Zhiyi Zhang for CodeWeavers
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "uxthemedll.h"
#include "vssym32.h"

static void uxtheme_draw_menu_button(HTHEME theme, HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                     RECT rect, BOOL down, BOOL grayed)
{
    int part, state;

    /* 映射到主窗口部件 [Window.CloseButton] 等。原先映射到 WP_MDI*BUTTON, 那是
     * MDI 子窗口菜单按钮的部件 (尺寸/素材都不同), 且 WP_MDIMAXBUTTON 不存在导致
     * 最大化按钮落回经典绘制 —— win32u 的 draw_nc_caption 现在用本函数绘制主窗口
     * 标题栏按钮, 映射必须对应主窗口段。 */
    switch (type)
    {
    case MENU_CLOSE_BUTTON:
        part = WP_CLOSEBUTTON;
        break;
    case MENU_MIN_BUTTON:
        part = WP_MINBUTTON;
        break;
    case MENU_MAX_BUTTON:
        part = WP_MAXBUTTON;
        break;
    case MENU_RESTORE_BUTTON:
        part = WP_RESTOREBUTTON;
        break;
    case MENU_HELP_BUTTON:
        part = WP_HELPBUTTON;
        break;
    default:
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    if (grayed)
        state = MINBS_DISABLED;
    else if (down)
        state = MINBS_PUSHED;
    else
        state = MINBS_NORMAL;

    if (IsThemeBackgroundPartiallyTransparent(theme, part, state))
        DrawThemeParentBackground(hwnd, hdc, &rect);
    DrawThemeBackground(theme, hdc, part, state, &rect, NULL);
}

void WINAPI UXTHEME_NonClientButtonDraw(HWND hwnd, HDC hdc, enum NONCLIENT_BUTTON_TYPE type,
                                        RECT rect, BOOL down, BOOL grayed)
{
    HTHEME theme;

    theme = OpenThemeDataForDpi(NULL, L"Window", GetDpiForWindow(hwnd));
    if (!theme)
    {
        user_api.pNonClientButtonDraw(hwnd, hdc, type, rect, down, grayed);
        return;
    }

    switch (type)
    {
    case MENU_CLOSE_BUTTON:
    case MENU_MIN_BUTTON:
    case MENU_MAX_BUTTON:
    case MENU_RESTORE_BUTTON:
    case MENU_HELP_BUTTON:
        uxtheme_draw_menu_button(theme, hwnd, hdc, type, rect, down, grayed);
        break;
    }

    CloseThemeData(theme);
}
