/* SPDX-License-Identifier: MIT */
/* Real Vulkan allocation contract probe. No rendering or GPU execution claim. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct device_api {
   VkDevice device = VK_NULL_HANDLE;
   PFN_vkDestroyDevice destroy = nullptr;
   PFN_vkCreateBuffer create_buffer = nullptr;
   PFN_vkDestroyBuffer destroy_buffer = nullptr;
   PFN_vkGetBufferMemoryRequirements requirements = nullptr;
   PFN_vkAllocateMemory allocate = nullptr;
   PFN_vkFreeMemory free_memory = nullptr;
   PFN_vkBindBufferMemory bind = nullptr;
   PFN_vkGetBufferDeviceAddress address = nullptr;
   PFN_vkGetDeviceMemoryOpaqueCaptureAddress capture = nullptr;
};

struct allocation {
   device_api *api;
   VkBuffer buffer = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
   explicit allocation(device_api *owner) : api(owner) {}
   void reset() {
      if (buffer) api->destroy_buffer(api->device, buffer, nullptr);
      if (memory) api->free_memory(api->device, memory, nullptr);
      buffer = VK_NULL_HANDLE;
      memory = VK_NULL_HANDLE;
   }
   ~allocation() { reset(); }
};

static bool parse_hex(const char *text, uint32_t *out)
{
   size_t length = strlen(text);
   uint32_t value = 0;
   if (!length || length > 8) return false;
   for (size_t i = 0; i < length; ++i) {
      unsigned digit;
      if (text[i] >= '0' && text[i] <= '9') digit = text[i] - '0';
      else if (text[i] >= 'a' && text[i] <= 'f') digit = text[i] - 'a' + 10;
      else if (text[i] >= 'A' && text[i] <= 'F') digit = text[i] - 'A' + 10;
      else return false;
      value = value * 16 + digit;
   }
   *out = value;
   return true;
}

static bool create_buffer(allocation *out, bool replay)
{
   VkBufferCreateInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
   info.size = 69632;
   info.usage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
   if (replay) info.flags = VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
   return out->api->create_buffer(out->api->device, &info, nullptr, &out->buffer) == VK_SUCCESS;
}

static bool allocate_bind(allocation *out, uint32_t type, uint32_t alignment,
                          uint64_t offset, bool replay, uint64_t opaque = 0)
{
   VkMemoryRequirements requirements = {};
   out->api->requirements(out->api->device, out->buffer, &requirements);
   if (!(requirements.memoryTypeBits & (1u << type)) || offset % requirements.alignment)
      return false;
   VkMemoryOpaqueCaptureAddressAllocateInfo capture = {};
   capture.sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO;
   capture.opaqueCaptureAddress = opaque;
   VkBufferDeviceAddressAlignmentAllocateInfoVALVE align = {};
   align.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_ALIGNMENT_ALLOCATE_INFO_VALVE;
   align.pNext = &capture;
   align.alignment = alignment;
   VkMemoryAllocateFlagsInfo flags = {};
   flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
   flags.pNext = &align;
   flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
   if (replay) flags.flags |= VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
   VkMemoryAllocateInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   info.pNext = &flags;
   info.allocationSize = requirements.size + offset;
   info.memoryTypeIndex = type;
   VkResult result = out->api->allocate(out->api->device, &info, nullptr, &out->memory);
   if (result == VK_SUCCESS)
      result = out->api->bind(out->api->device, out->buffer, out->memory, offset);
   if (result != VK_SUCCESS)
      fprintf(stderr, "FAIL allocation type=%u alignment=%u offset=%llu replay=%u result=%d\n",
              type, alignment, static_cast<unsigned long long>(offset), unsigned(replay), result);
   return result == VK_SUCCESS;
}

static uint64_t buffer_address(allocation *value)
{
   VkBufferDeviceAddressInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
   info.buffer = value->buffer;
   return value->api->address(value->api->device, &info);
}

static bool check_allocations(device_api *api, const VkPhysicalDeviceMemoryProperties &memory,
                              bool replay)
{
   uint32_t checked = 0, replayed = 0;
   for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
      const VkMemoryPropertyFlags properties = memory.memoryTypes[type].propertyFlags;
      if (properties & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT))
         continue;
      allocation query(api);
      if (!create_buffer(&query, false)) return false;
      VkMemoryRequirements requirements = {};
      api->requirements(api->device, query.buffer, &requirements);
      if (!(requirements.memoryTypeBits & (1u << type))) continue;
      query.reset();

      for (uint32_t alignment : {0u, 1u, 4096u, 8192u, 16384u, 32768u, 65536u}) {
         for (uint64_t offset : {0ull, 65536ull}) {
            allocation first(api), second(api);
            if (!create_buffer(&first, false) || !create_buffer(&second, false) ||
                !allocate_bind(&first, type, alignment, offset, false) ||
                !allocate_bind(&second, type, alignment, offset, false)) return false;
            for (auto *value : {&first, &second}) {
               uint64_t address = buffer_address(value);
               bool aligned = address && (!alignment || !((address - offset) % alignment));
               printf("%s Vulkan type=%u properties=0x%x alignment=%u bind_offset=%llu address=0x%llx\n",
                      aligned ? "ALIGNED" : "MISALIGNED", type, properties, alignment,
                      static_cast<unsigned long long>(offset), static_cast<unsigned long long>(address));
               if (!aligned) return false;
               checked++;
            }
            if (buffer_address(&first) == buffer_address(&second)) {
               fprintf(stderr, "FAIL simultaneously live allocations alias\n");
               return false;
            }
         }
      }
      if (replay) {
         allocation original(api), restored(api);
         if (!create_buffer(&original, true) || !allocate_bind(&original, type, 65536, 0, true))
            return false;
         const uint64_t address = buffer_address(&original);
         VkDeviceMemoryOpaqueCaptureAddressInfo capture = {};
         capture.sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO;
         capture.memory = original.memory;
         const uint64_t opaque = api->capture(api->device, &capture);
         if (!opaque || !address || address % 65536) return false;
         original.reset();
         /* Valid replay omits the original nonzero alignment request. */
         if (!create_buffer(&restored, true) || !allocate_bind(&restored, type, 0, 0, true, opaque) ||
             buffer_address(&restored) != address) return false;
         printf("PASS Vulkan capture replay type=%u address=0x%llx opaque=0x%llx\n", type,
                static_cast<unsigned long long>(address), static_cast<unsigned long long>(opaque));
         replayed++;
      }
   }
   printf("VULKAN_ALIGNMENT_CONTRACT=%s checked=%u replayed=%u; allocation-only validation\n",
          checked ? "PASS" : "FAIL", checked, replayed);
   return checked > 0 && (!replay || replayed > 0);
}

