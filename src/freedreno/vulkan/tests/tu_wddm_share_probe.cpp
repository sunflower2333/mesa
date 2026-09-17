/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Cross-context zero-copy sharing probe for the WDDM Turnip ICD.
 *
 * Two VkDevices in one process own two native contexts. Device A allocates a
 * linear RGBA8 image and exports its memory as an OPAQUE_WIN32_KMT handle.
 * Device B imports that handle into an identical image. The probe then proves
 * both directions share the same pages, with no copy in between:
 *   1. A writes a pattern through its CPU mapping; B copies the image to a
 *      host-visible buffer on its own queue and must read the pattern.
 *   2. B clears the image on its own queue; A must read the clear color through
 *      its CPU mapping.
 */

#define WIN32_LEAN_AND_MEAN
#define VK_NO_PROTOTYPES
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

/* The probe drives the app-local ICD directly, like the other WDDM probes:
 * every entry point comes from vk_icdGetInstanceProcAddr. */
#define PROBE_VK_FUNCTIONS(X)                                                  \
   X(vkEnumeratePhysicalDevices)                                                \
   X(vkGetPhysicalDeviceImageFormatProperties2)                                 \
   X(vkGetPhysicalDeviceMemoryProperties)                                       \
   X(vkGetPhysicalDeviceQueueFamilyProperties)                                  \
   X(vkCreateDevice)                                                            \
   X(vkDestroyDevice)                                                           \
   X(vkDestroyInstance)                                                         \
   X(vkGetDeviceQueue)                                                          \
   X(vkCreateCommandPool)                                                       \
   X(vkDestroyCommandPool)                                                      \
   X(vkAllocateCommandBuffers)                                                  \
   X(vkFreeCommandBuffers)                                                      \
   X(vkBeginCommandBuffer)                                                      \
   X(vkEndCommandBuffer)                                                        \
   X(vkCreateFence)                                                             \
   X(vkDestroyFence)                                                            \
   X(vkQueueSubmit)                                                             \
   X(vkWaitForFences)                                                           \
   X(vkCreateImage)                                                             \
   X(vkDestroyImage)                                                            \
   X(vkGetImageMemoryRequirements)                                              \
   X(vkGetImageSubresourceLayout)                                               \
   X(vkBindImageMemory)                                                         \
   X(vkCreateBuffer)                                                            \
   X(vkDestroyBuffer)                                                           \
   X(vkGetBufferMemoryRequirements)                                             \
   X(vkBindBufferMemory)                                                        \
   X(vkAllocateMemory)                                                          \
   X(vkFreeMemory)                                                              \
   X(vkMapMemory)                                                               \
   X(vkUnmapMemory)                                                             \
   X(vkFlushMappedMemoryRanges)                                                 \
   X(vkInvalidateMappedMemoryRanges)                                            \
   X(vkGetMemoryWin32HandleKHR)                                                 \
   X(vkCmdPipelineBarrier)                                                      \
   X(vkCmdCopyImageToBuffer)                                                    \
   X(vkCmdClearColorImage)

#define PROBE_DECLARE(name) static PFN_##name name = nullptr;
PROBE_VK_FUNCTIONS(PROBE_DECLARE)
#undef PROBE_DECLARE
static PFN_vkCreateInstance vkCreateInstance = nullptr;

namespace {

constexpr uint32_t kSize = 256;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr uint64_t kFenceTimeoutNs = UINT64_C(10000000000);
constexpr VkExternalMemoryHandleTypeFlagBits kHandleType =
   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;

int
fail(const char *what, VkResult result = VK_SUCCESS)
{
   fprintf(stderr, "FAIL tu WDDM share probe: %s (VkResult=%d)\n", what, result);
   return 1;
}

uint8_t
pattern_byte(uint32_t x, uint32_t y, uint32_t c)
{
   return static_cast<uint8_t>((x * 7u + y * 13u + c * 61u) & 0xffu);
}

struct Device {
   VkDevice device = VK_NULL_HANDLE;
   VkQueue queue = VK_NULL_HANDLE;
   uint32_t queue_family = 0;
   VkCommandPool pool = VK_NULL_HANDLE;
   VkImage image = VK_NULL_HANDLE;
   VkDeviceMemory memory = VK_NULL_HANDLE;
};

bool
find_memory_type(VkPhysicalDevice physical, uint32_t allowed,
                 VkMemoryPropertyFlags required, uint32_t *index)
{
   VkPhysicalDeviceMemoryProperties props;
   vkGetPhysicalDeviceMemoryProperties(physical, &props);
   for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
      if ((allowed & (1u << i)) != 0 &&
          (props.memoryTypes[i].propertyFlags & required) == required) {
         *index = i;
         return true;
      }
   }
   return false;
}

