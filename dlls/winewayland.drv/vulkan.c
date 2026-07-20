/* WAYLANDDRV Vulkan implementation
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2021 Alexandros Frantzis
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "waylanddrv.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

static const struct vulkan_driver_funcs wayland_vulkan_driver_funcs;

/* WineHua keeps Vulkan WSI private: the Guest never receives a native
 * VkSurfaceKHR. The tagged value carries only the Wayland proxy id, which
 * is later paired with the client pid by the Host presenter. */
#define WINEHUA_VULKAN_SURFACE_TAG UINT64_C(0x5748530000000000)

static BOOL winehua_vulkan_present_enabled(void)
{
    const char *value = getenv("WINEHUA_VULKAN_PRESENT");
    if (value && value[0] && strcmp(value, "0")) return TRUE;
    value = getenv("WINEHUA_PRESENT_BACKEND");
    if (value && (!strcmp(value, "venus_broker_present") ||
                  !strcmp(value, "venus_direct_present"))) return TRUE;
    /* See win32u/vulkan.c.  Child processes can retain the stable VirGL
     * marker even when per-launch present variables are not forwarded. */
    value = getenv("WINEHUA_GRAPHICS_BACKEND");
    if (value && !strcmp(value, "virgl"))
    {
        TRACE("WineHua: enabling private Wayland/Vulkan present from VirGL runtime marker\n");
        return TRUE;
    }
    return FALSE;
}

static VkResult wayland_vulkan_surface_create(HWND hwnd, const struct vulkan_instance *instance, VkSurfaceKHR *handle,
                                              struct client_surface **client)
{
    VkResult res;
    VkWaylandSurfaceCreateInfoKHR create_info_host;
    struct wayland_client_surface *surface;

    TRACE("%p %p %p %p\n", hwnd, instance, handle, client);

    if (winehua_vulkan_present_enabled())
    {
        if (!(surface = wayland_client_surface_create(hwnd))) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *handle = (VkSurfaceKHR)(uintptr_t)(WINEHUA_VULKAN_SURFACE_TAG |
                                            (uint64_t)wl_proxy_get_id((struct wl_proxy *)surface->wl_surface));
        set_client_surface(hwnd, surface);
        *client = &surface->client;
        TRACE("Created private WineHua surface=0x%s, client=%p\n",
              wine_dbgstr_longlong(*handle), *client);
        return VK_SUCCESS;
    }

    if (!(surface = wayland_client_surface_create(hwnd))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    create_info_host.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0; /* reserved */
    create_info_host.display = process_wayland.wl_display;
    create_info_host.surface = surface->wl_surface;

    res = instance->p_vkCreateWaylandSurfaceKHR(instance->host.instance, &create_info_host, NULL /* allocator */, handle);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create vulkan wayland surface, res=%d\n", res);
        client_surface_release(&surface->client);
        return res;
    }

    set_client_surface(hwnd, surface);
    *client = &surface->client;

    TRACE("Created surface=0x%s, client=%p\n", wine_dbgstr_longlong(*handle), *client);
    return VK_SUCCESS;
}

static VkBool32 wayland_get_physical_device_presentation_support(struct vulkan_physical_device *physical_device,
                                                                 uint32_t index)
{
    struct vulkan_instance *instance = physical_device->instance;

    TRACE("%p %u\n", physical_device, index);

    /* The WineHua surface is not a Host Wayland WSI object.  Presentation is
     * performed by the private Venus/Broker path, and every graphics queue
     * accepted by DXVK is present-capable from the Win32 application's point
     * of view.  Calling the Host Wayland entry here would dereference a NULL
     * function pointer because Harmony exposes only OHNativeWindow WSI. */
    if (winehua_vulkan_present_enabled()) return VK_TRUE;

    return instance->p_vkGetPhysicalDeviceWaylandPresentationSupportKHR(physical_device->host.physical_device, index,
                                                                        process_wayland.wl_display);
}

static void wayland_map_instance_extensions(struct vulkan_instance_extensions *extensions)
{
    if (winehua_vulkan_present_enabled())
    {
        if (extensions->has_VK_KHR_surface) extensions->has_VK_KHR_win32_surface = 1;
        return;
    }
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_wayland_surface = 1;
    if (extensions->has_VK_KHR_wayland_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void wayland_map_device_extensions(struct vulkan_device_extensions *extensions)
{
    if (winehua_vulkan_present_enabled())
    {
        /* The private swapchain is backed by regular Venus images; no Host
         * WSI extension is passed to vkCreateDevice. */
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

static const struct vulkan_driver_funcs wayland_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = wayland_vulkan_surface_create,
    .p_get_physical_device_presentation_support = wayland_get_physical_device_presentation_support,
    .p_map_instance_extensions = wayland_map_instance_extensions,
    .p_map_device_extensions = wayland_map_device_extensions,
};

/**********************************************************************
 *           WAYLAND_VulkanInit
 */
UINT WAYLAND_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    *driver_funcs = &wayland_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
