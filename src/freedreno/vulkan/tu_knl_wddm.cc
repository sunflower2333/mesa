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
tu_wddm_validate_adapter_info(const VIOGPU_WDDM_ADAPTER_INFO *info)
{
   if (info == NULL ||
       !tu_wddm_header_is_current(&info->Header,
                                  tu_wddm_sizeof<VIOGPU_WDDM_ADAPTER_INFO>()) ||
       info->Capabilities != VIOGPU_WDDM_CAPABILITIES_NONE || info->ResetGeneration == 0 ||
       info->MsmMajorVersion != 1 || info->MsmMinorVersion < 9 || info->GpuId == 0 ||
       info->ChipId == 0 || info->GmemSize == 0 || info->PriorityCount == 0 ||
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

   VIOGPU_WDDM_ADAPTER_INFO current = {};
   if (!tu_wddm_query_private_info(runtime, open.hAdapter, &current) ||
       (identity->private_info.ResetGeneration != 0 &&
        !tu_wddm_private_info_equal(&current, &identity->private_info))) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = open.hAdapter;
      runtime->dispatch.CloseAdapter(&close);
      return false;
   }

   adapter->runtime = runtime;
   adapter->luid = identity->luid;
   adapter->handle = open.hAdapter;
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
         WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, identity.description,
                             static_cast<int>(sizeof(identity.description)), NULL, NULL);

         struct tu_wddm_adapter adapter = {};
         if (tu_wddm_adapter_open(runtime, &identity, &adapter)) {
            identity.private_info = adapter.private_info;
            bool keep_going = callback(&identity, data);
            bool closed = tu_wddm_adapter_close(&adapter);
            dxgi_adapter->Release();
            if (!closed || !keep_going)
               return closed;
            continue;
         }
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
   if (!tu_wddm_adapter_open(runtime, identity, &device->adapter))
      return false;

   D3DKMT_CREATEDEVICE create = {};
   create.hAdapter = device->adapter.handle;
   NTSTATUS status = runtime->dispatch.CreateDevice(&create);
   if (!NT_SUCCESS(status) || create.hDevice == 0) {
      tu_wddm_adapter_close(&device->adapter);
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
   if (device == NULL || device->adapter.runtime == NULL || device->handle == 0)
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
   return tu_wddm_adapter_close(&device->adapter);
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
   if (!NT_SUCCESS(status) || create.hContext == 0)
      return false;

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
   if (!NT_SUCCESS(status) || allocation_info.hAllocation == 0)
      return false;

   allocation->context = context;
   allocation->handle = allocation_info.hAllocation;
   allocation->private_info = private_data;
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
tu_wddm_allocation_lock(struct tu_wddm_allocation *allocation,
                        bool read_only,
                        void **map)
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
   lock.Flags.ReadOnly = read_only ? 1 : 0;
   lock.Flags.LockEntire = 1;

   NTSTATUS status = allocation->context->device->adapter.runtime->dispatch.Lock(&lock);
   if (!NT_SUCCESS(status) || lock.pData == NULL)
      return false;

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
       !tu_wddm_header_is_current(&reference->allocation->private_info.Header,
                                  tu_wddm_sizeof<VIOGPU_WDDM_ALLOCATION_INFO>()) ||
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
                            uint32_t submit_queue_id)
{
   if (command_stream == NULL || references == NULL || command_stream_size < sizeof(tu_wddm_msm_submit_request) ||
       (command_stream_size & (sizeof(uint32_t) - 1)) != 0)
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

   if (!tu_wddm_native_submit_valid(command_stream, command_stream_size, references, reference_count,
                                    context->info.SubmitQueueId))
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

   NTSTATUS status = context->device->adapter.runtime->dispatch.Render(&render);
   context->command_buffer = render.pNewCommandBuffer;
   context->allocation_list = render.pNewAllocationList;
   context->patch_location_list = render.pNewPatchLocationList;
   if (NT_SUCCESS(status)) {
      context->command_buffer_size = render.NewCommandBufferSize;
      context->allocation_list_size = render.NewAllocationListSize;
      context->patch_location_list_size = render.NewPatchLocationListSize;
   }

   return NT_SUCCESS(status) && context->command_buffer != NULL && context->allocation_list != NULL &&
          context->patch_location_list != NULL && context->command_buffer_size != 0 &&
          context->allocation_list_size != 0 && context->patch_location_list_size != 0;
}
