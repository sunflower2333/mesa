/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "vulkan/vulkan_core.h"
#include "tu_wddm_abi.h"
#include "tu_wddm_lifetime.h"
#include "tu_wddm_residency.h"
#include "mesa_wddm_runtime.h"
#include "util/vma.h"

using NTSTATUS = int32_t;
using D3DKMT_HANDLE = uint32_t;
using LONG64 = int64_t;
#define NT_SUCCESS(status) ((status) >= 0)
static constexpr NTSTATUS TU_WDDM_STATUS_SUCCESS = 0;
static constexpr NTSTATUS TU_WDDM_STATUS_INVALID_DEVICE_STATE = static_cast<NTSTATUS>(0xc0000184u);
static constexpr NTSTATUS TU_WDDM_STATUS_DEVICE_BUSY = static_cast<NTSTATUS>(0x80000011u);
static constexpr uint32_t TU_WDDM_CREATE_BUSY_RETRIES = 1000;
static constexpr uint32_t TU_WDDM_MAX_RENDER_ALLOCATIONS = 1024;
static constexpr uint64_t os_page_size = 4096;
struct D3DDDI_ALLOCATIONINFO { void *pPrivateDriverData; uint32_t PrivateDriverDataSize, hAllocation; };
struct D3DKMT_CREATEALLOCATION {
   uint32_t hDevice, NumAllocations;
   D3DDDI_ALLOCATIONINFO *pAllocationInfo;
   struct { uint32_t NonSecure; } Flags;
};
struct D3DKMT_DESTROYALLOCATION2 {
   uint32_t hDevice;
   D3DKMT_HANDLE *phAllocationList;
   uint32_t AllocationCount;
   struct { uint32_t AssumeNotInUse; } Flags;
};
struct D3DDDI_MAKERESIDENT {
   D3DKMT_HANDLE hPagingQueue; uint32_t NumAllocations; const D3DKMT_HANDLE *AllocationList;
   const uint32_t *PriorityList; struct { uint32_t CantTrimFurther : 1; uint32_t MustSucceed : 1; } Flags;
   uint64_t PagingFenceValue, NumBytesToTrim;
};
struct tu_wddm_runtime {
   struct {
      NTSTATUS (*CreateAllocation)(D3DKMT_CREATEALLOCATION *);
      NTSTATUS (*DestroyAllocation2)(D3DKMT_DESTROYALLOCATION2 *);
      NTSTATUS (*MakeResident)(D3DDDI_MAKERESIDENT *);
   } dispatch;
};
struct tu_wddm_device {
   mwd_callbacks callbacks;
   void *runtime_owner;
   struct { tu_wddm_runtime *runtime; VIOGPU_WDDM_ADAPTER_INFO private_info; uint32_t driver_version; } adapter;
   D3DKMT_HANDLE handle;
   D3DKMT_HANDLE paging_queue;
   volatile LONG64 pending_paging_fence;
};
static LONG64 InterlockedCompareExchange64(LONG64 volatile *target, LONG64 exchange, LONG64 comparand) {
   LONG64 observed = *target;
   if (observed == comparand) *target = exchange;
   return observed;
}
struct tu_wddm_context {
   tu_wddm_device *device;
   D3DKMT_HANDLE handle;
   VIOGPU_WDDM_CONTEXT_INFO info;
};
// PRODUCTION_STRUCTS

