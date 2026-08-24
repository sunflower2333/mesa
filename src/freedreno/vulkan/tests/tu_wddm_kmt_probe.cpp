/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Direct ARM64 D3DKMT bring-up probe for the DroidVM Turnip private endpoint.
 * It does not load Vulkan or a D3D UMD.  It exercises the bounded KMT
 * allocation lock/unlock lifecycle after Context/VA bring-up. Every acquired
 * KMT handle is closed before exit, including adapters rejected by the private
 * ABI query.
 */

#include "../tu_knl_wddm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void
print_status(const char *operation, NTSTATUS status)
{
   printf("%s: status=0x%08lx\n", operation,
          static_cast<unsigned long>(status));
}

void
print_adapter_info(const VIOGPU_WDDM_ADAPTER_INFO *info)
{
   printf("    header: magic=0x%08x version=%u size=%u reserved=0x%08x\n",
          info->Header.Magic, info->Header.Version, info->Header.Size,
          info->Header.Reserved);
   printf("    capabilities=0x%016llx reset=%llu\n",
          static_cast<unsigned long long>(info->Capabilities),
          static_cast<unsigned long long>(info->ResetGeneration));
   printf("    msm=%u.%u.%u gpu=%u chip=0x%016llx\n",
          info->MsmMajorVersion, info->MsmMinorVersion,
          info->MsmPatchVersion, info->GpuId,
          static_cast<unsigned long long>(info->ChipId));
   printf("    gmem: size=%u base=0x%016llx highest-bank-bit=%u "
          "priorities=%u\n",
          info->GmemSize, static_cast<unsigned long long>(info->GmemBase),
          info->HighestBankBit, info->PriorityCount);
   printf("    coherent=%u ubwc=0x%016llx macrotile=0x%016llx\n",
          info->HasCachedCoherentMemory,
          static_cast<unsigned long long>(info->UbwcSwizzle),
          static_cast<unsigned long long>(info->MacrotileMode));
   printf("    uche-trap=0x%016llx ray-tracing=%u max-frequency=%u "
          "reserved=[0x%016llx,0x%016llx]\n",
          static_cast<unsigned long long>(info->UcheTrapBase),
          info->HasRayTracing, info->MaxFrequency,
          static_cast<unsigned long long>(info->Reserved[0]),
          static_cast<unsigned long long>(info->Reserved[1]));
}

NTSTATUS
query_private_info(tu_wddm_dispatch *dispatch,
                   D3DKMT_HANDLE adapter,
                   VIOGPU_WDDM_ADAPTER_INFO *info)
{
   memset(info, 0, sizeof(*info));
   D3DKMT_QUERYADAPTERINFO query = {};
   query.hAdapter = adapter;
   query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
   query.pPrivateDriverData = info;
   query.PrivateDriverDataSize = static_cast<UINT>(sizeof(*info));
   return dispatch->QueryAdapterInfo(&query);
}

bool
close_adapter(tu_wddm_dispatch *dispatch, D3DKMT_HANDLE *handle)
{
   if (*handle == 0)
      return true;

   D3DKMT_CLOSEADAPTER close = {};
   close.hAdapter = *handle;
   NTSTATUS status = dispatch->CloseAdapter(&close);
   print_status("CloseAdapter", status);
   if (NT_SUCCESS(status))
      *handle = 0;
   return NT_SUCCESS(status);
}