VkResult
create_device(VkPhysicalDevice physical, Device *out)
{
   uint32_t family_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
   std::vector<VkQueueFamilyProperties> families(family_count);
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
   bool found = false;
   for (uint32_t i = 0; i < family_count; i++) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         out->queue_family = i;
         found = true;
         break;
      }
   }
   if (!found)
      return VK_ERROR_INITIALIZATION_FAILED;

   const float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = out->queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const char *extensions[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
   VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 1,
      .ppEnabledExtensionNames = extensions,
   };
   VkResult result = vkCreateDevice(physical, &device_info, nullptr, &out->device);
   if (result != VK_SUCCESS)
      return result;
   vkGetDeviceQueue(out->device, out->queue_family, 0, &out->queue);
   VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = out->queue_family,
   };
   return vkCreateCommandPool(out->device, &pool_info, nullptr, &out->pool);
}

VkResult
create_shared_image(Device *dev)
{
   VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = kHandleType,
   };
   VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = kFormat,
      .extent = {kSize, kSize, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   return vkCreateImage(dev->device, &info, nullptr, &dev->image);
}

/* Records one command buffer, submits it and waits for its fence. */
template <typename Record>
VkResult
run_commands(Device *dev, Record record)
{
   VkCommandBufferAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = dev->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   VkResult result = vkAllocateCommandBuffers(dev->device, &alloc, &cmd);
   if (result != VK_SUCCESS)
      return result;
   VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   result = vkBeginCommandBuffer(cmd, &begin);
   if (result == VK_SUCCESS) {
      record(cmd);
      result = vkEndCommandBuffer(cmd);
   }
   VkFence fence = VK_NULL_HANDLE;
   if (result == VK_SUCCESS) {
      VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      result = vkCreateFence(dev->device, &fence_info, nullptr, &fence);
   }
   if (result == VK_SUCCESS) {
      VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .commandBufferCount = 1,
         .pCommandBuffers = &cmd,
      };
      result = vkQueueSubmit(dev->queue, 1, &submit, fence);
   }
   if (result == VK_SUCCESS)
      result = vkWaitForFences(dev->device, 1, &fence, VK_TRUE, kFenceTimeoutNs);
   if (fence != VK_NULL_HANDLE)
      vkDestroyFence(dev->device, fence, nullptr);
   vkFreeCommandBuffers(dev->device, dev->pool, 1, &cmd);
   return result;
}

void
transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
   };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                        1, &barrier);
}

} // namespace

int
main(int argc, char **argv)
{
   const char *icd_path = argc > 1 ? argv[1] : "vulkan_freedreno.dll";
   HMODULE icd = LoadLibraryA(icd_path);
   if (icd == nullptr)
      return fail("load the Turnip ICD");
   const PFN_vkGetInstanceProcAddr get_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd, "vk_icdGetInstanceProcAddr"));
   if (get_proc == nullptr)
      return fail("vk_icdGetInstanceProcAddr is missing");
   vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(get_proc(VK_NULL_HANDLE, "vkCreateInstance"));
   if (vkCreateInstance == nullptr)
      return fail("vkCreateInstance is missing");

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "tu_wddm_share_probe",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   VkResult result = vkCreateInstance(&instance_info, nullptr, &instance);
   if (result != VK_SUCCESS)
      return fail("vkCreateInstance", result);
