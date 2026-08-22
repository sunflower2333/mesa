/*
 * Copyright © 2016 Red Hat
 * SPDX-License-Identifier: MIT
 *
 * based on intel anv code:
 * Copyright © 2015 Intel Corporation
 */

#include "tu_wsi.h"

#ifdef HAVE_LIBDRM
#include "drm-uapi/drm_fourcc.h"
#include "wsi_common_drm.h"
#endif

#include "vk_util.h"

#include "tu_device.h"

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
tu_wsi_proc_addr(VkPhysicalDevice physicalDevice, const char *pName)
{
   VK_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);
   return vk_instance_get_proc_addr_unchecked(&pdevice->instance->vk, pName);
}

static bool
tu_wsi_can_present_on_device(VkPhysicalDevice physicalDevice, int fd)
{
#if defined(VK_USE_PLATFORM_WIN32_KHR) && defined(TU_HAS_WDDM)
   /* The WDDM path presents through the Win32 WSI CPU/GDI backend and has no
    * DRM file descriptor to compare with the surface. */
   (void)physicalDevice;
   (void)fd;
   return true;
#elif defined(HAVE_LIBDRM)
   VK_FROM_HANDLE(tu_physical_device, pdevice, physicalDevice);
   return wsi_common_drm_devices_equal(fd, pdevice->local_fd);
#else
   (void)physicalDevice;
   (void)fd;
   return true;
#endif
}

VkResult
tu_wsi_init(struct tu_physical_device *physical_device)
{
   VkResult result;

   const struct wsi_device_options options = {
#if defined(VK_USE_PLATFORM_WIN32_KHR) && defined(TU_HAS_WDDM)
      /* DroidVM's first Win32 WSI contract is the existing KMD CPU-copy path;
       * do not select Mesa's D3D12/DXGI swapchain backend. */
      .sw_device = true,
#else
      .sw_device = false,
#endif
   };
   result = wsi_device_init(&physical_device->wsi_device,
                            tu_physical_device_to_handle(physical_device),
                            tu_wsi_proc_addr,
                            &physical_device->instance->vk.alloc,
                            physical_device->master_fd,
                            &physical_device->instance->drirc.options,
                            &options);
   if (result != VK_SUCCESS)
      return result;

   physical_device->wsi_device.supports_modifiers =
#if defined(VK_USE_PLATFORM_WIN32_KHR) && defined(TU_HAS_WDDM)
      false;
#else
      true;
#endif
   physical_device->wsi_device.can_present_on_device =
      tu_wsi_can_present_on_device;

   physical_device->vk.wsi_device = &physical_device->wsi_device;

   return VK_SUCCESS;
}

void
tu_wsi_finish(struct tu_physical_device *physical_device)
{
   physical_device->vk.wsi_device = NULL;
   wsi_device_finish(&physical_device->wsi_device,
                     &physical_device->instance->vk.alloc);
}
