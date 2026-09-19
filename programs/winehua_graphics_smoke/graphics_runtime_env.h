#ifndef WINEHUA_SHARED_GRAPHICS_RUNTIME_ENV_H
#define WINEHUA_SHARED_GRAPHICS_RUNTIME_ENV_H

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINEHUA_GRAPHICS_ENV_BACKEND "WINEHUA_GRAPHICS_BACKEND"
#define WINEHUA_GRAPHICS_ENV_ACTIVE "WINEHUA_GRAPHICS_ACTIVE"
#define WINEHUA_GRAPHICS_ENV_SHM_FALLBACK "WINEHUA_SHM_FALLBACK"
#define WINEHUA_GRAPHICS_ENV_FRAME_ZERO_COPY "WINEHUA_FRAME_ZERO_COPY"
#define WINEHUA_GRAPHICS_ENV_FRAME_TRANSPORT "WINEHUA_FRAME_TRANSPORT"
#define WINEHUA_GRAPHICS_ENV_GUEST_GFX_READY "WINEHUA_GUEST_GFX_READY"
#define WINEHUA_GRAPHICS_ENV_GUEST_GFX_MODE "WINEHUA_GUEST_GFX_MODE"
#define WINEHUA_GRAPHICS_ENV_GUEST_GFX_DIR "WINEHUA_GUEST_GFX_DIR"
#define WINEHUA_GRAPHICS_ENV_VIRGL_SOCKET_READY "WINEHUA_VIRGL_SOCKET_READY"
#define WINEHUA_GRAPHICS_ENV_VIRGL_LIBRARY_READY "WINEHUA_VIRGL_LIBRARY_READY"
#define WINEHUA_GRAPHICS_ENV_VIRGL_SOCKET "WINEHUA_VIRGL_SOCKET"
#define WINEHUA_GRAPHICS_ENV_VIRGL_RENDERER_LIB "WINEHUA_VIRGLRENDERER_LIB"
#define WINEHUA_GRAPHICS_ENV_VIRGL_READY "WINEHUA_VIRGL_READY"
#define WINEHUA_GRAPHICS_ENV_NOTE "WINEHUA_GRAPHICS_NOTE"
#define WINEHUA_GRAPHICS_ENV_FORCE_GL "WINEHUA_GRAPHICS_FORCE_GL"
#define WINEHUA_GRAPHICS_ENV_VTEST_SOCKET "VTEST_SOCKET_NAME"

typedef struct WinehuaGraphicsRuntimeEnv
{
    const char *requested_backend;
    const char *active_backend;
    const char *shm_fallback;
    const char *frame_zero_copy;
    const char *frame_transport;
    const char *guest_gfx_ready;
    const char *guest_gfx_mode;
    const char *guest_gfx_dir;
    const char *virgl_socket_ready;
    const char *virgl_library_ready;
    const char *virgl_socket;
    const char *virgl_renderer_lib;
    const char *virgl_ready;
    const char *graphics_note;
    int force_gl;
} WinehuaGraphicsRuntimeEnv;

static inline const char *winehua_graphics_env_or(const char *value, const char *fallback)
{
    return (value && value[0]) ? value : fallback;
}

static inline int winehua_graphics_env_is_one(const char *value)
{
    return value && !strcmp(value, "1");
}

static inline void winehua_graphics_runtime_env_load(WinehuaGraphicsRuntimeEnv *env)
{
    const char *force_gl;

    if (!env) return;
    memset(env, 0, sizeof(*env));

    env->requested_backend = getenv(WINEHUA_GRAPHICS_ENV_BACKEND);
    env->active_backend = getenv(WINEHUA_GRAPHICS_ENV_ACTIVE);
    env->shm_fallback = getenv(WINEHUA_GRAPHICS_ENV_SHM_FALLBACK);
    env->frame_zero_copy = getenv(WINEHUA_GRAPHICS_ENV_FRAME_ZERO_COPY);
    env->frame_transport = getenv(WINEHUA_GRAPHICS_ENV_FRAME_TRANSPORT);
    env->guest_gfx_ready = getenv(WINEHUA_GRAPHICS_ENV_GUEST_GFX_READY);
    env->guest_gfx_mode = getenv(WINEHUA_GRAPHICS_ENV_GUEST_GFX_MODE);
    env->guest_gfx_dir = getenv(WINEHUA_GRAPHICS_ENV_GUEST_GFX_DIR);
    env->virgl_socket_ready = getenv(WINEHUA_GRAPHICS_ENV_VIRGL_SOCKET_READY);
    env->virgl_library_ready = getenv(WINEHUA_GRAPHICS_ENV_VIRGL_LIBRARY_READY);
    env->virgl_socket = getenv(WINEHUA_GRAPHICS_ENV_VIRGL_SOCKET);
    env->virgl_renderer_lib = getenv(WINEHUA_GRAPHICS_ENV_VIRGL_RENDERER_LIB);
    env->virgl_ready = getenv(WINEHUA_GRAPHICS_ENV_VIRGL_READY);
    env->graphics_note = getenv(WINEHUA_GRAPHICS_ENV_NOTE);
    force_gl = getenv(WINEHUA_GRAPHICS_ENV_FORCE_GL);
    env->force_gl = winehua_graphics_env_is_one(force_gl);
}

#ifdef __cplusplus
}
#endif

#endif