int main(int argc, char **argv)
{
   uint32_t luid[2], vendor, device_id;
   if (argc != 6 || strcmp(argv[1], "--adapter") || !parse_hex(argv[2], &luid[0]) ||
       !parse_hex(argv[3], &luid[1]) || !parse_hex(argv[4], &vendor) || !parse_hex(argv[5], &device_id)) {
      fprintf(stderr, "Usage: %s --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX\n", argv[0]);
      return 2;
   }
   HMODULE module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
   auto get_proc = module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      GetProcAddress(module, "vkGetInstanceProcAddr")) : nullptr;
   if (!get_proc) return 1;
   auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
   VkApplicationInfo app = {};
   app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
   app.pApplicationName = "Turnip WDDM BDA alignment contract";
   app.apiVersion = VK_API_VERSION_1_2;
   VkInstanceCreateInfo instance_info = {};
   instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
   instance_info.pApplicationInfo = &app;
   VkInstance instance = VK_NULL_HANDLE;
   if (!create_instance || create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS) return 1;
#define INSTANCE_PROC(name) auto name = reinterpret_cast<PFN_##name>(get_proc(instance, #name)); if (!name) return 1
   INSTANCE_PROC(vkDestroyInstance);
   INSTANCE_PROC(vkEnumeratePhysicalDevices);
   INSTANCE_PROC(vkGetPhysicalDeviceProperties2);
   INSTANCE_PROC(vkGetPhysicalDeviceFeatures2);
   INSTANCE_PROC(vkGetPhysicalDeviceMemoryProperties);
   INSTANCE_PROC(vkGetPhysicalDeviceQueueFamilyProperties);
   INSTANCE_PROC(vkEnumerateDeviceExtensionProperties);
   INSTANCE_PROC(vkCreateDevice);
   INSTANCE_PROC(vkGetDeviceProcAddr);
