/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Windows Turnip kernel-interface transport.  This file owns only the
 * WDDM/D3DKMT object and private-ABI plumbing.  Native submits stay behind the
 * KMD guest-backed allocation and VidSch retirement gates; this code never
 * bypasses those gates or manufactures a fence completion.
 */

#include "tu_knl_wddm.h"

#include <string.h>

#ifdef TU_HAS_WDDM
#include <errno.h>

#include "util/os_time.h"
#include "util/u_dynarray.h"
#include "vk_sync_dummy.h"
#include "vk_sync_timeline.h"

#include "tu_cs.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "tu_queue.h"
#include "tu_rmv.h"
#endif

template <typename T>
static constexpr uint32_t
tu_wddm_sizeof()
{
   static_assert(sizeof(T) <= UINT32_MAX, "WDDM size field overflow");
   return static_cast<uint32_t>(sizeof(T));
}

enum : uint32_t {
   TU_WDDM_MSM_CCMD_GEM_SUBMIT = 7,
   TU_WDDM_MSM_PIPE_3D0 = 0x10,
   TU_WDDM_MSM_SUBMIT_NO_IMPLICIT = 0x80000000,
   TU_WDDM_MSM_SUBMIT_BO_READ = 0x0001,
   TU_WDDM_MSM_SUBMIT_BO_WRITE = 0x0002,
   TU_WDDM_MSM_SUBMIT_BO_DUMP = 0x0004,
   TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT = 0x0008,
   TU_WDDM_MSM_SUBMIT_CMD_BUF = 0x0001,
   TU_WDDM_MSM_SUBMIT_CMD_IB_TARGET_BUF = 0x0002,
   /* Must match VioGpuWddmContextFenceTrackerCapacity in the dedicated KMD. */
   TU_WDDM_MAX_PENDING_SUBMISSIONS = 4096,
};

#pragma pack(push, 1)
struct tu_wddm_msm_submit_request {
   uint32_t command;
   uint32_t length;
   uint32_t sequence;
   uint32_t response_offset;
   uint32_t flags;
   uint32_t queue_id;
   uint32_t bo_count;
   uint32_t command_count;
   uint32_t fence;
};

struct tu_wddm_msm_submit_bo {
   uint32_t flags;
   uint32_t handle;
   uint64_t presumed;
};

struct tu_wddm_msm_submit_command {
   uint32_t type;
   uint32_t submit_index;
   uint32_t submit_offset;
   uint32_t size;
   uint32_t padding;
   uint32_t relocation_count;
   uint64_t iova;
};
#pragma pack(pop)

static_assert(sizeof(tu_wddm_msm_submit_request) == 36, "MSM submit request layout changed");
static_assert(offsetof(tu_wddm_msm_submit_request, flags) == 16, "MSM submit flags offset changed");
static_assert(offsetof(tu_wddm_msm_submit_request, fence) == 32, "MSM submit fence offset changed");
static_assert(sizeof(tu_wddm_msm_submit_bo) == 16, "MSM submit BO layout changed");
static_assert(offsetof(tu_wddm_msm_submit_bo, presumed) == 8, "MSM submit presumed offset changed");
static_assert(sizeof(tu_wddm_msm_submit_command) == 32, "MSM submit command layout changed");
static_assert(offsetof(tu_wddm_msm_submit_command, iova) == 24, "MSM submit command IOVA offset changed");

/* Native-context fences are 32-bit serial numbers.  Keep all ordering
 * decisions in one helper so the endpoint remains valid across UINT32 wrap,
 * while the half-range rule rejects an ambiguous distance. */
static inline bool
tu_wddm_fence_after(uint32_t a, uint32_t b)
{
   return a != b && static_cast<int32_t>(a - b) > 0;
}

static constexpr uint64_t
tu_wddm_fence_distance(uint32_t submitted, uint32_t completed)
{
   return submitted > completed
             ? static_cast<uint64_t>(submitted - completed)
             : static_cast<uint64_t>(UINT32_MAX - completed) + submitted;
}

static_assert(tu_wddm_fence_distance(4096, 0) == 4096,
              "WDDM initial fence distance changed");
static_assert(tu_wddm_fence_distance(1, UINT32_MAX) == 1,
              "WDDM wrapped fence distance changed");

static void
tu_wddm_init_header(VIOGPU_WDDM_ABI_HEADER *header, uint32_t size)
{
   memset(header, 0, size);
   header->Magic = VIOGPU_WDDM_ABI_MAGIC;
   header->Version = VIOGPU_WDDM_ABI_VERSION;
   header->Size = size;
}

static bool
tu_wddm_header_is_current(const VIOGPU_WDDM_ABI_HEADER *header, uint32_t size)
{
   return header != NULL && header->Magic == VIOGPU_WDDM_ABI_MAGIC &&
          header->Version == VIOGPU_WDDM_ABI_VERSION && header->Size == size &&
          header->Reserved == 0;
}

bool
tu_wddm_submitqueue_priority_is_supported(int priority)
{
   return priority == 0;
}

bool
tu_wddm_get_device_id_properties(const struct tu_wddm_adapter_info *identity,
                                 void *device_luid,
                                 size_t device_luid_size,
                                 uint32_t *device_node_mask)
{
   if (identity == NULL || device_luid == NULL ||
       device_luid_size != sizeof(identity->luid) || device_node_mask == NULL)
      return false;

   memcpy(device_luid, &identity->luid, sizeof(identity->luid));
   *device_node_mask = 1;
   return true;
}

static inline bool
tu_wddm_page_aligned_nonzero(uint64_t size)
{
   constexpr uint64_t page_size = UINT64_C(4096);
   return size != 0 && (size & (page_size - 1)) == 0;
}

static bool
tu_wddm_context_buffers_valid(const D3DKMT_CREATECONTEXT *info)
{
   if (info == NULL ||
       info->CommandBufferSize < sizeof(VIOGPU_WDDM_RENDER_COMMAND) +
                                  sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE) +
                                  sizeof(tu_wddm_msm_submit_request) ||
       info->pCommandBuffer == NULL || info->pAllocationList == NULL ||
       info->AllocationListSize == 0 ||
       info->AllocationListSize > TU_WDDM_MAX_RENDER_ALLOCATIONS ||
       info->pPatchLocationList == NULL || info->PatchLocationListSize == 0 ||
       info->PatchLocationListSize > TU_WDDM_MAX_RENDER_ALLOCATIONS)
      return false;

   return true;
}

bool
tu_wddm_select_heap_size(uint64_t dedicated_video_memory,
                         uint64_t va_size,
                         uint64_t *heap_size)
{
   if (heap_size == NULL)
      return false;

   *heap_size = 0;
   if (!tu_wddm_page_aligned_nonzero(dedicated_video_memory) ||
       !tu_wddm_page_aligned_nonzero(va_size))
      return false;

   *heap_size = dedicated_video_memory < va_size ? dedicated_video_memory : va_size;
   return *heap_size != 0;
}

bool
tu_wddm_validate_adapter_info(const VIOGPU_WDDM_ADAPTER_INFO *info)
{
   if (info == NULL ||
       !tu_wddm_header_is_current(&info->Header,
                                  tu_wddm_sizeof<VIOGPU_WDDM_ADAPTER_INFO>()) ||
       info->Capabilities != VIOGPU_WDDM_CAPABILITIES_NONE || info->ResetGeneration == 0 ||
       info->MsmMajorVersion != 1 || info->MsmMinorVersion < 9 || info->GpuId == 0 ||
       info->ChipId == 0 || info->GmemSize == 0 || info->PriorityCount != 1 ||
       info->HasCachedCoherentMemory > 1 || info->HasRayTracing > 1)
      return false;

   for (unsigned i = 0; i < sizeof(info->Reserved) / sizeof(info->Reserved[0]); i++) {
      if (info->Reserved[i] != 0)
         return false;
   }

   return true;
}

bool
tu_wddm_validate_context_info(const VIOGPU_WDDM_CONTEXT_INFO *info,
                              uint64_t expected_reset_generation)
{
   if (info == NULL || expected_reset_generation == 0 ||
       !tu_wddm_header_is_current(&info->Header, tu_wddm_sizeof<VIOGPU_WDDM_CONTEXT_INFO>()) ||
       info->Opcode != VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO || info->Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE ||
       info->ExpectedResetGeneration != expected_reset_generation || info->VaStart == 0 || info->VaSize == 0 ||
       info->ResetGeneration != expected_reset_generation || info->ContextId == 0 || info->SubmitQueueId == 0 ||
       (info->VaStart & 4095) != 0 || (info->VaSize & 4095) != 0 || info->VaSize > UINT64_MAX - info->VaStart)
      return false;

   return true;
}

bool
tu_wddm_validate_fence_info(const VIOGPU_WDDM_FENCE_INFO *info,
                            uint64_t expected_reset_generation,
                            uint32_t expected_context_id)
{
   if (info == NULL || expected_reset_generation == 0 || expected_context_id == 0 ||
       !tu_wddm_header_is_current(&info->Header, tu_wddm_sizeof<VIOGPU_WDDM_FENCE_INFO>()) ||
       info->Opcode != VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE ||
       info->Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE ||
       info->ExpectedResetGeneration != expected_reset_generation ||
       info->ResetGeneration != expected_reset_generation || info->ContextId != expected_context_id ||
       info->CompletedFence > UINT32_MAX || info->Reserved != 0)
      return false;

   return true;
}

static bool
tu_wddm_private_info_equal(const VIOGPU_WDDM_ADAPTER_INFO *a,
                           const VIOGPU_WDDM_ADAPTER_INFO *b)
{
   return a != NULL && b != NULL && memcmp(a, b, sizeof(*a)) == 0;
}

static bool
tu_wddm_query_private_info(struct tu_wddm_runtime *runtime,
                           D3DKMT_HANDLE adapter_handle,
                           VIOGPU_WDDM_ADAPTER_INFO *info)
{
   if (runtime == NULL || info == NULL || adapter_handle == 0)
      return false;

   memset(info, 0, sizeof(*info));
   D3DKMT_QUERYADAPTERINFO query = {};
   query.hAdapter = adapter_handle;
   query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
   query.pPrivateDriverData = info;
   query.PrivateDriverDataSize = tu_wddm_sizeof<VIOGPU_WDDM_ADAPTER_INFO>();

   NTSTATUS status = runtime->dispatch.QueryAdapterInfo(&query);
   return NT_SUCCESS(status) && tu_wddm_validate_adapter_info(info);
}

