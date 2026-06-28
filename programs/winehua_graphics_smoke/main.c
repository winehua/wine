/*
 * WineHua graphics smoke test.
 *
 * Creates a Win32 OpenGL window, renders a small animated 3D scene, and
 * reports FPS so the VirGL path can be visually confirmed.
 */

#define COBJMACROS

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <GL/gl.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct app_state
{
    HWND hwnd;
    HDC hdc;
    HGLRC glrc;
    int width;
    int height;
    BOOL running;
    BOOL loop_forever;
    DWORD duration_ms;
    LARGE_INTEGER freq;
    const char *requested_backend;
    const char *active_backend;
    const char *virgl_ready;
    const char *virgl_socket_ready;
    const char *virgl_library_ready;
    const char *guest_gfx_ready;
    const char *guest_gfx_mode;
    const char *frame_presenter;
    const char *frame_zero_copy;
    const char *frame_damage_upload;
    const char *native_buffer_available;
    const char *frame_fallback;
    const char *frame_transport;
    const char *graphics_note;
    BOOL force_gl;
    double current_fps;
};

enum seven_segment_bits
{
    SEG_A = 1 << 0,
    SEG_B = 1 << 1,
    SEG_C = 1 << 2,
    SEG_D = 1 << 3,
    SEG_E = 1 << 4,
    SEG_F = 1 << 5,
    SEG_G = 1 << 6,
};

static const unsigned char digit_segments[10] =
{
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,
    SEG_F | SEG_G | SEG_B | SEG_C,
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
};

static void load_graphics_env(struct app_state *state)
{
    const char *force_gl;

    state->requested_backend = getenv("WINEHUA_GRAPHICS_BACKEND");
    state->active_backend = getenv("WINEHUA_GRAPHICS_ACTIVE");
    state->virgl_ready = getenv("WINEHUA_VIRGL_READY");
    state->virgl_socket_ready = getenv("WINEHUA_VIRGL_SOCKET_READY");
    state->virgl_library_ready = getenv("WINEHUA_VIRGL_LIBRARY_READY");
    state->guest_gfx_ready = getenv("WINEHUA_GUEST_GFX_READY");
    state->guest_gfx_mode = getenv("WINEHUA_GUEST_GFX_MODE");
    state->frame_presenter = getenv("WINEHUA_FRAME_PRESENTER");
    state->frame_zero_copy = getenv("WINEHUA_FRAME_ZERO_COPY");
    state->frame_damage_upload = getenv("WINEHUA_FRAME_DAMAGE_UPLOAD");
    state->native_buffer_available = getenv("WINEHUA_NATIVE_BUFFER_AVAILABLE");
    state->frame_fallback = getenv("WINEHUA_FRAME_FALLBACK");
    state->frame_transport = getenv("WINEHUA_FRAME_TRANSPORT");
    state->graphics_note = getenv("WINEHUA_GRAPHICS_NOTE");
    force_gl = getenv("WINEHUA_GRAPHICS_FORCE_GL");
    state->force_gl = force_gl && !lstrcmpA(force_gl, "1");
}

