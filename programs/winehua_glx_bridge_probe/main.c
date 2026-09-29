/*
 * WineHua GLX-over-EGL bridge probe (M1-T6 R1 spike).
 *
 * 量取「GL 渲染 + 回读」与「回读结果投递到 X 窗口」两段耗时, 为显示路线
 * spec §5 R1 (GLX-over-EGL 桥) 出 go/no-go 裁决数据:
 *   A 段 (GL):   WGL 隐藏窗上下文 → glClear 帧生成 + glReadPixels 回读,
 *                分别计时 (512x512=1MB 与 800x600≈1.83MB 两档)。virpipe 下
 *                这正是桥方案的 GL 侧成本 (桥用 EGL surfaceless, 同一
 *                libEGL/virpipe 栈, 读回路径相同)。
 *   B 段 (投递): 32bpp DIB → BitBlt 到可见窗口, GDI 路径在 X 路线即
 *                winex11 的 XPutImage —— 桥方案的投递成本。MIT-SHM 已禁
 *                (M1-T5), 原计划的 XShmPutImage 无对应物, BitBlt 即实际
 *                路径。
 * 运行路线自判 (WAYLAND_DISPLAY/DISPLAY 环境变量, 与 wine_child 的路由
 * 契约一致): X 路线 A 段预期失败 (无 GLX —— 这正是桥要补的洞), 该失败
 * 本身就是 R1 数据点。
 *
 * 输出: stdout 人读表 + --result JSON metrics (automation 契约)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#include "../winehua_smoke_protocol.h"

#define PROBE_DEFAULT_SECONDS 10
#define PROBE_ITERATIONS 60
#define PROBE_WARMUP 5

struct probe_timing
{
    BOOL ok;
    double gen_ms;      /* 帧生成 (glClear+glFinish) 平均 */
    double read_ms;     /* 回读 (glReadPixels+sync) 平均 */
    double total_ms;
    double mbps;        /* 回读有效带宽 */
};

struct xfer_timing
{
    BOOL ok;
    double blit_ms;
    double mbps;
};