bool
tu_wddm_runtime_init(struct tu_wddm_runtime *runtime)
{
   if (runtime == NULL)
      return false;

   using PFN_CREATE_DXGI_FACTORY1 = HRESULT(WINAPI *)(REFIID, void **);
   PFN_CREATE_DXGI_FACTORY1 create_factory = NULL;

   memset(runtime, 0, sizeof(*runtime));
   if (!tu_wddm_dispatch_init(&runtime->dispatch))
      return false;

   runtime->dxgi = LoadLibraryExW(L"dxgi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (runtime->dxgi == NULL)
      goto fail;

   create_factory = reinterpret_cast<PFN_CREATE_DXGI_FACTORY1>(
      GetProcAddress(runtime->dxgi, "CreateDXGIFactory1"));
   if (create_factory == NULL ||
       FAILED(create_factory(__uuidof(IDXGIFactory1),
                             reinterpret_cast<void **>(&runtime->factory))))
      goto fail;

   return true;

fail:
   tu_wddm_runtime_finish(runtime);
   return false;
}

void
tu_wddm_runtime_finish(struct tu_wddm_runtime *runtime)
{
   if (runtime == NULL)
      return;

   if (runtime->factory != NULL)
      runtime->factory->Release();
   if (runtime->dxgi != NULL)
      FreeLibrary(runtime->dxgi);

   tu_wddm_dispatch_finish(&runtime->dispatch);
   memset(runtime, 0, sizeof(*runtime));
}

bool
tu_wddm_adapter_open(struct tu_wddm_runtime *runtime,
                     const struct tu_wddm_adapter_info *identity,
                     struct tu_wddm_adapter *adapter)
{
   if (runtime == NULL || identity == NULL || adapter == NULL)
      return false;

   memset(adapter, 0, sizeof(*adapter));
   D3DKMT_OPENADAPTERFROMLUID open = {};
   open.AdapterLuid = identity->luid;
   NTSTATUS status = runtime->dispatch.OpenAdapterFromLuid(&open);
   if (!NT_SUCCESS(status) || open.hAdapter == 0)
      return false;

   /* Publish the handle before private-info validation.  If closing a
    * rejected adapter fails, the caller retains enough ownership state to
    * retry the close instead of losing the KMT handle. */
   adapter->runtime = runtime;
   adapter->luid = identity->luid;
   adapter->handle = open.hAdapter;

   VIOGPU_WDDM_ADAPTER_INFO current = {};
   if (!tu_wddm_query_private_info(runtime, open.hAdapter, &current) ||
       (identity->private_info.ResetGeneration != 0 &&
        !tu_wddm_private_info_equal(&current, &identity->private_info))) {
      tu_wddm_adapter_close(adapter);
      return false;
   }

   adapter->private_info = current;
   return true;
}

bool
tu_wddm_adapter_close(struct tu_wddm_adapter *adapter)
{
   if (adapter == NULL || adapter->runtime == NULL || adapter->handle == 0)
      return false;

   D3DKMT_CLOSEADAPTER close = {};
   close.hAdapter = adapter->handle;
   NTSTATUS status = adapter->runtime->dispatch.CloseAdapter(&close);
   if (!NT_SUCCESS(status))
      return false;

   memset(adapter, 0, sizeof(*adapter));
   return true;
}

bool
tu_wddm_runtime_foreach_adapter(struct tu_wddm_runtime *runtime,
                                tu_wddm_adapter_callback callback,
                                void *data)
{
   if (runtime == NULL || runtime->factory == NULL || callback == NULL)
      return false;

   for (UINT index = 0;; index++) {
      IDXGIAdapter1 *dxgi_adapter = NULL;
      HRESULT hr = runtime->factory->EnumAdapters1(index, &dxgi_adapter);
      if (hr == DXGI_ERROR_NOT_FOUND)
         break;
      if (FAILED(hr) || dxgi_adapter == NULL)
         return false;

      DXGI_ADAPTER_DESC1 desc = {};
      hr = dxgi_adapter->GetDesc1(&desc);
      if (SUCCEEDED(hr)) {
         struct tu_wddm_adapter_info identity = {};
         identity.luid = desc.AdapterLuid;
         identity.vendor_id = desc.VendorId;
         identity.device_id = desc.DeviceId;
         identity.subsystem_id = desc.SubSysId;
         identity.revision = desc.Revision;
         identity.dedicated_video_memory = desc.DedicatedVideoMemory;
         identity.dedicated_system_memory = desc.DedicatedSystemMemory;
         identity.shared_system_memory = desc.SharedSystemMemory;
         WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, identity.description,
                             static_cast<int>(sizeof(identity.description)), NULL, NULL);

         struct tu_wddm_adapter adapter = {};
         if (tu_wddm_adapter_open(runtime, &identity, &adapter)) {
            identity.private_info = adapter.private_info;
            bool keep_going = callback(&identity, data);
            bool closed = tu_wddm_adapter_close(&adapter);
            dxgi_adapter->Release();
            /* A false callback result is an explicit enumeration abort.  Do
             * not turn it into success merely because the adapter close
             * succeeded: callers use the return value to propagate probe and
             * allocation failures after already-published devices are
             * cleaned up. */
            if (!closed || !keep_going)
               return false;
            continue;
         }

         /* A failed probe may retain a handle when CloseAdapter itself
          * failed.  Do not let enumeration silently abandon that owner. */
         bool closed = adapter.handle == 0 || tu_wddm_adapter_close(&adapter);
         dxgi_adapter->Release();
         if (!closed)
            return false;
         continue;
      }

      dxgi_adapter->Release();
   }

   return true;
}

bool
tu_wddm_device_open(struct tu_wddm_runtime *runtime,
                    const struct tu_wddm_adapter_info *identity,
                    struct tu_wddm_device *device)
{
   if (runtime == NULL || identity == NULL || device == NULL)
      return false;

   memset(device, 0, sizeof(*device));
   if (!tu_wddm_adapter_open(runtime, identity, &device->adapter)) {
      if (device->adapter.handle != 0)
         tu_wddm_adapter_close(&device->adapter);
      return false;
   }

   D3DKMT_CREATEDEVICE create = {};
   create.hAdapter = device->adapter.handle;
   NTSTATUS status = runtime->dispatch.CreateDevice(&create);
   if (!NT_SUCCESS(status) || create.hDevice == 0) {
      if (create.hDevice != 0) {
         D3DKMT_DESTROYDEVICE destroy = {};
         destroy.hDevice = create.hDevice;
         if (NT_SUCCESS(runtime->dispatch.DestroyDevice(&destroy)))
            create.hDevice = 0;
      }
      if (create.hDevice == 0)
         tu_wddm_adapter_close(&device->adapter);
      else {
         /* Preserve a partially-created device for a later retry. */
         device->handle = create.hDevice;
      }
      return false;
   }

   device->handle = create.hDevice;
   device->command_buffer = create.pCommandBuffer;
   device->command_buffer_size = create.CommandBufferSize;
   device->allocation_list = create.pAllocationList;
   device->allocation_list_size = create.AllocationListSize;
   device->patch_location_list = create.pPatchLocationList;
   device->patch_location_list_size = create.PatchLocationListSize;
   return true;
}

bool
tu_wddm_device_close(struct tu_wddm_device *device)
{
   if (device == NULL)
      return false;

   /* DestroyDevice and CloseAdapter are separate owners.  If the latter
    * fails, keep the adapter handle so a caller can retry without losing the
    * remaining owner; treating a zero device handle as already closed also
    * makes that retry path explicit. */
   if (device->handle != 0) {
      if (device->adapter.runtime == NULL)
         return false;

      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = device->handle;
      NTSTATUS status = device->adapter.runtime->dispatch.DestroyDevice(&destroy);
      if (!NT_SUCCESS(status))
         return false;

      device->handle = 0;
      device->command_buffer = NULL;
      device->allocation_list = NULL;
      device->patch_location_list = NULL;
      device->command_buffer_size = 0;
      device->allocation_list_size = 0;
      device->patch_location_list_size = 0;
   }

   if (device->adapter.handle != 0)
      return tu_wddm_adapter_close(&device->adapter);

   return device->adapter.runtime != NULL;
}

bool
tu_wddm_context_open(struct tu_wddm_device *device,
                     struct tu_wddm_context *context)
{
   if (device == NULL || context == NULL || device->adapter.runtime == NULL ||
       device->handle == 0 || device->adapter.private_info.ResetGeneration == 0)
      return false;

   memset(context, 0, sizeof(*context));
   VIOGPU_WDDM_CONTEXT_CREATE private_data = {};
   tu_wddm_init_header(&private_data.Header,
                       tu_wddm_sizeof<VIOGPU_WDDM_CONTEXT_CREATE>());
   private_data.ExpectedResetGeneration = device->adapter.private_info.ResetGeneration;
   private_data.Flags = VIOGPU_WDDM_CONTEXT_FLAGS_NONE;

   D3DKMT_CREATECONTEXT create = {};
   create.hDevice = device->handle;
   create.NodeOrdinal = 0;
   create.EngineAffinity = 1;
   create.pPrivateDriverData = &private_data;
   create.PrivateDriverDataSize = tu_wddm_sizeof<VIOGPU_WDDM_CONTEXT_CREATE>();
   create.ClientHint = D3DKMT_CLIENTHINT_VULKAN;

   NTSTATUS status = device->adapter.runtime->dispatch.CreateContext(&create);
   if (!NT_SUCCESS(status) || create.hContext == 0 ||
       !tu_wddm_context_buffers_valid(&create)) {
      if (create.hContext != 0) {
         D3DKMT_DESTROYCONTEXT destroy = {};
         destroy.hContext = create.hContext;
         if (NT_SUCCESS(device->adapter.runtime->dispatch.DestroyContext(&destroy)))
            create.hContext = 0;
         else {
            /* Preserve a failed-but-owned context for a later retry. */
            context->device = device;
            context->handle = create.hContext;
         }
      }
      return false;
   }

   context->device = device;
   context->handle = create.hContext;
   context->command_buffer = create.pCommandBuffer;
   context->command_buffer_size = create.CommandBufferSize;
   context->allocation_list = create.pAllocationList;
   context->allocation_list_size = create.AllocationListSize;
   context->patch_location_list = create.pPatchLocationList;
   context->patch_location_list_size = create.PatchLocationListSize;

   if (!tu_wddm_context_get_info(context)) {
      tu_wddm_context_close(context);
      return false;
   }

   return true;
}

bool
tu_wddm_context_get_info(struct tu_wddm_context *context)
{
   if (context == NULL || context->device == NULL || context->handle == 0 ||
       context->device->adapter.runtime == NULL)
      return false;

   const uint64_t generation = context->device->adapter.private_info.ResetGeneration;
   VIOGPU_WDDM_CONTEXT_INFO request = {};
   tu_wddm_init_header(&request.Header, tu_wddm_sizeof<VIOGPU_WDDM_CONTEXT_INFO>());
   request.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
   request.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
   request.ExpectedResetGeneration = generation;

   D3DKMT_ESCAPE escape = {};
   escape.hAdapter = context->device->adapter.handle;
   escape.hDevice = context->device->handle;
   escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   escape.pPrivateDriverData = &request;
   escape.PrivateDriverDataSize = tu_wddm_sizeof<VIOGPU_WDDM_CONTEXT_INFO>();
   escape.hContext = context->handle;

   NTSTATUS status = context->device->adapter.runtime->dispatch.Escape(&escape);
   if (!NT_SUCCESS(status) || !tu_wddm_validate_context_info(&request, generation))
      return false;

   context->info = request;
   return true;
}

bool
tu_wddm_context_get_completed_fence(struct tu_wddm_context *context,
                                    uint32_t *completed_fence)
{
   if (completed_fence != NULL)
      *completed_fence = 0;
   if (context == NULL || context->device == NULL || context->handle == 0 || completed_fence == NULL ||
       context->device->adapter.runtime == NULL ||
       !tu_wddm_validate_context_info(&context->info,
                                      context->device->adapter.private_info.ResetGeneration))
      return false;

   const uint64_t generation = context->info.ResetGeneration;
   VIOGPU_WDDM_FENCE_INFO request = {};
   tu_wddm_init_header(&request.Header, tu_wddm_sizeof<VIOGPU_WDDM_FENCE_INFO>());
   request.Opcode = VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE;
   request.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
   request.ExpectedResetGeneration = generation;

   D3DKMT_ESCAPE escape = {};
   escape.hAdapter = context->device->adapter.handle;
   escape.hDevice = context->device->handle;
   escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   escape.pPrivateDriverData = &request;
   escape.PrivateDriverDataSize = tu_wddm_sizeof<VIOGPU_WDDM_FENCE_INFO>();
   escape.hContext = context->handle;

   NTSTATUS status = context->device->adapter.runtime->dispatch.Escape(&escape);
   if (!NT_SUCCESS(status) ||
       !tu_wddm_validate_fence_info(&request, generation, context->info.ContextId))
      return false;

   *completed_fence = static_cast<uint32_t>(request.CompletedFence);
   return true;
}

bool
tu_wddm_context_wait_fence(struct tu_wddm_context *context,
                           uint32_t fence,
                           uint64_t timeout_ns)
{
   if (fence == 0 || context == NULL || context->device == NULL ||
       context->device->adapter.runtime == NULL)
      return false;

   const ULONGLONG start_ms = GetTickCount64();
   const uint64_t timeout_ms = timeout_ns == UINT64_MAX
                                  ? UINT64_MAX
                                  : timeout_ns / UINT64_C(1000000) +
                                       (timeout_ns % UINT64_C(1000000) != 0);

   for (;;) {
      uint32_t completed = 0;
      if (!tu_wddm_context_get_completed_fence(context, &completed))
         return false;
      if (completed == fence || tu_wddm_fence_after(completed, fence))
         return true;

      const ULONGLONG elapsed_ms = GetTickCount64() - start_ms;
      if (timeout_ms != UINT64_MAX && elapsed_ms >= timeout_ms)
         return false;

      Sleep(1);
   }
}

bool
tu_wddm_context_wait_submissions(struct tu_wddm_context *context,
                                 uint64_t timeout_ns)
{
   if (context == NULL)
      return false;

   const uint32_t fence = context->last_submitted_fence;
   return fence == 0 || tu_wddm_context_wait_fence(context, fence, timeout_ns);
}

bool
tu_wddm_context_close(struct tu_wddm_context *context)
{
   if (context == NULL || context->device == NULL || context->handle == 0 ||
       context->device->adapter.runtime == NULL)
      return false;

   D3DKMT_DESTROYCONTEXT destroy = {};
   destroy.hContext = context->handle;
   NTSTATUS status = context->device->adapter.runtime->dispatch.DestroyContext(&destroy);
   if (!NT_SUCCESS(status))
      return false;

   memset(context, 0, sizeof(*context));
   return true;
}

