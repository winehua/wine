/*
 * WineHua desktop keep-alive helper (Pad desktop mode only).
 *
 * When the last non-system process exits a virtual desktop, wineserver
 * schedules a 1-second close timeout that sends WM_CLOSE to the desktop
 * owner (explorer), destroying the desktop.  This is undesirable in Pad
 * desktop mode where the desktop should persist as a normal PC does.
 *
 * This helper joins the "shell" virtual desktop, creates a hidden
 * window, and runs a message loop that ignores WM_CLOSE.  Its thread
 * keeps desktop->users > explorer->running_threads, so the auto-close
 * condition in remove_desktop_user() is never satisfied.
 *
 * See server/winstation.c: close_desktop_timeout / remove_desktop_user.
 */
#include <windows.h>

/* Same as explorer's desktop.c — not available in mingw headers */
#define DESKTOP_ALL_ACCESS 0x01ff

static LRESULT CALLBACK keep_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CLOSE:
        /* Refuse to close — we are the desktop anchor. */
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    HDESK hDesk;
    WNDCLASSA wc = {0};
    HWND hwnd;
    MSG msg;

    /* Open the desktop that explorer created. */
    hDesk = OpenDesktopA("shell", 0, FALSE, DESKTOP_ALL_ACCESS);
    if (!hDesk) goto sleep_forever;

    SetThreadDesktop(hDesk);
    CloseDesktop(hDesk);

    /* Create a tiny hidden window on the desktop so we have
       a persistent user-object presence. */
    wc.lpfnWndProc = keep_wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "WineHuaKeep";
    RegisterClassA(&wc);

    hwnd = CreateWindowExA(0, "WineHuaKeep", NULL,
                           WS_POPUP, 0, 0, 1, 1,
                           NULL, NULL, hInstance, NULL);

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

sleep_forever:
    Sleep(INFINITE);
    return 0;
}