static double qpc_to_ms(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER freq)
{
    return (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static const char *detect_route(void)
{
    const char *wl = getenv("WAYLAND_DISPLAY");
    const char *x = getenv("DISPLAY");
    if (wl && wl[0]) return "wayland";
    if (x && x[0]) return "x11";
    return "none";
}

/* ── A 段: GL 帧生成 + 回读 ──────────────────────────────────────────── */

static struct probe_timing probe_gl(int width, int height)
{
    static const char *class_name = "WineHuaGLXBrdgProbe";
    struct probe_timing result;
    WNDCLASSA wc;
    HWND hwnd;
    HDC dc;
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    HGLRC (WINAPI *wgl_create_context)(HDC);
    BOOL (WINAPI *wgl_make_current)(HDC, HGLRC);
    BOOL (WINAPI *wgl_delete_context)(HGLRC);
    HMODULE mod;
    HGLRC glrc;
    LARGE_INTEGER freq, t0, t1;
    unsigned char *pixels;
    int iter, warm;
    double gen_sum = 0.0, read_sum = 0.0;
    double mbytes = (double)width * height * 4.0 / (1024.0 * 1024.0);

    memset(&result, 0, sizeof(result));

    mod = LoadLibraryA("opengl32.dll");
    if (!mod)
    {
        fprintf(stderr, "[probe] GL stage: opengl32.dll load failed\n");
        return result;
    }
    wgl_create_context = (HGLRC (WINAPI *)(HDC))GetProcAddress(mod, "wglCreateContext");
    wgl_make_current = (BOOL (WINAPI *)(HDC, HGLRC))GetProcAddress(mod, "wglMakeCurrent");
    wgl_delete_context = (BOOL (WINAPI *)(HGLRC))GetProcAddress(mod, "wglDeleteContext");
    if (!wgl_create_context || !wgl_make_current || !wgl_delete_context)
    {
        fprintf(stderr, "[probe] GL stage: wgl exports missing\n");
        return result;
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = class_name;
    RegisterClassA(&wc);
    /* 隐藏窗: 只借 DC 建上下文, 不 present —— present 成本不属于桥
     * (桥直接 readback)。 */
    hwnd = CreateWindowExA(0, class_name, "glxbrdg", WS_OVERLAPPED,
                           8, 8, width, height, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
        fprintf(stderr, "[probe] GL stage: window create failed %lu\n", GetLastError());
        return result;
    }
    dc = GetDC(hwnd);
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.iLayerType = PFD_MAIN_PLANE;
    pf = ChoosePixelFormat(dc, &pfd);
    if (pf <= 0 || !SetPixelFormat(dc, pf, &pfd))
    {
        fprintf(stderr, "[probe] GL stage: pixel format failed pf=%d err=%lu\n",
                pf, GetLastError());
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return result;
    }
    glrc = wgl_create_context(dc);
    if (!glrc || !wgl_make_current(dc, glrc))
    {
        /* X 路线预期走到这里 (winex11 无 GLX) —— R1 数据点本身 */
        fprintf(stderr, "[probe] GL stage: context create failed %lu "
                "(X 路线无 GLX = 桥要补的洞)\n", GetLastError());
        if (glrc) wgl_delete_context(glrc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return result;
    }

    fprintf(stderr, "[probe] GL ctx ok: ver=%s renderer=%s\n",
            (const char *)glGetString(GL_VERSION),
            (const char *)glGetString(GL_RENDERER));

    pixels = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)width * height * 4);
    if (!pixels)
    {
        wgl_make_current(dc, NULL);
        wgl_delete_context(glrc);
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return result;
    }

    QueryPerformanceFrequency(&freq);
    glViewport(0, 0, width, height);

    for (warm = 0; warm < PROBE_WARMUP; ++warm)
    {
        glClearColor(0.1f * (warm % 4), 0.2f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }

    for (iter = 0; iter < PROBE_ITERATIONS; ++iter)
    {
        LARGE_INTEGER t_read0;
        QueryPerformanceCounter(&t0);
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        QueryPerformanceCounter(&t1);
        gen_sum += qpc_to_ms(t0, t1, freq);

        t_read0 = t1;
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glFinish();
        QueryPerformanceCounter(&t1);
        read_sum += qpc_to_ms(t_read0, t1, freq);
    }

    result.ok = TRUE;
    result.gen_ms = gen_sum / PROBE_ITERATIONS;
    result.read_ms = read_sum / PROBE_ITERATIONS;
    result.total_ms = result.gen_ms + result.read_ms;
    result.mbps = result.read_ms > 0.0 ? mbytes / (result.read_ms / 1000.0) : 0.0;

    fprintf(stderr, "[probe] GL %dx%d (%.2f MB): gen=%.2fms read=%.2fms "
            "total=%.2fms read-bw=%.1fMB/s\n",
            width, height, mbytes, result.gen_ms, result.read_ms,
            result.total_ms, result.mbps);

    HeapFree(GetProcessHeap(), 0, pixels);
    wgl_make_current(dc, NULL);
    wgl_delete_context(glrc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return result;
}

/* ── B 段: 回读结果投递到窗口 (X 路线 = winex11 XPutImage) ───────────── */

static struct xfer_timing probe_xfer(int width, int height)
{
    static const char *class_name = "WineHuaGLXBrdgXfer";
    struct xfer_timing result;
    WNDCLASSA wc;
    HWND hwnd;
    HDC dc, mem_dc;
    HBITMAP dib, old_bmp;
    BITMAPV5HEADER bi;
    void *bits;
    LARGE_INTEGER freq, t0, t1;
    int iter, warm;
    unsigned char *row;
    double sum = 0.0;
    double mbytes = (double)width * height * 4.0 / (1024.0 * 1024.0);

    memset(&result, 0, sizeof(result));

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = class_name;
    RegisterClassA(&wc);
    /* 可见窗: X 路线走 winex11 映射 (T5 后屏幕尺寸有效), 投递是真实路径 */
    hwnd = CreateWindowExA(0, class_name, "glxbrdg-xfer",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           10, 10, width, height, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
        fprintf(stderr, "[probe] XFER stage: window create failed %lu\n", GetLastError());
        return result;
    }
    /* 泵消息 500ms 等映射完成: 映射是异步的, 不等的话计时窗落在未映射期,
     * BitBlt 被可见区域裁剪成空操作 —— 实测 0.04ms/1MB (24GB/s, 纯本地
     * memcpy 量级), 量到的不是投递 (r0929110602-t6b)。 */
    {
        DWORD deadline = GetTickCount() + 500;
        MSG msg;
        while (GetTickCount() < deadline)
        {
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            Sleep(10);
        }
    }
    dc = GetDC(hwnd);

    memset(&bi, 0, sizeof(bi));
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = width;
    bi.bV5Height = -height; /* top-down */
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask = 0x000000ff;
    dib = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits)
    {
        fprintf(stderr, "[probe] XFER stage: DIB create failed\n");
        ReleaseDC(hwnd, dc);
        DestroyWindow(hwnd);
        return result;
    }
    memset(bits, 0x40, (size_t)width * height * 4);
    mem_dc = CreateCompatibleDC(dc);
    old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

    /* 每迭代改一行再 blit: 模拟「新帧投递」而非全零去重 */
    row = (unsigned char *)bits;

    QueryPerformanceFrequency(&freq);
    for (warm = 0; warm < PROBE_WARMUP; ++warm)
    {
        row[(warm % height) * width * 4] = (unsigned char)(warm + 1);
        BitBlt(dc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
        GdiFlush();
    }
    for (iter = 0; iter < PROBE_ITERATIONS; ++iter)
    {
        QueryPerformanceCounter(&t0);
        row[(iter % height) * width * 4] = (unsigned char)((iter + 1) & 0xff);
        BitBlt(dc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
        GdiFlush();
        QueryPerformanceCounter(&t1);
        sum += qpc_to_ms(t0, t1, freq);
    }

    result.ok = TRUE;
    result.blit_ms = sum / PROBE_ITERATIONS;
    result.mbps = result.blit_ms > 0.0 ? mbytes / (result.blit_ms / 1000.0) : 0.0;

    fprintf(stderr, "[probe] XFER %dx%d (%.2f MB): blit=%.2fms bw=%.1fMB/s\n",
            width, height, mbytes, result.blit_ms, result.mbps);

    SelectObject(mem_dc, old_bmp);
    DeleteDC(mem_dc);
    DeleteObject(dib);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return result;
}

int main(int argc, char **argv)
{
    struct winehua_smoke_options options;
    struct probe_timing gl512, gl800;
    struct xfer_timing xfer;
    const char *route = detect_route();
    char metrics[1024];
    BOOL gl_any;

    if (!winehua_smoke_parse_options(&options, argc, argv, PROBE_DEFAULT_SECONDS))
        return 1;

    fprintf(stderr, "[probe] route=%s (WAYLAND_DISPLAY=%s DISPLAY=%s)\n", route,
            getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)",
            getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");

    /* A 段: 两档分辨率。X 路线预期 ctx 失败 (无 GLX)。 */
    gl512 = probe_gl(512, 512);
    gl800 = probe_gl(800, 600);
    /* B 段: 投递。 */
    xfer = probe_xfer(512, 512);

    gl_any = gl512.ok || gl800.ok;
    snprintf(metrics, sizeof(metrics),
             "{\"route\": \"%s\", "
             "\"gl512\": {\"ok\": %s, \"genMs\": %.2f, \"readMs\": %.2f, "
             "\"totalMs\": %.2f, \"readMBps\": %.1f}, "
             "\"gl800x600\": {\"ok\": %s, \"genMs\": %.2f, \"readMs\": %.2f, "
             "\"totalMs\": %.2f, \"readMBps\": %.1f}, "
             "\"xfer512\": {\"ok\": %s, \"blitMs\": %.2f, \"blitMBps\": %.1f}, "
             "\"frameBudgetMs30fps\": %.2f}",
             route,
             gl512.ok ? "true" : "false", gl512.gen_ms, gl512.read_ms,
             gl512.total_ms, gl512.mbps,
             gl800.ok ? "true" : "false", gl800.gen_ms, gl800.read_ms,
             gl800.total_ms, gl800.mbps,
             xfer.ok ? "true" : "false", xfer.blit_ms, xfer.mbps,
             1000.0 / 30.0);

    /* 裁决口径 (数据给 spec §5 R1, 本程序不下结论): 桥每帧成本 ≈
     * gl800.total_ms (渲染+回读) + xfer.blit_ms (投递), 对照
     * frameBudgetMs30fps。 */

    /* 终态口径是 runner 侧 FINAL_STATUSES (大写 PASS/FAIL/SKIP/UNSUPPORTED),
     * 小写会被判「写出结果前退出」(实测 r0929110602-t6b)。 */
    winehua_smoke_write_result(&options, "PASS", "probe",
                               gl_any ? "gl+xfer measured" : "xfer only (no GL ctx)",
                               metrics);
    fprintf(stderr, "[probe] done (gl=%s xfer=%s)\n",
            gl_any ? "ok" : "unavailable", xfer.ok ? "ok" : "failed");
    return 0;
}