bool
tu_wddm_probe_owner_cleanup(struct tu_wddm_device *device,
                            struct tu_wddm_context *context)
{
   if (device == NULL || context == NULL)
      return false;

   /* Contexts own the device while they are live.  Never destroy the device
    * after a failed context close: doing so would invalidate the remaining
    * KMT context handle and make a retry impossible. */
   if (context->handle != 0) {
      if (!tu_wddm_context_close(context))
         return false;
   }

   if (device->handle != 0 || device->adapter.handle != 0) {
      if (!tu_wddm_device_close(device))
         return false;
   }

   return true;
}

#ifdef TU_HAS_WDDM
bool
tu_wddm_probe_cleanup(struct tu_instance *instance)
{
   if (instance == NULL)
      return false;

   const bool cleaned = tu_wddm_probe_owner_cleanup(
      &instance->wddm_probe_device, &instance->wddm_probe_context);
   instance->wddm_probe_pending = !cleaned;
   return cleaned;
}

bool
tu_wddm_instance_prepare_destroy(struct tu_instance *instance)
{
   if (instance == NULL)
      return false;

   return !instance->wddm_runtime_initialized ||
          tu_wddm_probe_cleanup(instance);
}
#endif

static bool
tu_wddm_allocation_desc_valid(const struct tu_wddm_context *context,
                              const struct tu_wddm_allocation_desc *desc)
{
   const uint32_t valid_flags = VIOGPU_WDDM_ALLOCATION_PRIMARY |
                                VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE |
                                VIOGPU_WDDM_ALLOCATION_NATIVE |
                                VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY;
   const bool native = desc != NULL && (desc->flags & VIOGPU_WDDM_ALLOCATION_NATIVE) != 0;
   const bool has_surface = desc != NULL &&
                            (desc->width != 0 || desc->height != 0 || desc->pitch != 0 ||
                             desc->format != VIOGPU_WDDM_FORMAT_NONE);

   if (context == NULL || context->device == NULL || context->device->adapter.runtime == NULL ||
       context->device->handle == 0 || context->handle == 0 || desc == NULL || desc->size == 0 ||
       desc->alignment != 4096 || (desc->flags & ~valid_flags) != 0 ||
       desc->size > UINT64_MAX - 4095 ||
       !tu_wddm_validate_context_info(&context->info, context->device->adapter.private_info.ResetGeneration))
      return false;

   if (has_surface && (desc->width == 0 || desc->height == 0 || desc->pitch == 0 ||
                       (desc->format != VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM &&
                        desc->format != VIOGPU_WDDM_FORMAT_B8G8R8X8_UNORM) ||
                       desc->width > UINT32_MAX / 4 || desc->pitch < desc->width * 4 ||
                       static_cast<uint64_t>(desc->pitch) * desc->height > desc->size))
      return false;

   if (!has_surface && (desc->refresh_rate_numerator != 0 ||
                        desc->refresh_rate_denominator != 0))
      return false;

   if ((desc->flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) != 0 &&
       (!has_surface || (desc->flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0 ||
        desc->refresh_rate_numerator == 0 || desc->refresh_rate_denominator == 0))
      return false;

   if ((desc->flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) == 0 &&
       (desc->refresh_rate_numerator != 0 || desc->refresh_rate_denominator != 0))
      return false;

   if (native) {
      const uint64_t aligned_size = (desc->size + UINT64_C(4095)) & ~UINT64_C(4095);
      if ((desc->flags & VIOGPU_WDDM_ALLOCATION_PRIMARY) != 0 ||
          desc->requested_iova == 0 || (desc->requested_iova & 4095) != 0 ||
          context->info.ResetGeneration == 0 || context->info.ContextId == 0 ||
          aligned_size > UINT32_MAX ||
          desc->requested_iova < context->info.VaStart || aligned_size > context->info.VaSize ||
          desc->requested_iova > context->info.VaStart + context->info.VaSize - aligned_size)
         return false;
   } else if (desc->requested_iova != 0 || (desc->flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) != 0) {
      return false;
   }

   return true;
}

bool
tu_wddm_allocation_create(struct tu_wddm_context *context,
                          const struct tu_wddm_allocation_desc *desc,
                          struct tu_wddm_allocation *allocation)
{
   if (allocation == NULL || !tu_wddm_allocation_desc_valid(context, desc))
      return false;

   memset(allocation, 0, sizeof(*allocation));

   VIOGPU_WDDM_ALLOCATION_INFO private_data = {};
   tu_wddm_init_header(&private_data.Header, tu_wddm_sizeof<VIOGPU_WDDM_ALLOCATION_INFO>());
   private_data.Size = desc->size;
   private_data.Alignment = desc->alignment;
   private_data.Flags = desc->flags;
   private_data.Format = desc->format;
   private_data.Width = desc->width;
   private_data.Height = desc->height;
   private_data.Pitch = desc->pitch;
   private_data.RefreshRateNumerator = desc->refresh_rate_numerator;
   private_data.RefreshRateDenominator = desc->refresh_rate_denominator;
   if ((desc->flags & VIOGPU_WDDM_ALLOCATION_NATIVE) != 0) {
      private_data.RequestedIova = desc->requested_iova;
      private_data.ExpectedResetGeneration = context->info.ResetGeneration;
      private_data.ContextId = context->info.ContextId;
   }

   D3DDDI_ALLOCATIONINFO allocation_info = {};
   allocation_info.pPrivateDriverData = &private_data;
   allocation_info.PrivateDriverDataSize = tu_wddm_sizeof<VIOGPU_WDDM_ALLOCATION_INFO>();

   D3DKMT_CREATEALLOCATION create = {};
   create.hDevice = context->device->handle;
   create.NumAllocations = 1;
   create.pAllocationInfo = &allocation_info;
   create.Flags.NonSecure = 1;

   NTSTATUS status = context->device->adapter.runtime->dispatch.CreateAllocation(&create);
   if (!NT_SUCCESS(status) || allocation_info.hAllocation == 0) {
      /* A failing thunk is not allowed to leave a partially-created KMD
       * allocation behind.  Some test/KMD implementations can return a
       * handle together with a failure status, so explicitly attempt the
       * compensating destroy before dropping the user-mode descriptor. */
      if (allocation_info.hAllocation != 0) {
         allocation->context = context;
         allocation->handle = allocation_info.hAllocation;
         allocation->private_info = private_data;
         allocation->vma_size = (desc->size + UINT64_C(4095)) & ~UINT64_C(4095);
         D3DKMT_HANDLE handle = allocation_info.hAllocation;
         D3DKMT_DESTROYALLOCATION destroy = {};
         destroy.hDevice = context->device->handle;
         destroy.phAllocationList = &handle;
         destroy.AllocationCount = 1;
         if (NT_SUCCESS(context->device->adapter.runtime->dispatch.DestroyAllocation(&destroy)))
            memset(allocation, 0, sizeof(*allocation));
      }
      return false;
   }

   allocation->context = context;
   allocation->handle = allocation_info.hAllocation;
   allocation->private_info = private_data;
   allocation->vma_size = (desc->size + UINT64_C(4095)) & ~UINT64_C(4095);
   return true;
}

bool
tu_wddm_allocation_destroy(struct tu_wddm_allocation *allocation)
{
   if (allocation == NULL || allocation->context == NULL || allocation->handle == 0 ||
       allocation->locked || allocation->context->device == NULL ||
       allocation->context->device->adapter.runtime == NULL ||
       allocation->context->device->handle == 0)
      return false;

   D3DKMT_HANDLE handle = allocation->handle;
   D3DKMT_DESTROYALLOCATION destroy = {};
   destroy.hDevice = allocation->context->device->handle;
   destroy.phAllocationList = &handle;
   destroy.AllocationCount = 1;

   NTSTATUS status = allocation->context->device->adapter.runtime->dispatch.DestroyAllocation(&destroy);
   if (!NT_SUCCESS(status))
      return false;

   memset(allocation, 0, sizeof(*allocation));
   return true;
}

bool
tu_wddm_allocation_lock(struct tu_wddm_allocation *allocation, void **map)
{
   if (map != NULL)
      *map = NULL;
   if (allocation == NULL || allocation->context == NULL || allocation->handle == 0 ||
       allocation->locked || allocation->context->device == NULL ||
       allocation->context->device->adapter.runtime == NULL || map == NULL ||
       (allocation->private_info.Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) == 0)
      return false;

   D3DKMT_LOCK lock = {};
   lock.hDevice = allocation->context->device->handle;
   lock.hAllocation = allocation->handle;
   lock.Flags.ReadOnly = 0;
   lock.Flags.LockEntire = 1;

   NTSTATUS status = allocation->context->device->adapter.runtime->dispatch.Lock(&lock);
   if (!NT_SUCCESS(status))
      return false;
   if (lock.pData == NULL) {
      /* A successful Lock with no CPU pointer is unusable, but it may still
       * hold a VidMm lock.  Release that lock before reporting failure. */
      D3DKMT_HANDLE handle = allocation->handle;
      D3DKMT_UNLOCK unlock = {};
      unlock.hDevice = allocation->context->device->handle;
      unlock.NumAllocations = 1;
      unlock.phAllocations = &handle;
      NTSTATUS unlock_status =
         allocation->context->device->adapter.runtime->dispatch.Unlock(&unlock);
      if (!NT_SUCCESS(unlock_status)) {
         /* Preserve the successful Lock owner so teardown can retry Unlock.
          * The missing CPU pointer still makes this map attempt unusable. */
         allocation->locked = true;
      }
      return false;
   }

   allocation->map = lock.pData;
   allocation->locked = true;
   *map = lock.pData;
   return true;
}

bool
tu_wddm_allocation_unlock(struct tu_wddm_allocation *allocation)
{
   if (allocation == NULL || allocation->context == NULL || allocation->handle == 0 ||
       !allocation->locked || allocation->context->device == NULL ||
       allocation->context->device->adapter.runtime == NULL)
      return false;

   D3DKMT_HANDLE handle = allocation->handle;
   D3DKMT_UNLOCK unlock = {};
   unlock.hDevice = allocation->context->device->handle;
   unlock.NumAllocations = 1;
   unlock.phAllocations = &handle;

   NTSTATUS status = allocation->context->device->adapter.runtime->dispatch.Unlock(&unlock);
   if (!NT_SUCCESS(status))
      return false;

   allocation->map = NULL;
   allocation->locked = false;
   return true;
}

static bool
tu_wddm_render_reference_valid(const struct tu_wddm_context *context,
                               const struct tu_wddm_render_reference *reference,
                               uint32_t command_stream_size)
{
   if (context == NULL || reference == NULL || reference->allocation == NULL ||
       reference->allocation->context != context || reference->allocation->handle == 0 ||
       command_stream_size < sizeof(uint64_t) ||
       !tu_wddm_header_is_current(&reference->allocation->private_info.Header,
                                  tu_wddm_sizeof<VIOGPU_WDDM_ALLOCATION_INFO>()) ||
       reference->allocation->private_info.Size == 0 ||
       (reference->allocation->private_info.Flags & VIOGPU_WDDM_ALLOCATION_NATIVE) == 0 ||
       reference->allocation->private_info.ExpectedResetGeneration != context->info.ResetGeneration ||
       reference->allocation->private_info.ContextId != context->info.ContextId ||
       reference->flags == 0 ||
       (reference->flags & ~(VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE)) != 0 ||
       reference->length == 0 || reference->allocation_offset > UINT32_MAX ||
       reference->length > reference->allocation->private_info.Size -
                               (reference->allocation_offset <= reference->allocation->private_info.Size
                                   ? reference->allocation_offset
                                   : reference->allocation->private_info.Size) ||
       reference->patch_offset > command_stream_size - sizeof(uint64_t) ||
       ((reference->allocation->private_info.Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) != 0 &&
          (reference->flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0))
      return false;

   return true;
}

static bool
tu_wddm_native_submit_valid(const void *command_stream,
                            uint32_t command_stream_size,
                            const struct tu_wddm_render_reference *references,
                            uint32_t reference_count,
                            uint32_t submit_queue_id,
                            uint32_t *submitted_fence)
{
   if (submitted_fence != NULL)
      *submitted_fence = 0;
   if (command_stream == NULL || references == NULL || command_stream_size < sizeof(tu_wddm_msm_submit_request) ||
       (command_stream_size & (sizeof(uint32_t) - 1)) != 0 || submitted_fence == NULL)
      return false;

   const BYTE *stream = static_cast<const BYTE *>(command_stream);
   tu_wddm_msm_submit_request request = {};
   memcpy(&request, stream, sizeof(request));

   const uint32_t valid_submit_flags = TU_WDDM_MSM_PIPE_3D0 | TU_WDDM_MSM_SUBMIT_NO_IMPLICIT;
   if (request.command != TU_WDDM_MSM_CCMD_GEM_SUBMIT || request.length != command_stream_size ||
       request.sequence == 0 || request.response_offset != 0 || request.flags == 0 ||
       (request.flags & ~valid_submit_flags) != 0 || (request.flags & TU_WDDM_MSM_PIPE_3D0) != TU_WDDM_MSM_PIPE_3D0 ||
       request.queue_id != submit_queue_id || request.fence == 0 || request.bo_count != reference_count ||
       request.bo_count == 0 || request.command_count == 0)
      return false;

   const uint64_t bo_bytes = static_cast<uint64_t>(request.bo_count) * sizeof(tu_wddm_msm_submit_bo);
   const uint64_t command_bytes = static_cast<uint64_t>(request.command_count) * sizeof(tu_wddm_msm_submit_command);
   const uint64_t expected_size = sizeof(request) + bo_bytes + command_bytes;
   if (expected_size != command_stream_size)
      return false;

   const uint32_t valid_bo_flags = TU_WDDM_MSM_SUBMIT_BO_READ | TU_WDDM_MSM_SUBMIT_BO_WRITE |
                                   TU_WDDM_MSM_SUBMIT_BO_DUMP | TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;
   for (uint32_t i = 0; i < request.bo_count; i++) {
      tu_wddm_msm_submit_bo bo = {};
      const uint64_t bo_offset = sizeof(request) + static_cast<uint64_t>(i) * sizeof(tu_wddm_msm_submit_bo);
      memcpy(&bo, stream + bo_offset, sizeof(bo));

      uint32_t expected_access = 0;
      if ((references[i].flags & VIOGPU_WDDM_REFERENCE_READ) != 0)
         expected_access |= TU_WDDM_MSM_SUBMIT_BO_READ;
      if ((references[i].flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0)
         expected_access |= TU_WDDM_MSM_SUBMIT_BO_WRITE;

      const uint32_t expected_patch_offset =
         static_cast<uint32_t>(bo_offset + offsetof(tu_wddm_msm_submit_bo, presumed));
      if (bo.handle != 0 || bo.presumed != 0 || bo.flags == 0 || (bo.flags & ~valid_bo_flags) != 0 ||
          (bo.flags & (TU_WDDM_MSM_SUBMIT_BO_READ | TU_WDDM_MSM_SUBMIT_BO_WRITE)) != expected_access ||
          references[i].patch_offset != expected_patch_offset)
         return false;
   }

   const uint64_t commands_offset = sizeof(request) + bo_bytes;
   for (uint32_t i = 0; i < request.command_count; i++) {
      tu_wddm_msm_submit_command command = {};
      const uint64_t command_offset = commands_offset + static_cast<uint64_t>(i) * sizeof(tu_wddm_msm_submit_command);
      memcpy(&command, stream + command_offset, sizeof(command));

      if ((command.type != TU_WDDM_MSM_SUBMIT_CMD_BUF && command.type != TU_WDDM_MSM_SUBMIT_CMD_IB_TARGET_BUF) ||
          command.submit_index >= request.bo_count || command.size == 0 ||
          (command.size & (sizeof(uint32_t) - 1)) != 0 || command.padding != 0 || command.relocation_count != 0 ||
          command.iova != 0)
         return false;

      const struct tu_wddm_allocation *allocation = references[command.submit_index].allocation;
      if (command.submit_offset > allocation->private_info.Size ||
          command.size > allocation->private_info.Size - command.submit_offset)
         return false;
   }

   *submitted_fence = request.fence;
   return true;
}

static bool
tu_wddm_render_replacements_valid(const D3DKMT_RENDER *render)
{
   if (render == NULL || render->pNewCommandBuffer == NULL ||
       render->pNewAllocationList == NULL || render->pNewPatchLocationList == NULL ||
       render->NewCommandBufferSize < sizeof(VIOGPU_WDDM_RENDER_COMMAND) ||
       render->NewCommandBufferSize > TU_WDDM_MAX_RENDER_COMMAND_SIZE ||
       render->NewAllocationListSize == 0 ||
       render->NewAllocationListSize > TU_WDDM_MAX_RENDER_ALLOCATIONS ||
       render->NewPatchLocationListSize == 0 ||
       render->NewPatchLocationListSize > TU_WDDM_MAX_RENDER_ALLOCATIONS)
      return false;

   return true;
}

bool
tu_wddm_context_render(struct tu_wddm_context *context,
                       const void *command_stream,
                       uint32_t command_stream_size,
                       const struct tu_wddm_render_reference *references,
                       uint32_t reference_count)
{
   if (context == NULL || context->device == NULL || context->handle == 0 ||
       context->device->adapter.runtime == NULL || context->command_buffer == NULL ||
       context->allocation_list == NULL || context->patch_location_list == NULL ||
       command_stream == NULL || command_stream_size < sizeof(uint64_t) ||
       command_stream_size > TU_WDDM_MAX_RENDER_COMMAND_SIZE || references == NULL ||
       reference_count == 0 || reference_count > TU_WDDM_MAX_RENDER_ALLOCATIONS ||
       !tu_wddm_validate_context_info(&context->info, context->device->adapter.private_info.ResetGeneration))
      return false;

   const uint64_t references_size = static_cast<uint64_t>(reference_count) *
                                    sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE);
   const uint64_t command_offset = sizeof(VIOGPU_WDDM_RENDER_COMMAND) + references_size;
   const uint64_t command_length = command_offset + command_stream_size;
   if (command_length > TU_WDDM_MAX_RENDER_COMMAND_SIZE ||
       command_length > context->command_buffer_size ||
       reference_count > context->allocation_list_size ||
       reference_count > context->patch_location_list_size)
      return false;

   for (uint32_t i = 0; i < reference_count; i++) {
      if (!tu_wddm_render_reference_valid(context, &references[i], command_stream_size))
         return false;
      for (uint32_t j = 0; j < i; j++) {
         if (references[i].allocation->handle == references[j].allocation->handle)
            return false;

         const uint32_t a = references[i].patch_offset;
         const uint32_t b = references[j].patch_offset;
         if (a < b + sizeof(uint64_t) && b < a + sizeof(uint64_t))
            return false;
      }
   }

   uint32_t submitted_fence = 0;
   if (!tu_wddm_native_submit_valid(command_stream, command_stream_size, references, reference_count,
                                    context->info.SubmitQueueId, &submitted_fence))
      return false;
   if (context->last_submitted_fence != 0 &&
       !tu_wddm_fence_after(submitted_fence, context->last_submitted_fence))
      return false;

   BYTE *packet = static_cast<BYTE *>(context->command_buffer);
   memset(packet, 0, static_cast<size_t>(command_length));

   VIOGPU_WDDM_RENDER_COMMAND *header = reinterpret_cast<VIOGPU_WDDM_RENDER_COMMAND *>(packet);
   tu_wddm_init_header(&header->Header, static_cast<uint32_t>(command_length));
   header->Opcode = VIOGPU_WDDM_RENDER_NATIVE_SUBMIT;
   header->Flags = VIOGPU_WDDM_RENDER_FLAGS_NONE;
   header->ExpectedResetGeneration = context->info.ResetGeneration;
   header->AllocationReferencesOffset = sizeof(*header);
   header->AllocationReferenceCount = reference_count;
   header->CommandStreamOffset = static_cast<uint32_t>(command_offset);
   header->CommandStreamSize = command_stream_size;

   VIOGPU_WDDM_ALLOCATION_REFERENCE *wire_references =
      reinterpret_cast<VIOGPU_WDDM_ALLOCATION_REFERENCE *>(packet + sizeof(*header));
   memcpy(packet + command_offset, command_stream, command_stream_size);

   for (uint32_t i = 0; i < reference_count; i++) {
      const struct tu_wddm_render_reference *reference = &references[i];
      wire_references[i].AllocationIndex = i;
      wire_references[i].Flags = reference->flags;
      wire_references[i].AllocationOffset = reference->allocation_offset;
      wire_references[i].Length = reference->length;
      wire_references[i].PatchOffset = reference->patch_offset;

      context->allocation_list[i] = {};
      context->allocation_list[i].hAllocation = reference->allocation->handle;
      context->allocation_list[i].WriteOperation =
         (reference->flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0;

      context->patch_location_list[i] = {};
      context->patch_location_list[i].AllocationIndex = i;
      context->patch_location_list[i].SlotId = 0;
      context->patch_location_list[i].AllocationOffset =
         static_cast<UINT>(reference->allocation_offset);
      context->patch_location_list[i].PatchOffset =
         static_cast<UINT>(command_offset + reference->patch_offset);
   }

   D3DKMT_RENDER render = {};
   render.hContext = context->handle;
   render.CommandOffset = 0;
   render.CommandLength = static_cast<UINT>(command_length);
   render.AllocationCount = reference_count;
   render.PatchLocationCount = reference_count;
   render.NewCommandBufferSize = context->command_buffer_size;
   render.NewAllocationListSize = context->allocation_list_size;
   render.NewPatchLocationListSize = context->patch_location_list_size;
   /* D3DKMT_RENDER requires the replacement pointers to be consumed after
    * every call, including a failed call.  Seed them with the current owner
    * so a thunk failure that leaves [out] fields untouched remains retryable. */
   render.pNewCommandBuffer = context->command_buffer;
   render.pNewAllocationList = context->allocation_list;
   render.pNewPatchLocationList = context->patch_location_list;

   NTSTATUS status = context->device->adapter.runtime->dispatch.Render(&render);

   const bool replacements_valid = tu_wddm_render_replacements_valid(&render);
   if (!replacements_valid) {
      /* The KMT contract promises a replacement set after every call.  A
       * partial or out-of-range set cannot be safely owned, so disable the
       * context rather than retaining a pointer whose lifetime is unknown. */
      context->command_buffer = NULL;
      context->command_buffer_size = 0;
      context->allocation_list = NULL;
      context->allocation_list_size = 0;
      context->patch_location_list = NULL;
      context->patch_location_list_size = 0;
   } else {
      context->command_buffer = render.pNewCommandBuffer;
      context->command_buffer_size = render.NewCommandBufferSize;
      context->allocation_list = render.pNewAllocationList;
      context->allocation_list_size = render.NewAllocationListSize;
      context->patch_location_list = render.pNewPatchLocationList;
      context->patch_location_list_size = render.NewPatchLocationListSize;
   }

   /* A successful call may have transferred the submitted command buffer to
    * VidSch even when its replacement metadata is malformed.  Keep that
    * fence in context-owned storage so teardown never needs queue objects,
    * which the Vulkan device destroys first. */
   if (NT_SUCCESS(status) &&
       (context->last_submitted_fence == 0 ||
        tu_wddm_fence_after(submitted_fence, context->last_submitted_fence)))
      context->last_submitted_fence = submitted_fence;

   return NT_SUCCESS(status) && replacements_valid;
}

#ifdef TU_HAS_WDDM

/* The WDDM path deliberately starts with one context and one engine.  These
 * limits are also enforced by the private ABI/KMD, so rejecting an oversized
 * packet here keeps the UMD from constructing a request the KMD cannot own. */
enum {
   TU_WDDM_MAX_SUBMIT_COMMANDS = 256,
   TU_WDDM_MAX_SUBMIT_REFERENCES = TU_WDDM_MAX_RENDER_ALLOCATIONS,
};

struct tu_wddm_sync {
   struct vk_sync base;
   struct tu_wddm_context *context;
   uint64_t reset_generation;
   alignas(8) uint64_t state;
};

enum : uint64_t {
   TU_WDDM_SYNC_SIGNALED = UINT64_C(1) << 63,
};

static uint64_t
tu_wddm_sync_state_read(struct tu_wddm_sync *sync)
{
   return static_cast<uint64_t>(p_atomic_cmpxchg(&sync->state, 0, 0));
}

static void
tu_wddm_sync_state_set(struct tu_wddm_sync *sync, uint64_t state)
{
   p_atomic_xchg(&sync->state, state);
}

struct tu_wddm_submit_entry {
   struct tu_bo *bo;
   uint32_t offset;
   uint32_t size;
};

struct tu_wddm_submit_reference {
   struct tu_bo *bo;
   uint32_t access;
};

struct tu_wddm_submit {
   struct util_dynarray entries;
   struct util_dynarray references;
   bool failed;
};

static inline struct tu_wddm_sync *
tu_wddm_sync_from_vk(struct vk_sync *sync)
{
   return container_of(sync, struct tu_wddm_sync, base);
}

static bool
tu_wddm_sync_is_current(const struct tu_wddm_sync *sync)
{
   return sync != NULL && sync->context != NULL && sync->context->device != NULL &&
          sync->context->handle != 0 && sync->reset_generation != 0 &&
          sync->reset_generation == sync->context->info.ResetGeneration &&
          sync->reset_generation ==
             sync->context->device->adapter.private_info.ResetGeneration;
}

static VkResult
tu_wddm_sync_init(struct vk_device *_device, struct vk_sync *base,
                  uint64_t initial_value)
{
   struct tu_device *device = container_of(_device, struct tu_device, vk);
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);

   sync->context = &device->wddm_context;
   sync->reset_generation = sync->context->info.ResetGeneration;
   tu_wddm_sync_state_set(sync,
                          initial_value != 0 ? TU_WDDM_SYNC_SIGNALED : 0);
   return sync->reset_generation != 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static void
tu_wddm_sync_finish(struct vk_device *device, struct vk_sync *base)
{
   (void)device;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   sync->context = NULL;
   sync->reset_generation = 0;
   tu_wddm_sync_state_set(sync, 0);
}

static VkResult
tu_wddm_sync_signal(struct vk_device *device, struct vk_sync *base,
                    uint64_t value)
{
   (void)device;
   (void)value;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   if (!tu_wddm_sync_is_current(sync))
      return VK_ERROR_DEVICE_LOST;

   tu_wddm_sync_state_set(sync, TU_WDDM_SYNC_SIGNALED);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_sync_reset(struct vk_device *device, struct vk_sync *base)
{
   (void)device;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   if (!tu_wddm_sync_is_current(sync))
      return VK_ERROR_DEVICE_LOST;

   tu_wddm_sync_state_set(sync, 0);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_sync_move(struct vk_device *device, struct vk_sync *dst_base,
                  struct vk_sync *src_base)
{
   (void)device;
   struct tu_wddm_sync *dst = tu_wddm_sync_from_vk(dst_base);
   struct tu_wddm_sync *src = tu_wddm_sync_from_vk(src_base);
   if (!tu_wddm_sync_is_current(src) || !tu_wddm_sync_is_current(dst))
      return VK_ERROR_DEVICE_LOST;

   const uint64_t state = p_atomic_xchg(&src->state, 0);
   dst->context = src->context;
   dst->reset_generation = src->reset_generation;
   tu_wddm_sync_state_set(dst, state);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_sync_wait(struct vk_device *_device, struct vk_sync *base,
                  uint64_t wait_value, enum vk_sync_wait_flags wait_flags,
                  uint64_t abs_timeout_ns)
{
   (void)_device;
   (void)wait_value;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);

   for (;;) {
      const uint64_t state = tu_wddm_sync_state_read(sync);
      const uint32_t fence = static_cast<uint32_t>(state);
      const bool signaled = (state & TU_WDDM_SYNC_SIGNALED) != 0;

      if (signaled || ((wait_flags & VK_SYNC_WAIT_PENDING) && fence != 0))
         return VK_SUCCESS;
      if (!tu_wddm_sync_is_current(sync))
         return VK_ERROR_DEVICE_LOST;

      if (fence != 0) {
         uint32_t completed = 0;
         if (!tu_wddm_context_get_completed_fence(sync->context, &completed))
            return VK_ERROR_DEVICE_LOST;
         if (completed == fence || tu_wddm_fence_after(completed, fence)) {
            if (static_cast<uint64_t>(p_atomic_cmpxchg(
                   &sync->state, state, TU_WDDM_SYNC_SIGNALED)) == state)
               return VK_SUCCESS;
            continue;
         }
      }

      const uint64_t now = static_cast<uint64_t>(os_time_get_nano());
      if (abs_timeout_ns != OS_TIMEOUT_INFINITE && now >= abs_timeout_ns)
         return VK_TIMEOUT;
      os_time_sleep(1000);
   }
}

static VkResult
tu_wddm_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                       const struct vk_sync_wait *waits,
                       enum vk_sync_wait_flags wait_flags,
                       uint64_t abs_timeout_ns)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   if (wait_flags & VK_SYNC_WAIT_ANY) {
      for (;;) {
         for (uint32_t i = 0; i < wait_count; i++) {
            if (waits[i].sync == NULL)
               continue;
            VkResult result = tu_wddm_sync_wait(device, waits[i].sync,
                                                waits[i].wait_value,
                                                static_cast<enum vk_sync_wait_flags>(
                                                   wait_flags & ~VK_SYNC_WAIT_ANY),
                                                0);
            if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
               return result;
         }
         if (abs_timeout_ns != OS_TIMEOUT_INFINITE &&
             static_cast<uint64_t>(os_time_get_nano()) >= abs_timeout_ns)
            return VK_TIMEOUT;
         os_time_sleep(1000);
      }
   }

   for (uint32_t i = 0; i < wait_count; i++) {
      if (waits[i].sync == NULL)
         continue;
      VkResult result = tu_wddm_sync_wait(device, waits[i].sync,
                                          waits[i].wait_value, wait_flags,
                                          abs_timeout_ns);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static const struct vk_sync_type tu_wddm_sync_type = {
   .size = sizeof(struct tu_wddm_sync),
   .features = (enum vk_sync_features)(VK_SYNC_FEATURE_BINARY |
                                       VK_SYNC_FEATURE_GPU_WAIT |
                                       VK_SYNC_FEATURE_GPU_MULTI_WAIT |
                                       VK_SYNC_FEATURE_CPU_WAIT |
                                       VK_SYNC_FEATURE_CPU_RESET |
                                       VK_SYNC_FEATURE_CPU_SIGNAL |
                                       VK_SYNC_FEATURE_WAIT_ANY |
                                       VK_SYNC_FEATURE_WAIT_PENDING),
   .init = tu_wddm_sync_init,
   .finish = tu_wddm_sync_finish,
   .signal = tu_wddm_sync_signal,
   .reset = tu_wddm_sync_reset,
   .move = tu_wddm_sync_move,
   .wait = tu_wddm_sync_wait,
   .wait_many = tu_wddm_sync_wait_many,
};

/* ------------------------------------------------------------------------- */
/* WDDM-backed Turnip kernel interface                                       */

static inline bool
tu_wddm_bo_valid(const struct tu_bo *bo)
{
   return bo != NULL && bo->gem_handle != 0 && bo->wddm_allocation != NULL &&
          bo->wddm_allocation->handle != 0 && bo->wddm_allocation->context != NULL;
}

static void
tu_wddm_remove_bo_locked(struct tu_device *dev, struct tu_bo *bo)
{
   for (uint32_t i = 0; i < dev->wddm_bo_count; i++) {
      if (dev->wddm_bos[i] != bo)
         continue;
      dev->wddm_bos[i] = dev->wddm_bos[--dev->wddm_bo_count];
      return;
   }
}

static uint32_t
tu_wddm_alloc_token_locked(struct tu_device *dev)
{
   /* Token zero is reserved.  Keep the search bounded; a process cannot have
    * anywhere near this many live allocations and a wrapped token must never
    * alias a live sparse-array slot. */
   for (uint32_t attempts = 0; attempts < (1u << 20); attempts++) {
      uint32_t token = dev->wddm_next_handle++;
      if (token == 0)
         continue;
      struct tu_bo *slot = tu_device_lookup_bo(dev, token);
      if (slot->refcnt == 0 && slot->wddm_allocation == NULL)
         return token;
   }
   return 0;
}

static bool
tu_wddm_add_bo_locked(struct tu_device *dev, struct tu_bo *bo)
{
   if (dev->wddm_bo_count == dev->wddm_bo_capacity) {
      uint32_t capacity = dev->wddm_bo_capacity ? dev->wddm_bo_capacity * 2 : 64;
      struct tu_bo **bos = (struct tu_bo **)vk_realloc(
         &dev->vk.alloc, dev->wddm_bos, capacity * sizeof(*bos), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (bos == NULL)
         return false;
      dev->wddm_bos = bos;
      dev->wddm_bo_capacity = capacity;
   }
   dev->wddm_bos[dev->wddm_bo_count++] = bo;
   return true;
}

static VkResult
tu_wddm_device_init(struct tu_device *dev)
{
   struct tu_physical_device *physical = dev->physical_device;
   struct tu_instance *instance = physical->instance;

   dev->fd = -1;
   dev->wddm_next_handle = 1;
   dev->wddm_next_fence = 1;
   dev->wddm_pending_submission_upper_bound = 0;
   dev->wddm_teardown_failed = false;
   dev->wddm_bos = NULL;
   dev->wddm_bo_count = 0;
   dev->wddm_bo_capacity = 0;

   if (!instance->wddm_runtime_initialized ||
       !tu_wddm_device_open(&instance->wddm_runtime, &physical->wddm_adapter,
                            &dev->wddm_device)) {
      if (dev->wddm_device.handle != 0 || dev->wddm_device.adapter.handle != 0) {
         /* There is no context owner on this path, so a failed close can be
          * retried by the device-finish hook without destroying a live
          * context. */
         tu_wddm_device_close(&dev->wddm_device);
      }
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                               "failed to open WDDM adapter device");
   }

   if (!tu_wddm_context_open(&dev->wddm_device, &dev->wddm_context)) {
      if (dev->wddm_context.handle != 0 &&
          !tu_wddm_context_close(&dev->wddm_context))
         return vk_startup_errorf(instance, VK_ERROR_DEVICE_LOST,
                                  "failed to close WDDM native context");
      /* The device may only be closed after the context owner is gone. */
      if ((dev->wddm_device.handle != 0 ||
           dev->wddm_device.adapter.handle != 0) &&
          !tu_wddm_device_close(&dev->wddm_device))
         return vk_startup_errorf(instance, VK_ERROR_DEVICE_LOST,
                                  "failed to close WDDM adapter device");
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                               "failed to open WDDM native context");
   }

   dev->va_start = dev->wddm_context.info.VaStart;
   dev->va_size = dev->wddm_context.info.VaSize;
   dev->wddm_initialized = true;
   return VK_SUCCESS;
}

static void
tu_wddm_device_finish(struct tu_device *dev)
{
   if (!dev->wddm_initialized && dev->wddm_context.handle == 0 &&
       dev->wddm_device.handle == 0 && dev->wddm_device.adapter.handle == 0)
      return;

   /* This is the final Vulkan-device hook.  A failed KMT operation cannot be
    * retried after tu_DestroyDevice releases the outer device, so preserve the
    * complete owner graph on every failure path.  The retained KMT objects are
    * intentionally process-lifetime owned; leaking them is safer than freeing
    * UMD bookkeeping/VMA state while a live allocation or parent handle still
    * exists. */
   const bool submissions_retired =
      tu_wddm_context_wait_submissions(&dev->wddm_context, UINT64_MAX);
   if (!submissions_retired) {
      dev->wddm_teardown_failed = true;
      vk_device_set_lost(&dev->vk, "failed to retire WDDM queue work");
      return;
   }

   while (dev->wddm_bo_count != 0) {
      struct tu_bo *bo = dev->wddm_bos[dev->wddm_bo_count - 1];
      struct tu_wddm_allocation *allocation = bo->wddm_allocation;
      if (allocation == NULL) {
         /* A zero sparse-array slot is never an owned allocation.  Drop only
          * the bookkeeping entry; a non-zero handle is retained below on all
          * failure paths. */
         dev->wddm_bo_count--;
         continue;
      }
      const uint64_t allocation_iova = allocation->private_info.RequestedIova;
      const uint64_t allocation_size = allocation->vma_size != 0
                                          ? allocation->vma_size
                                          : (allocation->private_info.Size + UINT64_C(4095)) &
                                               ~UINT64_C(4095);

      if (allocation->locked && !tu_wddm_allocation_unlock(allocation)) {
         dev->wddm_teardown_failed = true;
         vk_device_set_lost(&dev->vk,
                            "failed to unlock WDDM allocation during teardown");
         return;
      }
      bo->map = NULL;
      if (allocation->handle != 0 &&
          !tu_wddm_allocation_destroy(allocation)) {
         dev->wddm_teardown_failed = true;
         vk_device_set_lost(&dev->vk,
                            "failed to destroy WDDM allocation during teardown");
         return;
      }

      tu_debug_bos_del(dev, bo);
      tu_dump_bo_del(dev, bo);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, allocation_iova, allocation_size);
      mtx_unlock(&dev->vma_mutex);
      vk_free(&dev->vk.alloc, allocation);
      memset(bo, 0, sizeof(*bo));
      dev->wddm_bo_count--;
   }

   vk_free(&dev->vk.alloc, dev->wddm_bos);
   dev->wddm_bos = NULL;
   dev->wddm_bo_capacity = 0;

   bool context_closed = dev->wddm_context.handle == 0;
   if (!context_closed) {
      context_closed = tu_wddm_context_close(&dev->wddm_context);
      if (!context_closed) {
         dev->wddm_teardown_failed = true;
         vk_device_set_lost(&dev->vk, "failed to close WDDM context");
         return;
      }
   }
   if (context_closed &&
       (dev->wddm_device.handle != 0 ||
        dev->wddm_device.adapter.handle != 0)) {
      if (!tu_wddm_device_close(&dev->wddm_device)) {
         dev->wddm_teardown_failed = true;
         vk_device_set_lost(&dev->vk, "failed to close WDDM device");
         return;
      }
   }

   memset(&dev->wddm_context, 0, sizeof(dev->wddm_context));
   memset(&dev->wddm_device, 0, sizeof(dev->wddm_device));
   dev->wddm_initialized = false;
   dev->wddm_teardown_failed = false;
}

static int
tu_wddm_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)
{
   (void)dev;
   if (ts != NULL)
      *ts = 0;
   return -ENOSYS;
}

static int
tu_wddm_device_get_suspend_count(struct tu_device *dev, uint64_t *suspend_count)
{
   (void)dev;
   if (suspend_count != NULL)
      *suspend_count = 0;
   return -ENOSYS;
}

static VkResult
tu_wddm_device_check_status(struct tu_device *dev)
{
   uint32_t completed = 0;
   if (!dev->wddm_initialized ||
       !tu_wddm_context_get_completed_fence(&dev->wddm_context, &completed))
      return vk_device_set_lost(&dev->vk,
                                "WDDM context completion query failed");
   return VK_SUCCESS;
}

static int
tu_wddm_submitqueue_new(struct tu_device *dev, struct tu_queue *queue)
{
   if (queue->type != TU_QUEUE_GFX ||
       dev->physical_device->submitqueue_priority_count != 1 ||
       !tu_wddm_submitqueue_priority_is_supported(queue->priority) ||
       dev->wddm_context.info.SubmitQueueId == 0)
      return -EINVAL;
   queue->msm_queue_id = dev->wddm_context.info.SubmitQueueId;
   return 0;
}

static void
tu_wddm_submitqueue_close(struct tu_device *dev, struct tu_queue *queue)
{
   (void)dev;
   queue->msm_queue_id = 0;
}

static VkResult
tu_wddm_bo_init(struct tu_device *dev, struct vk_object_base *base,
                struct tu_bo **out_bo, uint64_t size, uint64_t client_iova,
                VkMemoryPropertyFlags mem_property,
                enum tu_bo_alloc_flags flags,
                struct tu_sparse_vma *lazy_vma, const char *name)
{
   if (out_bo != NULL)
      *out_bo = NULL;
   if (out_bo == NULL || lazy_vma != NULL || size == 0 ||
       (flags & (TU_BO_ALLOC_DMABUF | TU_BO_ALLOC_SHAREABLE |
                 TU_BO_ALLOC_IMPLICIT_SYNC)) != 0)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);

   if ((mem_property & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);

   if (size > UINT64_MAX - UINT64_C(4095))
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   const uint64_t vma_size = (size + UINT64_C(4095)) & ~UINT64_C(4095);
   if (vma_size == 0)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   uint64_t iova = 0;
   mtx_lock(&dev->vma_mutex);
   if (client_iova != 0) {
      if (!util_vma_heap_alloc_addr(&dev->vma, client_iova, vma_size)) {
         mtx_unlock(&dev->vma_mutex);
         return vk_error(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
      }
      iova = client_iova;
   } else {
      iova = util_vma_heap_alloc(&dev->vma, vma_size, os_page_size);
   }
   mtx_unlock(&dev->vma_mutex);
   if (iova == 0)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct tu_wddm_allocation *allocation = (struct tu_wddm_allocation *)vk_zalloc(
      &dev->vk.alloc, sizeof(*allocation), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (allocation == NULL) {
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, vma_size);
      mtx_unlock(&dev->vma_mutex);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct tu_wddm_allocation_desc desc = {
      .size = size,
      .alignment = os_page_size,
      .requested_iova = iova,
      .flags = VIOGPU_WDDM_ALLOCATION_NATIVE |
               VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE |
               ((flags & TU_BO_ALLOC_GPU_READ_ONLY) != 0
                   ? VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY : 0),
   };

   /* Reserve the sparse-array slot before asking the KMD to create the
    * allocation.  The WDDM owner stays unpublished until CreateAllocation
    * returns, so a concurrent residency snapshot can skip this placeholder. */
   mtx_lock(&dev->bo_mutex);
   uint32_t token = tu_wddm_alloc_token_locked(dev);
   struct tu_bo *bo = token ? tu_device_lookup_bo(dev, token) : NULL;
   bool added = bo != NULL && tu_wddm_add_bo_locked(dev, bo);
   if (!added) {
      mtx_unlock(&dev->bo_mutex);
      vk_free(&dev->vk.alloc, allocation);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, vma_size);
      mtx_unlock(&dev->vma_mutex);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   *bo = (struct tu_bo) {
      .gem_handle = token,
      .size = size,
      .iova = iova,
      /* Account the debug name only after CreateAllocation succeeds. */
      .name = NULL,
      .refcnt = 1,
      .gpu_read_only = (flags & TU_BO_ALLOC_GPU_READ_ONLY) != 0,
      .base = base,
      .wddm_allocation = NULL,
   };
   mtx_unlock(&dev->bo_mutex);

   const bool allocation_created =
      tu_wddm_allocation_create(&dev->wddm_context, &desc, allocation);

   mtx_lock(&dev->bo_mutex);
   if (allocation_created || allocation->handle != 0) {
      bo->wddm_allocation = allocation;
   } else {
      tu_wddm_remove_bo_locked(dev, bo);
      memset(bo, 0, sizeof(*bo));
   }
   mtx_unlock(&dev->bo_mutex);

   if (!allocation_created) {
      if (allocation->handle != 0) {
         /* CreateAllocation returned a handle and its compensating destroy
          * failed.  Keep the sparse-array slot, VMA, and final BO reference as
          * the retry owner instead of orphaning a live KMT allocation. */
         return vk_device_set_lost(
            &dev->vk, "failed to roll back partial WDDM allocation creation");
      }
      vk_free(&dev->vk.alloc, allocation);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, vma_size);
      mtx_unlock(&dev->vma_mutex);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   bo->name = tu_debug_bos_add(dev, size, name);
   tu_dump_bo_init(dev, bo);
   *out_bo = bo;
   return VK_SUCCESS;
}

static VkResult
tu_wddm_bo_init_dmabuf(struct tu_device *dev, struct tu_bo **out_bo,
                       uint64_t size, enum tu_bo_alloc_flags flags, int prime_fd)
{
   (void)size;
   (void)flags;
   (void)prime_fd;
   if (out_bo != NULL)
      *out_bo = NULL;
   return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
}

static int
tu_wddm_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   (void)dev;
   (void)bo;
   errno = ENOSYS;
   return -1;
}

static VkResult
tu_wddm_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   if (placed_addr != NULL || !tu_wddm_bo_valid(bo))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   void *map = NULL;
   if (!tu_wddm_allocation_lock(bo->wddm_allocation, &map))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   bo->map = map;
   TU_RMV(bo_map, dev, bo);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_bo_unmap(struct tu_device *dev, struct tu_bo *bo, bool reserve)
{
   if (reserve || !tu_wddm_bo_valid(bo))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   if (!tu_wddm_allocation_unlock(bo->wddm_allocation))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   bo->map = NULL;
   return VK_SUCCESS;
}

static void
tu_wddm_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
   (void)dev;
   if (bo != NULL)
      bo->dump = true;
}

static void
tu_wddm_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   if (!tu_wddm_bo_valid(bo) || p_atomic_read(&bo->refcnt) <= 0 ||
       !p_atomic_dec_zero(&bo->refcnt))
      return;

   /* The callback has no error return.  Keep one final reference whenever a
    * KMD operation fails so device teardown (or a later retry) still owns the
    * allocation and its VMA. */
   const auto restore_owner = [bo]() {
      p_atomic_set(&bo->refcnt, 1);
   };

   /* Generic queue preparation already owns submit_mutex and can release a
    * replaced internal BO.  Use the WDDM-specific lock to serialize this
    * destruction with the live-BO residency snapshot and Render call without
    * recursively acquiring submit_mutex. */
   mtx_lock(&dev->wddm_mutex);
   if (!tu_wddm_context_wait_submissions(&dev->wddm_context, UINT64_MAX)) {
      vk_device_set_lost(&dev->vk, "failed to retire WDDM queue work");
      restore_owner();
      mtx_unlock(&dev->wddm_mutex);
      return;
   }

   if (bo->wddm_allocation->locked) {
      if (!tu_wddm_allocation_unlock(bo->wddm_allocation)) {
         vk_device_set_lost(&dev->vk, "failed to unlock WDDM allocation");
         restore_owner();
         mtx_unlock(&dev->wddm_mutex);
         return;
      }
      bo->map = NULL;
   }

   struct tu_wddm_allocation *allocation = bo->wddm_allocation;
   const uint64_t allocation_iova = allocation->private_info.RequestedIova;
   const uint64_t allocation_size = allocation->vma_size != 0
                                       ? allocation->vma_size
                                       : (allocation->private_info.Size + UINT64_C(4095)) &
                                            ~UINT64_C(4095);
   if (!tu_wddm_allocation_destroy(allocation)) {
      vk_device_set_lost(&dev->vk, "failed to destroy WDDM allocation");
      restore_owner();
      mtx_unlock(&dev->wddm_mutex);
      return;
   }

   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);

   mtx_lock(&dev->bo_mutex);
   tu_wddm_remove_bo_locked(dev, bo);
   memset(bo, 0, sizeof(*bo));
   mtx_unlock(&dev->bo_mutex);

   mtx_lock(&dev->vma_mutex);
   util_vma_heap_free(&dev->vma, allocation_iova, allocation_size);
   mtx_unlock(&dev->vma_mutex);
   vk_free(&dev->vk.alloc, allocation);
   mtx_unlock(&dev->wddm_mutex);
}

