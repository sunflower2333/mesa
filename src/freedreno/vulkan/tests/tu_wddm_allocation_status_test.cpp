/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "vulkan/vulkan_core.h"
#include "tu_wddm_abi.h"

using NTSTATUS = int32_t;
using D3DKMT_HANDLE = uint32_t;
#define NT_SUCCESS(status) ((status) >= 0)
static constexpr NTSTATUS TU_WDDM_STATUS_SUCCESS = 0;
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
struct tu_wddm_runtime {
   struct {
      NTSTATUS (*CreateAllocation)(D3DKMT_CREATEALLOCATION *);
      NTSTATUS (*DestroyAllocation2)(D3DKMT_DESTROYALLOCATION2 *);
   } dispatch;
};
struct tu_wddm_device {
   struct { tu_wddm_runtime *runtime; VIOGPU_WDDM_ADAPTER_INFO private_info; } adapter;
   D3DKMT_HANDLE handle;
};
struct tu_wddm_context {
   tu_wddm_device *device;
   D3DKMT_HANDLE handle;
   VIOGPU_WDDM_CONTEXT_INFO info;
};
// PRODUCTION_STRUCTS

enum tu_bo_alloc_flags {
   TU_BO_ALLOC_DMABUF = 1, TU_BO_ALLOC_SHAREABLE = 2,
   TU_BO_ALLOC_IMPLICIT_SYNC = 4, TU_BO_ALLOC_GPU_READ_ONLY = 8,
};
struct vk_object_base {};
struct tu_sparse_vma {};
struct vk_device { bool lost; int alloc; };
struct tu_bo {
   uint32_t gem_handle;
   uint64_t size, iova;
   const char *name;
   int refcnt;
   bool gpu_read_only;
   vk_object_base *base;
   tu_wddm_allocation *wddm_allocation;
};
struct tu_device {
   vk_device vk;
   int vma_mutex, bo_mutex, vma;
   tu_wddm_device wddm_device;
   tu_wddm_context wddm_context;
   bool wddm_initialized;
   uint32_t wddm_bo_count;
   tu_bo *wddm_bos[1];
};
struct state {
   uint32_t create_status, destroy_status, returned_handle;
   bool context_active, execution_active, lose_during_create, local_alloc_fail;
   uint32_t create_calls, destroy_calls, sleeps, context_queries, execution_queries;
   uint32_t allocations, frees, vmas, vma_frees, names;
   tu_bo slot;
} current;
static unsigned checks, failures;
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
   current.execution_queries++; return current.execution_active;
}
static void mtx_lock(int *mutex) { check((*mutex)++ == 0, "no recursive bookkeeping lock"); }
static void mtx_unlock(int *mutex) { check(--(*mutex) == 0, "balanced bookkeeping lock"); }
static uint64_t util_vma_heap_alloc(int *, uint64_t, uint64_t) { current.vmas++; return 0x100000000ull; }
static bool util_vma_heap_alloc_addr(int *, uint64_t, uint64_t) { current.vmas++; return true; }
static void util_vma_heap_free(int *, uint64_t, uint64_t) { current.vma_frees++; }
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
static void tu_dump_bo_init(tu_device *, tu_bo *) {}
using BOOL = int;
struct MEMORYSTATUSEX {
   uint32_t dwLength;
   uint64_t ullAvailPageFile, ullTotalPageFile, ullAvailPhys, ullTotalPhys;
};
static BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX *) { return 0; }
static uint64_t GetTickCount64() { return 0; }
static unsigned long GetCurrentProcessId() { return 0; }
static tu_device *active_device;
static NTSTATUS create_allocation(D3DKMT_CREATEALLOCATION *create) {
   current.create_calls++;
   check(active_device->bo_mutex == 0 && active_device->vma_mutex == 0,
         "create has no bookkeeping mutex held");
   check(create->Flags.NonSecure == 1 && create->NumAllocations == 1,
         "production create flags and count");
   create->pAllocationInfo->hAllocation = current.returned_handle;
   if (current.lose_during_create) active_device->vk.lost = true;
   return static_cast<NTSTATUS>(current.create_status);
}
static NTSTATUS destroy_allocation(D3DKMT_DESTROYALLOCATION2 *destroy) {
   current.destroy_calls++;
   check(destroy->Flags.AssumeNotInUse == 0 && destroy->AllocationCount == 1 &&
         *destroy->phAllocationList == current.returned_handle,
         "compensating destroy preserves VidMm ownership checks");
   return static_cast<NTSTATUS>(current.destroy_status);
}
// PRODUCTION_FUNCTIONS

static tu_wddm_runtime runtime = {{create_allocation, destroy_allocation}};
static void init(tu_device *dev, uint32_t status = 0) {
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
   printf("%s production WDDM allocation classification and rollback: %u checks, %u failures\n",
          failures ? "FAIL" : "PASS", checks, failures);
   return failures ? 1 : 0;
}
