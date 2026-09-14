/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Shared Freedreno Windows kernel-interface transport.  This file owns only the
 * WDDM/D3DKMT object and private-ABI plumbing.  Native submits stay behind the
 * KMD guest-backed allocation and VidSch retirement gates; this code never
 * bypasses those gates or manufactures a fence completion.
 */

#include "freedreno_wddm_private.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void
tu_wddm_diag(const char *format, ...)
{
   /* GetEnvironmentVariableA, not getenv(): this transport is compiled with
    * /W4 /WX and MSVC deprecates getenv as C4996, which fails the build. */
   char enabled[4] = {};
   const DWORD enabled_len =
      GetEnvironmentVariableA("TU_WDDM_DIAGNOSTICS", enabled, sizeof(enabled));
   if (format == NULL || enabled_len != 1 || enabled[0] != '1')
      return;

   va_list args;
   va_start(args, format);
   fputs("TU_WDDM_DIAG: ", stderr);
   vfprintf(stderr, format, args);
   va_end(args);
   fputc('\n', stderr);
   fflush(stderr);
}

/* A wait target must belong to this context's submitted serial stream.  The
 * private completion endpoint only reports the contiguous retired prefix; it
 * cannot distinguish an arbitrary future fence from work that is merely
 * pending.  Rejecting unowned targets before polling prevents an accidental
 * infinite wait and keeps the KMT query scoped to this context's ownership. */
static inline bool
tu_wddm_fence_was_submitted(const struct tu_wddm_context *context,
                            uint32_t fence)
{
   if (context == NULL || fence == 0 || context->last_submitted_fence == 0)
      return false;

   return fence == context->last_submitted_fence ||
          tu_wddm_fence_after(context->last_submitted_fence, fence);
}

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

   memset(runtime, 0, sizeof(*runtime));
   return tu_wddm_dispatch_init(&runtime->dispatch);
}

void
tu_wddm_runtime_finish(struct tu_wddm_runtime *runtime)
{
   if (runtime == NULL)
      return;

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
   if (runtime == NULL || runtime->dispatch.EnumAdapters2 == NULL ||
       runtime->dispatch.QueryAdapterInfo == NULL ||
       runtime->dispatch.CloseAdapter == NULL || callback == NULL)
      return false;

   D3DKMT_ENUMADAPTERS2 enumeration = {};
   NTSTATUS status = runtime->dispatch.EnumAdapters2(&enumeration);
   if (!NT_SUCCESS(status))
      return false;
   if (enumeration.NumAdapters == 0)
      return true;
   if (enumeration.NumAdapters > SIZE_MAX / sizeof(D3DKMT_ADAPTERINFO))
      return false;

   const ULONG capacity = enumeration.NumAdapters;
   D3DKMT_ADAPTERINFO *adapters = static_cast<D3DKMT_ADAPTERINFO *>(
      calloc(capacity, sizeof(*adapters)));
   if (adapters == NULL)
      return false;

   enumeration.NumAdapters = capacity;
   enumeration.pAdapters = adapters;
   status = runtime->dispatch.EnumAdapters2(&enumeration);
   if (!NT_SUCCESS(status) || enumeration.NumAdapters > capacity) {
      free(adapters);
      return false;
   }

   bool enumeration_ok = true;
   for (ULONG index = 0; index < enumeration.NumAdapters; index++) {
      D3DKMT_ADAPTERINFO *entry = &adapters[index];
      if (entry->hAdapter == 0) {
         enumeration_ok = false;
         break;
      }

      struct tu_wddm_adapter_info identity = {};
      identity.luid = entry->AdapterLuid;
      if (tu_wddm_query_private_info(runtime, entry->hAdapter,
                                     &identity.private_info) &&
          !callback(&identity, data))
         enumeration_ok = false;

      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = entry->hAdapter;
      if (NT_SUCCESS(runtime->dispatch.CloseAdapter(&close)))
         entry->hAdapter = 0;
      else
         enumeration_ok = false;

      if (!enumeration_ok)
         break;
   }

   /* EnumAdapters2 returns owned handles for the whole array.  Close entries
    * not visited after a callback abort, as well as one failed close retry,
    * before releasing the array. */
   for (ULONG index = 0; index < enumeration.NumAdapters; index++) {
      if (adapters[index].hAdapter == 0)
         continue;

      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = adapters[index].hAdapter;
      if (NT_SUCCESS(runtime->dispatch.CloseAdapter(&close)))
         adapters[index].hAdapter = 0;
      else
         enumeration_ok = false;
   }

   free(adapters);
   return enumeration_ok;
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

   tu_wddm_diag("device_close begin device=%u adapter=%u",
                static_cast<unsigned>(device->handle),
                static_cast<unsigned>(device->adapter.handle));

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
      if (!NT_SUCCESS(status)) {
         tu_wddm_diag("device_close DestroyDevice failed status=0x%08x handle=%u",
                      static_cast<unsigned>(status),
                      static_cast<unsigned>(device->handle));
         return false;
      }

      device->handle = 0;
      device->command_buffer = NULL;
      device->allocation_list = NULL;
      device->patch_location_list = NULL;
      device->command_buffer_size = 0;
      device->allocation_list_size = 0;
      device->patch_location_list_size = 0;
   }

   if (device->adapter.handle != 0) {
      const bool closed = tu_wddm_adapter_close(&device->adapter);
      tu_wddm_diag("device_close adapter_close success=%u",
                   static_cast<unsigned>(closed));
      return closed;
   }

   tu_wddm_diag("device_close complete");
   return device->adapter.runtime != NULL;
}