static void
tu_wddm_bo_set_metadata(struct tu_device *dev, struct tu_bo *bo,
                        void *metadata, uint32_t metadata_size)
{
   (void)dev;
   (void)bo;
   (void)metadata;
   (void)metadata_size;
}

static int
tu_wddm_bo_get_metadata(struct tu_device *dev, struct tu_bo *bo,
                        void *metadata, uint32_t metadata_size)
{
   (void)dev;
   (void)bo;
   (void)metadata;
   (void)metadata_size;
   return -ENOSYS;
}

static bool
tu_wddm_submit_add_reference(struct tu_wddm_submit *submit, struct tu_bo *bo,
                             uint32_t access)
{
   if (submit == NULL)
      return false;

   if (!tu_wddm_bo_valid(bo) || access == 0 ||
       (access & ~(TU_SUBMIT_BO_ACCESS_READ | TU_SUBMIT_BO_ACCESS_WRITE)) != 0 ||
       ((access & TU_SUBMIT_BO_ACCESS_WRITE) != 0 && bo->gpu_read_only)) {
      submit->failed = true;
      return false;
   }

   util_dynarray_foreach(&submit->references, struct tu_wddm_submit_reference, ref) {
      if (ref->bo == bo) {
         ref->access |= access;
         return true;
      }
   }

   const uint32_t reference_count = util_dynarray_num_elements(
      &submit->references, struct tu_wddm_submit_reference);
   if (reference_count >= TU_WDDM_MAX_SUBMIT_REFERENCES) {
      submit->failed = true;
      return false;
   }

   struct tu_wddm_submit_reference *ref = (struct tu_wddm_submit_reference *)
      util_dynarray_grow(&submit->references, struct tu_wddm_submit_reference, 1);
   if (ref == NULL) {
      submit->failed = true;
      return false;
   }
   ref->bo = bo;
   ref->access = access;
   return true;
}

