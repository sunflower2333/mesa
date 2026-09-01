/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * A headless ARM64 Windows workload probe for the app-local Turnip ICD.
 * Unlike the lifecycle probe, this file submits a real compute command and
 * validates the result through a host-visible buffer.
 */

#define WIN32_LEAN_AND_MEAN
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <vulkan/vulkan_core.h>
#include <windows.h>

namespace {

constexpr uint32_t kDefaultElementCount = 256;
constexpr uint32_t kMaxElementCount = 64 * 1024 * 1024;
constexpr uint32_t kDefaultIterationCount = 1;
constexpr uint32_t kMaxIterationCount = 16;
constexpr uint64_t kFenceTimeoutNs = UINT64_C(10000000000);

bool
parse_positive_u32(const char *text, uint32_t maximum, uint32_t *value)
{
   if (text == nullptr || value == nullptr || text[0] == '\0')
      return false;

   char *end = nullptr;
   errno = 0;
   const unsigned long long parsed = _strtoui64(text, &end, 10);
   if (errno != 0 || end == text || *end != '\0' || parsed == 0 || parsed > maximum)
      return false;
   *value = static_cast<uint32_t>(parsed);
   return true;
}

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
   long size = ftell(file);
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
      fprintf(stderr, "tu WDDM Vulkan compute probe: %s\n", message);
   else
      fprintf(stderr, "tu WDDM Vulkan compute probe: %s (VkResult=%d)\n", message, result);
   return 1;
}

} // namespace

