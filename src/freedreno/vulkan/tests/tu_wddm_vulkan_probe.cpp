/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * A small ARM64 Windows runtime probe for the app-local Turnip ICD bundle.
 * It intentionally uses the ICD entrypoint instead of the system Vulkan
 * loader, so it can run before registry installation.  It only proves that
 * the KMT-backed instance/device lifecycle can be opened; it does not submit
 * GPU work or claim a successful render workload.
 */

#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include <windows.h>

static int
fail(const char *message, VkResult result)
{
   if (result == VK_SUCCESS)
      fprintf(stderr, "tu WDDM Vulkan probe: %s\n", message);
   else
      fprintf(stderr, "tu WDDM Vulkan probe: %s (VkResult=%d)\n", message, result);
   return 1;
}

static bool
has_instance_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name)
{
   for (uint32_t i = 0; i < count; i++) {
      if (strcmp(extensions[i].extensionName, name) == 0)
         return true;
   }
   return false;
}

int
main()
{
   HMODULE icd = LoadLibraryW(L"vulkan_freedreno.dll");
   if (icd == NULL) {
      fprintf(stderr, "tu WDDM Vulkan probe: LoadLibraryW failed (%lu)\n", static_cast<unsigned long>(GetLastError()));
      return 1;
   }

   PFN_vkGetInstanceProcAddr get_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
   if (get_proc == NULL) {
      FreeLibrary(icd);
      return fail("vk_icdGetInstanceProcAddr is missing", VK_SUCCESS);
   }

   PFN_vkEnumerateInstanceExtensionProperties enumerate_extensions =
      reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
         get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
   if (enumerate_extensions == NULL) {
      FreeLibrary(icd);
      return fail("vkEnumerateInstanceExtensionProperties is missing", VK_SUCCESS);
   }

   uint32_t extension_count = 0;
   VkResult result = enumerate_extensions(NULL, &extension_count, NULL);
   if (result != VK_SUCCESS || extension_count == 0 || extension_count > 256) {
      FreeLibrary(icd);
      return fail("instance extension enumeration failed", result);
   }

   VkExtensionProperties extensions[256] = {};
   result = enumerate_extensions(NULL, &extension_count, extensions);
   if (result != VK_SUCCESS || !has_instance_extension(extensions, extension_count, "VK_KHR_surface") ||
       !has_instance_extension(extensions, extension_count, "VK_KHR_win32_surface")) {
      FreeLibrary(icd);
      return fail("required Win32 surface extensions are unavailable", result);
   }

   PFN_vkCreateInstance create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
   if (create_instance == NULL) {
      FreeLibrary(icd);
      return fail("vkCreateInstance is missing", VK_SUCCESS);
   }

   VkApplicationInfo application_info = {
      VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "DroidVM Turnip WDDM probe", 1, "DroidVM", 1, VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo instance_info = {
      VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &application_info, 0, NULL, 0, NULL,
   };
   VkInstance instance = VK_NULL_HANDLE;
   result = create_instance(&instance_info, NULL, &instance);
   if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
      FreeLibrary(icd);
      return fail("vkCreateInstance failed", result);
   }

   PFN_vkEnumeratePhysicalDevices enumerate_physical_devices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_proc(instance, "vkEnumeratePhysicalDevices"));
   PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get_proc(instance, "vkGetPhysicalDeviceProperties2"));
   PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
         get_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
   PFN_vkCreateDevice create_device = reinterpret_cast<PFN_vkCreateDevice>(get_proc(instance, "vkCreateDevice"));
   PFN_vkDestroyInstance destroy_instance =
      reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
   if (enumerate_physical_devices == NULL || get_physical_device_properties2 == NULL ||
       get_queue_family_properties == NULL || create_device == NULL || destroy_instance == NULL) {
      destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
      if (destroy_instance != NULL)
         destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("required instance entrypoints are missing", VK_SUCCESS);
   }

   uint32_t physical_device_count = 0;
   result = enumerate_physical_devices(instance, &physical_device_count, NULL);
   if (result != VK_SUCCESS || physical_device_count == 0 || physical_device_count > 16) {
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("physical-device enumeration failed", result);
   }

   VkPhysicalDevice physical_devices[16] = {};
   result = enumerate_physical_devices(instance, &physical_device_count, physical_devices);
   if (result != VK_SUCCESS || physical_device_count == 0) {
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("physical-device enumeration returned no device", result);
   }

   VkPhysicalDeviceIDProperties id_properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
      NULL,
   };
   VkPhysicalDeviceProperties2 properties = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      &id_properties,
   };
   get_physical_device_properties2(physical_devices[0], &properties);
   if (!id_properties.deviceLUIDValid || id_properties.deviceNodeMask != 1) {
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("WDDM adapter LUID properties are invalid", VK_SUCCESS);
   }

   uint32_t queue_family_count = 0;
   get_queue_family_properties(physical_devices[0], &queue_family_count, NULL);
   if (queue_family_count == 0 || queue_family_count > 16) {
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("queue-family enumeration failed", VK_SUCCESS);
   }
   VkQueueFamilyProperties queue_families[16] = {};
   get_queue_family_properties(physical_devices[0], &queue_family_count, queue_families);

   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_family_count; i++) {
      if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      for (uint32_t i = 0; i < queue_family_count; i++) {
         if ((queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0) {
            queue_family = i;
            break;
         }
      }
   }
   if (queue_family == UINT32_MAX || queue_families[queue_family].queueCount == 0) {
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("no usable graphics/compute queue family", VK_SUCCESS);
   }

   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, queue_family, 1, &priority,
   };
   VkDeviceCreateInfo device_info = {
      VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &queue_info, 0, NULL, 0, NULL, NULL,
   };
   VkDevice device = VK_NULL_HANDLE;
   result = create_device(physical_devices[0], &device_info, NULL, &device);
   if (result != VK_SUCCESS || device == VK_NULL_HANDLE) {
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("vkCreateDevice failed", result);
   }

   PFN_vkGetDeviceProcAddr get_device_proc =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_proc(instance, "vkGetDeviceProcAddr"));
   if (get_device_proc == NULL) {
      PFN_vkDestroyDevice destroy_device = reinterpret_cast<PFN_vkDestroyDevice>(get_proc(instance, "vkDestroyDevice"));
      if (destroy_device != NULL)
         destroy_device(device, NULL);
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("vkGetDeviceProcAddr is missing", VK_SUCCESS);
   }
   PFN_vkDestroyDevice destroy_device =
      reinterpret_cast<PFN_vkDestroyDevice>(get_device_proc(device, "vkDestroyDevice"));
   if (destroy_device == NULL) {
      PFN_vkDestroyDevice fallback_destroy_device =
         reinterpret_cast<PFN_vkDestroyDevice>(get_proc(instance, "vkDestroyDevice"));
      if (fallback_destroy_device != NULL)
         fallback_destroy_device(device, NULL);
      destroy_instance(instance, NULL);
      FreeLibrary(icd);
      return fail("vkDestroyDevice is missing", VK_SUCCESS);
   }
   destroy_device(device, NULL);
   destroy_instance(instance, NULL);
   FreeLibrary(icd);

   printf("tu WDDM Vulkan probe passed: %s, LUID-valid node mask %u, queue family %u\n",
          properties.properties.deviceName, id_properties.deviceNodeMask, queue_family);
   return 0;
}