bool
tu_wddm_device_execution_active(struct tu_wddm_device *device)
{
   if (device == NULL || device->adapter.runtime == NULL ||
       device->adapter.runtime->dispatch.GetDeviceState == NULL ||
       device->handle == 0)
      return false;

   D3DKMT_GETDEVICESTATE state = {};
   state.hDevice = device->handle;
   state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
   NTSTATUS status = device->adapter.runtime->dispatch.GetDeviceState(&state);
   return NT_SUCCESS(status) &&
          state.ExecutionState == D3DKMT_DEVICEEXECUTION_ACTIVE;
}

/* Preserve Turnip's client hint while allowing native OpenGL contexts. */
bool
tu_wddm_context_open(struct tu_wddm_device *device,
                     struct tu_wddm_context *context)
{
   return tu_wddm_context_open_with_hint(device, context, D3DKMT_CLIENTHINT_VULKAN);
}

/* The hint is OS metadata; it does not introduce a graphics API dependency. */
bool
tu_wddm_context_open_with_hint(
   struct tu_wddm_device *device,
   struct tu_wddm_context *context,
   D3DKMT_CLIENTHINT client_hint)
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
   create.ClientHint = client_hint;

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
   if (!tu_wddm_fence_was_submitted(context, fence))
      return false;

   const ULONGLONG start_ms = GetTickCount64();
   const uint64_t timeout_ms = timeout_ns == UINT64_MAX
                                  ? UINT64_MAX
                                  : timeout_ns / UINT64_C(1000000) +
                                       (timeout_ns % UINT64_C(1000000) != 0);
   tu_wddm_fence_poll_wait poll_wait;

   for (;;) {
      uint32_t completed = 0;
      if (!tu_wddm_context_get_completed_fence(context, &completed))
         return false;
      if (!tu_wddm_device_execution_active(context->device))
         return false;
      if (completed == fence || tu_wddm_fence_after(completed, fence))
         return true;

      const ULONGLONG elapsed_ms = GetTickCount64() - start_ms;
      if (timeout_ms != UINT64_MAX && elapsed_ms >= timeout_ms)
         return false;

      poll_wait.wait();
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

   tu_wddm_diag("context_close begin context=%u last_fence=%u",
                static_cast<unsigned>(context->handle),
                context->last_submitted_fence);

   D3DKMT_DESTROYCONTEXT destroy = {};
   destroy.hContext = context->handle;
   NTSTATUS status = TU_WDDM_STATUS_SUCCESS;
   for (uint32_t attempt = 0; attempt <= TU_WDDM_DESTROY_BUSY_RETRIES; attempt++) {
      status = context->device->adapter.runtime->dispatch.DestroyContext(&destroy);
      context->last_destroy_status = static_cast<uint32_t>(status);
      context->destroy_attempt_count = attempt + 1;
      if (!NT_SUCCESS(status) || attempt != 0)
         tu_wddm_diag("context_close DestroyContext attempt=%u status=0x%08x handle=%u",
                      attempt + 1, static_cast<unsigned>(status),
                      static_cast<unsigned>(context->handle));
      if (NT_SUCCESS(status) ||
          (status != TU_WDDM_STATUS_DEVICE_BUSY &&
           status != TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY) ||
          attempt == TU_WDDM_DESTROY_BUSY_RETRIES)
         break;

      /* D3DKMTDestroyAllocation can complete before VidMm invokes the KMD
       * callback that drops the Native Context allocation reference. */
      Sleep(1);
   }
   if (!NT_SUCCESS(status)) {
      tu_wddm_diag("context_close failed status=0x%08x attempts=%u handle=%u",
                   static_cast<unsigned>(status), context->destroy_attempt_count,
                   static_cast<unsigned>(context->handle));
      return false;
   }

   tu_wddm_diag("context_close complete attempts=%u", context->destroy_attempt_count);
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

/* AssumeNotInUse is never asserted here, because this driver cannot honestly
 * prove it.
 *
 * The flag tells VidMm to skip its own in-use check.  When the promise is false
 * VidMm has no recourse and bugchecks 0x0000010E
 * VIDEO_MEMORY_MANAGEMENT_INTERNAL with STATUS_GRAPHICS_ALLOCATION_BUSY
 * (0xC01E0102) rather than returning an error.
 *
 * The only completion evidence available to the user-mode driver is the
 * driver-private fence read through VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE, and
 * the miniport publishes that fence *before* it reports the completion to
 * dxgkrnl: in viogpuwddm/wddmddi.cpp the submission-complete path calls
 * RetireContextUmdFence() - the sole writer of context->CompletedUmdFence, which
 * is what the escape returns - and only afterwards calls
 * NotifyNativeSubmissionCompletion(), which is what raises
 * DXGK_INTERRUPT_DMA_COMPLETED and lets VidMm retire the allocation's reference.
 * So a wait satisfied through the escape can, and does, precede VidMm dropping
 * that reference; asserting AssumeNotInUse on the strength of it is a false
 * promise, and the six 0x10E dumps on this guest are that promise being broken.
 *
 * Waiting longer in user mode cannot close the window, because nothing visible
 * to user mode reports VidMm's state.  Without the flag VidMm performs its own
 * check and *returns* STATUS_GRAPHICS_ALLOCATION_BUSY, which the caller can
 * retry - an error instead of a bugchecked machine. */
static NTSTATUS
tu_wddm_destroy_allocation_handle(struct tu_wddm_context *context,
                                  D3DKMT_HANDLE handle)
{
   D3DKMT_DESTROYALLOCATION2 destroy = {};
   destroy.hDevice = context->device->handle;
   destroy.phAllocationList = &handle;
   destroy.AllocationCount = 1;
   destroy.Flags.AssumeNotInUse = 0;
   return context->device->adapter.runtime->dispatch.DestroyAllocation2(&destroy);
}

bool
tu_wddm_allocation_create(struct tu_wddm_context *context,
                          const struct tu_wddm_allocation_desc *desc,
                          struct tu_wddm_allocation *allocation)
{
   if (allocation == NULL)
      return false;

   memset(allocation, 0, sizeof(*allocation));
   if (!tu_wddm_allocation_desc_valid(context, desc))
      return false;

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

   NTSTATUS status = TU_WDDM_STATUS_DEVICE_BUSY;
   for (uint32_t attempt = 0; attempt <= TU_WDDM_CREATE_BUSY_RETRIES; ++attempt) {
      allocation_info.hAllocation = 0;
      status = context->device->adapter.runtime->dispatch.CreateAllocation(&create);
      if (status != TU_WDDM_STATUS_DEVICE_BUSY || allocation_info.hAllocation != 0 ||
          attempt == TU_WDDM_CREATE_BUSY_RETRIES)
         break;
      Sleep(1);
   }
   allocation->last_create_status = static_cast<uint32_t>(status);
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
         /* This allocation failed to be created, so it was never submitted and
          * an AssumeNotInUse claim would in fact hold here.  It is still not
          * made: with the flag clear VidMm runs its own check, finds the
          * allocation idle and succeeds anyway, so the claim buys nothing and
          * leaving it out keeps a single, uniformly safe teardown path. */
         const NTSTATUS destroy_status =
            tu_wddm_destroy_allocation_handle(context, allocation_info.hAllocation);
         allocation->last_destroy_status = static_cast<uint32_t>(destroy_status);
         if (destroy_status == TU_WDDM_STATUS_SUCCESS)
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

/* Attempt destruction once; retain every field on any non-success status.
 * The caller supplies retirement evidence and owns any retry scheduling. */
NTSTATUS
tu_wddm_allocation_try_destroy(struct tu_wddm_allocation *allocation)
{
   if (allocation == NULL || allocation->context == NULL || allocation->handle == 0 ||
       allocation->locked || allocation->context->device == NULL ||
       allocation->context->device->adapter.runtime == NULL ||
       allocation->context->device->handle == 0)
      return static_cast<NTSTATUS>(0xc000000dL); /* STATUS_INVALID_PARAMETER */

   const NTSTATUS status =
      tu_wddm_destroy_allocation_handle(allocation->context, allocation->handle);
   allocation->last_destroy_status = static_cast<uint32_t>(status);
   if (allocation->destroy_attempt_count != UINT32_MAX)
      allocation->destroy_attempt_count++;
   if (status == TU_WDDM_STATUS_SUCCESS) {
      free(allocation->metadata);
      memset(allocation, 0, sizeof(*allocation));
   }
   return status;
}

bool
tu_wddm_allocation_destroy(struct tu_wddm_allocation *allocation)
{
   if (allocation == NULL || allocation->context == NULL || allocation->handle == 0 ||
       allocation->locked || allocation->context->device == NULL ||
       allocation->context->device->adapter.runtime == NULL ||
       allocation->context->device->handle == 0)
      return false;

   tu_wddm_diag("allocation_destroy begin allocation=%u locked=%u",
                static_cast<unsigned>(allocation->handle),
                static_cast<unsigned>(allocation->locked));

   D3DKMT_HANDLE handle = allocation->handle;
   /* Retiring this context's submissions first keeps the common case off the
    * retry path, but it is neither a proof of idleness nor a precondition: no
    * AssumeNotInUse claim is made from it, so VidMm decides.  The wait is
    * therefore bounded and its result is advisory - a fence that never retires
    * must not hang the caller, and the destroy is attempted either way.  An
    * unbounded wait here is what hung the fake-dispatch fixture. */
   if (!tu_wddm_context_wait_submissions(allocation->context,
                                         TU_WDDM_DESTROY_WAIT_TIMEOUT_NS)) {
      tu_wddm_diag("allocation_destroy submissions did not retire in time allocation=%u fence=%u"
                   " - destroying anyway, VidMm arbitrates",
                   static_cast<unsigned>(allocation->handle),
                   allocation->context->last_submitted_fence);
   }

   /* VidMm may still hold a reference from a completion it has not processed
    * yet, and now says so instead of bugchecking.  Retry on the busy statuses
    * the same way context teardown does. */
   NTSTATUS status = TU_WDDM_STATUS_SUCCESS;
   for (uint32_t attempt = 0; attempt <= TU_WDDM_DESTROY_BUSY_RETRIES; attempt++) {
      status = tu_wddm_destroy_allocation_handle(allocation->context, handle);
      allocation->destroy_attempt_count = attempt + 1;
      if (status == TU_WDDM_STATUS_SUCCESS ||
          (status != TU_WDDM_STATUS_DEVICE_BUSY &&
           status != TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY) ||
          attempt == TU_WDDM_DESTROY_BUSY_RETRIES)
         break;
      Sleep(1);
   }
   allocation->last_destroy_status = static_cast<uint32_t>(status);
   if (status != TU_WDDM_STATUS_SUCCESS) {
      tu_wddm_diag("allocation_destroy failed allocation=%u status=0x%08x attempts=%u",
                   static_cast<unsigned>(allocation->handle),
                   static_cast<unsigned>(status),
                   allocation->destroy_attempt_count);
      return false;
   }

   free(allocation->metadata);
   tu_wddm_diag("allocation_destroy complete allocation=%u",
                static_cast<unsigned>(allocation->handle));
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
   if (!NT_SUCCESS(status)) {
      tu_wddm_diag("allocation_unlock failed allocation=%u status=0x%08x",
                   static_cast<unsigned>(allocation->handle),
                   static_cast<unsigned>(status));
      return false;
   }

   allocation->map = NULL;
   allocation->locked = false;
   tu_wddm_diag("allocation_unlock complete allocation=%u",
                static_cast<unsigned>(allocation->handle));
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
       (reference->patch_offset & (sizeof(uint32_t) - 1)) != 0 ||
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
       request.bo_count == 0 || request.command_count == 0 ||
       request.command_count > TU_WDDM_MAX_SUBMIT_COMMANDS)
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

   /* Render is the only path that can publish a new Host submission.  Keep
    * the execution-state check adjacent to the context/epoch validation so a
    * reset or stopped device cannot consume a DMA buffer or mutate the KMT
    * replacement lists before the KMD reset gate rejects it. */
   if (!tu_wddm_device_execution_active(context->device))
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
   /* D3DKMT_RENDER exposes the command, allocation, and patch lists through
    * pNew* in/out fields: callers provide the current storage and KMT may
    * return replacement storage for the next submission. */
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
    * which an API client may destroy first. */
   if (NT_SUCCESS(status) &&
       (context->last_submitted_fence == 0 ||
        tu_wddm_fence_after(submitted_fence, context->last_submitted_fence)))
      context->last_submitted_fence = submitted_fence;

   return NT_SUCCESS(status) && replacements_valid;
}
