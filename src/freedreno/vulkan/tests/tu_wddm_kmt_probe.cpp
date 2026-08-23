/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Direct ARM64 D3DKMT bring-up probe for the DroidVM Turnip private endpoint.
 * It does not load Vulkan or a D3D UMD.  Every acquired KMT handle is closed
 * before exit, including adapters rejected by the private ABI query.
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
   status = dispatch->OpenAdapterFromLuid(&open);
   printf("  OpenAdapterFromLuid: status=0x%08lx handle=0x%08x\n",
          static_cast<unsigned long>(status), open.hAdapter);
   if (!NT_SUCCESS(status) || open.hAdapter == 0)
      return false;

   bool ready = false;
   D3DKMT_HANDLE opened_adapter = open.hAdapter;
   D3DKMT_HANDLE device_handle = 0;
   D3DKMT_HANDLE context_handle = 0;

   VIOGPU_WDDM_ADAPTER_INFO opened_info = {};
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
      VIOGPU_WDDM_CONTEXT_INFO info = {};
      info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      info.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      info.Header.Size = static_cast<uint32_t>(sizeof(info));
      info.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
      info.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
      info.ExpectedResetGeneration = opened_info.ResetGeneration;

      D3DKMT_ESCAPE escape = {};
      escape.hAdapter = opened_adapter;
      escape.hDevice = device_handle;
      escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
      escape.pPrivateDriverData = &info;
      escape.PrivateDriverDataSize = static_cast<UINT>(sizeof(info));
      escape.hContext = context_handle;
      status = dispatch->Escape(&escape);
      printf("  Escape(GET_CONTEXT_INFO): status=0x%08lx valid=%u "
             "va=0x%llx+0x%llx reset=%llu context=%u queue=%u\n",
             static_cast<unsigned long>(status),
             static_cast<unsigned>(
                NT_SUCCESS(status) &&
                tu_wddm_validate_context_info(
                   &info, opened_info.ResetGeneration)),
             static_cast<unsigned long long>(info.VaStart),
             static_cast<unsigned long long>(info.VaSize),
             static_cast<unsigned long long>(info.ResetGeneration),
             info.ContextId, info.SubmitQueueId);
      ready = NT_SUCCESS(status) &&
              tu_wddm_validate_context_info(&info,
                                            opened_info.ResetGeneration);
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
   tu_wddm_dispatch dispatch = {};
   if (!tu_wddm_dispatch_init(&dispatch)) {
      fprintf(stderr, "tu WDDM KMT probe: thunk initialization failed\n");
      return 1;
   }

   D3DKMT_ENUMADAPTERS2 enumeration = {};
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