static BOOL validate_graphics_backend(const struct app_state *state, char *reason, size_t reason_size)
{
    const char *requested = state->requested_backend ? state->requested_backend : "?";
    const char *active = state->active_backend ? state->active_backend : "?";
    const char *ready = state->virgl_ready ? state->virgl_ready : "?";
    const char *socket_ready = state->virgl_socket_ready ? state->virgl_socket_ready : "?";
    const char *library_ready = state->virgl_library_ready ? state->virgl_library_ready : "?";
    const char *guest_ready = state->guest_gfx_ready ? state->guest_gfx_ready : "?";
    const char *guest_mode = state->guest_gfx_mode ? state->guest_gfx_mode : "?";
    const char *transport = state->frame_transport ? state->frame_transport : "?";
    const char *note = state->graphics_note ? state->graphics_note : "(none)";
    const char *force_mode = state->force_gl ? "1" : "0";

    if (reason_size) reason[0] = '\0';

    if (!state->active_backend || lstrcmpiA(state->active_backend, "virgl"))
    {
        if (state->force_gl)
        {
            snprintf(reason, reason_size,
                     "VirGL backend is not active, but force_gl=1 so continuing with OpenGL diagnostics.\n\n"
                     "requested=%s\nactive=%s\nready=%s\nguest_gfx_ready=%s\nguest_gfx_mode=%s\nsocket_ready=%s\nlibrary_ready=%s\ntransport=%s\nforce_gl=%s\n\n"
                     "note=%s",
                     requested, active, ready, guest_ready, guest_mode, socket_ready, library_ready, transport, force_mode, note);
            return TRUE;
        }
        snprintf(reason, reason_size,
                 "VirGL backend is not active.\n\nrequested=%s\nactive=%s\nready=%s\nguest_gfx_ready=%s\nguest_gfx_mode=%s\nsocket_ready=%s\nlibrary_ready=%s\ntransport=%s\nforce_gl=%s\n\nnote=%s\n\n"
                 "Current build is still falling back to shm, so the OpenGL smoke test cannot run yet.",
                 requested, active, ready, guest_ready, guest_mode, socket_ready, library_ready, transport, force_mode, note);
        return FALSE;
    }

    if (!state->virgl_ready || lstrcmpA(state->virgl_ready, "1"))
    {
        if (state->force_gl)
        {
            snprintf(reason, reason_size,
                     "VirGL backend is selected but not ready, yet force_gl=1 so continuing with OpenGL diagnostics.\n\n"
                     "requested=%s\nactive=%s\nready=%s\nguest_gfx_ready=%s\nguest_gfx_mode=%s\nsocket_ready=%s\nlibrary_ready=%s\ntransport=%s\nforce_gl=%s\n\n"
                     "note=%s",
                     requested, active, ready, guest_ready, guest_mode, socket_ready, library_ready, transport, force_mode, note);
            return TRUE;
        }
        snprintf(reason, reason_size,
                 "VirGL backend is selected but not ready.\n\nrequested=%s\nactive=%s\nready=%s\nguest_gfx_ready=%s\nguest_gfx_mode=%s\nsocket_ready=%s\nlibrary_ready=%s\ntransport=%s\nforce_gl=%s\n\nnote=%s\n\n"
                 "Current build has not finished the required VirGL/OpenGL runtime wiring.",
                 requested, active, ready, guest_ready, guest_mode, socket_ready, library_ready, transport, force_mode, note);
        return FALSE;
    }

    return TRUE;
}

