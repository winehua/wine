// dinput_click_probe.c — dinput 静止点击探针
//
// 目的: 实证 "无位移伴随的点击" 在 wine 的 dinput 轮询层与 wndproc 消息层
// 分别呈现什么, 用于判定 PAL2 (仙剑2) 静止点击失效的归因:
//   假说 A (游戏侧门控): 游戏只在 GetDeviceState 的 lX/lY 非零的轮询里处理
//     按键。若静止点击时本探针的 POLL 行出现且 lX=lY=0 → dinput 层按键投递
//     完整 → 假说 A 实锤, compositor 的 1px nudge 是正确层级的兼容 shim。
//   假说 B (wine 投递缺口): 静止点击的按键状态根本到不了 dinput 轮询层 →
//     POLL 行缺席 → wine 缺陷, nudge 只是掩盖, 应修 wine。
//
// 探针复刻 PAL2 的输入模式: DISCL_NONEXCLUSIVE|DISCL_BACKGROUND 协作级别,
// c_dfDIMouse 数据格式, 独立线程 ~125Hz GetDeviceState 轮询, 有轴数据时
// SetCursorPos(320,240) 回中 (PAL2 实测行为)。
//
// 观测输出 (写 C:\windows\temp\dinput_probe.log, = 设备沙箱 temp/, hdc 可拉):
//   POLL DOWN/UP  — dinput 轮询看到的按键沿, 附同轮 lX/lY
//   WND  DOWN/UP  — wndproc 收到的 WM_LBUTTON*, 附坐标与距上条 WM_MOUSEMOVE 的间隔
//   WND  MOVE     — wndproc 收到的 WM_MOUSEMOVE (前 50 条逐条, 之后只计数)
//   STAT          — 每 5s 统计: 轮询次数/回中次数/wndproc 消息计数
//
// 用法: dinput_click_probe.exe [pump]
//   pump 模式每轮轮询都 SetCursorPos(320,240) (含 no-op), 用于验证 wine 对
//   位置未变的 SetCursorPos 是否仍合成 WM_MOUSEMOVE (server/queue.c:4059
//   无条件 set_cursor_pos, 客户端 win32u/input.c:707 无早退 — 若属实,
//   pump 模式静止时 WND MOVE 应持续刷屏)。
//
// 构建 (mingw-w64, 与 PAL2 同为 32 位 PE): make smoke-pe
//   产物 build/smoke-pe/dinput_click_probe.exe

#define DIRECTINPUT_VERSION 0x0800
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>

static void plog(const char *fmt, ...)
{
    FILE *f = fopen("C:\\windows\\temp\\dinput_probe.log", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt);
    fprintf(f, "[%8lu] ", (unsigned long)GetTickCount());
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
    fclose(f);
}

static HWND g_hwnd;
static volatile LONG g_quit;
static int g_pump;

/* wndproc 级统计 (消息线程写, 轮询线程读, 用 Interlocked 保证可见性) */
static DWORD g_lastMoveTick;
static volatile LONG g_moveCount, g_downCount, g_upCount;
/* 界面显示用: 轮询线程写的实时状态 */
static volatile LONG g_pollIters;      /* 当前 5s 窗口内已轮询次数 (增长=线程活着) */
static volatile LONG g_pollBtn, g_pollLX, g_pollLY; /* 最近一次按键沿及同轮轴数据 */
static volatile LONG g_lastSinceMove = -1;          /* 最近一次 WND DOWN 距上条 MOVE 的 ms */
static volatile LONG g_rawCount, g_rawBtnFlags;     /* WM_INPUT(rawinput) 计数/最近按键标志 */

