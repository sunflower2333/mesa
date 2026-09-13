/* SPDX-License-Identifier: MIT
 * System-loader timestamp acceptance: exact Turnip LUID; DEVICE/QPC units,
 * conservative deviation, and actual CP query values bracketed by calibration.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../../../vulkan/runtime/vk_calibrated_time.h"

static bool hex(const char *text, uint32_t *value)
{
   *value = 0;
   size_t n = strlen(text);
   if (!n || n > 8) return false;
   for (size_t i = 0; i < n; i++) {
      unsigned c = static_cast<unsigned char>(text[i]);
      unsigned d = c >= '0' && c <= '9' ? c - '0' :
                   c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                   c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
      if (d == 16) return false;
      *value = *value * 16 + d;
   }
   return true;
}

static bool run_queries(VkDevice device, uint32_t family, PFN_vkGetDeviceProcAddr proc)
{
#define DEVICE(name) auto name = reinterpret_cast<PFN_##name>(proc(device, #name)); if (!name) return false
   DEVICE(vkGetCalibratedTimestampsKHR);
   DEVICE(vkCreateQueryPool); DEVICE(vkDestroyQueryPool); DEVICE(vkGetQueryPoolResults);
   DEVICE(vkCreateCommandPool); DEVICE(vkDestroyCommandPool); DEVICE(vkAllocateCommandBuffers);
   DEVICE(vkBeginCommandBuffer); DEVICE(vkEndCommandBuffer);
   DEVICE(vkCmdResetQueryPool); DEVICE(vkCmdWriteTimestamp);
   DEVICE(vkCreateFence); DEVICE(vkDestroyFence); DEVICE(vkResetFences); DEVICE(vkWaitForFences);
   DEVICE(vkGetDeviceQueue); DEVICE(vkQueueSubmit);
#undef DEVICE
   VkQueryPoolCreateInfo qi = {};
   qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
   qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 1;
   VkCommandPoolCreateInfo pi = {};
   pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.queueFamilyIndex = family;
   VkFenceCreateInfo fi = {}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
   struct Cleanup {
      VkDevice d; PFN_vkDestroyQueryPool dq; PFN_vkDestroyCommandPool dp; PFN_vkDestroyFence df;
      VkQueryPool q = VK_NULL_HANDLE; VkCommandPool p = VK_NULL_HANDLE; VkFence f = VK_NULL_HANDLE;
      ~Cleanup() { if (f) df(d, f, nullptr); if (p) dp(d, p, nullptr); if (q) dq(d, q, nullptr); }
   } cleanup{device, vkDestroyQueryPool, vkDestroyCommandPool, vkDestroyFence};
   if (vkCreateQueryPool(device, &qi, nullptr, &cleanup.q) != VK_SUCCESS ||
       vkCreateCommandPool(device, &pi, nullptr, &cleanup.p) != VK_SUCCESS ||
       vkCreateFence(device, &fi, nullptr, &cleanup.f) != VK_SUCCESS) return false;
   VkCommandBufferAllocateInfo ai = {};
   ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   ai.commandPool = cleanup.p; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
   VkCommandBuffer cmd = VK_NULL_HANDLE;
   if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) return false;
   VkCommandBufferBeginInfo bi = {}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
   vkCmdResetQueryPool(cmd, cleanup.q, 0, 1);
   vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, cleanup.q, 0);
   if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
   VkQueue queue = VK_NULL_HANDLE; vkGetDeviceQueue(device, family, 0, &queue);
   VkSubmitInfo submit = {}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
   submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
   VkCalibratedTimestampInfoKHR clocks[2] = {};
   clocks[0].sType = clocks[1].sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
   clocks[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
   clocks[1].timeDomain = VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
   LARGE_INTEGER hz;
   if (!QueryPerformanceFrequency(&hz) || hz.QuadPart <= 0) return false;
   auto calibrate = [&](uint64_t values[2], uint64_t *deviation) {
      LARGE_INTEGER begin, end;
      if (!QueryPerformanceCounter(&begin)) return false;
      VkResult result = vkGetCalibratedTimestampsKHR(device, 2, clocks, values, deviation);
      if (!QueryPerformanceCounter(&end)) return false;
      uint64_t outer = vk_time_calibrated_deviation(begin.QuadPart, end.QuadPart, hz.QuadPart, 53);
      bool valid = result == VK_SUCCESS && values[0] < (1ULL << 48) &&
                   values[1] >= static_cast<uint64_t>(begin.QuadPart) &&
                   values[1] <= static_cast<uint64_t>(end.QuadPart) &&
                   *deviation >= 53 && *deviation <= outer;
      printf("VK_CALIBRATION result=%d device=%llu qpc=%llu frequency=%lld max_deviation_ns=%llu"
             " external_bound_ns=%llu valid=%u\n", result,
             static_cast<unsigned long long>(values[0]), static_cast<unsigned long long>(values[1]),
             static_cast<long long>(hz.QuadPart), static_cast<unsigned long long>(*deviation),
             static_cast<unsigned long long>(outer), unsigned(valid));
      return valid;
   };
   uint64_t previous = 0;
   for (unsigned i = 0; i < 8; i++) {
      uint64_t before[2] = {}, after[2] = {}, deviation = 0, gpu = 0;
      if (!calibrate(before, &deviation) || vkResetFences(device, 1, &cleanup.f) != VK_SUCCESS ||
          vkQueueSubmit(queue, 1, &submit, cleanup.f) != VK_SUCCESS) return false;
      VkResult wait = vkWaitForFences(device, 1, &cleanup.f, VK_TRUE, 2000000000ULL);
      if (wait != VK_SUCCESS) {
         printf("FAIL bounded timestamp submission completion result=%d\n", wait);
         /* Avoid destroying resources while an unconfirmed submission owns
          * them. Process exit hands teardown to normal KMT/kernel ownership. */
         ExitProcess(1);
      }
      if (vkGetQueryPoolResults(device, cleanup.q, 0, 1, sizeof(gpu), &gpu, sizeof(gpu),
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || !calibrate(after, &deviation)) return false;
      gpu &= (1ULL << 48) - 1;
      uint64_t span = (after[0] - before[0]) & ((1ULL << 48) - 1);
      uint64_t position = (gpu - before[0]) & ((1ULL << 48) - 1);
      bool valid = span < (1ULL << 47) && position <= span &&
                   (!i || ((gpu - previous) & ((1ULL << 48) - 1)) < (1ULL << 47));
      printf("VK_CP_TIMESTAMP sample=%u before=%llu query=%llu after=%llu within_bracket=%u\n", i,
             static_cast<unsigned long long>(before[0]), static_cast<unsigned long long>(gpu),
             static_cast<unsigned long long>(after[0]), unsigned(valid));
      if (!valid) return false;
      previous = gpu;
      if (i != 7) Sleep(10);
   }
   puts("PASS Vulkan DEVICE/QPC units, maximum deviation and eight actual CP timestamp brackets");
   return true;
}