static LRESULT CALLBACK smoke_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct app_state *state = (struct app_state *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_NCCREATE:
    {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
        return TRUE;
    }
    case WM_SIZE:
        if (state)
        {
            state->width = LOWORD(lparam);
            state->height = HIWORD(lparam);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (state) state->running = FALSE;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

static BOOL init_window(struct app_state *state)
{
    static const WCHAR class_name[] = L"WineHuaGraphicsSmokeWindow";
    WNDCLASSEXW wc = {0};
    RECT rect = {0, 0, 960, 540};
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;

    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = smoke_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = class_name;

    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        fprintf(stderr, "winehua_graphics_smoke: RegisterClassExW failed: %lu\n", GetLastError());
        return FALSE;
    }

    AdjustWindowRect(&rect, style, FALSE);
    state->hwnd = CreateWindowExW(0, class_name, L"WineHua Graphics Smoke",
                                  style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left, rect.bottom - rect.top,
                                  NULL, NULL, wc.hInstance, state);
    if (!state->hwnd)
    {
        fprintf(stderr, "winehua_graphics_smoke: CreateWindowExW failed: %lu\n", GetLastError());
        return FALSE;
    }

    state->hdc = GetDC(state->hwnd);
    if (!state->hdc)
    {
        fprintf(stderr, "winehua_graphics_smoke: GetDC failed: %lu\n", GetLastError());
        return FALSE;
    }

    return TRUE;
}

static BOOL init_opengl(struct app_state *state)
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    int format;

    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(state->hdc, &pfd);
    if (!format)
    {
        fprintf(stderr, "winehua_graphics_smoke: ChoosePixelFormat failed: %lu\n", GetLastError());
        return FALSE;
    }

    if (!SetPixelFormat(state->hdc, format, &pfd))
    {
        fprintf(stderr, "winehua_graphics_smoke: SetPixelFormat failed: %lu\n", GetLastError());
        return FALSE;
    }

    state->glrc = wglCreateContext(state->hdc);
    if (!state->glrc)
    {
        fprintf(stderr, "winehua_graphics_smoke: wglCreateContext failed: %lu\n", GetLastError());
        return FALSE;
    }

    if (!wglMakeCurrent(state->hdc, state->glrc))
    {
        fprintf(stderr, "winehua_graphics_smoke: wglMakeCurrent failed: %lu\n", GetLastError());
        return FALSE;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glClearDepth(1.0);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    fprintf(stderr, "winehua_graphics_smoke: GL vendor=%s renderer=%s version=%s\n",
            (const char *)glGetString(GL_VENDOR),
            (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_VERSION));
    fprintf(stderr, "winehua_graphics_smoke: env requested=%s active=%s virgl_ready=%s guest_gfx_ready=%s guest_gfx_mode=%s socket_ready=%s library_ready=%s presenter=%s zero_copy=%s damage_upload=%s native_buffer=%s fallback=%s transport=%s force_gl=%d note=%s\n",
            state->requested_backend ? state->requested_backend : "(null)",
            state->active_backend ? state->active_backend : "(null)",
            state->virgl_ready ? state->virgl_ready : "(null)",
            state->guest_gfx_ready ? state->guest_gfx_ready : "(null)",
            state->guest_gfx_mode ? state->guest_gfx_mode : "(null)",
            state->virgl_socket_ready ? state->virgl_socket_ready : "(null)",
            state->virgl_library_ready ? state->virgl_library_ready : "(null)",
            state->frame_presenter ? state->frame_presenter : "(null)",
            state->frame_zero_copy ? state->frame_zero_copy : "(null)",
            state->frame_damage_upload ? state->frame_damage_upload : "(null)",
            state->native_buffer_available ? state->native_buffer_available : "(null)",
            state->frame_fallback ? state->frame_fallback : "(null)",
            state->frame_transport ? state->frame_transport : "(null)",
            state->force_gl ? 1 : 0,
            state->graphics_note ? state->graphics_note : "(null)");
    return TRUE;
}

static void set_window_title_once(const struct app_state *state)
{
    char title[256];

    snprintf(title, sizeof(title),
             "WineHua Graphics Smoke [req=%s act=%s ready=%s force=%d]",
             state->requested_backend ? state->requested_backend : "?",
             state->active_backend ? state->active_backend : "?",
             state->virgl_ready ? state->virgl_ready : "?",
             state->force_gl ? 1 : 0);
    SetWindowTextA(state->hwnd, title);
}

static void draw_ui_quad(float x0, float y0, float x1, float y1)
{
    glBegin(GL_QUADS);
    glVertex2f(x0, y0);
    glVertex2f(x1, y0);
    glVertex2f(x1, y1);
    glVertex2f(x0, y1);
    glEnd();
}

static void draw_seven_segment_digit(float x, float y, float scale, char ch)
{
    float glyph_w = scale * 0.68f;
    float glyph_h = scale * 1.20f;
    float thickness = scale * 0.16f;
    unsigned char mask = 0;

    if (ch >= '0' && ch <= '9')
        mask = digit_segments[ch - '0'];
    else if (ch == '-')
        mask = SEG_G;

    if (mask & SEG_A) draw_ui_quad(x + thickness, y, x + glyph_w - thickness, y + thickness);
    if (mask & SEG_B) draw_ui_quad(x + glyph_w - thickness, y + thickness, x + glyph_w, y + glyph_h * 0.5f - thickness * 0.5f);
    if (mask & SEG_C) draw_ui_quad(x + glyph_w - thickness, y + glyph_h * 0.5f + thickness * 0.5f, x + glyph_w, y + glyph_h - thickness);
    if (mask & SEG_D) draw_ui_quad(x + thickness, y + glyph_h - thickness, x + glyph_w - thickness, y + glyph_h);
    if (mask & SEG_E) draw_ui_quad(x, y + glyph_h * 0.5f + thickness * 0.5f, x + thickness, y + glyph_h - thickness);
    if (mask & SEG_F) draw_ui_quad(x, y + thickness, x + thickness, y + glyph_h * 0.5f - thickness * 0.5f);
    if (mask & SEG_G) draw_ui_quad(x + thickness, y + glyph_h * 0.5f - thickness * 0.5f,
                                   x + glyph_w - thickness, y + glyph_h * 0.5f + thickness * 0.5f);
}

static void draw_overlay_letter(float x, float y, float scale, char ch)
{
    float glyph_w = scale * 0.68f;
    float glyph_h = scale * 1.20f;
    float thickness = scale * 0.16f;

    switch (ch)
    {
    case 'F':
        draw_ui_quad(x, y, x + thickness, y + glyph_h);
        draw_ui_quad(x, y, x + glyph_w, y + thickness);
        draw_ui_quad(x, y + glyph_h * 0.5f - thickness * 0.5f,
                     x + glyph_w * 0.78f, y + glyph_h * 0.5f + thickness * 0.5f);
        break;
    case 'P':
        draw_ui_quad(x, y, x + thickness, y + glyph_h);
        draw_ui_quad(x, y, x + glyph_w - thickness, y + thickness);
        draw_ui_quad(x + glyph_w - thickness, y + thickness, x + glyph_w, y + glyph_h * 0.5f - thickness * 0.5f);
        draw_ui_quad(x, y + glyph_h * 0.5f - thickness * 0.5f,
                     x + glyph_w - thickness, y + glyph_h * 0.5f + thickness * 0.5f);
        break;
    case 'S':
        draw_seven_segment_digit(x, y, scale, '5');
        break;
    default:
        break;
    }
}

static void draw_overlay_text(float x, float y, float scale, const char *text)
{
    size_t i;
    float pen_x = x;

    for (i = 0; text[i]; ++i)
    {
        char ch = text[i];

        if (ch >= '0' && ch <= '9')
        {
            draw_seven_segment_digit(pen_x, y, scale, ch);
            pen_x += scale * 0.86f;
        }
        else if (ch == '.')
        {
            float dot = scale * 0.14f;
            draw_ui_quad(pen_x, y + scale * 1.02f, pen_x + dot, y + scale * 1.02f + dot);
            pen_x += scale * 0.34f;
        }
        else if (ch == '-')
        {
            draw_seven_segment_digit(pen_x, y, scale, ch);
            pen_x += scale * 0.86f;
        }
        else if (ch == ' ')
        {
            pen_x += scale * 0.46f;
        }
        else
        {
            draw_overlay_letter(pen_x, y, scale, ch);
            pen_x += scale * 0.90f;
        }
    }
}

static void draw_fps_overlay(const struct app_state *state)
{
    char text[64];
    int width = state->width > 0 ? state->width : 1;
    int height = state->height > 0 ? state->height : 1;
    float scale = 26.0f;
    float overlay_w = scale * 7.8f;
    float overlay_h = scale * 1.8f;
    float x = 18.0f;
    float y = 18.0f;

    if (state->current_fps > 0.05)
        snprintf(text, sizeof(text), "FPS %.1f", state->current_fps);
    else
        strcpy(text, "FPS --.-");

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, width, height, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glColor3f(0.04f, 0.05f, 0.07f);
    draw_ui_quad(x - 10.0f, y - 8.0f, x + overlay_w, y + overlay_h);

    glColor3f(0.18f, 0.95f, 0.42f);
    draw_overlay_text(x, y, scale, text);

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

static void shutdown_opengl(struct app_state *state)
{
    if (state->glrc)
    {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(state->glrc);
        state->glrc = NULL;
    }

    if (state->hdc && state->hwnd)
    {
        ReleaseDC(state->hwnd, state->hdc);
        state->hdc = NULL;
    }
}

static void setup_perspective(const struct app_state *state)
{
    double aspect = state->height > 0 ? (double)state->width / (double)state->height : (16.0 / 9.0);
    double fov_deg = 60.0;
    double z_near = 1.0;
    double z_far = 40.0;
    double top = tan(fov_deg * (M_PI / 360.0)) * z_near;
    double right = top * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, z_near, z_far);
}

static void draw_ground(float span, float step)
{
    int x_index;
    int z_index;
    int cells = (int)(span / step);
    float start = -span * 0.5f;

    glDisable(GL_CULL_FACE);
    glBegin(GL_QUADS);
    for (z_index = 0; z_index < cells; ++z_index)
    {
        for (x_index = 0; x_index < cells; ++x_index)
        {
            float x0 = start + x_index * step;
            float x1 = x0 + step;
            float z0 = start + z_index * step;
            float z1 = z0 + step;
            float shade = ((x_index + z_index) & 1) ? 0.22f : 0.14f;

            glColor3f(shade, shade + 0.03f, shade + 0.05f);
            glVertex3f(x0, -1.35f, z0);
            glVertex3f(x1, -1.35f, z0);
            glVertex3f(x1, -1.35f, z1);
            glVertex3f(x0, -1.35f, z1);
        }
    }
    glEnd();

    glColor3f(0.12f, 0.12f, 0.16f);
    glBegin(GL_LINES);
    for (x_index = 0; x_index <= cells; ++x_index)
    {
        float x = start + x_index * step;
        glVertex3f(x, -1.349f, start);
        glVertex3f(x, -1.349f, start + cells * step);
    }
    for (z_index = 0; z_index <= cells; ++z_index)
    {
        float z = start + z_index * step;
        glVertex3f(start, -1.349f, z);
        glVertex3f(start + cells * step, -1.349f, z);
    }
    glEnd();
    glEnable(GL_CULL_FACE);
}

static void draw_cube(float half_extent)
{
    glBegin(GL_QUADS);

    glColor3f(0.96f, 0.24f, 0.22f);
    glVertex3f(-half_extent, -half_extent, half_extent);
    glVertex3f(half_extent, -half_extent, half_extent);
    glVertex3f(half_extent, half_extent, half_extent);
    glVertex3f(-half_extent, half_extent, half_extent);

    glColor3f(0.25f, 0.92f, 0.35f);
    glVertex3f(-half_extent, -half_extent, -half_extent);
    glVertex3f(-half_extent, half_extent, -half_extent);
    glVertex3f(half_extent, half_extent, -half_extent);
    glVertex3f(half_extent, -half_extent, -half_extent);

    glColor3f(0.22f, 0.46f, 1.0f);
    glVertex3f(-half_extent, -half_extent, -half_extent);
    glVertex3f(-half_extent, -half_extent, half_extent);
    glVertex3f(-half_extent, half_extent, half_extent);
    glVertex3f(-half_extent, half_extent, -half_extent);

    glColor3f(0.98f, 0.78f, 0.18f);
    glVertex3f(half_extent, -half_extent, -half_extent);
    glVertex3f(half_extent, half_extent, -half_extent);
    glVertex3f(half_extent, half_extent, half_extent);
    glVertex3f(half_extent, -half_extent, half_extent);

    glColor3f(0.88f, 0.30f, 0.95f);
    glVertex3f(-half_extent, half_extent, -half_extent);
    glVertex3f(-half_extent, half_extent, half_extent);
    glVertex3f(half_extent, half_extent, half_extent);
    glVertex3f(half_extent, half_extent, -half_extent);

    glColor3f(0.18f, 0.86f, 0.90f);
    glVertex3f(-half_extent, -half_extent, -half_extent);
    glVertex3f(half_extent, -half_extent, -half_extent);
    glVertex3f(half_extent, -half_extent, half_extent);
    glVertex3f(-half_extent, -half_extent, half_extent);

    glEnd();
}

static void render_frame(const struct app_state *state, float angle_deg, float clear_phase)
{
    float r = 0.15f + 0.10f * (sinf(clear_phase) * 0.5f + 0.5f);
    float g = 0.12f + 0.10f * (cosf(clear_phase * 0.8f) * 0.5f + 0.5f);
    float b = 0.18f + 0.10f * (sinf(clear_phase * 0.6f + 1.2f) * 0.5f + 0.5f);
    float orbit = angle_deg * 1.9f;

    glViewport(0, 0, state->width > 0 ? state->width : 1, state->height > 0 ? state->height : 1);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    setup_perspective(state);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, -0.20f, -7.2f);
    glRotatef(16.0f, 1.0f, 0.0f, 0.0f);

    draw_ground(10.0f, 1.0f);

    glPushMatrix();
    glTranslatef(0.0f, 0.35f, 0.0f);
    glRotatef(angle_deg * 0.85f, 0.0f, 1.0f, 0.0f);
    glRotatef(angle_deg * 0.45f, 1.0f, 0.0f, 0.0f);
    draw_cube(1.15f);
    glPopMatrix();

    glPushMatrix();
    glTranslatef(cosf(orbit * (float)(M_PI / 180.0)) * 2.4f,
                 0.05f + sinf(clear_phase * 1.6f) * 0.35f,
                 sinf(orbit * (float)(M_PI / 180.0)) * 1.4f);
    glRotatef(-angle_deg * 1.8f, 1.0f, 1.0f, 0.0f);
    draw_cube(0.42f);
    glPopMatrix();

    draw_fps_overlay(state);

    (void)SwapBuffers(state->hdc);
}