static void *
tu_wddm_submit_create(struct tu_device *device)
{
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)vk_zalloc(
      &device->vk.alloc, sizeof(*submit), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (submit == NULL)
      return NULL;
   util_dynarray_init(&submit->entries, NULL);
   util_dynarray_init(&submit->references, NULL);
   return submit;
}

static void
tu_wddm_submit_finish(struct tu_device *device, void *_submit)
{
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit == NULL)
      return;
   util_dynarray_fini(&submit->entries);
   util_dynarray_fini(&submit->references);
   vk_free(&device->vk.alloc, submit);
}

static void
tu_wddm_submit_add_entries(struct tu_device *device, void *_submit,
                           struct tu_cs_entry *entries, unsigned num_entries)
{
   (void)device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit == NULL || entries == NULL || num_entries == 0 ||
       util_dynarray_num_elements(&submit->entries, struct tu_wddm_submit_entry) >
          TU_WDDM_MAX_SUBMIT_COMMANDS ||
       num_entries > TU_WDDM_MAX_SUBMIT_COMMANDS -
          util_dynarray_num_elements(&submit->entries, struct tu_wddm_submit_entry)) {
      if (submit != NULL)
         submit->failed = true;
      return;
   }

   for (unsigned i = 0; i < num_entries; i++) {
      const struct tu_cs_entry *entry = &entries[i];
      if (!tu_wddm_bo_valid(entry->bo) || entry->size == 0 ||
          (entry->size & 3) != 0 || entry->offset > entry->bo->size ||
          entry->size > entry->bo->size - entry->offset ||
          !tu_wddm_submit_add_reference(submit, (struct tu_bo *)entry->bo,
                                        TU_SUBMIT_BO_ACCESS_READ)) {
         submit->failed = true;
         return;
      }
   }

   struct tu_wddm_submit_entry *out = (struct tu_wddm_submit_entry *)
      util_dynarray_grow(&submit->entries, struct tu_wddm_submit_entry, num_entries);
   if (out == NULL) {
      submit->failed = true;
      return;
   }
   for (unsigned i = 0; i < num_entries; i++) {
      out[i] = (struct tu_wddm_submit_entry) {
         .bo = (struct tu_bo *)entries[i].bo,
         .offset = entries[i].offset,
         .size = entries[i].size,
      };
   }
}