static void PaintStatus(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    char buf[256];
    int y = 10;
    int n;
    n = wsprintfA(buf, "poll alive: %ld iters in current 5s window   (pump=%d)",
                  g_pollIters, g_pump);
    TextOutA(hdc, 10, y, buf, n); y += 24;
    n = wsprintfA(buf, "POLL last edge: btn=%ld lX=%ld lY=%ld",
                  g_pollBtn, g_pollLX, g_pollLY);
    TextOutA(hdc, 10, y, buf, n); y += 24;
    n = wsprintfA(buf, "WND counts: move=%ld down=%ld up=%ld",
                  g_moveCount, g_downCount, g_upCount);
    TextOutA(hdc, 10, y, buf, n); y += 24;
    n = wsprintfA(buf, "last WND DOWN sinceMove=%ldms", (long)g_lastSinceMove);
    TextOutA(hdc, 10, y, buf, n); y += 24;
    n = wsprintfA(buf, "RAW count=%ld lastBtnFlags=0x%lx", g_rawCount, g_rawBtnFlags);
    TextOutA(hdc, 10, y, buf, n);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE: {
        LONG n = InterlockedIncrement(&g_moveCount);
        g_lastMoveTick = GetTickCount();
        if (n <= 50) /* 前 50 条逐条记录, 足以覆盖静止期与点击邻域 */
            plog("WND  MOVE #%ld pt=(%ld,%ld)", n,
                 (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp));
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        DWORD now = GetTickCount();
        long since = g_lastMoveTick ? (long)(now - g_lastMoveTick) : -1;
        if (msg == WM_LBUTTONDOWN) {
            InterlockedIncrement(&g_downCount);
            g_lastSinceMove = since;
        } else {
            InterlockedIncrement(&g_upCount);
        }
        plog("WND  %s lp=(%ld,%ld) sinceMove=%ldms",
             msg == WM_LBUTTONDOWN ? "DOWN" : "UP  ",
             (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp), since);
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_INPUT: {
        /* 直接注册 rawinput 观测: 静止点击的按键是否以 WM_INPUT 到达窗口 */
        char buf[sizeof(RAWINPUT) + 16];
        UINT size = sizeof(buf);
        RAWINPUT *ri = (RAWINPUT *)buf;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, ri, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri->header.dwType == RIM_TYPEMOUSE) {
            InterlockedIncrement(&g_rawCount);
            if (ri->data.mouse.usButtonFlags) {
                g_rawBtnFlags = ri->data.mouse.usButtonFlags;
                plog("RAW  btn=0x%x dx=%ld dy=%ld wp=%#Ix",
                     ri->data.mouse.usButtonFlags,
                     (long)ri->data.mouse.lLastX, (long)ri->data.mouse.lLastY, wp);
            }
        }
        return 0;
    }
    case WM_PAINT:
        PaintStatus(hwnd);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_quit, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI PollThread(LPVOID arg)
{
    IDirectInput8A *di = NULL;
    IDirectInputDevice8A *dev = NULL;
    DIMOUSESTATE st;
    BYTE prevBtn = 0;
    DWORD iters = 0, recenters = 0, lastStat = GetTickCount();
    HRESULT hr;

    (void)arg;
    hr = DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                            &IID_IDirectInput8A, (void **)&di, NULL);
    if (FAILED(hr)) { plog("POLL DirectInput8Create failed hr=0x%lx", hr); return 1; }
    hr = di->lpVtbl->CreateDevice(di, &GUID_SysMouse, &dev, NULL);
    if (FAILED(hr)) { plog("POLL CreateDevice failed hr=0x%lx", hr); return 1; }
    dev->lpVtbl->SetDataFormat(dev, &c_dfDIMouse);
    /* 与 PAL2 实测相同的协作级别 */
    dev->lpVtbl->SetCooperativeLevel(dev, g_hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND);
    dev->lpVtbl->Acquire(dev);
    plog("POLL thread started, pump=%d", g_pump);

    while (!g_quit) {
        DWORD now;
        hr = dev->lpVtbl->GetDeviceState(dev, sizeof(st), &st);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            dev->lpVtbl->Acquire(dev);
            Sleep(8);
            continue;
        }
        if (FAILED(hr)) { Sleep(8); continue; }
        iters++;
        g_pollIters = (LONG)iters; /* 界面心跳: 持续增长 = 轮询线程活着 */
        if (st.lX || st.lY || g_pump) {
            SetCursorPos(320, 240); /* PAL2 式回中; pump 模式含 no-op */
            recenters++;
        }
        if ((st.rgbButtons[0] & 0x80) != prevBtn) {
            prevBtn = st.rgbButtons[0] & 0x80;
            g_pollBtn = prevBtn ? 1 : 0;
            g_pollLX = st.lX;
            g_pollLY = st.lY;
            plog("POLL %s lX=%ld lY=%ld", prevBtn ? "DOWN" : "UP  ",
                 (long)st.lX, (long)st.lY);
        }
        now = GetTickCount();
        if (now - lastStat >= 5000) {
            plog("STAT iters=%lu recenters=%lu wnd(d=%ld,u=%ld,m=%ld)",
                 iters, recenters, g_downCount, g_upCount, g_moveCount);
            iters = 0;
            recenters = 0;
            lastStat = now;
        }
        Sleep(8);
    }
    dev->lpVtbl->Unacquire(dev);
    dev->lpVtbl->Release(dev);
    di->lpVtbl->Release(di);
    return 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASSA wc = {0};
    MSG msg;
    HANDLE th;

    (void)prev; (void)show;
    g_pump = cmdline && strstr(cmdline, "pump") != NULL;

    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "DinputClickProbe";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowA(wc.lpszClassName, "DinputClickProbe",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 640, 480, NULL, NULL, inst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    SetTimer(g_hwnd, 1, 200, NULL); /* 200ms 刷新界面状态 */
    {
        /* 与 dinput (DISCL_BACKGROUND) 相同的 INPUTSINK 注册, 观测原始 WM_INPUT */
        RAWINPUTDEVICE rid = {0};
        rid.usUsagePage = 0x01; /* HID_USAGE_PAGE_GENERIC */
        rid.usUsage = 0x02;     /* HID_USAGE_GENERIC_MOUSE */
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = g_hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)))
            plog("RAW  RegisterRawInputDevices failed err=%lu", GetLastError());
    }

    plog("==== probe start, pump=%d ====", g_pump);
    th = CreateThread(NULL, 0, PollThread, NULL, 0, NULL);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    InterlockedExchange(&g_quit, 1);
    WaitForSingleObject(th, 2000);
    plog("==== probe exit ====");
    return 0;
}
