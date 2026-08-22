/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * An offscreen ARM64 Windows graphics workload for the app-local Turnip ICD.
 * It draws a fullscreen triangle into an optimal image, copies the image to a
 * host-visible buffer, and verifies the rendered RGBA8 bytes.
 */

#define WIN32_LEAN_AND_MEAN
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <vulkan/vulkan_core.h>
#include <windows.h>

namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr VkDeviceSize kReadbackSize = static_cast<VkDeviceSize>(kWidth) * kHeight * 4;
constexpr uint64_t kFenceTimeoutNs = UINT64_C(10000000000);

bool
load_spirv(const char *path, std::vector<uint32_t> *code)
{
   FILE *file = nullptr;
   if (fopen_s(&file, path, "rb") != 0 || file == nullptr)
      return false;

   if (fseek(file, 0, SEEK_END) != 0) {
      fclose(file);
      return false;
   }
   const long size = ftell(file);
   if (size <= 0 || (size % static_cast<long>(sizeof(uint32_t))) != 0 || fseek(file, 0, SEEK_SET) != 0) {
      fclose(file);
      return false;
   }

   code->resize(static_cast<size_t>(size) / sizeof(uint32_t));
   const size_t read = fread(code->data(), sizeof(uint32_t), code->size(), file);
   fclose(file);
   return read == code->size() && !code->empty() && code->front() == 0x07230203u;
}

int
report(const char *message, VkResult result)
{
   if (result == VK_SUCCESS)
      fprintf(stderr, "tu WDDM Vulkan graphics probe: %s\n", message);
   else
      fprintf(stderr, "tu WDDM Vulkan graphics probe: %s (VkResult=%d)\n", message, result);
   return 1;
}

} // namespace