static void parse_args(struct app_state *state, int argc, char **argv)
{
    int i;

    state->duration_ms = 20000;
    state->loop_forever = FALSE;

    for (i = 1; i < argc; ++i)
    {
        if (!lstrcmpiA(argv[i], "--loop"))
        {
            state->loop_forever = TRUE;
        }
        else if (!lstrcmpiA(argv[i], "--seconds") && i + 1 < argc)
        {
            int seconds = atoi(argv[++i]);
            if (seconds > 0) state->duration_ms = (DWORD)seconds * 1000;
        }
    }
}

int main(int argc, char **argv)
{
    struct app_state state = {0};
    char reason[1024] = {0};
    MSG msg;
    LARGE_INTEGER start, now, last_fps;
    unsigned int frames = 0;
    unsigned int last_report_frames = 0;
    BOOL forced_continue = FALSE;

    parse_args(&state, argc, argv);
    load_graphics_env(&state);
    if (!QueryPerformanceFrequency(&state.freq) || !state.freq.QuadPart)
    {
        fprintf(stderr, "winehua_graphics_smoke: QueryPerformanceFrequency failed\n");
        return 1;
    }

    if (!validate_graphics_backend(&state, reason, sizeof(reason)))
    {
        fprintf(stderr, "winehua_graphics_smoke: %s\n", reason);
        MessageBoxA(NULL, reason, "WineHua Graphics Smoke", MB_OK | MB_ICONWARNING);
        return 4;
    }
    if (state.force_gl && reason[0])
    {
        forced_continue = TRUE;
        fprintf(stderr, "winehua_graphics_smoke: forced diagnostics mode: %s\n", reason);
    }

    if (!init_window(&state)) return 2;
    if (forced_continue)
        fprintf(stderr, "winehua_graphics_smoke: continuing past readiness gate without modal dialog\n");
    if (!init_opengl(&state))
    {
        snprintf(reason, sizeof(reason),
                 "OpenGL initialization failed.\n\nrequested=%s\nactive=%s\nready=%s\nguest_gfx_ready=%s\nguest_gfx_mode=%s\nsocket_ready=%s\nlibrary_ready=%s\ntransport=%s\nforce_gl=%d\n\nnote=%s\n\n"
                 "ChoosePixelFormat / WGL initialization is not working in the current build.",
                 state.requested_backend ? state.requested_backend : "?",
                 state.active_backend ? state.active_backend : "?",
                 state.virgl_ready ? state.virgl_ready : "?",
                 state.guest_gfx_ready ? state.guest_gfx_ready : "?",
                 state.guest_gfx_mode ? state.guest_gfx_mode : "?",
                 state.virgl_socket_ready ? state.virgl_socket_ready : "?",
                 state.virgl_library_ready ? state.virgl_library_ready : "?",
                 state.frame_transport ? state.frame_transport : "?",
                 state.force_gl ? 1 : 0,
                 state.graphics_note ? state.graphics_note : "(none)");
        MessageBoxA(state.hwnd, reason, "WineHua Graphics Smoke", MB_OK | MB_ICONERROR);
        return 3;
    }

    state.running = TRUE;
    state.current_fps = 0.0;
    QueryPerformanceCounter(&start);
    last_fps = start;
    set_window_title_once(&state);
    fprintf(stderr, "winehua_graphics_smoke: running%s\n",
            state.loop_forever ? " (loop mode)" : "");

    while (state.running)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) state.running = FALSE;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        QueryPerformanceCounter(&now);
        if (!state.loop_forever)
        {
            LONGLONG elapsed_ms = (now.QuadPart - start.QuadPart) * 1000 / state.freq.QuadPart;
            if (elapsed_ms >= state.duration_ms) break;
        }

        {
            double elapsed_seconds = (double)(now.QuadPart - start.QuadPart) / (double)state.freq.QuadPart;
            float angle = (float)(elapsed_seconds * 72.0);
            float phase = (float)(elapsed_seconds * 1.8);

            render_frame(&state, angle, phase);
        }

        frames++;
        if (now.QuadPart - last_fps.QuadPart >= state.freq.QuadPart)
        {
            double report_seconds = (double)(now.QuadPart - last_fps.QuadPart) / (double)state.freq.QuadPart;
            unsigned int report_frames = frames - last_report_frames;
            double fps = report_seconds > 0.0 ? report_frames / report_seconds : 0.0;

            state.current_fps = fps;
            fprintf(stderr, "winehua_graphics_smoke: fps=%.2f size=%dx%d frames=%u\n",
                    fps, state.width, state.height, frames);
            last_fps = now;
            last_report_frames = frames;
        }

        Sleep(1);
    }

    fprintf(stderr, "winehua_graphics_smoke: finished after %u frames\n", frames);
    shutdown_opengl(&state);
    if (state.hwnd) DestroyWindow(state.hwnd);
    return 0;
}