#define PROBE_LOAD(name)                                                       \
   name = reinterpret_cast<PFN_##name>(get_proc(instance, #name));              \
   if (name == nullptr)                                                         \
      return fail("missing entry point " #name);
   PROBE_VK_FUNCTIONS(PROBE_LOAD)
#undef PROBE_LOAD

   uint32_t physical_count = 1;
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   result = vkEnumeratePhysicalDevices(instance, &physical_count, &physical);
   if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || physical_count == 0)
      return fail("no physical device", result);

   VkPhysicalDeviceExternalImageFormatInfo external_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
      .handleType = kHandleType,
   };
   VkPhysicalDeviceImageFormatInfo2 format_query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .pNext = &external_query,
      .format = kFormat,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
   };
   VkExternalImageFormatProperties external_props = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
   };
   VkImageFormatProperties2 format_props = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
      .pNext = &external_props,
   };
   result = vkGetPhysicalDeviceImageFormatProperties2(physical, &format_query, &format_props);
   const VkExternalMemoryFeatureFlags features =
      external_props.externalMemoryProperties.externalMemoryFeatures;
   if (result != VK_SUCCESS ||
       (features & (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT |
                    VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) !=
          (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
      return fail("OPAQUE_WIN32_KMT linear RGBA8 images are not exportable and importable", result);

   Device a, b;
   result = create_device(physical, &a);
   if (result != VK_SUCCESS)
      return fail("create device A", result);
   result = create_device(physical, &b);
   if (result != VK_SUCCESS)
      return fail("create device B", result);

   /* Device A: exportable, host-visible memory. */
   result = create_shared_image(&a);
   if (result != VK_SUCCESS)
      return fail("create image A", result);
   VkMemoryRequirements req_a;
   vkGetImageMemoryRequirements(a.device, a.image, &req_a);
   uint32_t type_a = 0;
   if (!find_memory_type(physical, req_a.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type_a))
      return fail("no host-visible memory type for image A");
   VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = kHandleType,
   };
   VkMemoryDedicatedAllocateInfo dedicated_a = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &export_info,
      .image = a.image,
   };
   VkMemoryAllocateInfo alloc_a = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated_a,
      .allocationSize = req_a.size,
      .memoryTypeIndex = type_a,
   };
   result = vkAllocateMemory(a.device, &alloc_a, nullptr, &a.memory);
   if (result != VK_SUCCESS)
      return fail("allocate exportable memory A", result);
   result = vkBindImageMemory(a.device, a.image, a.memory, 0);
   if (result != VK_SUCCESS)
      return fail("bind image A", result);

   VkSubresourceLayout layout;
   VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
   vkGetImageSubresourceLayout(a.device, a.image, &subresource, &layout);
   void *map_a = nullptr;
   result = vkMapMemory(a.device, a.memory, 0, VK_WHOLE_SIZE, 0, &map_a);
   if (result != VK_SUCCESS)
      return fail("map memory A", result);
   const uint8_t *base_a = static_cast<uint8_t *>(map_a) + layout.offset;
   for (uint32_t y = 0; y < kSize; y++) {
      uint8_t *row = static_cast<uint8_t *>(map_a) + layout.offset + y * layout.rowPitch;
      for (uint32_t x = 0; x < kSize; x++)
         for (uint32_t c = 0; c < 4; c++)
            row[x * 4 + c] = pattern_byte(x, y, c);
   }
   VkMappedMemoryRange range_a = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = a.memory,
      .offset = 0,
      .size = VK_WHOLE_SIZE,
   };
   vkFlushMappedMemoryRanges(a.device, 1, &range_a);

   VkMemoryGetWin32HandleInfoKHR get_handle = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
      .memory = a.memory,
      .handleType = kHandleType,
   };
   HANDLE handle = nullptr;
   result = vkGetMemoryWin32HandleKHR(a.device, &get_handle, &handle);
   if (result != VK_SUCCESS || handle == nullptr)
      return fail("export KMT handle from A", result);
   printf("export key=0x%llx size=%llu rowPitch=%llu\n",
          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(handle)),
          static_cast<unsigned long long>(req_a.size),
          static_cast<unsigned long long>(layout.rowPitch));

   /* Device B: import the same pages into an identical image. */
   result = create_shared_image(&b);
   if (result != VK_SUCCESS)
      return fail("create image B", result);
   VkMemoryRequirements req_b;
   vkGetImageMemoryRequirements(b.device, b.image, &req_b);
   if (req_b.size != req_a.size)
      return fail("image memory requirements differ between devices");
   VkImportMemoryWin32HandleInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      .handleType = kHandleType,
      .handle = handle,
   };
   VkMemoryDedicatedAllocateInfo dedicated_b = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &import_info,
      .image = b.image,
   };
   VkMemoryAllocateInfo alloc_b = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated_b,
      .allocationSize = req_b.size,
      .memoryTypeIndex = type_a,
   };
   result = vkAllocateMemory(b.device, &alloc_b, nullptr, &b.memory);
   if (result != VK_SUCCESS)
      return fail("import KMT handle into B", result);
   result = vkBindImageMemory(b.device, b.image, b.memory, 0);
   if (result != VK_SUCCESS)
      return fail("bind image B", result);

   /* B reads what A's CPU wrote: copy the imported image to a readback buffer. */
   const VkDeviceSize readback_size = static_cast<VkDeviceSize>(kSize) * kSize * 4;
   VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = readback_size,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer readback = VK_NULL_HANDLE;
   result = vkCreateBuffer(b.device, &buffer_info, nullptr, &readback);
   if (result != VK_SUCCESS)
      return fail("create readback buffer B", result);
   VkMemoryRequirements req_rb;
   vkGetBufferMemoryRequirements(b.device, readback, &req_rb);
   uint32_t type_rb = 0;
   if (!find_memory_type(physical, req_rb.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type_rb))
      return fail("no host-visible memory type for readback");
   VkMemoryAllocateInfo alloc_rb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req_rb.size,
      .memoryTypeIndex = type_rb,
   };
   VkDeviceMemory readback_memory = VK_NULL_HANDLE;
   result = vkAllocateMemory(b.device, &alloc_rb, nullptr, &readback_memory);
   if (result == VK_SUCCESS)
      result = vkBindBufferMemory(b.device, readback, readback_memory, 0);
   if (result != VK_SUCCESS)
      return fail("allocate readback buffer B", result);

   result = run_commands(&b, [&](VkCommandBuffer cmd) {
      /* The pages already hold A's pattern; the probe relies on a layout
       * transition not touching image contents. */
      transition(cmd, b.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy copy = {
         .bufferOffset = 0,
         .bufferRowLength = 0,
         .bufferImageHeight = 0,
         .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
         .imageOffset = {0, 0, 0},
         .imageExtent = {kSize, kSize, 1},
      };
      vkCmdCopyImageToBuffer(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &copy);
   });
   if (result != VK_SUCCESS)
      return fail("device B copy of the imported image", result);
   void *map_rb = nullptr;
   result = vkMapMemory(b.device, readback_memory, 0, VK_WHOLE_SIZE, 0, &map_rb);
   if (result != VK_SUCCESS)
      return fail("map readback buffer B", result);
   uint32_t mismatches = 0;
   const uint8_t *pixels = static_cast<uint8_t *>(map_rb);
   for (uint32_t y = 0; y < kSize; y++)
      for (uint32_t x = 0; x < kSize; x++)
         for (uint32_t c = 0; c < 4; c++)
            mismatches += pixels[(y * kSize + x) * 4 + c] != pattern_byte(x, y, c);
   vkUnmapMemory(b.device, readback_memory);
   if (mismatches != 0) {
      fprintf(stderr, "B read %u mismatching bytes; first bytes %02x %02x %02x %02x\n",
              mismatches, pixels[0], pixels[1], pixels[2], pixels[3]);
      return fail("device B does not see device A's CPU writes");
   }
   printf("PASS device B read device A's pattern through the shared pages (%u bytes)\n",
          kSize * kSize * 4);

   /* A reads what B's GPU wrote: clear the imported image on B. */
   result = run_commands(&b, [&](VkCommandBuffer cmd) {
      transition(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue color = {};
      color.float32[0] = 1.0f;
      color.float32[1] = 0.0f;
      color.float32[2] = 1.0f;
      color.float32[3] = 1.0f;
      VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &whole);
   });
   if (result != VK_SUCCESS)
      return fail("device B clear of the imported image", result);
   vkInvalidateMappedMemoryRanges(a.device, 1, &range_a);
   mismatches = 0;
   for (uint32_t y = 0; y < kSize; y++) {
      const uint8_t *row = base_a + y * layout.rowPitch;
      for (uint32_t x = 0; x < kSize; x++)
         mismatches += row[x * 4] != 0xff || row[x * 4 + 1] != 0x00 ||
                       row[x * 4 + 2] != 0xff || row[x * 4 + 3] != 0xff;
   }
   if (mismatches != 0) {
      fprintf(stderr, "A read %u mismatching pixels; first pixel %02x %02x %02x %02x\n",
              mismatches, base_a[0], base_a[1], base_a[2], base_a[3]);
      return fail("device A does not see device B's GPU writes");
   }
   printf("PASS device A read device B's clear through the shared pages (%u pixels)\n",
          kSize * kSize);

   vkUnmapMemory(a.device, a.memory);
   vkDestroyBuffer(b.device, readback, nullptr);
   vkFreeMemory(b.device, readback_memory, nullptr);
   vkDestroyImage(b.device, b.image, nullptr);
   vkFreeMemory(b.device, b.memory, nullptr);
   vkDestroyImage(a.device, a.image, nullptr);
   vkFreeMemory(a.device, a.memory, nullptr);
   vkDestroyCommandPool(b.device, b.pool, nullptr);
   vkDestroyCommandPool(a.device, a.pool, nullptr);
   vkDestroyDevice(b.device, nullptr);
   vkDestroyDevice(a.device, nullptr);
   vkDestroyInstance(instance, nullptr);
   printf("PASS tu WDDM share probe: zero-copy cross-context import, both directions\n");
   return 0;
}
