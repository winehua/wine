/*
 * WineHua private present surface channel — id publication + readiness probe
 * (shared by the user drivers that present frames to the host presenter).
 *
 * The guest driver publishes, for the duration of one swap, the id of the
 * surface it is presenting.  The guest Mesa VirGL/vtest winsys reads that id
 * while emitting the present command (virgl_vtest_winsys.c
 * virgl_vtest_flush_frontbuffer -> winehua_vtest_get_present_surface_id) and
 * ships it to the host, which resolves it to one of its own present targets.
 * The id is route specific:
 *
 *   Wayland route: Wayland proxy id of the window's wl_surface
 *                  (dlls/winewayland.drv/opengl_readback.c)
 *   X route:       X window id of the window's managed toplevel
 *                  (dlls/winex11.drv/opengl_winehua.c)
 *
 * Publication is unconditional (not gated on readiness): the host learns about
 * a surface *from* its presents — a driver that only published once the host
 * had attached a target would never be discovered.  Readiness is therefore a
 * separate probe, used by drivers to report "host never attached" degradation
 * instead of dropping frames silently.
 *
 * The page format is a wire contract with Mesa (the consumer): magic/version
 * must match thirdparty/mesa virgl vtest.  Header here, implementation in
 * dlls/win32u/winehua_present.c, so both drivers share one copy.
 */

#ifndef __WINE_WINEHUA_PRESENT_H
#define __WINE_WINEHUA_PRESENT_H

#include <stdint.h>

#include "ntuser.h" /* W32KAPI + BOOL (实现只在 win32u, 驱动侧是导入声明) */

#define WINEHUA_PRESENT_SURFACE_MAGIC 0x57535053u
#define WINEHUA_PRESENT_SURFACE_VERSION 1u

/* Binds the per-process page (idempotent).  Drivers that know a swap is coming
 * can call this early; begin() does it too. */
W32KAPI void winehua_present_surface_init(void);
/* Publish / retract the id around the swap that presents that surface. */
W32KAPI void winehua_present_surface_begin(uint32_t surface_id);
W32KAPI void winehua_present_surface_end(void);
/* Host side handshake for this process' surface: ready marker (see
 * WINEHUA_ZERO_COPY_READY_DIR) present ⇒ the host has a target attached. */
W32KAPI BOOL winehua_present_surface_ready(uint32_t surface_id);
/* Whether this process' page is mapped at all (TMPDIR missing / mmap failed =
 * ids never reach the host).  Diagnostics only: it distinguishes "channel
 * broken" from "host has not attached yet". */
W32KAPI BOOL winehua_present_surface_mapped(void);

#endif /* __WINE_WINEHUA_PRESENT_H */