enum tu_bo_alloc_flags {
   TU_BO_ALLOC_DMABUF = 1, TU_BO_ALLOC_SHAREABLE = 2,
   TU_BO_ALLOC_IMPLICIT_SYNC = 4, TU_BO_ALLOC_GPU_READ_ONLY = 8,
   TU_BO_ALLOC_BDA_64K = 128,
};
struct vk_object_base {};
struct tu_sparse_vma {};
struct vk_device {
   bool lost;
   int alloc;
   struct { bool bufferDeviceAddressAllocationAlignment; } enabled_features;
};
struct tu_instance { bool wddm; };
struct tu_physical_device { tu_instance *instance; };
static bool is_wddm(tu_instance *instance) { return instance->wddm; }
struct fixture_next {
   const void *value;
   template <typename T> operator const T *() const { return static_cast<const T *>(value); }
};
static fixture_next fixture_find(const void *chain, VkStructureType type) {
   auto *next = static_cast<const VkBaseInStructure *>(chain);
   while (next && next->sType != type) next = next->pNext;
   return {next};
}
#define vk_find_struct_const(chain, type) fixture_find(chain, VK_STRUCTURE_TYPE_##type)
#define BITMASK_ENUM(E) enum E
static tu_bo_alloc_flags &operator|=(tu_bo_alloc_flags &a, tu_bo_alloc_flags b) {
   a = static_cast<tu_bo_alloc_flags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
   return a;
}
struct tu_bo {
   uint32_t gem_handle;
   uint64_t size, iova;
   const char *name;
   int refcnt;
   bool gpu_read_only, never_unmap;
   vk_object_base *base;
   tu_wddm_allocation *wddm_allocation;
};
struct tu_device {
   vk_device vk;
   int vma_mutex, bo_mutex;
   /* Deferred BO retirement: tu_wddm_bo_init reaps opportunistically under
    * this lock before it takes IOVA, so the fixture models both. */
   int wddm_mutex;
   uint32_t wddm_retired_count;
   bool wddm_reap_fails;
   uint32_t wddm_reap_calls;
   util_vma_heap vma;
   tu_physical_device *physical_device;
   tu_wddm_device wddm_device;
   tu_wddm_context wddm_context;
   bool wddm_initialized;
   void *wddm_runtime_owner;
   uint32_t wddm_bo_count;
   tu_bo *wddm_bos[1];
   uint32_t adapter_driver_version() const { return wddm_device.adapter.driver_version; }
};
/* Models tu_wddm_reap_retired_bos_locked for this fixture: an empty
 * retirement list is a cheap success and a refused reap must fail the caller
 * before any IOVA or accounting is taken. */