int
main(int argc, char **argv)
{
   if (argc != 3) {
      fprintf(stderr, "usage: %s vertex.spv fragment.spv\n", argv[0]);
      return 2;
   }

   std::vector<uint32_t> vertex_code;
   std::vector<uint32_t> fragment_code;
   if (!load_spirv(argv[1], &vertex_code) || !load_spirv(argv[2], &fragment_code))
      return report("failed to read valid vertex and fragment SPIR-V shaders", VK_SUCCESS);

   HMODULE icd = LoadLibraryW(L"vulkan_freedreno.dll");
   if (icd == nullptr) {
      fprintf(stderr, "tu WDDM Vulkan graphics probe: LoadLibraryW failed (%lu)\n",
              static_cast<unsigned long>(GetLastError()));
      return 1;
   }

   VkInstance instance = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory image_memory = VK_NULL_HANDLE;
   VkImageView image_view = VK_NULL_HANDLE;
   VkBuffer readback = VK_NULL_HANDLE;
   VkDeviceMemory readback_memory = VK_NULL_HANDLE;
   VkRenderPass render_pass = VK_NULL_HANDLE;
   VkFramebuffer framebuffer = VK_NULL_HANDLE;
   VkShaderModule vertex_shader = VK_NULL_HANDLE;
   VkShaderModule fragment_shader = VK_NULL_HANDLE;
   VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   void *mapped = nullptr;
   bool submission_needs_quiesce = false;

   PFN_vkDestroyInstance destroy_instance = nullptr;
   PFN_vkDestroyDevice destroy_device = nullptr;
   PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
   PFN_vkUnmapMemory unmap_memory = nullptr;
   PFN_vkDestroyFence destroy_fence = nullptr;
   PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
   PFN_vkDestroyFramebuffer destroy_framebuffer = nullptr;
   PFN_vkDestroyPipeline destroy_pipeline = nullptr;
   PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
   PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
   PFN_vkDestroyRenderPass destroy_render_pass = nullptr;
   PFN_vkDestroyImageView destroy_image_view = nullptr;
   PFN_vkDestroyBuffer destroy_buffer = nullptr;
   PFN_vkDestroyImage destroy_image = nullptr;
   PFN_vkFreeMemory free_memory = nullptr;

   auto cleanup = [&]() {
      if (submission_needs_quiesce && device_wait_idle != nullptr) {
         if (device_wait_idle(device) != VK_SUCCESS)
            return;
         submission_needs_quiesce = false;
      }
      if (mapped != nullptr && unmap_memory != nullptr)
         unmap_memory(device, readback_memory);
      if (destroy_fence != nullptr && fence != VK_NULL_HANDLE)
         destroy_fence(device, fence, nullptr);
      if (destroy_command_pool != nullptr && command_pool != VK_NULL_HANDLE)
         destroy_command_pool(device, command_pool, nullptr);
      if (destroy_framebuffer != nullptr && framebuffer != VK_NULL_HANDLE)
         destroy_framebuffer(device, framebuffer, nullptr);
      if (destroy_pipeline != nullptr && pipeline != VK_NULL_HANDLE)
         destroy_pipeline(device, pipeline, nullptr);
      if (destroy_pipeline_layout != nullptr && pipeline_layout != VK_NULL_HANDLE)
         destroy_pipeline_layout(device, pipeline_layout, nullptr);
      if (destroy_shader_module != nullptr && fragment_shader != VK_NULL_HANDLE)
         destroy_shader_module(device, fragment_shader, nullptr);
      if (destroy_shader_module != nullptr && vertex_shader != VK_NULL_HANDLE)
         destroy_shader_module(device, vertex_shader, nullptr);
      if (destroy_render_pass != nullptr && render_pass != VK_NULL_HANDLE)
         destroy_render_pass(device, render_pass, nullptr);
      if (destroy_image_view != nullptr && image_view != VK_NULL_HANDLE)
         destroy_image_view(device, image_view, nullptr);
      if (destroy_buffer != nullptr && readback != VK_NULL_HANDLE)
         destroy_buffer(device, readback, nullptr);
      if (free_memory != nullptr && readback_memory != VK_NULL_HANDLE)
         free_memory(device, readback_memory, nullptr);
      if (destroy_image != nullptr && image != VK_NULL_HANDLE)
         destroy_image(device, image, nullptr);
      if (free_memory != nullptr && image_memory != VK_NULL_HANDLE)
         free_memory(device, image_memory, nullptr);
      if (destroy_device != nullptr && device != VK_NULL_HANDLE)
         destroy_device(device, nullptr);
      if (destroy_instance != nullptr && instance != VK_NULL_HANDLE)
         destroy_instance(instance, nullptr);
      FreeLibrary(icd);
   };

   PFN_vkGetInstanceProcAddr get_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
   if (get_proc == nullptr) {
      cleanup();
      return report("vk_icdGetInstanceProcAddr is missing", VK_SUCCESS);
   }
   PFN_vkCreateInstance create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
   if (create_instance == nullptr) {
      cleanup();
      return report("vkCreateInstance is missing", VK_SUCCESS);
   }

   VkApplicationInfo app = {};
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "DroidVM Turnip WDDM graphics probe";
   app.applicationVersion = 1;
   app.pEngineName = "DroidVM";
   app.engineVersion = 1;
   app.apiVersion = VK_API_VERSION_1_1;
   VkInstanceCreateInfo instance_info = {};
   instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   instance_info.pApplicationInfo = &app;

   VkResult result = create_instance(&instance_info, nullptr, &instance);
   if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
      cleanup();
      return report("vkCreateInstance failed", result);
   }

   PFN_vkEnumeratePhysicalDevices enumerate_physical_devices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get_proc(instance, "vkEnumeratePhysicalDevices"));
   PFN_vkGetPhysicalDeviceProperties get_physical_device_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get_proc(instance, "vkGetPhysicalDeviceProperties"));
   PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
         get_proc(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
   PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
         get_proc(instance, "vkGetPhysicalDeviceMemoryProperties"));
   PFN_vkGetPhysicalDeviceFormatProperties get_format_properties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
         get_proc(instance, "vkGetPhysicalDeviceFormatProperties"));
   PFN_vkCreateDevice create_device = reinterpret_cast<PFN_vkCreateDevice>(get_proc(instance, "vkCreateDevice"));
   destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
   if (enumerate_physical_devices == nullptr || get_physical_device_properties == nullptr ||
       get_queue_family_properties == nullptr || get_memory_properties == nullptr || get_format_properties == nullptr ||
       create_device == nullptr || destroy_instance == nullptr) {
      cleanup();
      return report("required instance entrypoints are missing", VK_SUCCESS);
   }

   uint32_t physical_count = 0;
   result = enumerate_physical_devices(instance, &physical_count, nullptr);
   if (result != VK_SUCCESS || physical_count == 0 || physical_count > 16) {
      cleanup();
      return report("physical-device enumeration failed", result);
   }
   VkPhysicalDevice physical_devices[16] = {};
   result = enumerate_physical_devices(instance, &physical_count, physical_devices);
   if (result != VK_SUCCESS || physical_count == 0) {
      cleanup();
      return report("physical-device enumeration returned no device", result);
   }

   VkPhysicalDevice physical = VK_NULL_HANDLE;
   VkPhysicalDeviceProperties properties = {};
   for (uint32_t i = 0; i < physical_count; i++) {
      VkPhysicalDeviceProperties candidate = {};
      get_physical_device_properties(physical_devices[i], &candidate);
      if (candidate.vendorID == 0x5143 && strstr(candidate.deviceName, "Adreno") != nullptr) {
         physical = physical_devices[i];
         properties = candidate;
         break;
      }
   }
   if (physical == VK_NULL_HANDLE) {
      cleanup();
      return report("no Turnip Adreno adapter was found", VK_SUCCESS);
   }

   VkFormatProperties format_properties = {};
   get_format_properties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
   const VkFormatFeatureFlags required_format_features =
      VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
   if ((format_properties.optimalTilingFeatures & required_format_features) != required_format_features) {
      cleanup();
      return report("RGBA8 optimal images do not support color attachment and transfer source", VK_SUCCESS);
   }

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
      if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && queue_families[i].queueCount != 0) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      cleanup();
      return report("no graphics queue family is available", VK_SUCCESS);
   }

   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {};
   queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   queue_info.queueFamilyIndex = queue_family;
   queue_info.queueCount = 1;
   queue_info.pQueuePriorities = &priority;
   VkDeviceCreateInfo device_info = {};
   device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   device_info.queueCreateInfoCount = 1;
   device_info.pQueueCreateInfos = &queue_info;
   result = create_device(physical, &device_info, nullptr, &device);
   if (result != VK_SUCCESS || device == VK_NULL_HANDLE) {
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
   PFN_vkGetDeviceQueue get_device_queue = LOAD_DEVICE(GetDeviceQueue);
   PFN_vkCreateImage create_image = LOAD_DEVICE(CreateImage);
   PFN_vkGetImageMemoryRequirements get_image_memory_requirements = LOAD_DEVICE(GetImageMemoryRequirements);
   PFN_vkAllocateMemory allocate_memory = LOAD_DEVICE(AllocateMemory);
   PFN_vkBindImageMemory bind_image_memory = LOAD_DEVICE(BindImageMemory);
   PFN_vkCreateImageView create_image_view = LOAD_DEVICE(CreateImageView);
   PFN_vkCreateBuffer create_buffer = LOAD_DEVICE(CreateBuffer);
   PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = LOAD_DEVICE(GetBufferMemoryRequirements);
   PFN_vkBindBufferMemory bind_buffer_memory = LOAD_DEVICE(BindBufferMemory);
   PFN_vkMapMemory map_memory = LOAD_DEVICE(MapMemory);
   unmap_memory = LOAD_DEVICE(UnmapMemory);
   PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = LOAD_DEVICE(FlushMappedMemoryRanges);
   PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = LOAD_DEVICE(InvalidateMappedMemoryRanges);
   PFN_vkCreateRenderPass create_render_pass = LOAD_DEVICE(CreateRenderPass);
   PFN_vkCreateFramebuffer create_framebuffer = LOAD_DEVICE(CreateFramebuffer);
   PFN_vkCreateShaderModule create_shader_module = LOAD_DEVICE(CreateShaderModule);
   PFN_vkCreatePipelineLayout create_pipeline_layout = LOAD_DEVICE(CreatePipelineLayout);
   PFN_vkCreateGraphicsPipelines create_graphics_pipelines = LOAD_DEVICE(CreateGraphicsPipelines);
   PFN_vkCreateCommandPool create_command_pool = LOAD_DEVICE(CreateCommandPool);
   PFN_vkAllocateCommandBuffers allocate_command_buffers = LOAD_DEVICE(AllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_command_buffer = LOAD_DEVICE(BeginCommandBuffer);
   PFN_vkEndCommandBuffer end_command_buffer = LOAD_DEVICE(EndCommandBuffer);
   PFN_vkCmdBeginRenderPass cmd_begin_render_pass = LOAD_DEVICE(CmdBeginRenderPass);
   PFN_vkCmdEndRenderPass cmd_end_render_pass = LOAD_DEVICE(CmdEndRenderPass);
   PFN_vkCmdBindPipeline cmd_bind_pipeline = LOAD_DEVICE(CmdBindPipeline);
   PFN_vkCmdDraw cmd_draw = LOAD_DEVICE(CmdDraw);
   PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = LOAD_DEVICE(CmdPipelineBarrier);
   PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer = LOAD_DEVICE(CmdCopyImageToBuffer);
   PFN_vkCreateFence create_fence = LOAD_DEVICE(CreateFence);
   PFN_vkQueueSubmit queue_submit = LOAD_DEVICE(QueueSubmit);
   PFN_vkWaitForFences wait_for_fences = LOAD_DEVICE(WaitForFences);
   device_wait_idle = LOAD_DEVICE(DeviceWaitIdle);
   destroy_fence = LOAD_DEVICE(DestroyFence);
   destroy_command_pool = LOAD_DEVICE(DestroyCommandPool);
   destroy_framebuffer = LOAD_DEVICE(DestroyFramebuffer);
   destroy_pipeline = LOAD_DEVICE(DestroyPipeline);
   destroy_pipeline_layout = LOAD_DEVICE(DestroyPipelineLayout);
   destroy_shader_module = LOAD_DEVICE(DestroyShaderModule);
   destroy_render_pass = LOAD_DEVICE(DestroyRenderPass);
   destroy_image_view = LOAD_DEVICE(DestroyImageView);
   destroy_buffer = LOAD_DEVICE(DestroyBuffer);
   destroy_image = LOAD_DEVICE(DestroyImage);
   free_memory = LOAD_DEVICE(FreeMemory);
   destroy_device = LOAD_DEVICE(DestroyDevice);
#undef LOAD_DEVICE
   if (get_device_queue == nullptr || create_image == nullptr || get_image_memory_requirements == nullptr ||
       allocate_memory == nullptr || bind_image_memory == nullptr || create_image_view == nullptr ||
       create_buffer == nullptr || get_buffer_memory_requirements == nullptr || bind_buffer_memory == nullptr ||
       map_memory == nullptr || unmap_memory == nullptr || flush_mapped_memory_ranges == nullptr ||
       invalidate_mapped_memory_ranges == nullptr || create_render_pass == nullptr || create_framebuffer == nullptr ||
       create_shader_module == nullptr || create_pipeline_layout == nullptr || create_graphics_pipelines == nullptr ||
       create_command_pool == nullptr || allocate_command_buffers == nullptr || begin_command_buffer == nullptr ||
       end_command_buffer == nullptr || cmd_begin_render_pass == nullptr || cmd_end_render_pass == nullptr ||
       cmd_bind_pipeline == nullptr || cmd_draw == nullptr || cmd_pipeline_barrier == nullptr ||
       cmd_copy_image_to_buffer == nullptr || create_fence == nullptr || queue_submit == nullptr ||
       wait_for_fences == nullptr || device_wait_idle == nullptr || destroy_fence == nullptr ||
       destroy_command_pool == nullptr || destroy_framebuffer == nullptr || destroy_pipeline == nullptr ||
       destroy_pipeline_layout == nullptr || destroy_shader_module == nullptr || destroy_render_pass == nullptr ||
       destroy_image_view == nullptr || destroy_buffer == nullptr || destroy_image == nullptr ||
       free_memory == nullptr || destroy_device == nullptr) {
      cleanup();
      return report("required device entrypoints are missing", VK_SUCCESS);
   }

   VkQueue queue = VK_NULL_HANDLE;
   get_device_queue(device, queue_family, 0, &queue);
   if (queue == VK_NULL_HANDLE) {
      cleanup();
      return report("graphics queue is null", VK_SUCCESS);
   }

   VkPhysicalDeviceMemoryProperties memory_properties = {};
   get_memory_properties(physical, &memory_properties);

   VkImageCreateInfo image_info = {};
   image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image_info.imageType = VK_IMAGE_TYPE_2D;
   image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
   image_info.extent = { kWidth, kHeight, 1 };
   image_info.mipLevels = 1;
   image_info.arrayLayers = 1;
   image_info.samples = VK_SAMPLE_COUNT_1_BIT;
   image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
   image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   if ((result = create_image(device, &image_info, nullptr, &image)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateImage failed", result);
   }

   VkMemoryRequirements image_requirements = {};
   get_image_memory_requirements(device, image, &image_requirements);
   uint32_t image_memory_type = UINT32_MAX;
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
      if ((image_requirements.memoryTypeBits & (UINT32_C(1) << i)) == 0)
         continue;
      if (image_memory_type == UINT32_MAX)
         image_memory_type = i;
      if ((memory_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
         image_memory_type = i;
         break;
      }
   }
   if (image_memory_type == UINT32_MAX) {
      cleanup();
      return report("no compatible image memory type", VK_SUCCESS);
   }
   VkMemoryAllocateInfo image_allocation_info = {};
   image_allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   image_allocation_info.allocationSize = image_requirements.size;
   image_allocation_info.memoryTypeIndex = image_memory_type;
   result = allocate_memory(device, &image_allocation_info, nullptr, &image_memory);
   if (result == VK_SUCCESS)
      result = bind_image_memory(device, image, image_memory, 0);
   if (result != VK_SUCCESS) {
      cleanup();
      return report("offscreen image allocation failed", result);
   }

   VkImageViewCreateInfo image_view_info = {};
   image_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
   image_view_info.image = image;
   image_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
   image_view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
   image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   image_view_info.subresourceRange.levelCount = 1;
   image_view_info.subresourceRange.layerCount = 1;
   if ((result = create_image_view(device, &image_view_info, nullptr, &image_view)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateImageView failed", result);
   }

   VkBufferCreateInfo buffer_info = {};
   buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   buffer_info.size = kReadbackSize;
   buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   if ((result = create_buffer(device, &buffer_info, nullptr, &readback)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateBuffer failed", result);
   }
   VkMemoryRequirements buffer_requirements = {};
   get_buffer_memory_requirements(device, readback, &buffer_requirements);
   uint32_t readback_memory_type = UINT32_MAX;
   VkMemoryPropertyFlags readback_memory_flags = 0;
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
      const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
      if ((buffer_requirements.memoryTypeBits & (UINT32_C(1) << i)) != 0 &&
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
         readback_memory_type = i;
         readback_memory_flags = flags;
         if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
            break;
      }
   }
   if (readback_memory_type == UINT32_MAX) {
      cleanup();
      return report("no host-visible transfer-buffer memory type", VK_SUCCESS);
   }
   VkMemoryAllocateInfo buffer_allocation_info = {};
   buffer_allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   buffer_allocation_info.allocationSize = buffer_requirements.size;
   buffer_allocation_info.memoryTypeIndex = readback_memory_type;
   result = allocate_memory(device, &buffer_allocation_info, nullptr, &readback_memory);
   if (result == VK_SUCCESS)
      result = bind_buffer_memory(device, readback, readback_memory, 0);
   if (result == VK_SUCCESS)
      result = map_memory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
   if (result != VK_SUCCESS) {
      cleanup();
      return report("readback buffer allocation failed", result);
   }
   memset(mapped, 0x5a, static_cast<size_t>(kReadbackSize));
   if ((readback_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = readback_memory;
      range.size = VK_WHOLE_SIZE;
      if ((result = flush_mapped_memory_ranges(device, 1, &range)) != VK_SUCCESS) {
         cleanup();
         return report("vkFlushMappedMemoryRanges failed", result);
      }
   }

   VkAttachmentDescription attachment = {};
   attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
   attachment.samples = VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
   attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
   attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   VkAttachmentReference color_attachment = {};
   color_attachment.attachment = 0;
   color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   VkSubpassDescription subpass = {};
   subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
   subpass.colorAttachmentCount = 1;
   subpass.pColorAttachments = &color_attachment;
   VkSubpassDependency dependency = {};
   dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
   dependency.dstSubpass = 0;
   dependency.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
   dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   VkRenderPassCreateInfo render_pass_info = {};
   render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
   render_pass_info.attachmentCount = 1;
   render_pass_info.pAttachments = &attachment;
   render_pass_info.subpassCount = 1;
   render_pass_info.pSubpasses = &subpass;
   render_pass_info.dependencyCount = 1;
   render_pass_info.pDependencies = &dependency;
   if ((result = create_render_pass(device, &render_pass_info, nullptr, &render_pass)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateRenderPass failed", result);
   }

   VkFramebufferCreateInfo framebuffer_info = {};
   framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
   framebuffer_info.renderPass = render_pass;
   framebuffer_info.attachmentCount = 1;
   framebuffer_info.pAttachments = &image_view;
   framebuffer_info.width = kWidth;
   framebuffer_info.height = kHeight;
   framebuffer_info.layers = 1;
   if ((result = create_framebuffer(device, &framebuffer_info, nullptr, &framebuffer)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateFramebuffer failed", result);
   }

   VkShaderModuleCreateInfo shader_info = {};
   shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   shader_info.codeSize = vertex_code.size() * sizeof(uint32_t);
   shader_info.pCode = vertex_code.data();
   if ((result = create_shader_module(device, &shader_info, nullptr, &vertex_shader)) != VK_SUCCESS) {
      cleanup();
      return report("vertex vkCreateShaderModule failed", result);
   }
   shader_info.codeSize = fragment_code.size() * sizeof(uint32_t);
   shader_info.pCode = fragment_code.data();
   if ((result = create_shader_module(device, &shader_info, nullptr, &fragment_shader)) != VK_SUCCESS) {
      cleanup();
      return report("fragment vkCreateShaderModule failed", result);
   }

   VkPipelineLayoutCreateInfo pipeline_layout_info = {};
   pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
   if ((result = create_pipeline_layout(device, &pipeline_layout_info, nullptr, &pipeline_layout)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreatePipelineLayout failed", result);
   }

   VkPipelineShaderStageCreateInfo stages[2] = {};
   stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
   stages[0].module = vertex_shader;
   stages[0].pName = "main";
   stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
   stages[1].module = fragment_shader;
   stages[1].pName = "main";
   VkPipelineVertexInputStateCreateInfo vertex_input = {};
   vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
   input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
   input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   VkViewport viewport = {};
   viewport.width = static_cast<float>(kWidth);
   viewport.height = static_cast<float>(kHeight);
   viewport.maxDepth = 1.0f;
   VkRect2D scissor = {};
   scissor.extent = { kWidth, kHeight };
   VkPipelineViewportStateCreateInfo viewport_state = {};
   viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
   viewport_state.viewportCount = 1;
   viewport_state.pViewports = &viewport;
   viewport_state.scissorCount = 1;
   viewport_state.pScissors = &scissor;
   VkPipelineRasterizationStateCreateInfo rasterization = {};
   rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
   rasterization.polygonMode = VK_POLYGON_MODE_FILL;
   rasterization.cullMode = VK_CULL_MODE_NONE;
   rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
   rasterization.lineWidth = 1.0f;
   VkPipelineMultisampleStateCreateInfo multisample = {};
   multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
   multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState blend_attachment = {};
   blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
   VkPipelineColorBlendStateCreateInfo blend = {};
   blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
   blend.attachmentCount = 1;
   blend.pAttachments = &blend_attachment;
   VkGraphicsPipelineCreateInfo pipeline_info = {};
   pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
   pipeline_info.stageCount = 2;
   pipeline_info.pStages = stages;
   pipeline_info.pVertexInputState = &vertex_input;
   pipeline_info.pInputAssemblyState = &input_assembly;
   pipeline_info.pViewportState = &viewport_state;
   pipeline_info.pRasterizationState = &rasterization;
   pipeline_info.pMultisampleState = &multisample;
   pipeline_info.pColorBlendState = &blend;
   pipeline_info.layout = pipeline_layout;
   pipeline_info.renderPass = render_pass;
   if ((result = create_graphics_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline)) !=
       VK_SUCCESS) {
      cleanup();
      return report("vkCreateGraphicsPipelines failed", result);
   }

   VkCommandPoolCreateInfo command_pool_info = {};
   command_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
   command_pool_info.queueFamilyIndex = queue_family;
   if ((result = create_command_pool(device, &command_pool_info, nullptr, &command_pool)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateCommandPool failed", result);
   }
   VkCommandBuffer command_buffer = VK_NULL_HANDLE;
   VkCommandBufferAllocateInfo command_buffer_info = {};
   command_buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   command_buffer_info.commandPool = command_pool;
   command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   command_buffer_info.commandBufferCount = 1;
   if ((result = allocate_command_buffers(device, &command_buffer_info, &command_buffer)) != VK_SUCCESS) {
      cleanup();
      return report("vkAllocateCommandBuffers failed", result);
   }
   VkCommandBufferBeginInfo begin_info = {};
   begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
   if ((result = begin_command_buffer(command_buffer, &begin_info)) != VK_SUCCESS) {
      cleanup();
      return report("vkBeginCommandBuffer failed", result);
   }

   VkClearValue clear = {};
   clear.color.float32[2] = 1.0f;
   clear.color.float32[3] = 1.0f;
   VkRenderPassBeginInfo render_begin = {};
   render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
   render_begin.renderPass = render_pass;
   render_begin.framebuffer = framebuffer;
   render_begin.renderArea.extent = { kWidth, kHeight };
   render_begin.clearValueCount = 1;
   render_begin.pClearValues = &clear;
   cmd_begin_render_pass(command_buffer, &render_begin, VK_SUBPASS_CONTENTS_INLINE);
   cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   cmd_draw(command_buffer, 3, 1, 0, 0);
   cmd_end_render_pass(command_buffer);

   VkImageMemoryBarrier image_barrier = {};
   image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
   image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   image_barrier.image = image;
   image_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   image_barrier.subresourceRange.levelCount = 1;
   image_barrier.subresourceRange.layerCount = 1;
   cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &image_barrier);

   VkBufferImageCopy copy = {};
   copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
   copy.imageSubresource.layerCount = 1;
   copy.imageExtent = { kWidth, kHeight, 1 };
   cmd_copy_image_to_buffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);

   VkBufferMemoryBarrier readback_barrier = {};
   readback_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
   readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   readback_barrier.buffer = readback;
   readback_barrier.size = VK_WHOLE_SIZE;
   cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                        &readback_barrier, 0, nullptr);
   if ((result = end_command_buffer(command_buffer)) != VK_SUCCESS) {
      cleanup();
      return report("vkEndCommandBuffer failed", result);
   }

   VkFenceCreateInfo fence_info = {};
   fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   if ((result = create_fence(device, &fence_info, nullptr, &fence)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateFence failed", result);
   }
   VkSubmitInfo submit_info = {};
   submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit_info.commandBufferCount = 1;
   submit_info.pCommandBuffers = &command_buffer;
   result = queue_submit(queue, 1, &submit_info, fence);
   if (result == VK_SUCCESS)
      submission_needs_quiesce = true;
   if (result != VK_SUCCESS || (result = wait_for_fences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs)) != VK_SUCCESS) {
      cleanup();
      return report("graphics submission or fence wait failed", result);
   }
   result = device_wait_idle(device);
   if (result == VK_SUCCESS)
      submission_needs_quiesce = false;
   if (result != VK_SUCCESS) {
      cleanup();
      return report("vkDeviceWaitIdle failed after graphics submission", result);
   }

   if ((readback_memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = readback_memory;
      range.size = VK_WHOLE_SIZE;
      if ((result = invalidate_mapped_memory_ranges(device, 1, &range)) != VK_SUCCESS) {
         cleanup();
         return report("vkInvalidateMappedMemoryRanges failed", result);
      }
   }

   const uint8_t expected[4] = { 255, 0, 0, 255 };
   const uint8_t *pixels = static_cast<const uint8_t *>(mapped);
   uint64_t checksum = 0;
   for (uint32_t pixel = 0; pixel < kWidth * kHeight; pixel++) {
      const uint8_t *actual = pixels + pixel * 4;
      for (uint32_t channel = 0; channel < 4; channel++)
         checksum += actual[channel];
      if (memcmp(actual, expected, sizeof(expected)) != 0) {
         fprintf(stderr, "tu WDDM Vulkan graphics probe: pixel %u is rgba(%u,%u,%u,%u), expected rgba(255,0,0,255)\n",
                 pixel, static_cast<unsigned>(actual[0]), static_cast<unsigned>(actual[1]),
                 static_cast<unsigned>(actual[2]), static_cast<unsigned>(actual[3]));
         cleanup();
         return 1;
      }
   }

   cleanup();
   printf("tu WDDM Vulkan graphics probe passed: %s, %ux%u RGBA8, checksum %llu\n", properties.deviceName, kWidth,
          kHeight, static_cast<unsigned long long>(checksum));
   return 0;
}