int main(int argc, char **argv)
{
   uint32_t luid[2], vendor, device_id;
   if (argc != 6 || strcmp(argv[1], "--adapter") || !hex(argv[2], &luid[0]) ||
       !hex(argv[3], &luid[1]) || !hex(argv[4], &vendor) || !hex(argv[5], &device_id)) {
      fprintf(stderr, "Usage: %s --adapter LUID_LOW_HEX LUID_HIGH_HEX VENDOR_HEX DEVICE_HEX\n", argv[0]); return 2;
   }
   HMODULE module = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
   auto proc = module ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr")) : nullptr;
   if (!proc) return 1;
   auto create = reinterpret_cast<PFN_vkCreateInstance>(proc(VK_NULL_HANDLE, "vkCreateInstance"));
   VkApplicationInfo app = {}; app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.apiVersion = VK_API_VERSION_1_2;
   VkInstanceCreateInfo ii = {}; ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ii.pApplicationInfo = &app;
   VkInstance instance = VK_NULL_HANDLE;
   if (!create || create(&ii, nullptr, &instance) != VK_SUCCESS) return 1;
#define INSTANCE(name) auto name = reinterpret_cast<PFN_##name>(proc(instance, #name)); if (!name) return 1
   INSTANCE(vkDestroyInstance); INSTANCE(vkEnumeratePhysicalDevices); INSTANCE(vkGetPhysicalDeviceProperties2);
   INSTANCE(vkEnumerateDeviceExtensionProperties); INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
   INSTANCE(vkGetPhysicalDeviceCalibrateableTimeDomainsKHR); INSTANCE(vkCreateDevice); INSTANCE(vkGetDeviceProcAddr);
#undef INSTANCE
   uint32_t count = 0;
   if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || !count || count > 32) return 1;
   std::vector<VkPhysicalDevice> devices(count);
   if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS) return 1;
   VkPhysicalDevice selected = VK_NULL_HANDLE;
   for (auto physical : devices) {
      VkPhysicalDeviceDriverProperties driver = {}; driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      VkPhysicalDeviceIDProperties id = {}; id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES; id.pNext = &driver;
      VkPhysicalDeviceProperties2 properties = {}; properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      properties.pNext = &id; vkGetPhysicalDeviceProperties2(physical, &properties);
      if (id.deviceLUIDValid && !memcmp(id.deviceLUID, luid, sizeof(luid)) && properties.properties.vendorID == vendor &&
          properties.properties.deviceID == device_id && driver.driverID == VK_DRIVER_ID_MESA_TURNIP) {
         if (selected || properties.properties.limits.timestampPeriod < 52.08f ||
             properties.properties.limits.timestampPeriod > 52.09f) return 1;
         selected = physical;
         printf("TIMESTAMP_ADAPTER name=%s period_ns=%.9f\n", properties.properties.deviceName,
                properties.properties.limits.timestampPeriod);
      }
   }
   if (!selected) return 1;
   if (vkEnumerateDeviceExtensionProperties(selected, nullptr, &count, nullptr) != VK_SUCCESS || count > 4096) return 1;
   std::vector<VkExtensionProperties> extensions(count);
   if (vkEnumerateDeviceExtensionProperties(selected, nullptr, &count, extensions.data()) != VK_SUCCESS) return 1;
   const char *extension = VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
   bool supported = false;
   for (const auto &ext : extensions) if (!strcmp(ext.extensionName, extension)) supported = true;
   if (!supported) { puts("UNSUPPORTED calibrated timestamps"); vkDestroyInstance(instance, nullptr); return 4; }
   if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(selected, &count, nullptr) != VK_SUCCESS || count > 16) return 1;
   std::vector<VkTimeDomainKHR> domains(count);
   if (vkGetPhysicalDeviceCalibrateableTimeDomainsKHR(selected, &count, domains.data()) != VK_SUCCESS) return 1;
   bool device_domain = false, qpc_domain = false;
   for (auto d : domains) {
      device_domain |= d == VK_TIME_DOMAIN_DEVICE_KHR;
      qpc_domain |= d == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
   }
   if (!device_domain || !qpc_domain) return 1;
   vkGetPhysicalDeviceQueueFamilyProperties(selected, &count, nullptr);
   if (!count || count > 64) return 1;
   std::vector<VkQueueFamilyProperties> queues(count);
   vkGetPhysicalDeviceQueueFamilyProperties(selected, &count, queues.data());
   uint32_t family = 0;
   while (family < count && (!queues[family].queueCount || queues[family].timestampValidBits != 48 ||
                             !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT))) family++;
   if (family == count) return 1;
   float priority = 1.0f;
   VkDeviceQueueCreateInfo qi = {}; qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
   qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &priority;
   VkDeviceCreateInfo di = {}; di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
   di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
   di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = &extension;
   VkDevice device = VK_NULL_HANDLE;
   if (vkCreateDevice(selected, &di, nullptr, &device) != VK_SUCCESS) return 1;
   auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(vkGetDeviceProcAddr(device, "vkDestroyDevice"));
   if (!destroy) return 1;
   bool passed = run_queries(device, family, vkGetDeviceProcAddr);
   destroy(device, nullptr); vkDestroyInstance(instance, nullptr); FreeLibrary(module);
   return passed ? 0 : 1;
}
