/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Win32 WSI source probe for the app-local Turnip ICD.  It intentionally
 * creates a hidden window and a CPU/GDI swapchain; it does not require a
 * system Vulkan registry entry or a D3D12/DXGI device.
 */

#define WIN32_LEAN_AND_MEAN
// Win32 Vulkan types require the platform header before vulkan_win32.h.
// clang-format off
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>
// clang-format on

namespace {

constexpr wchar_t kWindowClass[] = L"DroidVMTurnipWddmProbe";

LRESULT CALLBACK
window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
   (void) window;
   (void) message;
   (void) wparam;
   (void) lparam;
   return DefWindowProcW(window, message, wparam, lparam);
}

int
report(const char *message, VkResult result)
{
   if (result == VK_SUCCESS)
      fprintf(stderr, "tu WDDM Win32 probe: %s\n", message);
   else
      fprintf(stderr, "tu WDDM Win32 probe: %s (VkResult=%d)\n", message, result);
   return 1;
}

bool
has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name)
{
   for (uint32_t i = 0; i < count; i++) {
      if (strcmp(extensions[i].extensionName, name) == 0)
         return true;
   }
   return false;
}

} // namespace

int
main()
{
   HMODULE icd = LoadLibraryW(L"vulkan_freedreno.dll");
   if (icd == nullptr) {
      fprintf(stderr, "tu WDDM Win32 probe: LoadLibraryW failed (%lu)\n", static_cast<unsigned long>(GetLastError()));
      return 1;
   }

   VkInstance instance = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkSurfaceKHR surface = VK_NULL_HANDLE;
   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   VkFence acquire_fence = VK_NULL_HANDLE;
   HWND window = nullptr;
   HINSTANCE module = GetModuleHandleW(nullptr);
   bool class_registered = false;
   PFN_vkDestroyInstance destroy_instance = nullptr;
   PFN_vkDestroyDevice destroy_device = nullptr;
   PFN_vkDestroySurfaceKHR destroy_surface = nullptr;
   PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
   PFN_vkDestroyFence destroy_fence = nullptr;

   auto cleanup = [&]() {
      if (destroy_fence != nullptr && acquire_fence != VK_NULL_HANDLE)
         destroy_fence(device, acquire_fence, nullptr);
      if (destroy_swapchain != nullptr && swapchain != VK_NULL_HANDLE)
         destroy_swapchain(device, swapchain, nullptr);
      if (destroy_device != nullptr && device != VK_NULL_HANDLE)
         destroy_device(device, nullptr);
      if (destroy_surface != nullptr && surface != VK_NULL_HANDLE)
         destroy_surface(instance, surface, nullptr);
      if (destroy_instance != nullptr && instance != VK_NULL_HANDLE)
         destroy_instance(instance, nullptr);
      if (window != nullptr)
         DestroyWindow(window);
      if (class_registered)
         UnregisterClassW(kWindowClass, module);
      FreeLibrary(icd);
   };

   PFN_vkGetInstanceProcAddr get_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
   if (get_proc == nullptr) {
      cleanup();
      return report("vk_icdGetInstanceProcAddr is missing", VK_SUCCESS);
   }
   PFN_vkEnumerateInstanceExtensionProperties enumerate_instance_extensions =
      reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
         get_proc(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
   PFN_vkCreateInstance create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
   if (enumerate_instance_extensions == nullptr || create_instance == nullptr) {
      cleanup();
      return report("required instance entrypoints are missing", VK_SUCCESS);
   }
   uint32_t extension_count = 0;
   VkResult result = enumerate_instance_extensions(nullptr, &extension_count, nullptr);
   if (result != VK_SUCCESS || extension_count == 0 || extension_count > 256) {
      cleanup();
      return report("instance extension enumeration failed", result);
   }
   VkExtensionProperties extensions[256] = {};
   result = enumerate_instance_extensions(nullptr, &extension_count, extensions);
   if (result != VK_SUCCESS || !has_extension(extensions, extension_count, VK_KHR_SURFACE_EXTENSION_NAME) ||
       !has_extension(extensions, extension_count, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
      cleanup();
      return report("required Win32 surface extensions are unavailable", result);
   }

   const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
   VkApplicationInfo application_info = {};
   application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   application_info.pApplicationName = "DroidVM Turnip WDDM Win32 probe";
   application_info.applicationVersion = 1;
   application_info.pEngineName = "DroidVM";
   application_info.engineVersion = 1;
   application_info.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo instance_info = {};
   instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   instance_info.pApplicationInfo = &application_info;
   instance_info.enabledExtensionCount = 2;
   instance_info.ppEnabledExtensionNames = instance_extensions;
   result = create_instance(&instance_info, nullptr, &instance);
   if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
      cleanup();
      return report("vkCreateInstance failed", result);
   }
   PFN_vkEnumeratePhysicalDevices enumerate_physical_devices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_proc(instance, "vkEnumeratePhysicalDevices"));
   PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
         get_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
   PFN_vkCreateWin32SurfaceKHR create_win32_surface =
      reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(get_proc(instance, "vkCreateWin32SurfaceKHR"));
   PFN_vkDestroySurfaceKHR loaded_destroy_surface =
      reinterpret_cast<PFN_vkDestroySurfaceKHR>(get_proc(instance, "vkDestroySurfaceKHR"));
   PFN_vkGetPhysicalDeviceSurfaceSupportKHR get_surface_support =
      reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
         get_proc(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"));
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities =
      reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
         get_proc(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
   PFN_vkGetPhysicalDeviceSurfaceFormatsKHR get_surface_formats =
      reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
         get_proc(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
   PFN_vkGetPhysicalDeviceSurfacePresentModesKHR get_present_modes =
      reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
         get_proc(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
   PFN_vkCreateDevice create_device = reinterpret_cast<PFN_vkCreateDevice>(get_proc(instance, "vkCreateDevice"));
   PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
         get_proc(instance, "vkEnumerateDeviceExtensionProperties"));
   destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
   destroy_surface = loaded_destroy_surface;
   if (enumerate_physical_devices == nullptr || get_queue_family_properties == nullptr ||
       create_win32_surface == nullptr || destroy_surface == nullptr || get_surface_support == nullptr ||
       get_surface_capabilities == nullptr || get_surface_formats == nullptr || get_present_modes == nullptr ||
       create_device == nullptr || enumerate_device_extensions == nullptr || destroy_instance == nullptr) {
      cleanup();
      return report("required Win32 WSI entrypoints are missing", VK_SUCCESS);
   }

   WNDCLASSW window_class = {};
   window_class.lpfnWndProc = window_proc;
   window_class.hInstance = module;
   window_class.lpszClassName = kWindowClass;
   if (RegisterClassW(&window_class) == 0) {
      cleanup();
      return report("RegisterClassW failed", VK_SUCCESS);
   }
   class_registered = true;
   window = CreateWindowExW(0, kWindowClass, L"DroidVM Turnip WDDM probe", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, 640, 480, nullptr, nullptr, module, nullptr);
   if (window == nullptr) {
      cleanup();
      return report("CreateWindowExW failed", VK_SUCCESS);
   }
   VkWin32SurfaceCreateInfoKHR surface_info = {};
   surface_info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
   surface_info.hinstance = module;
   surface_info.hwnd = window;
   if ((result = create_win32_surface(instance, &surface_info, nullptr, &surface)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateWin32SurfaceKHR failed", result);
   }

   uint32_t physical_count = 0;
   if ((result = enumerate_physical_devices(instance, &physical_count, nullptr)) != VK_SUCCESS || physical_count == 0 ||
       physical_count > 16) {
      cleanup();
      return report("physical-device enumeration failed", result);
   }
   VkPhysicalDevice physical_devices[16] = {};
   if ((result = enumerate_physical_devices(instance, &physical_count, physical_devices)) != VK_SUCCESS ||
       physical_count == 0) {
      cleanup();
      return report("physical-device enumeration returned no device", result);
   }
   VkPhysicalDevice physical = physical_devices[0];
   uint32_t queue_family_count = 0;
   get_queue_family_properties(physical, &queue_family_count, nullptr);
   if (queue_family_count == 0 || queue_family_count > 32) {
      cleanup();
      return report("queue-family enumeration failed", VK_SUCCESS);
   }
   VkQueueFamilyProperties queue_families[32] = {};
   get_queue_family_properties(physical, &queue_family_count, queue_families);
   uint32_t queue_family = UINT32_MAX;
   for (uint32_t i = 0; i < queue_family_count; i++) {
      VkBool32 supported = VK_FALSE;
      if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && queue_families[i].queueCount != 0 &&
          get_surface_support(physical, i, surface, &supported) == VK_SUCCESS && supported) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      cleanup();
      return report("no graphics queue supports the Win32 surface", VK_SUCCESS);
   }

   VkSurfaceCapabilitiesKHR capabilities = {};
   if ((result = get_surface_capabilities(physical, surface, &capabilities)) != VK_SUCCESS ||
       capabilities.minImageCount == 0) {
      cleanup();
      return report("surface capability query failed", result);
   }
   uint32_t format_count = 0;
   if ((result = get_surface_formats(physical, surface, &format_count, nullptr)) != VK_SUCCESS || format_count == 0 ||
       format_count > 128) {
      cleanup();
      return report("surface format query failed", result);
   }
   VkSurfaceFormatKHR formats[128] = {};
   if ((result = get_surface_formats(physical, surface, &format_count, formats)) != VK_SUCCESS) {
      cleanup();
      return report("surface format enumeration failed", result);
   }
   uint32_t present_mode_count = 0;
   if ((result = get_present_modes(physical, surface, &present_mode_count, nullptr)) != VK_SUCCESS ||
       present_mode_count == 0 || present_mode_count > 32) {
      cleanup();
      return report("present-mode query failed", result);
   }
   VkPresentModeKHR present_modes[32] = {};
   if ((result = get_present_modes(physical, surface, &present_mode_count, present_modes)) != VK_SUCCESS) {
      cleanup();
      return report("present-mode enumeration failed", result);
   }
   bool has_fifo = false;
   for (uint32_t i = 0; i < present_mode_count; i++)
      has_fifo |= present_modes[i] == VK_PRESENT_MODE_FIFO_KHR;
   if (!has_fifo) {
      cleanup();
      return report("surface does not provide the mandatory FIFO present mode", VK_SUCCESS);
   }

   uint32_t device_extension_count = 0;
   if ((result = enumerate_device_extensions(physical, nullptr, &device_extension_count, nullptr)) != VK_SUCCESS ||
       device_extension_count == 0 || device_extension_count > 256) {
      cleanup();
      return report("device extension query failed", result);
   }
   VkExtensionProperties device_extensions[256] = {};
   if ((result = enumerate_device_extensions(physical, nullptr, &device_extension_count, device_extensions)) !=
          VK_SUCCESS ||
       !has_extension(device_extensions, device_extension_count, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
      cleanup();
      return report("VK_KHR_swapchain is unavailable", result);
   }
   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {};
   queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   queue_info.queueFamilyIndex = queue_family;
   queue_info.queueCount = 1;
   queue_info.pQueuePriorities = &priority;
   const char *device_extensions_enabled[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
   VkDeviceCreateInfo device_info = {};
   device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   device_info.queueCreateInfoCount = 1;
   device_info.pQueueCreateInfos = &queue_info;
   device_info.enabledExtensionCount = 1;
   device_info.ppEnabledExtensionNames = device_extensions_enabled;
   if ((result = create_device(physical, &device_info, nullptr, &device)) != VK_SUCCESS || device == VK_NULL_HANDLE) {
      cleanup();
      return report("vkCreateDevice failed", result);
   }
   PFN_vkGetDeviceProcAddr get_device_proc =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_proc(instance, "vkGetDeviceProcAddr"));
   if (get_device_proc == nullptr) {
      cleanup();
      return report("vkGetDeviceProcAddr is missing", VK_SUCCESS);
   }
#define LOAD_DEVICE(name) reinterpret_cast<PFN_vk##name>(get_device_proc(device, "vk" #name))
   PFN_vkGetSwapchainImagesKHR get_swapchain_images = LOAD_DEVICE(GetSwapchainImagesKHR);
   PFN_vkDeviceWaitIdle device_wait_idle = LOAD_DEVICE(DeviceWaitIdle);
   PFN_vkCreateSwapchainKHR create_swapchain = LOAD_DEVICE(CreateSwapchainKHR);
   PFN_vkAcquireNextImageKHR acquire_next_image = LOAD_DEVICE(AcquireNextImageKHR);
   PFN_vkCreateFence create_fence = LOAD_DEVICE(CreateFence);
   PFN_vkWaitForFences wait_for_fences = LOAD_DEVICE(WaitForFences);
   destroy_swapchain = LOAD_DEVICE(DestroySwapchainKHR);
   destroy_device = LOAD_DEVICE(DestroyDevice);
   destroy_fence = LOAD_DEVICE(DestroyFence);
#undef LOAD_DEVICE
   if (get_swapchain_images == nullptr || device_wait_idle == nullptr || create_swapchain == nullptr ||
       acquire_next_image == nullptr || create_fence == nullptr || wait_for_fences == nullptr ||
       destroy_swapchain == nullptr || destroy_device == nullptr || destroy_fence == nullptr) {
      cleanup();
      return report("required swapchain entrypoints are missing", VK_SUCCESS);
   }
   VkExtent2D extent = capabilities.currentExtent;
   if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
      extent.width = 640;
      extent.height = 480;
      extent.width =
         (extent.width < capabilities.minImageExtent.width) ? capabilities.minImageExtent.width : extent.width;
      extent.height =
         (extent.height < capabilities.minImageExtent.height) ? capabilities.minImageExtent.height : extent.height;
      extent.width =
         (extent.width > capabilities.maxImageExtent.width) ? capabilities.maxImageExtent.width : extent.width;
      extent.height =
         (extent.height > capabilities.maxImageExtent.height) ? capabilities.maxImageExtent.height : extent.height;
   }
   uint32_t image_count = capabilities.minImageCount + 1;
   if (capabilities.maxImageCount != 0 && image_count > capabilities.maxImageCount)
      image_count = capabilities.maxImageCount;
   VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   if ((capabilities.supportedCompositeAlpha & composite_alpha) == 0) {
      const VkCompositeAlphaFlagBitsKHR alternatives[] = {
         VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
         VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
         VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
      };
      composite_alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(0);
      for (VkCompositeAlphaFlagBitsKHR candidate : alternatives) {
         if ((capabilities.supportedCompositeAlpha & candidate) != 0) {
            composite_alpha = candidate;
            break;
         }
      }
      if (composite_alpha == 0) {
         cleanup();
         return report("surface has no usable composite-alpha mode", VK_SUCCESS);
      }
   }
   VkImageUsageFlags image_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   if ((capabilities.supportedUsageFlags & image_usage) == 0)
      image_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   if ((capabilities.supportedUsageFlags & image_usage) == 0) {
      cleanup();
      return report("surface has no usable image usage", VK_SUCCESS);
   }
   VkSwapchainCreateInfoKHR swapchain_info = {};
   swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
   swapchain_info.surface = surface;
   swapchain_info.minImageCount = image_count;
   swapchain_info.imageFormat = formats[0].format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format;
   swapchain_info.imageColorSpace = formats[0].colorSpace;
   swapchain_info.imageExtent = extent;
   swapchain_info.imageArrayLayers = 1;
   swapchain_info.imageUsage = image_usage;
   swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
   swapchain_info.preTransform = capabilities.currentTransform;
   swapchain_info.compositeAlpha = composite_alpha;
   swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
   swapchain_info.clipped = VK_TRUE;
   if ((result = create_swapchain(device, &swapchain_info, nullptr, &swapchain)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateSwapchainKHR failed", result);
   }
   uint32_t swapchain_image_count = 0;
   if ((result = get_swapchain_images(device, swapchain, &swapchain_image_count, nullptr)) != VK_SUCCESS ||
       swapchain_image_count == 0) {
      cleanup();
      return report("swapchain image query failed", result);
   }
   VkFenceCreateInfo acquire_fence_info = {};
   acquire_fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   if ((result = create_fence(device, &acquire_fence_info, nullptr, &acquire_fence)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateFence for swapchain acquire failed", result);
   }
   uint32_t image_index = 0;
   result = acquire_next_image(device, swapchain, UINT64_C(1000000000), VK_NULL_HANDLE, acquire_fence, &image_index);
   if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
      cleanup();
      return report("vkAcquireNextImageKHR failed", result);
   }
   if ((result = wait_for_fences(device, 1, &acquire_fence, VK_TRUE, UINT64_C(5000000000))) != VK_SUCCESS) {
      cleanup();
      return report("swapchain acquire fence wait failed", result);
   }
   SetWindowPos(window, nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
   ShowWindow(window, SW_MINIMIZE);
   ShowWindow(window, SW_RESTORE);
   device_wait_idle(device);
   cleanup();
   printf("tu WDDM Win32 probe passed: swapchain images %u, acquired %u, extent %ux%u\n", swapchain_image_count,
          image_index, extent.width, extent.height);
   return 0;
}