bool
probe_adapter(tu_wddm_dispatch *dispatch,
              const D3DKMT_ADAPTERINFO *enumerated)
{
   VIOGPU_WDDM_ADAPTER_INFO enumerated_info = {};
   printf("  QueryAdapterInfo(enum): begin\n");
   NTSTATUS status = query_private_info(dispatch, enumerated->hAdapter,
                                        &enumerated_info);
   printf("  QueryAdapterInfo(enum): status=0x%08lx valid=%u\n",
          static_cast<unsigned long>(status),
          static_cast<unsigned>(
             NT_SUCCESS(status) &&
             tu_wddm_validate_adapter_info(&enumerated_info)));
   print_adapter_info(&enumerated_info);
   if (!NT_SUCCESS(status) ||
       !tu_wddm_validate_adapter_info(&enumerated_info))
      return false;

   D3DKMT_OPENADAPTERFROMLUID open = {};
   open.AdapterLuid = enumerated->AdapterLuid;
   printf("  OpenAdapterFromLuid: begin\n");
   status = dispatch->OpenAdapterFromLuid(&open);
   printf("  OpenAdapterFromLuid: status=0x%08lx handle=0x%08x\n",
          static_cast<unsigned long>(status), open.hAdapter);
   if (!NT_SUCCESS(status) || open.hAdapter == 0)
      return false;

   bool ready = false;
   D3DKMT_HANDLE opened_adapter = open.hAdapter;
   D3DKMT_HANDLE device_handle = 0;
   D3DKMT_HANDLE context_handle = 0;
   VIOGPU_WDDM_CONTEXT_INFO context_info = {};

   VIOGPU_WDDM_ADAPTER_INFO opened_info = {};
   printf("  QueryAdapterInfo(open): begin\n");
   status = query_private_info(dispatch, opened_adapter, &opened_info);
   printf("  QueryAdapterInfo(open): status=0x%08lx valid=%u exact=%u\n",
          static_cast<unsigned long>(status),
          static_cast<unsigned>(
             NT_SUCCESS(status) &&
             tu_wddm_validate_adapter_info(&opened_info)),
          static_cast<unsigned>(
             NT_SUCCESS(status) &&
             memcmp(&opened_info, &enumerated_info,
                    sizeof(opened_info)) == 0));
   print_adapter_info(&opened_info);
   if (!NT_SUCCESS(status) ||
       !tu_wddm_validate_adapter_info(&opened_info) ||
       memcmp(&opened_info, &enumerated_info, sizeof(opened_info)) != 0)
      goto cleanup;

   {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = opened_adapter;
      printf("  CreateDevice: begin\n");
      status = dispatch->CreateDevice(&create);
      printf("  CreateDevice: status=0x%08lx handle=0x%08x command=%u "
             "allocations=%u patches=%u\n",
             static_cast<unsigned long>(status), create.hDevice,
             create.CommandBufferSize, create.AllocationListSize,
             create.PatchLocationListSize);
      if (!NT_SUCCESS(status) || create.hDevice == 0)
         goto cleanup;
      device_handle = create.hDevice;
   }

   {
      D3DKMT_GETDEVICESTATE state = {};
      state.hDevice = device_handle;
      state.StateType = D3DKMT_DEVICESTATE_EXECUTION;
      printf("  GetDeviceState: begin\n");
      status = dispatch->GetDeviceState(&state);
      printf("  GetDeviceState: status=0x%08lx execution=%u\n",
             static_cast<unsigned long>(status),
             static_cast<unsigned>(state.ExecutionState));
      if (!NT_SUCCESS(status) ||
          state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE)
         goto cleanup;
   }

   {
      VIOGPU_WDDM_CONTEXT_CREATE private_data = {};
      private_data.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      private_data.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      private_data.Header.Size = static_cast<uint32_t>(sizeof(private_data));
      private_data.ExpectedResetGeneration = opened_info.ResetGeneration;
      private_data.Flags = VIOGPU_WDDM_CONTEXT_FLAGS_NONE;

      D3DKMT_CREATECONTEXT create = {};
      create.hDevice = device_handle;
      create.NodeOrdinal = 0;
      create.EngineAffinity = 1;
      create.pPrivateDriverData = &private_data;
      create.PrivateDriverDataSize = static_cast<UINT>(sizeof(private_data));
      create.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
      printf("  CreateContext: begin\n");
      status = dispatch->CreateContext(&create);
      printf("  CreateContext: status=0x%08lx handle=0x%08x command=%u "
             "allocations=%u patches=%u\n",
             static_cast<unsigned long>(status), create.hContext,
             create.CommandBufferSize, create.AllocationListSize,
             create.PatchLocationListSize);
      if (!NT_SUCCESS(status) || create.hContext == 0)
         goto cleanup;
      context_handle = create.hContext;
   }

   {
      context_info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      context_info.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      context_info.Header.Size = static_cast<uint32_t>(sizeof(context_info));
      context_info.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
      context_info.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
      context_info.ExpectedResetGeneration = opened_info.ResetGeneration;

      D3DKMT_ESCAPE escape = {};
      escape.hAdapter = opened_adapter;
      escape.hDevice = device_handle;
      escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
      escape.pPrivateDriverData = &context_info;
      escape.PrivateDriverDataSize = static_cast<UINT>(sizeof(context_info));
      escape.hContext = context_handle;
      printf("  Escape(GET_CONTEXT_INFO): begin\n");
      status = dispatch->Escape(&escape);
      printf("  Escape(GET_CONTEXT_INFO): status=0x%08lx valid=%u "
             "va=0x%llx+0x%llx reset=%llu context=%u queue=%u\n",
             static_cast<unsigned long>(status),
             static_cast<unsigned>(
                NT_SUCCESS(status) &&
                tu_wddm_validate_context_info(
                   &context_info, opened_info.ResetGeneration)),
             static_cast<unsigned long long>(context_info.VaStart),
             static_cast<unsigned long long>(context_info.VaSize),
             static_cast<unsigned long long>(context_info.ResetGeneration),
             context_info.ContextId, context_info.SubmitQueueId);
      ready = NT_SUCCESS(status) &&
              tu_wddm_validate_context_info(&context_info,
                                            opened_info.ResetGeneration);
   }

   if (ready) {
      /* Reuse the production WDDM ownership helpers so this probe exercises
       * the exact private allocation ABI used by Turnip. The requested IOVA
       * is the first page of this context's VA slice; no command is submitted
       * by this phase. */
      tu_wddm_runtime runtime = {};
      runtime.dispatch = *dispatch;
      tu_wddm_device device = {};
      device.adapter.runtime = &runtime;
      device.adapter.luid = enumerated->AdapterLuid;
      device.adapter.handle = opened_adapter;
      device.adapter.private_info = opened_info;
      device.handle = device_handle;

      tu_wddm_context context = {};
      context.device = &device;
      context.handle = context_handle;
      context.info = context_info;

      tu_wddm_allocation_desc desc = {};
      desc.size = 4096;
      desc.alignment = 4096;
      desc.requested_iova = context_info.VaStart;
      desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE |
                   VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;

      tu_wddm_allocation allocation = {};
      printf("  CreateAllocation(native 4KiB): begin\n");
      const bool created =
         tu_wddm_allocation_create(&context, &desc, &allocation);
      printf("  CreateAllocation(native 4KiB): success=%u handle=0x%08x\n",
             static_cast<unsigned>(created), allocation.handle);
      bool allocation_ready = created;
      bool locked = false;
      bool unlocked = false;
      if (created) {
         void *map = nullptr;
         printf("  Lock(native 4KiB): begin\n");
         locked = tu_wddm_allocation_lock(&allocation, &map);
         printf("  Lock(native 4KiB): success=%u map=%p\n",
                static_cast<unsigned>(locked), map);
         if (locked && map != nullptr) {
            static_cast<unsigned char *>(map)[0] = 0xA5;
            printf("  Unlock(native 4KiB): begin\n");
            unlocked = tu_wddm_allocation_unlock(&allocation);
            if (!unlocked) {
               /* A transient thunk failure must not leave the owner behind
                * when the probe can safely retry the exact Unlock operation. */
               printf("  Unlock(native 4KiB): retry\n");
               unlocked = tu_wddm_allocation_unlock(&allocation);
            }
            printf("  Unlock(native 4KiB): success=%u\n",
                   static_cast<unsigned>(unlocked));
         }
         allocation_ready = locked && unlocked;
      }

      bool allocation_clean = true;
      if (allocation.handle != 0 && allocation.locked) {
         /* A failed Unlock keeps ownership in the helper. Give the thunk one
          * final bounded cleanup attempt before Context teardown. */
         printf("  Unlock(native 4KiB): cleanup retry\n");
         allocation_clean = tu_wddm_allocation_unlock(&allocation);
      }
      if (allocation.handle != 0 && !allocation.locked) {
         printf("  DestroyAllocation(native 4KiB): begin\n");
         bool destroyed = tu_wddm_allocation_destroy(&allocation);
         if (!destroyed) {
            printf("  DestroyAllocation(native 4KiB): retry\n");
            destroyed = tu_wddm_allocation_destroy(&allocation);
         }
         printf("  DestroyAllocation(native 4KiB): success=%u\n",
                static_cast<unsigned>(destroyed));
         allocation_clean = allocation_clean && destroyed;
      } else if (allocation.handle != 0) {
         allocation_clean = false;
      }
      ready = allocation_ready && allocation_clean && allocation.handle == 0;
   }

cleanup:
   if (context_handle != 0) {
      D3DKMT_DESTROYCONTEXT destroy = {};
      destroy.hContext = context_handle;
      status = dispatch->DestroyContext(&destroy);
      print_status("  DestroyContext", status);
      if (!NT_SUCCESS(status))
         ready = false;
   }
   if (device_handle != 0) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = device_handle;
      status = dispatch->DestroyDevice(&destroy);
      print_status("  DestroyDevice", status);
      if (!NT_SUCCESS(status))
         ready = false;
   }
   if (!close_adapter(dispatch, &opened_adapter))
      ready = false;
   return ready;
}

} /* namespace */