#undef INSTANCE_PROC
   uint32_t count = 0;
   if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || !count || count > 32) return 1;
   std::vector<VkPhysicalDevice> devices(count);
   if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) return 1;
   VkPhysicalDevice selected = VK_NULL_HANDLE;
   for (auto physical : devices) {
      VkPhysicalDeviceDriverProperties driver = {};
      driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      VkPhysicalDeviceIDProperties id = {};
      id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
      id.pNext = &driver;
      VkPhysicalDeviceProperties2 properties = {};
      properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      properties.pNext = &id;
      vkGetPhysicalDeviceProperties2(physical, &properties);
      if (id.deviceLUIDValid && !memcmp(id.deviceLUID, luid, sizeof(luid)) &&
          properties.properties.vendorID == vendor && properties.properties.deviceID == device_id &&
          driver.driverID == VK_DRIVER_ID_MESA_TURNIP) {
         if (selected) return 1;
         selected = physical;
         printf("Turnip %s LUID=%08x:%08x vendor=%08x device=%08x\n",
                properties.properties.deviceName, luid[1], luid[0], vendor, device_id);
      }
   }
   if (!selected) { fprintf(stderr, "FAIL exact Turnip adapter unavailable\n"); return 1; }
   if (vkEnumerateDeviceExtensionProperties(selected, nullptr, &count, nullptr) != VK_SUCCESS || count > 4096) return 1;
   std::vector<VkExtensionProperties> extensions(count);
   if (vkEnumerateDeviceExtensionProperties(selected, nullptr, &count, extensions.data()) != VK_SUCCESS) return 1;
   bool supported = false;
   for (const auto &extension : extensions)
      supported |= !strcmp(extension.extensionName, VK_VALVE_BUFFER_DEVICE_ADDRESS_ALLOCATION_ALIGNMENT_EXTENSION_NAME);
   if (!supported) { fprintf(stderr, "UNSUPPORTED Vulkan allocation alignment extension\n"); return 4; }
   VkPhysicalDeviceBufferDeviceAddressAllocationAlignmentFeaturesVALVE alignment = {};
   alignment.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_ALLOCATION_ALIGNMENT_FEATURES_VALVE;
   VkPhysicalDeviceBufferDeviceAddressFeatures bda = {};
   bda.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
   bda.pNext = &alignment;
   VkPhysicalDeviceFeatures2 features = {};
   features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
   features.pNext = &bda;
   vkGetPhysicalDeviceFeatures2(selected, &features);
   VkPhysicalDeviceBufferDeviceAddressAllocationAlignmentPropertiesVALVE limits = {};
   limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_ALLOCATION_ALIGNMENT_PROPERTIES_VALVE;
   VkPhysicalDeviceProperties2 properties = {};
   properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
   properties.pNext = &limits;
   vkGetPhysicalDeviceProperties2(selected, &properties);
   if (!alignment.bufferDeviceAddressAllocationAlignment || !bda.bufferDeviceAddress ||
       limits.maxBufferDeviceAddressAllocationAlignment < 65536) return 1;
   printf("Vulkan alignment feature=1 max=%u capture_replay=%u\n",
          limits.maxBufferDeviceAddressAllocationAlignment, bda.bufferDeviceAddressCaptureReplay);
   bda.bufferDeviceAddressMultiDevice = false;
   VkPhysicalDeviceMemoryProperties memory = {};
   vkGetPhysicalDeviceMemoryProperties(selected, &memory);
   vkGetPhysicalDeviceQueueFamilyProperties(selected, &count, nullptr);
   if (!count || count > 64) return 1;
   std::vector<VkQueueFamilyProperties> queues(count);
   vkGetPhysicalDeviceQueueFamilyProperties(selected, &count, queues.data());
   uint32_t family = 0;
   while (family < count && (!queues[family].queueCount || !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT))) family++;
   if (family == count) return 1;
   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {};
   queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   queue_info.queueFamilyIndex = family;
   queue_info.queueCount = 1;
   queue_info.pQueuePriorities = &priority;
   const char *extension = VK_VALVE_BUFFER_DEVICE_ADDRESS_ALLOCATION_ALIGNMENT_EXTENSION_NAME;
   VkDeviceCreateInfo info = {};
   info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   info.pNext = &bda;
   info.queueCreateInfoCount = 1;
   info.pQueueCreateInfos = &queue_info;
   info.enabledExtensionCount = 1;
   info.ppEnabledExtensionNames = &extension;
   device_api api;
   if (vkCreateDevice(selected, &info, nullptr, &api.device) != VK_SUCCESS) return 1;
#define DEVICE_PROC(field, name) api.field = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(api.device, #name)); if (!api.field) return 1
   DEVICE_PROC(destroy, vkDestroyDevice);
   DEVICE_PROC(create_buffer, vkCreateBuffer);
   DEVICE_PROC(destroy_buffer, vkDestroyBuffer);
   DEVICE_PROC(requirements, vkGetBufferMemoryRequirements);
   DEVICE_PROC(allocate, vkAllocateMemory);
   DEVICE_PROC(free_memory, vkFreeMemory);
   DEVICE_PROC(bind, vkBindBufferMemory);
   DEVICE_PROC(address, vkGetBufferDeviceAddress);
   DEVICE_PROC(capture, vkGetDeviceMemoryOpaqueCaptureAddress);
#undef DEVICE_PROC
   bool success = check_allocations(&api, memory, bda.bufferDeviceAddressCaptureReplay);
   api.destroy(api.device, nullptr);
   vkDestroyInstance(instance, nullptr);
   FreeLibrary(module);
   return success ? 0 : 1;
}
