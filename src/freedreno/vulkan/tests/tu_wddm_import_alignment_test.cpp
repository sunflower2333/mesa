/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include "vulkan/vulkan_core.h"
#include "mesa_wddm_runtime.h"
enum tu_bo_alloc_flags { TU_BO_ALLOC_NO_FLAGS = 0, TU_BO_ALLOC_BDA_64K = 128, TU_BO_ALLOC_GPU_READ_ONLY = 8 };
#define BITMASK_ENUM(E) enum E
static tu_bo_alloc_flags &operator|=(tu_bo_alloc_flags &a, tu_bo_alloc_flags b) {
   a = static_cast<tu_bo_alloc_flags>(static_cast<unsigned>(a) | static_cast<unsigned>(b)); return a;
}
struct fixture_next {
   const void *value;
   template<typename T> operator const T *() const { return static_cast<const T *>(value); }
   explicit operator bool() const { return value != nullptr; }
};
static fixture_next fixture_find(const void *chain, VkStructureType type) {
   auto *next = static_cast<const VkBaseInStructure *>(chain);
   while (next && next->sType != type) next = next->pNext;
   return {next};
}
#define vk_find_struct_const(chain, type) fixture_find(chain, VK_STRUCTURE_TYPE_##type)
struct tu_instance { bool wddm; };
static bool is_wddm(tu_instance *instance) { return instance->wddm; }
struct tu_physical_device {
   tu_instance *instance;
   struct { uint32_t types[2]; } memory;
};
struct tu_device {
   tu_instance *instance;
   tu_physical_device *physical_device;
   struct { struct { bool bufferDeviceAddressAllocationAlignment; } enabled_features; } vk;
};
constexpr uint32_t VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE = 2;
struct allocation { struct { uint32_t Flags; } private_info; };
struct tu_bo { uint64_t iova; allocation *wddm_allocation; unsigned refs; };
struct tu_device_memory { struct { void *ahardware_buffer; } vk; uint64_t size; tu_bo *bo; };
static unsigned import_calls, release_calls;
static VkResult import_result = VK_SUCCESS;
static void *expected_owner, *expected_token;
static tu_bo *backing;
static void check(bool valid, const char *message) {
   if (!valid) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}
static VkResult tu_wddm_bo_import_runtime(tu_device *, tu_bo **out, uint64_t size, void *owner, void *token) {
   check(owner == expected_owner && token == expected_token && size == 65536, "exact import owner token and size");
   ++import_calls; *out = nullptr;
   if (import_result == VK_SUCCESS) { *out = backing; ++backing->refs; }
   return import_result;
}
static void tu_bo_finish(tu_device *, tu_bo *bo) {
   check(bo == backing && bo->refs > 1, "owned import reference released once");
   --bo->refs; ++release_calls;
}
// PRODUCTION_FUNCTIONS
int main() {
   int owner, token;
   expected_owner = &owner; expected_token = &token;
   tu_instance instance{true};
   tu_physical_device physical{&instance, {{VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT}}};
   tu_device device{&instance, &physical, {{true}}};
   allocation allocation{{6}}; tu_bo bo{0x10010000, &allocation, 1}; backing = &bo;
   mwd_import_memory_info import{MWD_STYPE_IMPORT, nullptr, &owner, &token};
   VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &import,
      VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
   VkBufferDeviceAddressAlignmentAllocateInfoVALVE alignment{
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_ALIGNMENT_ALLOCATE_INFO_VALVE, &flags, 65536};
   VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &alignment, 65536, 1};
   for (uint32_t requested : {0u, 1u, 4096u, 8192u, 16384u, 32768u, 65536u}) {
      alignment.alignment = requested;
      tu_bo_alloc_flags parsed = TU_BO_ALLOC_NO_FLAGS;
      check(tu_memory_bda_alignment(&device, &info, &parsed) == VK_SUCCESS, "actual BDA parser accepts request");
      tu_device_memory mem{{}, 65536, nullptr};
      check(tu_memory_import_runtime(&device, &mem, &info, &import, parsed, false) == VK_SUCCESS,
            "aligned native runtime import accepted");
      check(mem.bo == &bo && bo.iova == 0x10010000 && bo.refs == 2 && mem.size == 65536,
            "import preserves exact backing address size and ownership");
      tu_bo_finish(&device, mem.bo);
   }
   alignment.alignment = 65536; bo.iova += 4096;
   tu_device_memory mem{{}, 65536, nullptr};
   const auto releases = release_calls;
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_BDA_64K, false) ==
         VK_ERROR_INVALID_EXTERNAL_HANDLE && !mem.bo && bo.refs == 1 && release_calls == releases + 1,
         "misaligned imported address rejected and released");
   // A 4 KiB request must not be incorrectly strengthened to 64 KiB on import.
   alignment.alignment = 4096;
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_BDA_64K, false) == VK_SUCCESS,
         "smaller alignment preserves exact valid imported address");
   tu_bo_finish(&device, mem.bo); mem.bo = nullptr;
   bo.iova -= 4096; alignment.alignment = 65536;
   const auto imports = import_calls;
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_GPU_READ_ONLY, false) ==
         VK_ERROR_INVALID_EXTERNAL_HANDLE && import_calls == imports, "unrelated allocation flag rejected before import");
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_BDA_64K, true) ==
         VK_ERROR_INVALID_EXTERNAL_HANDLE && import_calls == imports, "mixed fd import rejected");
   mem.vk.ahardware_buffer = &owner;
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_BDA_64K, false) ==
         VK_ERROR_INVALID_EXTERNAL_HANDLE && import_calls == imports, "mixed AHB import rejected");
   mem.vk.ahardware_buffer = nullptr;
   allocation.private_info.Flags = 4;
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_BDA_64K, false) ==
         VK_ERROR_INVALID_EXTERNAL_HANDLE && !mem.bo && bo.refs == 1, "CPU visibility rejection releases imported reference");
   allocation.private_info.Flags = 6;
   import_result = VK_ERROR_DEVICE_LOST;
   check(tu_memory_import_runtime(&device, &mem, &info, &import, TU_BO_ALLOC_BDA_64K, false) ==
         VK_ERROR_DEVICE_LOST && !mem.bo && bo.refs == 1, "underlying import failure preserved");
   std::puts("PASS production native import alignment, exact backing ownership, rejected address cleanup and error preservation");
   return 0;
}
