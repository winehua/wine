/*
 * WineHua private present surface channel — id page + readiness probe.
 *
 * Mechanism only; the per-driver present policy (which id, when to publish,
 * how to report degradation) stays in the drivers.  See
 * include/wine/winehua_present.h for the contract with guest Mesa.
 *
 * Copyright 2025-2026 WineHua contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"

#include "wine/winehua_present.h"

/* 契约: 与 guest mesa 的 vtest winsys 逐字节一致 (magic/version/字段序)。
 * 改这里就必须同步 thirdparty/mesa/src/gallium/winsys/virgl/vtest/
 * virgl_vtest_socket.c 的 winehua_present_surface_page。 */
struct winehua_present_surface_page
{
    uint32_t magic;
    uint32_t version;
    uint32_t surface_id;
    uint32_t reserved;
};

static pthread_once_t winehua_present_surface_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t winehua_present_surface_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct winehua_present_surface_page *winehua_present_surface_page;
static char winehua_present_page_path[256];   /* 进程退出时删除 (见下) */
static char winehua_present_ready_dir[192];   /* 读一次的缓存, 见 ready() */

/* 页文件是每进程一个 (名字带 pid), 不删就在沙箱 temp 里逐次累积; 退出时清掉。
 * SIGKILL 下不会执行 —— 残留页由下一次启动的同 pid/新 pid 覆盖或忽略, 无害。 */
static void winehua_present_page_cleanup(void)
{
    if (winehua_present_page_path[0])
        unlink(winehua_present_page_path);
}

static void winehua_init_present_surface_page(void)
{
    const char *tmp_dir = getenv("TMPDIR");
    struct winehua_present_surface_page *page;
    char path[256];
    int fd;

    if (!tmp_dir || !tmp_dir[0] ||
        snprintf(path, sizeof(path), "%s/winehua_present_surface_%u.shm",
                 tmp_dir, (uint32_t)getpid()) >= sizeof(path))
        return;
    if ((fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600)) < 0)
        return;
    if (ftruncate(fd, sizeof(*page)) ||
        (page = mmap(NULL, sizeof(*page), PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0)) == MAP_FAILED)
    {
        close(fd);
        return;
    }
    close(fd);

    page->version = WINEHUA_PRESENT_SURFACE_VERSION;
    page->reserved = 0;
    __atomic_store_n(&page->surface_id, 0, __ATOMIC_RELAXED);
    /* magic last: 写它之前 consumer 读到的是无效页, 不会当半初始化页用 */
    __atomic_store_n(&page->magic, WINEHUA_PRESENT_SURFACE_MAGIC, __ATOMIC_RELEASE);
    winehua_present_surface_page = page;
    snprintf(winehua_present_page_path, sizeof(winehua_present_page_path), "%s", path);
    atexit(winehua_present_page_cleanup);
}

W32KAPI void winehua_present_surface_init(void)
{
    pthread_once(&winehua_present_surface_once, winehua_init_present_surface_page);
}

/* 呈现期发布: begin 与 end 之间的 swap 会被 mesa 认成"这一帧的目标是 surface_id"。
 * 互斥保护的是页面指针生命周期与多线程 swap (同一进程内多个 GL 线程)。 */
W32KAPI void winehua_present_surface_begin(uint32_t surface_id)
{
    winehua_present_surface_init();
    pthread_mutex_lock(&winehua_present_surface_mutex);
    if (winehua_present_surface_page)
        __atomic_store_n(&winehua_present_surface_page->surface_id,
                         surface_id, __ATOMIC_RELEASE);
}

W32KAPI void winehua_present_surface_end(void)
{
    if (winehua_present_surface_page)
        __atomic_store_n(&winehua_present_surface_page->surface_id,
                         0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&winehua_present_surface_mutex);
}

static pthread_once_t winehua_present_ready_once = PTHREAD_ONCE_INIT;

static void winehua_init_ready_dir(void)
{
    const char *dir = getenv("WINEHUA_ZERO_COPY_READY_DIR");

    if (dir && dir[0])
        snprintf(winehua_present_ready_dir, sizeof(winehua_present_ready_dir), "%s", dir);
}

/* 宿主挂接握手: 宿主给这个 (pid, surface_id) 挂好目标后会写 ready 标记
 * (宿主侧 graphics_broker.cpp SetZeroCopySurfaceReady)。未挂接时 present 会被
 * 宿主丢弃 (kPresentNoTarget) —— 驱动据此报降级, 不静默。 */
W32KAPI BOOL winehua_present_surface_ready(uint32_t surface_id)
{
    char path[256];
    uint64_t surface_key;

    /* 目录读一次就够 (它在进程生命周期内不变): 本函数每次 swap 都被调用
     * (117fps ⇒ 每秒百余次), 原来每次都 getenv —— 既有开销, 又在并发 setenv
     * 下不线程安全 (调用点在渲染线程上)。getenv 结果缓存到 pthread_once。 */
    pthread_once(&winehua_present_ready_once, winehua_init_ready_dir);
    if (!surface_id || !winehua_present_ready_dir[0]) return FALSE;
    surface_key = ((uint64_t)(uint32_t)getpid() << 32) | surface_id;
    if (snprintf(path, sizeof(path), "%s/winehua_zc_surface_%llu.ready",
                 winehua_present_ready_dir, (unsigned long long)surface_key) >= sizeof(path))
        return FALSE;
    return access(path, F_OK) == 0;
}

W32KAPI BOOL winehua_present_surface_mapped(void)
{
    winehua_present_surface_init();
    return winehua_present_surface_page != NULL;
}
