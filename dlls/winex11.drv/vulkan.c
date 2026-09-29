/* X11DRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
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

/* NOTE: If making changes here, consider whether they should be reflected in
 * the other drivers. */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <dlfcn.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"

#include "wine/debug.h"
#include "x11drv.h"
#include "xcomposite.h"

#define WINE_VULKAN_NO_X11_TYPES
#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"
#include "wine/winehua_vulkan.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static const struct vulkan_driver_funcs x11drv_vulkan_driver_funcs;

static VkResult X11DRV_vulkan_surface_create( HWND hwnd, const struct vulkan_instance *instance, VkSurfaceKHR *handle,
                                              struct client_surface **client )
{
    VkXlibSurfaceCreateInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
        .dpy = gdi_display,
    };

    TRACE( "%p %p %p %p\n", hwnd, instance, handle, client );

    if (winehua_vulkan_present_enabled())
    {
        /* Private present on the X route: no native Xlib surface is created.
         * The guest renders into Venus images and presents them to the host
         * tagged with the window's managed toplevel id; win32u derives
         * surface->winehua_surface_id from the handle's low 32 bits and the
         * host maps that id back to its wlr_xwayland_surface (see
         * display_route_surface_for_xwindow()).  The client surface is still
         * created: win32u calls client_surface_update()/present() around every
         * private present, exactly as on the Wayland route. */
        Window xwindow = X11DRV_get_whole_window( hwnd );

        if (!xwindow || xwindow == root_window)
        {
            ERR("No managed toplevel for hwnd %p (xwindow %lx), private present surface unavailable\n",
                hwnd, xwindow);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        if (!x11drv_client_surface_create( hwnd, 0, client )) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *handle = (VkSurfaceKHR)(uintptr_t)(WINEHUA_VULKAN_SURFACE_TAG | (uint64_t)xwindow);

        TRACE( "Created private WineHua surface=0x%s (xwindow %lx), client %s\n",
               wine_dbgstr_longlong( *handle ), xwindow, debugstr_client_surface( *client ) );
        return VK_SUCCESS;
    }

    if (!(info.window = x11drv_client_surface_create( hwnd, 0, client ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (instance->p_vkCreateXlibSurfaceKHR( instance->host.instance, &info, NULL /* allocator */, handle ))
    {
        ERR("Failed to create Xlib surface\n");
        client_surface_release( *client );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    TRACE( "Created surface 0x%s, client %s\n", wine_dbgstr_longlong( *handle ), debugstr_client_surface( *client ) );
    return VK_SUCCESS;
}

static VkBool32 X11DRV_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t index )
{
    struct vulkan_instance *instance = physical_device->instance;
    TRACE( "%p %u\n", physical_device, index );

    /* 私有 present 下宿主 WSI 一栏不成立: 呈现由 Venus/Broker 私有链路完成,
     * DXVK 接受的每条 graphics 队列从 Win32 应用看来都可 present。查宿主
     * Xlib 表达会命中宿主侧没有的入口 (X 路线宿主是 OHOS 合成器, 不是 X)。 */
    if (winehua_vulkan_present_enabled()) return VK_TRUE;

    return instance->p_vkGetPhysicalDeviceXlibPresentationSupportKHR( physical_device->host.physical_device, index, gdi_display,
                                                                      default_visual.visual->visualid );
}

static void X11DRV_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    /* 私有 present: 只对外声明 win32 surface —— guest 拿到的不是原生 WSI 面
     * (低 32 位是 X 顶层窗 id), 声明 xlib 会让应用去创建宿主不存在的面。 */
    if (winehua_vulkan_present_enabled())
    {
        if (extensions->has_VK_KHR_surface) extensions->has_VK_KHR_win32_surface = 1;
        return;
    }

    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_xlib_surface = 1;
    if (extensions->has_VK_KHR_xlib_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void X11DRV_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    /* 私有 present: swapchain 由 win32u 的 Venus 镜像路径兑现, 不向宿主
     * vkCreateDevice 传任何 WSI 扩展 (镜像 winewayland 实现)。 */
    if (winehua_vulkan_present_enabled())
    {
        extensions->has_VK_KHR_swapchain = 1;
        return;
    }

    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs x11drv_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = X11DRV_vulkan_surface_create,
    .p_get_physical_device_presentation_support = X11DRV_get_physical_device_presentation_support,
    .p_map_instance_extensions = X11DRV_map_instance_extensions,
    .p_map_device_extensions = X11DRV_map_device_extensions,
};

UINT X11DRV_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }

    *driver_funcs = &x11drv_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