int
main()
{
   /* KMT calls can each wait for a bounded host response.  Keep every phase
    * visible when the probe is run through SSH, even before process exit. */
   (void)setvbuf(stdout, nullptr, _IONBF, 0);
   (void)setvbuf(stderr, nullptr, _IONBF, 0);
   printf("tu WDDM KMT probe: begin\n");

   tu_wddm_dispatch dispatch = {};
   if (!tu_wddm_dispatch_init(&dispatch)) {
      fprintf(stderr, "tu WDDM KMT probe: thunk initialization failed\n");
      return 1;
   }

   D3DKMT_ENUMADAPTERS2 enumeration = {};
   printf("EnumAdapters2(count): begin\n");
   NTSTATUS status = dispatch.EnumAdapters2(&enumeration);
   printf("EnumAdapters2(count): status=0x%08lx capacity=%lu\n",
          static_cast<unsigned long>(status), enumeration.NumAdapters);
   if (!NT_SUCCESS(status) || enumeration.NumAdapters == 0 ||
       enumeration.NumAdapters > SIZE_MAX / sizeof(D3DKMT_ADAPTERINFO)) {
      tu_wddm_dispatch_finish(&dispatch);
      return 1;
   }

   const ULONG capacity = enumeration.NumAdapters;
   D3DKMT_ADAPTERINFO *adapters = static_cast<D3DKMT_ADAPTERINFO *>(
      calloc(capacity, sizeof(*adapters)));
   if (adapters == NULL) {
      tu_wddm_dispatch_finish(&dispatch);
      return 1;
   }

   enumeration.NumAdapters = capacity;
   enumeration.pAdapters = adapters;
   printf("EnumAdapters2(fill): begin\n");
   status = dispatch.EnumAdapters2(&enumeration);
   printf("EnumAdapters2(fill): status=0x%08lx count=%lu\n",
          static_cast<unsigned long>(status), enumeration.NumAdapters);

   bool ready = false;
   if (NT_SUCCESS(status) && enumeration.NumAdapters <= capacity) {
      for (ULONG index = 0; index < enumeration.NumAdapters; index++) {
         printf("adapter[%lu]: handle=0x%08x luid=%08lx:%08lx sources=%lu\n",
                index, adapters[index].hAdapter,
                static_cast<unsigned long>(adapters[index].AdapterLuid.HighPart),
                static_cast<unsigned long>(adapters[index].AdapterLuid.LowPart),
                adapters[index].NumOfSources);
         if (adapters[index].hAdapter != 0 &&
             probe_adapter(&dispatch, &adapters[index]))
            ready = true;
      }
   }

   const ULONG close_count = NT_SUCCESS(status) &&
                             enumeration.NumAdapters <= capacity
                                ? enumeration.NumAdapters
                                : 0;
   for (ULONG index = 0; index < close_count; index++)
      close_adapter(&dispatch, &adapters[index].hAdapter);

   free(adapters);
   tu_wddm_dispatch_finish(&dispatch);
   if (!ready) {
      fprintf(stderr, "tu WDDM KMT probe: no adapter completed context bring-up\n");
      return 1;
   }

   printf("tu WDDM KMT probe passed\n");
   return 0;
}