int
main(int argc, char **argv)
{
   uint32_t element_count = kDefaultElementCount;
   uint32_t iteration_count = kDefaultIterationCount;
   bool elements_seen = false;
   bool iterations_seen = false;
   if (argc < 2 || (argc % 2) != 0) {
      fprintf(stderr, "usage: %s compute.spv [--elements COUNT] [--iterations COUNT]\n", argv[0]);
      return 2;
   }
   for (int argument = 2; argument < argc; argument += 2) {
      if (strcmp(argv[argument], "--elements") == 0 && !elements_seen &&
          parse_positive_u32(argv[argument + 1], kMaxElementCount, &element_count)) {
         elements_seen = true;
      } else if (strcmp(argv[argument], "--iterations") == 0 && !iterations_seen &&
                 parse_positive_u32(argv[argument + 1], kMaxIterationCount, &iteration_count)) {
         iterations_seen = true;
      } else {
         fprintf(stderr, "usage: %s compute.spv [--elements COUNT] [--iterations COUNT]\n", argv[0]);
         return 2;
      }
   }

   std::vector<uint32_t> shader_code;
   if (!load_spirv(argv[1], &shader_code))
      return report("failed to read a valid SPIR-V compute shader", VK_SUCCESS);

   HMODULE icd = LoadLibraryW(L"vulkan_freedreno.dll");
   if (icd == nullptr) {
      fprintf(stderr, "tu WDDM Vulkan compute probe: LoadLibraryW failed (%lu)\n",
              static_cast<unsigned long>(GetLastError()));
      return 1;
   }

   VkInstance instance = VK_NULL_HANDLE;
   VkDevice device = VK_NULL_HANDLE;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
   VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
   VkShaderModule shader = VK_NULL_HANDLE;
   VkPipeline pipeline = VK_NULL_HANDLE;
   VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
   VkCommandPool command_pool = VK_NULL_HANDLE;
   VkFence fence = VK_NULL_HANDLE;
   void *mapped = nullptr;

   PFN_vkDestroyInstance destroy_instance = nullptr;
   PFN_vkDestroyDevice destroy_device = nullptr;
   PFN_vkUnmapMemory unmap_memory = nullptr;
   PFN_vkDestroyFence destroy_fence = nullptr;
   PFN_vkDestroyCommandPool destroy_command_pool = nullptr;
   PFN_vkDestroyDescriptorPool destroy_descriptor_pool = nullptr;
   PFN_vkDestroyPipeline destroy_pipeline = nullptr;
   PFN_vkDestroyShaderModule destroy_shader_module = nullptr;
   PFN_vkDestroyPipelineLayout destroy_pipeline_layout = nullptr;
   PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout = nullptr;
   PFN_vkDestroyBuffer destroy_buffer = nullptr;
   PFN_vkFreeMemory free_memory = nullptr;

   auto cleanup = [&]() {
      if (mapped != nullptr && unmap_memory != nullptr)
         unmap_memory(device, memory);
      if (destroy_fence != nullptr && fence != VK_NULL_HANDLE)
         destroy_fence(device, fence, nullptr);
      if (destroy_command_pool != nullptr && command_pool != VK_NULL_HANDLE)
         destroy_command_pool(device, command_pool, nullptr);
      if (destroy_descriptor_pool != nullptr && descriptor_pool != VK_NULL_HANDLE)
         destroy_descriptor_pool(device, descriptor_pool, nullptr);
      if (destroy_pipeline != nullptr && pipeline != VK_NULL_HANDLE)
         destroy_pipeline(device, pipeline, nullptr);
      if (destroy_shader_module != nullptr && shader != VK_NULL_HANDLE)
         destroy_shader_module(device, shader, nullptr);
      if (destroy_pipeline_layout != nullptr && pipeline_layout != VK_NULL_HANDLE)
         destroy_pipeline_layout(device, pipeline_layout, nullptr);
      if (destroy_descriptor_set_layout != nullptr && descriptor_layout != VK_NULL_HANDLE)
         destroy_descriptor_set_layout(device, descriptor_layout, nullptr);
      if (destroy_buffer != nullptr && buffer != VK_NULL_HANDLE)
         destroy_buffer(device, buffer, nullptr);
      if (free_memory != nullptr && memory != VK_NULL_HANDLE)
         free_memory(device, memory, nullptr);
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
   app.pApplicationName = "DroidVM Turnip WDDM compute probe";
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
   destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
   if (enumerate_physical_devices == nullptr || get_physical_device_properties == nullptr ||
       get_queue_family_properties == nullptr || get_memory_properties == nullptr || destroy_instance == nullptr) {
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
   VkPhysicalDevice physical = physical_devices[0];
   VkPhysicalDeviceProperties properties = {};
   get_physical_device_properties(physical, &properties);
   if (properties.vendorID != 0x5143 || strstr(properties.deviceName, "Adreno") == nullptr) {
      cleanup();
      return report("the selected device is not the expected Turnip Adreno adapter", VK_SUCCESS);
   }
   const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(element_count) * sizeof(uint32_t);
   if (buffer_size > properties.limits.maxStorageBufferRange) {
      cleanup();
      return report("requested storage buffer exceeds maxStorageBufferRange", VK_SUCCESS);
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
      if ((queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && queue_families[i].queueCount != 0) {
         queue_family = i;
         break;
      }
   }
   if (queue_family == UINT32_MAX) {
      cleanup();
      return report("no compute queue family is available", VK_SUCCESS);
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
   PFN_vkCreateDevice create_device = reinterpret_cast<PFN_vkCreateDevice>(get_proc(instance, "vkCreateDevice"));
   if (create_device == nullptr) {
      cleanup();
      return report("vkCreateDevice is missing", VK_SUCCESS);
   }
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
   PFN_vkCreateBuffer create_buffer = LOAD_DEVICE(CreateBuffer);
   PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = LOAD_DEVICE(GetBufferMemoryRequirements);
   PFN_vkAllocateMemory allocate_memory = LOAD_DEVICE(AllocateMemory);
   PFN_vkBindBufferMemory bind_buffer_memory = LOAD_DEVICE(BindBufferMemory);
   PFN_vkMapMemory map_memory = LOAD_DEVICE(MapMemory);
   unmap_memory = LOAD_DEVICE(UnmapMemory);
   PFN_vkFlushMappedMemoryRanges flush_mapped_memory_ranges = LOAD_DEVICE(FlushMappedMemoryRanges);
   PFN_vkInvalidateMappedMemoryRanges invalidate_mapped_memory_ranges = LOAD_DEVICE(InvalidateMappedMemoryRanges);
   PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout = LOAD_DEVICE(CreateDescriptorSetLayout);
   destroy_descriptor_set_layout = LOAD_DEVICE(DestroyDescriptorSetLayout);
   PFN_vkCreatePipelineLayout create_pipeline_layout = LOAD_DEVICE(CreatePipelineLayout);
   destroy_pipeline_layout = LOAD_DEVICE(DestroyPipelineLayout);
   PFN_vkCreateShaderModule create_shader_module = LOAD_DEVICE(CreateShaderModule);
   destroy_shader_module = LOAD_DEVICE(DestroyShaderModule);
   PFN_vkCreateComputePipelines create_compute_pipelines = LOAD_DEVICE(CreateComputePipelines);
   destroy_pipeline = LOAD_DEVICE(DestroyPipeline);
   PFN_vkCreateDescriptorPool create_descriptor_pool = LOAD_DEVICE(CreateDescriptorPool);
   destroy_descriptor_pool = LOAD_DEVICE(DestroyDescriptorPool);
   PFN_vkAllocateDescriptorSets allocate_descriptor_sets = LOAD_DEVICE(AllocateDescriptorSets);
   PFN_vkUpdateDescriptorSets update_descriptor_sets = LOAD_DEVICE(UpdateDescriptorSets);
   PFN_vkCreateCommandPool create_command_pool = LOAD_DEVICE(CreateCommandPool);
   destroy_command_pool = LOAD_DEVICE(DestroyCommandPool);
   PFN_vkAllocateCommandBuffers allocate_command_buffers = LOAD_DEVICE(AllocateCommandBuffers);
   PFN_vkBeginCommandBuffer begin_command_buffer = LOAD_DEVICE(BeginCommandBuffer);
   PFN_vkEndCommandBuffer end_command_buffer = LOAD_DEVICE(EndCommandBuffer);
   PFN_vkCmdBindPipeline cmd_bind_pipeline = LOAD_DEVICE(CmdBindPipeline);
   PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets = LOAD_DEVICE(CmdBindDescriptorSets);
   PFN_vkCmdDispatch cmd_dispatch = LOAD_DEVICE(CmdDispatch);
   PFN_vkCmdPipelineBarrier cmd_pipeline_barrier = LOAD_DEVICE(CmdPipelineBarrier);
   PFN_vkCreateFence create_fence = LOAD_DEVICE(CreateFence);
   destroy_fence = LOAD_DEVICE(DestroyFence);
   PFN_vkResetFences reset_fences = LOAD_DEVICE(ResetFences);
   PFN_vkQueueSubmit queue_submit = LOAD_DEVICE(QueueSubmit);
   PFN_vkWaitForFences wait_for_fences = LOAD_DEVICE(WaitForFences);
   destroy_buffer = LOAD_DEVICE(DestroyBuffer);
   free_memory = LOAD_DEVICE(FreeMemory);
   destroy_device = LOAD_DEVICE(DestroyDevice);
#undef LOAD_DEVICE
   if (get_device_queue == nullptr || create_buffer == nullptr || get_buffer_memory_requirements == nullptr ||
       allocate_memory == nullptr || bind_buffer_memory == nullptr || map_memory == nullptr ||
       unmap_memory == nullptr || flush_mapped_memory_ranges == nullptr || invalidate_mapped_memory_ranges == nullptr ||
       create_descriptor_set_layout == nullptr || destroy_descriptor_set_layout == nullptr ||
       create_pipeline_layout == nullptr || destroy_pipeline_layout == nullptr || create_shader_module == nullptr ||
       destroy_shader_module == nullptr || create_compute_pipelines == nullptr || destroy_pipeline == nullptr ||
       create_descriptor_pool == nullptr || destroy_descriptor_pool == nullptr || allocate_descriptor_sets == nullptr ||
       update_descriptor_sets == nullptr || create_command_pool == nullptr || destroy_command_pool == nullptr ||
       allocate_command_buffers == nullptr || begin_command_buffer == nullptr || end_command_buffer == nullptr ||
       cmd_bind_pipeline == nullptr || cmd_bind_descriptor_sets == nullptr || cmd_dispatch == nullptr ||
       cmd_pipeline_barrier == nullptr || create_fence == nullptr || destroy_fence == nullptr ||
       reset_fences == nullptr || queue_submit == nullptr || wait_for_fences == nullptr || destroy_buffer == nullptr ||
       free_memory == nullptr || destroy_device == nullptr) {
      cleanup();
      return report("required device entrypoints are missing", VK_SUCCESS);
   }

   VkQueue queue = VK_NULL_HANDLE;
   get_device_queue(device, queue_family, 0, &queue);
   if (queue == VK_NULL_HANDLE) {
      cleanup();
      return report("compute queue is null", VK_SUCCESS);
   }
   const uint64_t total_groups = (static_cast<uint64_t>(element_count) + 63) / 64;
   const uint64_t max_groups_x = properties.limits.maxComputeWorkGroupCount[0];
   if (max_groups_x == 0 || properties.limits.maxComputeWorkGroupCount[1] == 0) {
      cleanup();
      return report("compute workgroup limits are zero", VK_SUCCESS);
   }
   const uint64_t group_count_y_u64 = (total_groups + max_groups_x - 1) / max_groups_x;
   if (group_count_y_u64 == 0 || group_count_y_u64 > properties.limits.maxComputeWorkGroupCount[1]) {
      cleanup();
      return report("requested dispatch exceeds maxComputeWorkGroupCount", VK_SUCCESS);
   }
   const uint64_t group_count_x_u64 = (total_groups + group_count_y_u64 - 1) / group_count_y_u64;
   if (group_count_x_u64 == 0 || group_count_x_u64 > max_groups_x) {
      cleanup();
      return report("requested dispatch exceeds maxComputeWorkGroupCount", VK_SUCCESS);
   }
   const uint32_t group_count_x = static_cast<uint32_t>(group_count_x_u64);
   const uint32_t group_count_y = static_cast<uint32_t>(group_count_y_u64);

   VkBufferCreateInfo buffer_info = {};
   buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   buffer_info.size = buffer_size;
   buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   result = create_buffer(device, &buffer_info, nullptr, &buffer);
   if (result != VK_SUCCESS) {
      cleanup();
      return report("vkCreateBuffer failed", result);
   }
   VkMemoryRequirements requirements = {};
   get_buffer_memory_requirements(device, buffer, &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties = {};
   get_memory_properties(physical, &memory_properties);
   uint32_t memory_type = UINT32_MAX;
   VkMemoryPropertyFlags memory_flags = 0;
   for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
      const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[i].propertyFlags;
      if ((requirements.memoryTypeBits & (UINT32_C(1) << i)) != 0 &&
          (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) ==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
         memory_type = i;
         memory_flags = flags;
         if ((flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0)
            break;
      }
   }
   if (memory_type == UINT32_MAX) {
      cleanup();
      return report("no host-visible, host-cached storage-buffer memory type", VK_SUCCESS);
   }
   VkMemoryAllocateInfo allocation_info = {};
   allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   allocation_info.allocationSize = requirements.size;
   allocation_info.memoryTypeIndex = memory_type;
   result = allocate_memory(device, &allocation_info, nullptr, &memory);
   if (result == VK_SUCCESS)
      result = bind_buffer_memory(device, buffer, memory, 0);
   if (result == VK_SUCCESS)
      result = map_memory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
   if (result != VK_SUCCESS) {
      cleanup();
      return report("host-visible buffer allocation failed", result);
   }
   /* Seed the same storage buffer that the shader reads and writes.  The
    * result therefore proves the CPU-to-GPU handoff as well as readback. */
   for (uint32_t i = 0; i < element_count; i++)
      static_cast<uint32_t *>(mapped)[i] = UINT32_C(0x1000) + i * 5;
   if ((memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = memory;
      range.size = VK_WHOLE_SIZE;
      result = flush_mapped_memory_ranges(device, 1, &range);
      if (result != VK_SUCCESS) {
         cleanup();
         return report("vkFlushMappedMemoryRanges failed", result);
      }
   }

   VkDescriptorSetLayoutBinding binding = {};
   binding.binding = 0;
   binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   binding.descriptorCount = 1;
   binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
   VkDescriptorSetLayoutCreateInfo descriptor_layout_info = {};
   descriptor_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
   descriptor_layout_info.bindingCount = 1;
   descriptor_layout_info.pBindings = &binding;
   if ((result = create_descriptor_set_layout(device, &descriptor_layout_info, nullptr, &descriptor_layout)) !=
       VK_SUCCESS) {
      cleanup();
      return report("vkCreateDescriptorSetLayout failed", result);
   }
   VkPipelineLayoutCreateInfo pipeline_layout_info = {};
   pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
   pipeline_layout_info.setLayoutCount = 1;
   pipeline_layout_info.pSetLayouts = &descriptor_layout;
   if ((result = create_pipeline_layout(device, &pipeline_layout_info, nullptr, &pipeline_layout)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreatePipelineLayout failed", result);
   }
   VkShaderModuleCreateInfo shader_info = {};
   shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
   shader_info.codeSize = shader_code.size() * sizeof(uint32_t);
   shader_info.pCode = shader_code.data();
   if ((result = create_shader_module(device, &shader_info, nullptr, &shader)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateShaderModule failed", result);
   }
   VkComputePipelineCreateInfo pipeline_info = {};
   pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
   pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
   pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
   pipeline_info.stage.module = shader;
   pipeline_info.stage.pName = "main";
   pipeline_info.layout = pipeline_layout;
   if ((result = create_compute_pipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline)) !=
       VK_SUCCESS) {
      cleanup();
      return report("vkCreateComputePipelines failed", result);
   }

   VkDescriptorPoolSize pool_size = {};
   pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   pool_size.descriptorCount = 1;
   VkDescriptorPoolCreateInfo pool_info = {};
   pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
   pool_info.maxSets = 1;
   pool_info.poolSizeCount = 1;
   pool_info.pPoolSizes = &pool_size;
   if ((result = create_descriptor_pool(device, &pool_info, nullptr, &descriptor_pool)) != VK_SUCCESS) {
      cleanup();
      return report("vkCreateDescriptorPool failed", result);
   }
   VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
   VkDescriptorSetAllocateInfo set_info = {};
   set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
   set_info.descriptorPool = descriptor_pool;
   set_info.descriptorSetCount = 1;
   set_info.pSetLayouts = &descriptor_layout;
   if ((result = allocate_descriptor_sets(device, &set_info, &descriptor_set)) != VK_SUCCESS) {
      cleanup();
      return report("vkAllocateDescriptorSets failed", result);
   }
   VkDescriptorBufferInfo descriptor_buffer = {};
   descriptor_buffer.buffer = buffer;
   descriptor_buffer.range = VK_WHOLE_SIZE;
   VkWriteDescriptorSet write = {};
   write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
   write.dstSet = descriptor_set;
   write.dstBinding = 0;
   write.descriptorCount = 1;
   write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   write.pBufferInfo = &descriptor_buffer;
   update_descriptor_sets(device, 1, &write, 0, nullptr);

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
   begin_info.flags = iteration_count == 1 ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;
   if ((result = begin_command_buffer(command_buffer, &begin_info)) != VK_SUCCESS) {
      cleanup();
      return report("vkBeginCommandBuffer failed", result);
   }
   cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   cmd_bind_descriptor_sets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &descriptor_set, 0,
                            nullptr);
   VkBufferMemoryBarrier to_compute = {};
   to_compute.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
   to_compute.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
   to_compute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
   to_compute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   to_compute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   to_compute.buffer = buffer;
   to_compute.size = VK_WHOLE_SIZE;
   cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &to_compute, 0, nullptr);
   cmd_dispatch(command_buffer, group_count_x, group_count_y, 1);
   VkBufferMemoryBarrier to_host = {};
   to_host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
   to_host.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
   to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   to_host.buffer = buffer;
   to_host.size = VK_WHOLE_SIZE;
   cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                        1, &to_host, 0, nullptr);
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
   for (uint32_t iteration = 0; iteration < iteration_count; iteration++) {
      if (iteration != 0 && (result = reset_fences(device, 1, &fence)) != VK_SUCCESS) {
         cleanup();
         return report("vkResetFences failed", result);
      }
      if ((result = queue_submit(queue, 1, &submit_info, fence)) != VK_SUCCESS ||
          (result = wait_for_fences(device, 1, &fence, VK_TRUE, kFenceTimeoutNs)) != VK_SUCCESS) {
         cleanup();
         return report("compute submission or fence wait failed", result);
      }
   }
   if ((memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = memory;
      range.size = VK_WHOLE_SIZE;
      if ((result = invalidate_mapped_memory_ranges(device, 1, &range)) != VK_SUCCESS) {
         cleanup();
         return report("vkInvalidateMappedMemoryRanges failed", result);
      }
   }
   uint64_t checksum = 0;
   for (uint32_t i = 0; i < element_count; i++) {
      uint32_t expected = UINT32_C(0x1000) + i * 5;
      for (uint32_t iteration = 0; iteration < iteration_count; iteration++)
         expected = expected * 3 + 7;
      const uint32_t actual = static_cast<uint32_t *>(mapped)[i];
      if (actual != expected) {
         cleanup();
         fprintf(stderr, "tu WDDM Vulkan compute probe: element %u is %u, expected %u\n", i, actual, expected);
         return 1;
      }
      checksum += actual;
   }

   cleanup();
   printf("tu WDDM Vulkan compute probe passed: %s, elements %u, checksum %llu\n", properties.deviceName, element_count,
          static_cast<unsigned long long>(checksum));
   if (element_count != kDefaultElementCount || iteration_count != kDefaultIterationCount) {
      printf(
         "tu WDDM Vulkan coherence pressure passed: elements %u, iterations %u, bytes %llu, host-cached 1, host-coherent %u\n",
         element_count, iteration_count, static_cast<unsigned long long>(buffer_size),
         (memory_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0 ? 1U : 0U);
   }
   return 0;
}
