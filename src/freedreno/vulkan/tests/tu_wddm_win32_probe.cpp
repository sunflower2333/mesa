/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Win32 WSI source probe for the app-local Turnip ICD.  It clears, submits,
 * and presents two CPU/GDI swapchains around a resize/minimize/restore cycle;
 * it does not require a system Vulkan registry entry or a D3D12/DXGI device.
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
constexpr uint32_t kFrameCount = 2;
constexpr uint32_t kMaxSwapchainImages = 16;
constexpr uint64_t kFenceTimeoutNs = UINT64_C(5000000000);

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

void
pump_window_messages()
{
   MSG message = {};
   while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
   }
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
   VkFence acquire_fences[kFrameCount] = {};
   VkFence submit_fences[kFrameCount] = {};
   VkSemaphore present_ready[kFrameCount] = {};
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkCommandBuffer command_buffers[kFrameCount] = {};
   HWND window = nullptr;
   HINSTANCE module = GetModuleHandleW(nullptr);
   bool class_registered = false;
   PFN_vkDestroyInstance destroy_instance = nullptr;
   PFN_vkDestroyDevice destroy_device = nullptr;
   PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
   PFN_vkDestroySurfaceKHR destroy_surface = nullptr;
   PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
   PFN_vkDestroyFence destroy_fence = nullptr;
   PFN_vkDestroySemaphore destroy_semaphore = nullptr;
   PFN_vkDestroyCommandPool destroy_command_pool = nullptr;

   auto cleanup = [&]() {
      if (device_wait_idle != nullptr && device != VK_NULL_HANDLE)
         device_wait_idle(device);
      if (destroy_fence != nullptr) {
         for (uint32_t i = 0; i < kFrameCount; i++) {
            if (submit_fences[i] != VK_NULL_HANDLE)
               destroy_fence(device, submit_fences[i], nullptr);
            if (acquire_fences[i] != VK_NULL_HANDLE)
               destroy_fence(device, acquire_fences[i], nullptr);
         }
      }
      if (destroy_semaphore != nullptr) {
         for (VkSemaphore semaphore : present_ready) {
            if (semaphore != VK_NULL_HANDLE)
               destroy_semaphore(device, semaphore, nullptr);
         }
      }
      if (destroy_command_pool != nullptr && command_pool != VK_NULL_HANDLE)
         destroy_command_pool(device, command_pool, nullptr);
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
   device_wait_idle = LOAD_DEVICE(DeviceWaitIdle);
   PFN_vkGetDeviceQueue get_device_queue = LOAD_DEVICE(GetDeviceQueue);
   PFN_vkCreateSwapchainKHR create_swapchain = LOAD_DEVICE(CreateSwapchainKHR);
   PFN_vkAcquireNextImageKHR acquire_next_image = LOAD_DEVICE(AcquireNextImageKHR);
   PFN_vkQueuePresentKHR queue_present = LOAD_DEVICE(QueuePresentKHR);
   PFN_vkCreateFence create_fence = LOAD_DEVICE(CreateFence);
   PFN_vkWaitForFences wait_for_fences = LOAD_DEVICE(WaitForFences);
   PFN_vkCreateSemaphore create_semaphore = LOAD_DEVICE(CreateSemaphore);
   PFN_vkCreateCommandPool create_command_pool = LOAD_DEVICE(CreateCommandPool);
   PFN_vkAllocateCommandBuffers allocate_command_buffers = LOAD_DEVICE(AllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_command_buffer = LOAD_DEVICE(BeginCommandBuffer);
   PFN_vkEndCommandBuffer end_command_buffer = LOAD_DEVICE(EndCommandBuffer);
   PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = LOAD_DEVICE(CmdPipelineBarrier);
   PFN_vkCmdClearColorImage cmd_clear_color_image = LOAD_DEVICE(CmdClearColorImage);
   PFN_vkQueueSubmit queue_submit = LOAD_DEVICE(QueueSubmit);
   destroy_swapchain = LOAD_DEVICE(DestroySwapchainKHR);
   destroy_device = LOAD_DEVICE(DestroyDevice);
   destroy_fence = LOAD_DEVICE(DestroyFence);
   destroy_semaphore = LOAD_DEVICE(DestroySemaphore);
   destroy_command_pool = LOAD_DEVICE(DestroyCommandPool);
#undef LOAD_DEVICE
   if (get_swapchain_images == nullptr || device_wait_idle == nullptr || get_device_queue == nullptr ||
       create_swapchain == nullptr || acquire_next_image == nullptr || queue_present == nullptr ||
       create_fence == nullptr || wait_for_fences == nullptr || create_semaphore == nullptr ||
       create_command_pool == nullptr || allocate_command_buffers == nullptr || begin_command_buffer == nullptr ||
       end_command_buffer == nullptr || cmd_pipeline_barrier == nullptr || cmd_clear_color_image == nullptr ||
       queue_submit == nullptr || destroy_swapchain == nullptr || destroy_device == nullptr ||
       destroy_fence == nullptr || destroy_semaphore == nullptr || destroy_command_pool == nullptr) {
      cleanup();
      return report("required swapchain entrypoints are missing", VK_SUCCESS);
   }

   VkQueue queue = VK_NULL_HANDLE;
   get_device_queue(device, queue_family, 0, &queue);
   if (queue == VK_NULL_HANDLE) {
      cleanup();
      return report("graphics/present queue is null", VK_SUCCESS);
   }

   VkCommandPoolCreateInfo command_pool_info = {};
   command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   command_pool_info.queueFamilyIndex = queue_family;
   if ((result = create_command_pool(device, &command_pool_info, nullptr, &command_pool)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateCommandPool failed", result);
   }
   VkCommandBufferAllocateInfo command_buffer_info = {};
   command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   command_buffer_info.commandPool = command_pool;
   command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   command_buffer_info.commandBufferCount = kFrameCount;
   if ((result = allocate_command_buffers(device, &command_buffer_info, command_buffers)) != VK_SUCCESS) {
      cleanup();
      return report("vkAllocateCommandBuffers failed", result);
   }

   VkFenceCreateInfo fence_info = {};
   fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   VkSemaphoreCreateInfo semaphore_info = {};
   semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
   for (uint32_t i = 0; i < kFrameCount; i++) {
      if ((result = create_fence(device, &fence_info, nullptr, &acquire_fences[i])) != VK_SUCCESS ||
          (result = create_fence(device, &fence_info, nullptr, &submit_fences[i])) != VK_SUCCESS ||
          (result = create_semaphore(device, &semaphore_info, nullptr, &present_ready[i])) != VK_SUCCESS) {
         cleanup();
         return report("frame synchronization object creation failed", result);
      }
   }
   const VkSurfaceFormatKHR surface_format = {
      formats[0].format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : formats[0].format,
      formats[0].colorSpace,
   };

   auto create_surface_swapchain = [&](VkSwapchainKHR old_swapchain, VkSwapchainKHR *out_swapchain,
                                       VkExtent2D *out_extent, VkImage *out_images,
                                       uint32_t *out_image_count) -> VkResult {
      *out_swapchain = VK_NULL_HANDLE;
      *out_extent = {};
      *out_image_count = 0;

      VkSurfaceCapabilitiesKHR current_capabilities = {};
      VkResult create_result = get_surface_capabilities(physical, surface, &current_capabilities);
      if (create_result != VK_SUCCESS || current_capabilities.minImageCount == 0 ||
          (current_capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
         return create_result == VK_SUCCESS ? VK_ERROR_FORMAT_NOT_SUPPORTED : create_result;

      VkExtent2D current_extent = current_capabilities.currentExtent;
      if (current_extent.width == UINT32_MAX || current_extent.height == UINT32_MAX) {
         RECT client_rect = {};
         if (!GetClientRect(window, &client_rect))
            return VK_ERROR_INITIALIZATION_FAILED;
         current_extent.width = static_cast<uint32_t>(client_rect.right - client_rect.left);
         current_extent.height = static_cast<uint32_t>(client_rect.bottom - client_rect.top);
         current_extent.width = (current_extent.width < current_capabilities.minImageExtent.width)
                                   ? current_capabilities.minImageExtent.width
                                   : current_extent.width;
         current_extent.height = (current_extent.height < current_capabilities.minImageExtent.height)
                                    ? current_capabilities.minImageExtent.height
                                    : current_extent.height;
         current_extent.width = (current_extent.width > current_capabilities.maxImageExtent.width)
                                   ? current_capabilities.maxImageExtent.width
                                   : current_extent.width;
         current_extent.height = (current_extent.height > current_capabilities.maxImageExtent.height)
                                    ? current_capabilities.maxImageExtent.height
                                    : current_extent.height;
      }
      if (current_extent.width == 0 || current_extent.height == 0)
         return VK_ERROR_OUT_OF_DATE_KHR;

      uint32_t current_image_count = current_capabilities.minImageCount + 1;
      if (current_capabilities.maxImageCount != 0 && current_image_count > current_capabilities.maxImageCount)
         current_image_count = current_capabilities.maxImageCount;
      if (current_image_count > kMaxSwapchainImages)
         return VK_ERROR_TOO_MANY_OBJECTS;

      VkCompositeAlphaFlagBitsKHR composite_alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
      if ((current_capabilities.supportedCompositeAlpha & composite_alpha) == 0) {
         const VkCompositeAlphaFlagBitsKHR alternatives[] = {
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
         };
         composite_alpha = static_cast<VkCompositeAlphaFlagBitsKHR>(0);
         for (VkCompositeAlphaFlagBitsKHR candidate : alternatives) {
            if ((current_capabilities.supportedCompositeAlpha & candidate) != 0) {
               composite_alpha = candidate;
               break;
            }
         }
         if (composite_alpha == 0)
            return VK_ERROR_INITIALIZATION_FAILED;
      }

      VkSwapchainCreateInfoKHR swapchain_info = {};
      swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
      swapchain_info.surface = surface;
      swapchain_info.minImageCount = current_image_count;
      swapchain_info.imageFormat = surface_format.format;
      swapchain_info.imageColorSpace = surface_format.colorSpace;
      swapchain_info.imageExtent = current_extent;
      swapchain_info.imageArrayLayers = 1;
      swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
      swapchain_info.preTransform = current_capabilities.currentTransform;
      swapchain_info.compositeAlpha = composite_alpha;
      swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
      swapchain_info.clipped = VK_TRUE;
      swapchain_info.oldSwapchain = old_swapchain;

      VkSwapchainKHR created_swapchain = VK_NULL_HANDLE;
      create_result = create_swapchain(device, &swapchain_info, nullptr, &created_swapchain);
      if (create_result != VK_SUCCESS)
         return create_result;

      uint32_t created_image_count = kMaxSwapchainImages;
      create_result = get_swapchain_images(device, created_swapchain, &created_image_count, out_images);
      if (create_result != VK_SUCCESS || created_image_count == 0 || created_image_count > kMaxSwapchainImages) {
         destroy_swapchain(device, created_swapchain, nullptr);
         return create_result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : create_result;
      }

      *out_swapchain = created_swapchain;
      *out_extent = current_extent;
      *out_image_count = created_image_count;
      return VK_SUCCESS;
   };

   auto present_frame = [&](uint32_t frame, VkSwapchainKHR frame_swapchain, const VkImage *images,
                            uint32_t image_count) -> VkResult {
      uint32_t image_index = 0;
      VkResult frame_result = acquire_next_image(device, frame_swapchain, UINT64_C(1000000000), VK_NULL_HANDLE,
                                                 acquire_fences[frame], &image_index);
      if ((frame_result != VK_SUCCESS && frame_result != VK_SUBOPTIMAL_KHR) || image_index >= image_count)
         return frame_result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : frame_result;
      frame_result = wait_for_fences(device, 1, &acquire_fences[frame], VK_TRUE, kFenceTimeoutNs);
      if (frame_result != VK_SUCCESS)
         return frame_result;

      VkCommandBufferBeginInfo begin_info = {};
      begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      frame_result = begin_command_buffer(command_buffers[frame], &begin_info);
      if (frame_result != VK_SUCCESS)
         return frame_result;

      VkImageMemoryBarrier to_transfer = {};
      to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.image = images[image_index];
      to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      to_transfer.subresourceRange.levelCount = 1;
      to_transfer.subresourceRange.layerCount = 1;
      cmd_pipeline_barrier(command_buffers[frame], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           0, nullptr, 0, nullptr, 1, &to_transfer);

      const VkClearColorValue clear_color = {
         { frame == 0 ? 0.125f : 0.75f, frame == 0 ? 0.5f : 0.25f, 0.875f, 1.0f },
      };
      VkImageSubresourceRange clear_range = {};
      clear_range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      clear_range.levelCount = 1;
      clear_range.layerCount = 1;
      cmd_clear_color_image(command_buffers[frame], images[image_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &clear_color, 1, &clear_range);

      VkImageMemoryBarrier to_present = to_transfer;
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_present.dstAccessMask = 0;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      cmd_pipeline_barrier(command_buffers[frame], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &to_present);
      frame_result = end_command_buffer(command_buffers[frame]);
      if (frame_result != VK_SUCCESS)
         return frame_result;

      VkSubmitInfo submit_info = {};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &command_buffers[frame];
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &present_ready[frame];
      frame_result = queue_submit(queue, 1, &submit_info, submit_fences[frame]);
      if (frame_result != VK_SUCCESS)
         return frame_result;

      VkPresentInfoKHR present_info = {};
      present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
      present_info.waitSemaphoreCount = 1;
      present_info.pWaitSemaphores = &present_ready[frame];
      present_info.swapchainCount = 1;
      present_info.pSwapchains = &frame_swapchain;
      present_info.pImageIndices = &image_index;
      frame_result = queue_present(queue, &present_info);
      if (frame_result != VK_SUCCESS && frame_result != VK_SUBOPTIMAL_KHR)
         return frame_result;
      frame_result = wait_for_fences(device, 1, &submit_fences[frame], VK_TRUE, kFenceTimeoutNs);
      if (frame_result != VK_SUCCESS)
         return frame_result;
      return device_wait_idle(device);
   };

   ShowWindow(window, SW_SHOW);
   pump_window_messages();

   VkExtent2D initial_extent = {};
   VkImage initial_images[kMaxSwapchainImages] = {};
   uint32_t initial_image_count = 0;
   if ((result = create_surface_swapchain(VK_NULL_HANDLE, &swapchain, &initial_extent, initial_images,
                                          &initial_image_count)) != VK_SUCCESS ||
       (result = present_frame(0, swapchain, initial_images, initial_image_count)) != VK_SUCCESS) {
      cleanup();
      return report("initial clear/submit/present failed", result);
   }

   SetWindowPos(window, nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
   pump_window_messages();
   ShowWindow(window, SW_MINIMIZE);
   pump_window_messages();
   ShowWindow(window, SW_RESTORE);
   pump_window_messages();

   VkSwapchainKHR resized_swapchain = VK_NULL_HANDLE;
   VkExtent2D resized_extent = {};
   VkImage resized_images[kMaxSwapchainImages] = {};
   uint32_t resized_image_count = 0;
   if ((result = create_surface_swapchain(swapchain, &resized_swapchain, &resized_extent, resized_images,
                                          &resized_image_count)) != VK_SUCCESS) {
      cleanup();
      return report("resized swapchain recreation failed", result);
   }
   destroy_swapchain(device, swapchain, nullptr);
   swapchain = resized_swapchain;
   if ((result = present_frame(1, swapchain, resized_images, resized_image_count)) != VK_SUCCESS) {
      cleanup();
      return report("resized clear/submit/present failed", result);
   }

   cleanup();
   printf("tu WDDM Win32 probe passed: clear/submit/present %ux%u -> %ux%u (%u/%u images)\n", initial_extent.width,
          initial_extent.height, resized_extent.width, resized_extent.height, initial_image_count, resized_image_count);
   return 0;
}
