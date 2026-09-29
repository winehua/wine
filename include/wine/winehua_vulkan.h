/*
 * WineHua private Vulkan present — capability probe (shared by all drivers).
 *
 * WineHua never hands the guest a native host VkSurfaceKHR: the guest gets a
 * tagged handle whose low 32 bits carry a surface id that the host presenter
 * resolves to one of its own surfaces.  The id is route specific:
 *
 *   Wayland route: Wayland proxy id of the window's wl_surface
 *                  (dlls/winewayland.drv/vulkan.c)
 *   X route:       X window id of the window's managed toplevel
 *                  (dlls/winex11.drv/vulkan.c)
 *
 * The predicate lives here, in one place, because it is a capability answer
 * that three callers must agree on (dlls/win32u/vulkan.c, winex11, winewayland
 * all branch on it); three private copies is exactly the setup where one of
 * them silently drifts and the others keep presenting into nothing.
 */

#ifndef __WINE_WINEHUA_VULKAN_H
#define __WINE_WINEHUA_VULKAN_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WINEHUA_VULKAN_SURFACE_TAG UINT64_C(0x5748530000000000)

static inline BOOL winehua_vulkan_present_enabled(void)
{
    const char *value = getenv( "WINEHUA_VULKAN_PRESENT" );

    if (value && value[0] && strcmp( value, "0" )) return TRUE;

    value = getenv( "WINEHUA_PRESENT_BACKEND" );
    if (value && (!strcmp( value, "venus_broker_present" ) ||
                  !strcmp( value, "venus_direct_present" ))) return TRUE;

    /* Children launched through the NCP/Box64 boundary can retain the stable
     * VirGL marker even when the per-launch present variables are not
     * serialized by Wine's Windows environment.  On this product path a VirGL
     * guest is necessarily paired with the private Venus presenter, so the
     * marker is used as a final capability fallback. */
    value = getenv( "WINEHUA_GRAPHICS_BACKEND" );
    if (value && !strcmp( value, "virgl" )) return TRUE;

    return FALSE;
}

#endif /* __WINE_WINEHUA_VULKAN_H */