static bool tu_wddm_reap_retired_bos_locked(tu_device *dev, uint32_t budget) {
   (void)budget;
   dev->wddm_reap_calls++;
   if (dev->wddm_reap_fails)
      return false;
   dev->wddm_retired_count = 0;
   return true;
}
struct state {
   uint32_t create_status, destroy_status, returned_handle;
   bool context_active, execution_active, lose_during_create, lose_during_health_query, local_alloc_fail;
   uint32_t create_calls, destroy_calls, sleeps, context_queries, execution_queries;
   uint32_t make_resident_calls, make_resident_status[4];
   uint32_t allocations, frees, vmas, vma_frees, names;
   uint64_t requested_iova, backing_alignment;
   uint64_t shared_size, shared_address, shared_alignment;
   uint32_t shared_calls, shared_refs, shared_releases;
   bool shared_misaligned;
   tu_bo slot;
} current;
static unsigned checks, failures;
static tu_device *active_device;
static void check(bool value, const char *label) {
   checks++;
   if (!value) { failures++; printf("FAIL %s\n", label); }
}
template <typename T> static uint32_t tu_wddm_sizeof() { return sizeof(T); }
static void tu_wddm_diag(const char *, ...) {}
static bool tu_wddm_diag_enabled() { return false; }
static void Sleep(uint32_t) { current.sleeps++; }
static bool vk_device_is_lost(vk_device *device) { return device->lost; }
static VkResult vk_device_set_lost(vk_device *device, const char *, ...) {
   device->lost = true; return VK_ERROR_DEVICE_LOST;
}
static VkResult vk_error(tu_device *, VkResult result) { return result; }
static VkResult vk_errorf(tu_device *, VkResult result, const char *, ...) { return result; }
static bool tu_wddm_context_get_completed_fence(tu_wddm_context *, uint32_t *completed) {
   current.context_queries++; *completed = 0; return current.context_active;
}
static bool tu_wddm_device_execution_active(tu_wddm_device *) {
   current.execution_queries++;
   if (current.lose_during_health_query) active_device->vk.lost = true;
   return current.execution_active;
}
static void mtx_lock(int *mutex) { check((*mutex)++ == 0, "no recursive bookkeeping lock"); }
static void mtx_unlock(int *mutex) { check(--(*mutex) == 0, "balanced bookkeeping lock"); }
static uint64_t fixture_vma_alloc(util_vma_heap *heap, uint64_t size, uint64_t alignment) {
   uint64_t address = util_vma_heap_alloc(heap, size, alignment);
   current.vmas += address != 0;
   return address;
}
static bool fixture_vma_alloc_addr(util_vma_heap *heap, uint64_t address, uint64_t size) {
   bool success = util_vma_heap_alloc_addr(heap, address, size);
   current.vmas += success;
   return success;
}
static void fixture_vma_free(util_vma_heap *heap, uint64_t address, uint64_t size) {
   current.vma_frees++;
   util_vma_heap_free(heap, address, size);
}
static void *vk_zalloc(int *, size_t size, size_t, int) {
   if (current.local_alloc_fail) return nullptr;
   current.allocations++; return calloc(1, size);
}
static void vk_free(int *, void *memory) { current.frees++; free(memory); }
static uint32_t tu_wddm_alloc_token_locked(tu_device *) { return 1; }
static tu_bo *tu_device_lookup_bo(tu_device *, uint32_t) { return &current.slot; }
static bool tu_wddm_add_bo_locked(tu_device *dev, tu_bo *bo) {
   dev->wddm_bos[dev->wddm_bo_count++] = bo; return true;
}
static const char *tu_debug_bos_add(tu_device *, uint64_t, const char *name) { current.names++; return name; }
static int p_atomic_read(int *value) { return *value; }
static void p_atomic_inc(int *value) { ++*value; }
static void tu_dump_bo_init(tu_device *, tu_bo *) {}
using BOOL = int;
struct MEMORYSTATUSEX {
   uint32_t dwLength;
   uint64_t ullAvailPageFile, ullTotalPageFile, ullAvailPhys, ullTotalPhys;
};
static BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *) { return 0; }
static uint64_t GetTickCount64() { return 0; }
static unsigned long GetCurrentProcessId() { return 0; }
static NTSTATUS create_allocation(D3DKMT_CREATEALLOCATION *create) {
   current.create_calls++;
   check(active_device->bo_mutex == 0 && active_device->vma_mutex == 0,
         "create has no bookkeeping mutex held");
   check(create->Flags.NonSecure == 1 && create->NumAllocations == 1,
         "production create flags and count");
   create->pAllocationInfo->hAllocation = current.returned_handle;
   const auto *info = static_cast<const VIOGPU_WDDM_ALLOCATION_INFO *>(create->pAllocationInfo->pPrivateDriverData);
   current.requested_iova = info->RequestedIova;
   current.backing_alignment = info->Alignment;
   if (current.lose_during_create) active_device->vk.lost = true;
   return static_cast<NTSTATUS>(current.create_status);
}
static NTSTATUS make_resident(D3DDDI_MAKERESIDENT *request) {
   check(active_device->adapter_driver_version() >= 2000 && request->hPagingQueue == 0x50 &&
         request->NumAllocations == 1 && *request->AllocationList == current.returned_handle,
         "only WDDM 2.0 allocations are made resident, on the device paging queue");
   const uint32_t status = current.make_resident_calls < 4 ? current.make_resident_status[current.make_resident_calls] : 0;
   current.make_resident_calls++;
   return static_cast<NTSTATUS>(status);
}
static NTSTATUS destroy_allocation(D3DKMT_DESTROYALLOCATION2 *destroy) {
   current.destroy_calls++;
   check(destroy->Flags.AssumeNotInUse == 0 && destroy->AllocationCount == 1 &&
         *destroy->phAllocationList == current.returned_handle,
         "compensating destroy preserves VidMm ownership checks");
   return static_cast<NTSTATUS>(current.destroy_status);
}
#define util_vma_heap_alloc fixture_vma_alloc
#define util_vma_heap_alloc_addr fixture_vma_alloc_addr
#define util_vma_heap_free fixture_vma_free
// PRODUCTION_FUNCTIONS
#undef util_vma_heap_alloc
#undef util_vma_heap_alloc_addr
#undef util_vma_heap_free