static void
tu_wddm_submit_add_bos(struct tu_device *device, void *_submit,
                       struct tu_bo **bos, unsigned num_bos,
                       uint32_t access_flags)
{
   (void)device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit == NULL || bos == NULL || num_bos == 0) {
      if (submit != NULL && num_bos != 0)
         submit->failed = true;
      return;
   }
   for (unsigned i = 0; i < num_bos; i++)
      tu_wddm_submit_add_reference(submit, bos[i], access_flags);
}

static bool
tu_wddm_submit_add_live_bos(struct tu_device *device,
                            struct tu_wddm_submit *submit)
{
   /* Command packets contain raw IOVAs for descriptors, images, buffers, and
    * internal resources.  Like the legacy drm/msm submit path, conservatively
    * make every live BO resident because the packet itself cannot enumerate
    * those transitive dependencies. */
   mtx_lock(&device->bo_mutex);
   for (uint32_t i = 0; i < device->wddm_bo_count && !submit->failed; i++) {
      struct tu_bo *bo = device->wddm_bos[i];
      if (!tu_wddm_bo_valid(bo))
         continue;
      const uint32_t access = bo->gpu_read_only
                                 ? TU_SUBMIT_BO_ACCESS_READ
                                 : TU_SUBMIT_BO_ACCESS_READ |
                                      TU_SUBMIT_BO_ACCESS_WRITE;
      tu_wddm_submit_add_reference(submit, bo, access);
   }
   mtx_unlock(&device->bo_mutex);
   return !submit->failed;
}