static tu_wddm_runtime runtime = {{create_allocation, destroy_allocation, make_resident}};
static int32_t MWD_CALL shared_allocate(void *owner, uint64_t size, uint64_t alignment,
                                      uint64_t address, uint32_t flags, mwd_allocation *reply) {
   check(owner == active_device && !active_device->bo_mutex && !active_device->vma_mutex,
         "runtime allocation preserves owner outside bookkeeping locks");
   check(!address && flags == 14, "runtime allocation preserves native CPU visible read only flags");
   ++current.shared_calls;
   current.shared_alignment = alignment;
   current.shared_size = size;
   current.shared_address = util_vma_heap_alloc(&active_device->vma, size,
                                               current.shared_misaligned ? 4096 : alignment);
   if (!current.shared_address) return static_cast<int32_t>(0x8007000eu);
   ++current.shared_refs;
   *reply = {&current, current.shared_address, size, 7, 101, flags};
   return 0;
}
static int32_t MWD_CALL shared_release(void *owner, void *token) {
   check(owner == active_device && token == &current && current.shared_refs == 1,
         "runtime allocation failure releases exact retained token once");
   --current.shared_refs; ++current.shared_releases;
   util_vma_heap_free(&active_device->vma, current.shared_address, current.shared_size);
   return 0;
}
static tu_instance instance = {true};
static tu_physical_device physical = {&instance};
static bool vma_initialized;
static void init(tu_device *dev, uint32_t status = 0) {
   if (vma_initialized) util_vma_heap_finish(&dev->vma);
   memset(&current, 0, sizeof(current)); memset(dev, 0, sizeof(*dev));
   current.create_status = status; current.context_active = current.execution_active = true;
   dev->wddm_initialized = true;
   dev->wddm_device.adapter.runtime = &runtime;
   dev->wddm_device.handle = 2; dev->wddm_device.adapter.private_info.ResetGeneration = 7;
   dev->wddm_context.device = &dev->wddm_device; dev->wddm_context.handle = 3;
   auto &info = dev->wddm_context.info;
   tu_wddm_init_header(&info.Header, sizeof(info));
   info.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
   info.ExpectedResetGeneration = info.ResetGeneration = 7;
   info.ContextId = 11; info.SubmitQueueId = 17;
   info.VaStart = 0x100000000ull; info.VaSize = 0x01000000;
   util_vma_heap_init(&dev->vma, info.VaStart, info.VaSize);
   vma_initialized = true;
   instance.wddm = true;
   dev->physical_device = &physical;
   dev->vk.enabled_features.bufferDeviceAddressAllocationAlignment = true;
   active_device = dev;
}
static VkResult allocate(tu_device *dev, tu_bo **bo) {
   *bo = reinterpret_cast<tu_bo *>(uintptr_t(1));
   return tu_wddm_bo_init(dev, nullptr, bo, 4096, 0, 0,
                          static_cast<tu_bo_alloc_flags>(0), nullptr, "fixture");
}
static void no_owner(tu_device *dev, tu_bo *bo) {
   check(bo == nullptr && dev->wddm_bo_count == 0 && current.slot.refcnt == 0 &&
         current.slot.wddm_allocation == nullptr, "failed zero-handle create publishes no BO owner");
   check(current.allocations == current.frees && current.vmas == current.vma_frees,
         "zero-handle failure releases descriptor and VMA exactly once");
   check(current.names == 0, "failed create never accounts a debug name");
}
int main() {
   tu_device dev; tu_bo *bo;
   for (uint32_t status : {0xc0000017u, 0xc000009au, 0xc000012du, 0xc01e0100u}) {
      init(&dev, status);
      check(allocate(&dev, &bo) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !dev.vk.lost,
            "true allocation pressure on healthy device stays allocation error");
      check(current.context_queries == 1 && current.execution_queries == 1,
            "allocation pressure checked against current context and OS execution");
      no_owner(&dev, bo);
   }
   for (uint32_t status : {0xc00000a3u, 0xc00002b6u, 0xc01e0200u}) {
      init(&dev, status);
      check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost,
            "lost or gated KMT allocation status is device lost not OOM");
      no_owner(&dev, bo);
   }
   for (uint32_t status : {0u, 0x102u, 0xc000000du, 0xc0000022u, 0x80000011u, 0xdeadbeefu}) {
      init(&dev, status);
      check(allocate(&dev, &bo) == VK_ERROR_UNKNOWN && !dev.vk.lost,
            "unknown or malformed allocation failure is not invented pressure");
      check(current.create_calls == (status == 0x80000011u ? 1001u : 1u),
            "existing bounded busy retry count is unchanged");
      no_owner(&dev, bo);
   }
   init(&dev, 0xc0000017u); current.context_active = false;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost,
         "closed context gate overrides secondary allocation pressure");
   no_owner(&dev, bo);
   init(&dev, 0xc0000017u); current.execution_active = false;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost,
         "OS execution loss overrides secondary allocation pressure");
   no_owner(&dev, bo);
   init(&dev, 0xc0000017u); current.lose_during_create = true;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost,
         "concurrent Vulkan device loss is not replaced with OOM");
   no_owner(&dev, bo);
   init(&dev, 0xc0000017u); current.lose_during_health_query = true;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost,
         "concurrent loss during health query overrides allocation pressure");
   no_owner(&dev, bo);
   init(&dev); dev.vk.lost = true;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && bo == nullptr &&
         current.create_calls == 0 && current.allocations == 0 && current.vmas == 0,
         "already lost device rejects before allocation and KMT calls");

   for (uint32_t status : {0xc0000017u, 0xc00000a3u, 0xc000000du}) {
      init(&dev, status); current.returned_handle = 4;
      VkResult expected = status == 0xc0000017u ? VK_ERROR_OUT_OF_DEVICE_MEMORY :
                          status == 0xc00000a3u ? VK_ERROR_DEVICE_LOST : VK_ERROR_UNKNOWN;
      check(allocate(&dev, &bo) == expected && current.destroy_calls == 1,
            "successful partial-create rollback preserves original status classification");
      no_owner(&dev, bo);
   }
   init(&dev, 0xc0000017u); current.returned_handle = 4; current.destroy_status = 0xc000000du;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && bo == nullptr && dev.vk.lost,
         "failed partial-create rollback marks device lost");
   check(dev.wddm_bo_count == 1 && current.slot.refcnt == 1 && current.slot.wddm_allocation &&
         current.slot.wddm_allocation->handle == 4 &&
         current.slot.wddm_allocation->context == &dev.wddm_context &&
         current.slot.wddm_allocation->last_create_status == 0xc0000017u &&
         current.slot.wddm_allocation->last_destroy_status == 0xc000000du &&
         current.frees == 0 && current.vma_frees == 0,
         "failed rollback retains exact handle descriptor VMA and retry owner");
   free(current.slot.wddm_allocation); // fixture owns its retained peer after assertions

   /* WDDM 1.x: every scenario above issued no residency call. */
   check(current.make_resident_calls == 0, "WDDM 1.x allocation makes no residency call");

   /* WDDM 2.0: residency failures reach the same Vulkan classification and
    * roll back exactly like a failed CreateAllocation. */
   init(&dev); current.returned_handle = 4;
   dev.wddm_device.adapter.driver_version = 2000; dev.wddm_device.paging_queue = 0x50;
   check(allocate(&dev, &bo) == VK_SUCCESS && bo == &current.slot && bo->wddm_allocation->resident &&
         current.make_resident_calls == 1 && current.destroy_calls == 0,
         "WDDM 2.0 allocation is resident before it is published");
   free(current.slot.wddm_allocation);
   for (uint32_t status : {0xc0000017u, 0xc01e0100u}) {
      init(&dev); current.returned_handle = 4;
      dev.wddm_device.adapter.driver_version = 2000; dev.wddm_device.paging_queue = 0x50;
      current.make_resident_status[0] = status; current.make_resident_status[1] = 0xc0000017u;
      check(allocate(&dev, &bo) == VK_ERROR_OUT_OF_DEVICE_MEMORY && !dev.vk.lost &&
            current.make_resident_calls == 2 && current.destroy_calls == 1,
            "over-budget residency is bounded and reports out of device memory");
      no_owner(&dev, bo);
   }
   init(&dev); current.returned_handle = 4;
   dev.wddm_device.adapter.driver_version = 2000; dev.wddm_device.paging_queue = 0x50;
   current.make_resident_status[0] = 0xc00002b6u;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost && current.make_resident_calls == 1,
         "residency on a removed device reports device lost");
   no_owner(&dev, bo);
   init(&dev); current.returned_handle = 4;
   dev.wddm_device.adapter.driver_version = 2000; dev.wddm_device.paging_queue = 0x50;
   current.make_resident_status[0] = 0xc0000017u; current.make_resident_status[1] = 0xc0000017u;
   current.destroy_status = 0xc000000du;
   check(allocate(&dev, &bo) == VK_ERROR_DEVICE_LOST && dev.vk.lost && dev.wddm_bo_count == 1 &&
         current.slot.wddm_allocation && !current.slot.wddm_allocation->resident &&
         current.slot.wddm_allocation->handle == 4 && current.vma_frees == 0,
         "failed residency rollback retains a non-resident retry owner");
   free(current.slot.wddm_allocation);

   init(&dev); current.local_alloc_fail = true;
   check(allocate(&dev, &bo) == VK_ERROR_OUT_OF_HOST_MEMORY && current.create_calls == 0,
         "local descriptor allocation remains host OOM without KMT");
   check(current.vmas == current.vma_frees, "local host OOM rolls back VMA");
   init(&dev); current.returned_handle = 4;
   check(allocate(&dev, &bo) == VK_SUCCESS && bo == &current.slot && current.names == 1 &&
         current.context_queries == 0 && current.execution_queries == 0,
         "successful allocation has no extra health query or behavior change");
   free(current.slot.wddm_allocation);

   tu_wddm_allocation invalid;
   init(&dev);
   check(!tu_wddm_allocation_create(&dev.wddm_context, nullptr, &invalid) &&
         invalid.handle == 0 && invalid.last_create_status == 0xc000000du && current.create_calls == 0,
         "local descriptor rejection records invalid parameter not success");

   VkMemoryOpaqueCaptureAddressAllocateInfo capture = {
      VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO, nullptr, 0};
   VkMemoryAllocateFlagsInfo flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
      &capture, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
   VkBufferDeviceAddressAlignmentAllocateInfoVALVE alignment = {
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_ALIGNMENT_ALLOCATE_INFO_VALVE, &flags, 0};
   VkMemoryAllocateInfo memory = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &alignment, 4096, 0};
   for (uint32_t requested : {0u, 1u, 4096u, 8192u, 16384u, 32768u, 65536u}) {
      for (uint64_t bytes : {4096ull, 12288ull, 69632ull}) {
         init(&dev); current.returned_handle = 4;
         alignment.alignment = requested;
         auto alloc_flags = TU_BO_ALLOC_GPU_READ_ONLY;
         check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_SUCCESS,
               "valid allocation alignment request accepted");
         /* Make the highest 64 KiB boundary unavailable. This uses Mesa's
          * actual VMA allocator, so passing a flag without honoring it fails. */
         check(util_vma_heap_alloc_addr(&dev.vma, 0x100ff0000ull, 4096),
               "reserve fragmentation guard in actual VMA");
         uint64_t free_before = dev.vma.free_size;
         check(tu_wddm_bo_init(&dev, nullptr, &bo, bytes, 0, 0, alloc_flags,
                              nullptr, "alignment") == VK_SUCCESS && bo,
               "production allocation succeeds with fragmented actual VMA");
         if (bo) {
            check(!requested || !(bo->iova % requested),
                  "requested buffer address alignment is honored by actual VMA");
            check(requested || bo->iova % 65536,
                  "zero request preserves existing page-aligned allocation behavior");
            check(current.requested_iova == bo->iova && current.backing_alignment == 4096 &&
                  dev.vma.free_size == free_before - bytes && bo->gpu_read_only,
                  "overalignment preserves exact backing size alignment flags and RequestedIova");
            util_vma_heap_free(&dev.vma, bo->iova, bytes);
            free(bo->wddm_allocation);
         }
      }
   }
   for (uint32_t requested : {3u, 4097u, 65537u, 131072u, 0xffffffffu}) {
      init(&dev); alignment.alignment = requested;
      auto alloc_flags = TU_BO_ALLOC_GPU_READ_ONLY;
      check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_ERROR_FEATURE_NOT_PRESENT &&
            alloc_flags == TU_BO_ALLOC_GPU_READ_ONLY && !current.create_calls && !current.vmas,
            "invalid power of two or over-limit alignment rejects before ownership changes");
   }
   init(&dev); alignment.alignment = 65536;
   auto alloc_flags = TU_BO_ALLOC_GPU_READ_ONLY;
   instance.wddm = false;
   check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_ERROR_FEATURE_NOT_PRESENT,
         "unimplemented non-WDDM backend never accepts alignment promise");
   instance.wddm = true; dev.vk.enabled_features.bufferDeviceAddressAllocationAlignment = false;
   check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_ERROR_FEATURE_NOT_PRESENT,
         "disabled feature rejects nonzero alignment");
   dev.vk.enabled_features.bufferDeviceAddressAllocationAlignment = true; flags.flags = 0;
   check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_ERROR_FEATURE_NOT_PRESENT,
         "nonzero alignment requires device address allocation flag");
   flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; capture.opaqueCaptureAddress = 0x100001000ull;
   check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
         "alignment request cannot silently override explicit capture address");
   alignment.alignment = 0; current.returned_handle = 4;
   check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_SUCCESS &&
         tu_wddm_bo_init(&dev, nullptr, &bo, 4096, capture.opaqueCaptureAddress, 0,
                        alloc_flags, nullptr, "capture") == VK_SUCCESS &&
         bo->iova == capture.opaqueCaptureAddress,
         "zero request retains exact page-aligned capture replay address");
   if (bo) free(bo->wddm_allocation);
   init(&dev, 0xc0000017u); alignment.alignment = 65536; capture.opaqueCaptureAddress = 0;
   alloc_flags = TU_BO_ALLOC_GPU_READ_ONLY;
   uint64_t free_before = dev.vma.free_size;
   check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_SUCCESS &&
         tu_wddm_bo_init(&dev, nullptr, &bo, 4096, 0, 0, alloc_flags, nullptr, "rollback") ==
            VK_ERROR_OUT_OF_DEVICE_MEMORY && dev.vma.free_size == free_before,
         "aligned allocation failure restores exact VMA reservation");
   no_owner(&dev, bo);
   for (uint32_t requested : {0u, 1u, 4096u, 8192u, 16384u, 32768u, 65536u}) {
      for (uint64_t bytes : {4096ull, 12288ull, 69632ull}) {
         init(&dev); dev.wddm_runtime_owner = &dev;
         dev.wddm_device.runtime_owner = &dev;
         dev.wddm_device.callbacks.allocate = shared_allocate;
         dev.wddm_device.callbacks.release = shared_release;
         alignment.alignment = requested;
         alloc_flags = TU_BO_ALLOC_GPU_READ_ONLY;
         check(tu_memory_bda_alignment(&dev, &memory, &alloc_flags) == VK_SUCCESS,
               "shared allocation parses real BDA alignment");
         check(util_vma_heap_alloc_addr(&dev.vma, 0x100ff0000ull, 4096),
               "shared allocation reserves actual fragmentation guard");
         free_before = dev.vma.free_size;
         check(tu_wddm_bo_init(&dev, nullptr, &bo, bytes, 0, 0, alloc_flags,
                              nullptr, "shared alignment") == VK_SUCCESS && bo,
               "shared runtime allocation succeeds with requested alignment");
         if (bo) {
            check(current.shared_alignment == (requested ? 65536u : 4096u) &&
                  (!requested || !(bo->iova % requested)),
                  "shared runtime receives and honors requested BDA alignment");
            check(bo->iova == current.shared_address && bo->size == bytes &&
                  bo->wddm_allocation->private_info.Alignment == 4096 &&
                  bo->wddm_allocation->runtime_token == &current &&
                  current.shared_refs == 1 && !current.create_calls &&
                  dev.vma.free_size == free_before - bytes,
                  "shared overalignment preserves exact backing and unchanged KMD packet");
            shared_release(&dev, &current);
            free(bo->wddm_allocation);
         }
      }
   }
   init(&dev); dev.wddm_runtime_owner = &dev;
   dev.wddm_device.runtime_owner = &dev;
   dev.wddm_device.callbacks.allocate = shared_allocate;
   dev.wddm_device.callbacks.release = shared_release;
   current.shared_misaligned = true;
   free_before = dev.vma.free_size;
   alloc_flags = static_cast<tu_bo_alloc_flags>(TU_BO_ALLOC_GPU_READ_ONLY | TU_BO_ALLOC_BDA_64K);
   check(tu_wddm_bo_init(&dev, nullptr, &bo, 4096, 0, 0, alloc_flags,
                        nullptr, "malformed shared alignment") != VK_SUCCESS && !bo &&
         current.shared_calls == 1 && !current.shared_refs && current.shared_releases == 1 &&
         dev.vma.free_size == free_before && !dev.wddm_bo_count,
         "misaligned shared reply rejected with exact reference unwind");
   // Make mutation fixtures leak-free after they intentionally accept a bad reply.
   if (bo) { shared_release(&dev, &current); free(bo->wddm_allocation); }
   for (uint64_t bad_alignment : std::initializer_list<uint64_t>{0, 1, 4095, 4097, 131072, UINT64_MAX}) {
      tu_wddm_allocation_desc invalid_desc = {};
      invalid_desc.size = 4096; invalid_desc.alignment = bad_alignment; invalid_desc.flags = 14;
      const auto calls = current.shared_calls;
      check(!tu_wddm_allocation_create(&dev.wddm_context, &invalid_desc, &invalid) &&
            current.shared_calls == calls && !invalid.handle && !invalid.runtime_token,
            "invalid shared alignment rejected before runtime callback");
   }
   util_vma_heap_finish(&dev.vma);
   vma_initialized = false;
   printf("%s production WDDM allocation classification and rollback: %u checks, %u failures\n",
          failures ? "FAIL" : "PASS", checks, failures);
   return failures ? 1 : 0;
}