static void
tu_wddm_submit_add_bind(struct tu_device *device, void *_submit,
                        struct tu_sparse_vma *vma, uint64_t vma_offset,
                        struct tu_bo *bo, uint64_t bo_offset, uint64_t size)
{
   (void)device;
   (void)vma;
   (void)vma_offset;
   (void)bo;
   (void)bo_offset;
   (void)size;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit != NULL)
      submit->failed = true;
}

static int
tu_wddm_submit_reference_index(const struct tu_wddm_submit *submit,
                               const struct tu_bo *bo)
{
   unsigned i = 0;
   util_dynarray_foreach(&submit->references, struct tu_wddm_submit_reference, ref) {
      if (ref->bo == bo)
         return (int)i;
      i++;
   }
   return -1;
}

static VkResult
tu_wddm_submit_render(struct tu_queue *queue, struct tu_wddm_submit *submit,
                      uint32_t fence)
{
   struct tu_device *device = queue->device;
   const uint32_t entry_count = util_dynarray_num_elements(
      &submit->entries, struct tu_wddm_submit_entry);
   const uint32_t reference_count = util_dynarray_num_elements(
      &submit->references, struct tu_wddm_submit_reference);
   if (entry_count == 0 || reference_count == 0 ||
       entry_count > TU_WDDM_MAX_SUBMIT_COMMANDS ||
       reference_count > TU_WDDM_MAX_SUBMIT_REFERENCES)
      return VK_ERROR_INITIALIZATION_FAILED;

   const uint64_t packet_size64 = sizeof(tu_wddm_msm_submit_request) +
      (uint64_t)reference_count * sizeof(tu_wddm_msm_submit_bo) +
      (uint64_t)entry_count * sizeof(tu_wddm_msm_submit_command);
   if (packet_size64 > TU_WDDM_MAX_RENDER_COMMAND_SIZE ||
       packet_size64 > UINT32_MAX)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   const uint32_t packet_size = (uint32_t)packet_size64;

   uint8_t *packet = (uint8_t *)vk_zalloc(&device->vk.alloc, packet_size, 8,
                                          VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (packet == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   tu_wddm_msm_submit_request request = {
      .command = TU_WDDM_MSM_CCMD_GEM_SUBMIT,
      .length = packet_size,
      .sequence = fence,
      .response_offset = 0,
      .flags = TU_WDDM_MSM_PIPE_3D0 | TU_WDDM_MSM_SUBMIT_NO_IMPLICIT,
      .queue_id = queue->msm_queue_id,
      .bo_count = reference_count,
      .command_count = entry_count,
      .fence = fence,
   };
   memcpy(packet, &request, sizeof(request));

   struct tu_wddm_render_reference render_refs[TU_WDDM_MAX_SUBMIT_REFERENCES] = {};
   const uint32_t bo_offset = sizeof(request);
   for (uint32_t i = 0; i < reference_count; i++) {
      struct tu_wddm_submit_reference *ref = util_dynarray_element(
         &submit->references, struct tu_wddm_submit_reference, i);
      uint32_t flags = 0;
      if (ref->access & TU_SUBMIT_BO_ACCESS_READ)
         flags |= TU_WDDM_MSM_SUBMIT_BO_READ;
      if (ref->access & TU_SUBMIT_BO_ACCESS_WRITE)
         flags |= TU_WDDM_MSM_SUBMIT_BO_WRITE;
      if (ref->bo->dump)
         flags |= TU_WDDM_MSM_SUBMIT_BO_DUMP;
      flags |= TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;

      tu_wddm_msm_submit_bo bo = { .flags = flags, .handle = 0, .presumed = 0 };
      memcpy(packet + bo_offset + i * sizeof(bo), &bo, sizeof(bo));
      render_refs[i] = {
         .allocation = ref->bo->wddm_allocation,
         .flags = ((ref->access & TU_SUBMIT_BO_ACCESS_READ) ?
                      VIOGPU_WDDM_REFERENCE_READ : 0) |
                  ((ref->access & TU_SUBMIT_BO_ACCESS_WRITE) ?
                      VIOGPU_WDDM_REFERENCE_WRITE : 0),
         .allocation_offset = 0,
         .length = ref->bo->size,
         .patch_offset = bo_offset + i * sizeof(bo) +
                         offsetof(tu_wddm_msm_submit_bo, presumed),
      };
   }

   const uint32_t command_offset = bo_offset +
      reference_count * sizeof(tu_wddm_msm_submit_bo);
   for (uint32_t i = 0; i < entry_count; i++) {
      struct tu_wddm_submit_entry *entry = util_dynarray_element(
         &submit->entries, struct tu_wddm_submit_entry, i);
      int ref_index = tu_wddm_submit_reference_index(submit, entry->bo);
      if (ref_index < 0) {
         vk_free(&device->vk.alloc, packet);
         return VK_ERROR_DEVICE_LOST;
      }
      tu_wddm_msm_submit_command command = {
         .type = TU_WDDM_MSM_SUBMIT_CMD_BUF,
         .submit_index = (uint32_t)ref_index,
         .submit_offset = entry->offset,
         .size = entry->size,
         .padding = 0,
         .relocation_count = 0,
         .iova = 0,
      };
      memcpy(packet + command_offset + i * sizeof(command), &command,
             sizeof(command));
   }

   bool rendered = tu_wddm_context_render(&device->wddm_context, packet,
                                          packet_size, render_refs,
                                          reference_count);
   vk_free(&device->vk.alloc, packet);
   return rendered ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static bool
tu_wddm_sync_set_submit_fence(struct vk_sync *base,
                              struct tu_wddm_context *context,
                              uint32_t fence, bool signal_now)
{
   if (base == NULL || vk_sync_type_is_dummy(base->type) ||
       base->type != &tu_wddm_sync_type)
      return vk_sync_type_is_dummy(base ? base->type : NULL);
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   if (!tu_wddm_sync_is_current(sync) || sync->context != context)
      return false;
   tu_wddm_sync_state_set(sync,
                          signal_now ? TU_WDDM_SYNC_SIGNALED : fence);
   return true;
}

static bool
tu_wddm_pending_fence_count(uint32_t submitted, uint32_t completed,
                            uint32_t *pending)
{
   if (pending == NULL)
      return false;
   *pending = 0;

   if (submitted == 0)
      return completed == 0;
   if (submitted == completed)
      return true;
   if (completed != 0 && !tu_wddm_fence_after(submitted, completed))
      return false;

   const uint64_t distance = tu_wddm_fence_distance(submitted, completed);
   if (distance > UINT32_MAX)
      return false;
   *pending = static_cast<uint32_t>(distance);
   return true;
}

static VkResult
tu_wddm_wait_submission_slot(struct tu_device *device)
{
   while (device->wddm_pending_submission_upper_bound >=
          TU_WDDM_MAX_PENDING_SUBMISSIONS) {
      uint32_t completed = 0;
      if (!tu_wddm_context_get_completed_fence(&device->wddm_context,
                                               &completed))
         return VK_ERROR_DEVICE_LOST;

      uint32_t pending = 0;
      if (!tu_wddm_pending_fence_count(
             device->wddm_context.last_submitted_fence, completed, &pending) ||
          pending > device->wddm_pending_submission_upper_bound)
         return VK_ERROR_DEVICE_LOST;

      device->wddm_pending_submission_upper_bound = pending;
      if (pending >= TU_WDDM_MAX_PENDING_SUBMISSIONS)
         os_time_sleep(1000);
   }

   return VK_SUCCESS;
}

static bool
tu_wddm_sync_signal_valid(struct vk_sync *base,
                          struct tu_wddm_context *context)
{
   if (base == NULL || vk_sync_type_is_dummy(base->type))
      return base != NULL && vk_sync_type_is_dummy(base->type);
   if (base->type != &tu_wddm_sync_type)
      return false;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   return tu_wddm_sync_is_current(sync) && sync->context == context;
}

static VkResult
tu_wddm_queue_submit_locked(struct tu_queue *queue,
                            struct tu_wddm_submit *submit,
                            struct vk_sync_signal *signals,
                            uint32_t signal_count)
{
   struct tu_device *device = queue->device;
   const uint32_t entry_count = util_dynarray_num_elements(
      &submit->entries, struct tu_wddm_submit_entry);
   uint32_t fence = 0;
   if (entry_count != 0) {
      if (!tu_wddm_submit_add_live_bos(device, submit))
         return vk_device_set_lost(
            &device->vk, "WDDM submit exceeds the allocation-list capacity");

      /* tu_queue stores the last fence in a signed field for compatibility
       * with the other Turnip backends, but WDDM treats that field as an
       * opaque 32-bit token.  Keep zero reserved and use serial arithmetic
       * across the complete UINT32 range; the KMD's bounded ring keeps the
       * half-range ordering rule unambiguous. */
      if (device->wddm_next_fence == 0)
         return VK_ERROR_DEVICE_LOST;
      VkResult slot_result = tu_wddm_wait_submission_slot(device);
      if (slot_result != VK_SUCCESS)
         return slot_result;
      fence = device->wddm_next_fence;
      VkResult result = tu_wddm_submit_render(queue, submit, fence);

      /* A successful D3DKMTRender transfers this fence to VidSch even if its
       * replacement metadata is malformed or signal publication races reset.
       * Consume the token immediately so no later submit can reuse it. */
      if (device->wddm_context.last_submitted_fence == fence) {
         queue->fence = (int)fence;
         device->wddm_next_fence = fence + 1;
         if (device->wddm_next_fence == 0)
            device->wddm_next_fence = 1;
         device->wddm_pending_submission_upper_bound++;
      }
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < signal_count; i++) {
      if (!tu_wddm_sync_set_submit_fence(signals[i].sync,
                                         &device->wddm_context, fence,
                                         entry_count == 0))
         return VK_ERROR_DEVICE_LOST;
   }

   return VK_SUCCESS;
}

static VkResult
tu_wddm_queue_submit(struct tu_queue *queue,
                     void *_submit,
                     struct vk_sync_wait *waits,
                     uint32_t wait_count,
                     struct vk_sync_signal *signals,
                     uint32_t signal_count,
                     struct tu_u_trace_submission_data *u_trace_submission_data)
{
   (void) u_trace_submission_data;
   struct tu_device *device = queue->device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *) _submit;
   if (submit == NULL || submit->failed || !device->wddm_initialized)
      return VK_ERROR_DEVICE_LOST;

   for (uint32_t i = 0; i < signal_count; i++) {
      if (!tu_wddm_sync_signal_valid(signals[i].sync, &device->wddm_context))
         return VK_ERROR_DEVICE_LOST;
   }

   /* The pre-v1 private ABI has no scheduler wait list.  Resolve waits through
    * the same context-scoped completion endpoint before issuing Render. */
   for (uint32_t i = 0; i < wait_count; i++) {
      if (waits[i].sync == NULL || vk_sync_type_is_dummy(waits[i].sync->type))
         continue;
      VkResult result =
         vk_sync_wait(&device->vk, waits[i].sync, waits[i].wait_value, VK_SYNC_WAIT_COMPLETE, OS_TIMEOUT_INFINITE);
      if (result != VK_SUCCESS)
         return result == VK_TIMEOUT ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
   }

   /* queue_submit already owns submit_mutex.  The nested WDDM lock protects
    * the all-live-BO snapshot and Render transfer from concurrent BO teardown
    * without making generic BO release recursively acquire submit_mutex. */
   mtx_lock(&device->wddm_mutex);
   VkResult result = tu_wddm_queue_submit_locked(queue, submit, signals, signal_count);
   mtx_unlock(&device->wddm_mutex);
   return result;
}

static VkResult
tu_wddm_queue_wait_fence(struct tu_queue *queue, uint32_t fence,
                         uint64_t timeout_ns)
{
   if (fence == 0)
      return VK_SUCCESS;
   if (!queue->device->wddm_initialized)
      return VK_ERROR_DEVICE_LOST;

   const uint64_t start = (uint64_t)os_time_get_nano();
   for (;;) {
      uint32_t completed = 0;
      if (!tu_wddm_context_get_completed_fence(&queue->device->wddm_context,
                                               &completed))
         return VK_ERROR_DEVICE_LOST;
      if (completed == fence || tu_wddm_fence_after(completed, fence))
         return VK_SUCCESS;
      uint64_t elapsed = (uint64_t)os_time_get_nano() - start;
      if (timeout_ns != UINT64_MAX && elapsed >= timeout_ns)
         return VK_TIMEOUT;
      os_time_sleep(1000);
   }
}

static VkResult
tu_wddm_sparse_vma_init(struct tu_device *dev, struct vk_object_base *base,
                        struct tu_sparse_vma *out_vma, uint64_t *out_iova,
                        enum tu_sparse_vma_flags flags, uint64_t size,
                        uint64_t client_iova)
{
   (void)base;
   (void)out_vma;
   (void)out_iova;
   (void)flags;
   (void)size;
   (void)client_iova;
   return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
tu_wddm_sparse_vma_finish(struct tu_device *dev, struct tu_sparse_vma *vma)
{
   (void)dev;
   (void)vma;
}

static const struct tu_knl wddm_knl_funcs = {
   .name = "wddm",
   .device_init = tu_wddm_device_init,
   .device_finish = tu_wddm_device_finish,
   .device_get_gpu_timestamp = tu_wddm_device_get_gpu_timestamp,
   .device_get_suspend_count = tu_wddm_device_get_suspend_count,
   .device_check_status = tu_wddm_device_check_status,
   .submitqueue_new = tu_wddm_submitqueue_new,
   .submitqueue_close = tu_wddm_submitqueue_close,
   .bo_init = tu_wddm_bo_init,
   .bo_init_dmabuf = tu_wddm_bo_init_dmabuf,
   .bo_export_dmabuf = tu_wddm_bo_export_dmabuf,
   .bo_map = tu_wddm_bo_map,
   .bo_unmap = tu_wddm_bo_unmap,
   .bo_allow_dump = tu_wddm_bo_allow_dump,
   .bo_finish = tu_wddm_bo_finish,
   .bo_set_metadata = tu_wddm_bo_set_metadata,
   .bo_get_metadata = tu_wddm_bo_get_metadata,
   .submit_create = tu_wddm_submit_create,
   .submit_finish = tu_wddm_submit_finish,
   .submit_add_entries = tu_wddm_submit_add_entries,
   .submit_add_bos = tu_wddm_submit_add_bos,
   .submit_add_bind = tu_wddm_submit_add_bind,
   .queue_submit = tu_wddm_queue_submit,
   .queue_wait_fence = tu_wddm_queue_wait_fence,
   .sparse_vma_init = tu_wddm_sparse_vma_init,
   .sparse_vma_finish = tu_wddm_sparse_vma_finish,
};

struct tu_wddm_probe_state {
   struct tu_instance *instance;
   VkResult result;
   bool found;
   uint32_t added_devices;
};

static void
tu_wddm_destroy_added_devices(struct tu_instance *instance, uint32_t count)
{
   if (instance == NULL || instance->vk.physical_devices.destroy == NULL)
      return;

   while (count != 0 && !list_is_empty(&instance->vk.physical_devices.list)) {
      struct vk_physical_device *device = list_last_entry(
         &instance->vk.physical_devices.list, struct vk_physical_device, link);
      list_del(&device->link);
      instance->vk.physical_devices.destroy(device);
      count--;
   }
}

static bool
tu_wddm_probe_adapter(const struct tu_wddm_adapter_info *identity, void *data)
{
   struct tu_wddm_probe_state *state = (struct tu_wddm_probe_state *)data;
   struct tu_wddm_device *probe_device = &state->instance->wddm_probe_device;
   struct tu_wddm_context *probe_context = &state->instance->wddm_probe_context;

   if (state->instance->wddm_probe_pending &&
       !tu_wddm_probe_cleanup(state->instance)) {
      state->result = VK_ERROR_DEVICE_LOST;
      return false;
   }

   memset(probe_device, 0, sizeof(*probe_device));
   memset(probe_context, 0, sizeof(*probe_context));
   /* DXGI must describe the fixed gpu_guest VidMm segment.  A zero or
    * sub-page descriptor cannot identify a usable WDDM heap. */
   if (!tu_wddm_page_aligned_nonzero(identity->dedicated_video_memory))
      return true;

   if (!tu_wddm_device_open(&state->instance->wddm_runtime, identity,
                            probe_device)) {
      if (!tu_wddm_probe_cleanup(state->instance)) {
         state->result = VK_ERROR_DEVICE_LOST;
         return false;
      }
      return true;
   }
   bool opened_context = tu_wddm_context_open(probe_device, probe_context);
   uint64_t va_start = 0;
   uint64_t va_size = 0;
   if (opened_context) {
      va_start = probe_context->info.VaStart;
      va_size = probe_context->info.VaSize;
   }
   uint64_t heap_size = 0;
   const bool valid_heap = tu_wddm_select_heap_size(
      identity->dedicated_video_memory, va_size, &heap_size);
   if (!tu_wddm_probe_cleanup(state->instance)) {
      state->result = VK_ERROR_DEVICE_LOST;
      return false;
   }
   if (!opened_context) {
      state->result = VK_ERROR_DEVICE_LOST;
      return true;
   }
   if (!valid_heap)
      return true;

   struct tu_physical_device *device = (struct tu_physical_device *)vk_zalloc(
      &state->instance->vk.alloc, sizeof(*device), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      state->result = VK_ERROR_OUT_OF_HOST_MEMORY;
      return false;
   }

   device->instance = state->instance;
   device->local_fd = -1;
   device->master_fd = -1;
   device->kgsl_dma_fd = -1;
   device->wddm_adapter = *identity;
   device->msm_major_version = (int)identity->private_info.MsmMajorVersion;
   device->msm_minor_version = (int)identity->private_info.MsmMinorVersion;
   device->dev_id.gpu_id = identity->private_info.GpuId;
   device->dev_id.chip_id = identity->private_info.ChipId;
   device->gmem_size = debug_get_num_option("TU_GMEM",
                                             identity->private_info.GmemSize);
   device->gmem_base = identity->private_info.GmemBase;
   device->va_start = va_start;
   device->va_size = va_size;
   device->has_set_iova = true;
   device->has_cached_coherent_memory =
      identity->private_info.HasCachedCoherentMemory != 0;
   device->has_cached_non_coherent_memory = false;
   device->has_raytracing = identity->private_info.HasRayTracing != 0;
   device->has_preemption = false;
   device->has_vm_bind = false;
   device->has_sparse = false;
   device->has_sparse_prr = false;
   device->has_lazy_bos = false;
   device->is_perf_cntr_selectable = false;
   /* The WDDM context exposes one host submitqueue, fixed at priority zero. */
   device->submitqueue_priority_count = 1;
   device->uche_trap_base = identity->private_info.UcheTrapBase;
   device->ubwc_config.highest_bank_bit = identity->private_info.HighestBankBit;
   device->ubwc_config.bank_swizzle_levels =
      identity->private_info.UbwcSwizzle ?
         (uint32_t)identity->private_info.UbwcSwizzle : ~0u;
   device->ubwc_config.macrotile_mode = identity->private_info.MacrotileMode ?
      (enum fdl_macrotile_mode)identity->private_info.MacrotileMode :
      FDL_MACROTILE_INVALID;
   device->timeline_type = vk_sync_timeline_get_type(&tu_wddm_sync_type);
   device->sync_types[0] = &tu_wddm_sync_type;
   device->sync_types[1] = &device->timeline_type.sync;
   device->sync_types[2] = NULL;
   /* DXGI DedicatedVideoMemory is the sole non-system VidMm segment published
    * by the KMD (the gpu_guest pool).  Keep it bounded by the context VA
    * window; do not replace this fixed pool size with a guest-RAM estimate. */
   device->heap.size = heap_size;
   device->heap.used = 0;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   state->instance->knl = &wddm_knl_funcs;
   state->result = tu_physical_device_init(device, state->instance);
   if (state->result != VK_SUCCESS) {
      vk_free(&state->instance->vk.alloc, device);
      return true;
   }

   list_addtail(&device->vk.link, &state->instance->vk.physical_devices.list);
   state->found = true;
   state->added_devices++;
   return true;
}

VkResult
tu_knl_wddm_load(struct tu_instance *instance)
{
   if (instance == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!instance->wddm_runtime_initialized) {
      if (!tu_wddm_runtime_init(&instance->wddm_runtime))
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      instance->wddm_runtime_initialized = true;
   }

   struct tu_wddm_probe_state state = {
      .instance = instance,
      .result = VK_ERROR_INCOMPATIBLE_DRIVER,
      .found = false,
   };
   const bool enumeration_ok = tu_wddm_runtime_foreach_adapter(
      &instance->wddm_runtime, tu_wddm_probe_adapter, &state);
   if (!tu_wddm_probe_cleanup(instance)) {
      /* Keep the runtime initialized so tu_DestroyInstance() can retry the
       * close while the dispatch table is still valid. */
      return VK_ERROR_DEVICE_LOST;
   }
   if (!enumeration_ok) {
      /* The callback may have published earlier adapters before a later
       * adapter, DXGI call, or KMT close failed.  Remove exactly those
       * devices before returning an error so a subsequent enumeration cannot
       * duplicate them. */
      tu_wddm_destroy_added_devices(instance, state.added_devices);
      if (state.result == VK_SUCCESS || state.result == VK_ERROR_INCOMPATIBLE_DRIVER)
         state.result = VK_ERROR_INITIALIZATION_FAILED;
   }
   if (enumeration_ok && state.found)
      return VK_SUCCESS;
   if (state.result == VK_ERROR_OUT_OF_HOST_MEMORY)
      return state.result;

   tu_wddm_runtime_finish(&instance->wddm_runtime);
   instance->wddm_runtime_initialized = false;
   return state.result == VK_SUCCESS ? VK_ERROR_INCOMPATIBLE_DRIVER : state.result;
}

#endif /* TU_HAS_WDDM */
