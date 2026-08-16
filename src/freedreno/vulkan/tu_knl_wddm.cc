/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Windows Turnip kernel-interface transport.  This file owns only the
 * WDDM/D3DKMT object and private-ABI plumbing.  It intentionally does not
 * submit a command stream or manufacture a fence completion; those operations
 * remain behind the KMD guest-backed allocation/retirement gates.
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
       !tu_wddm_header_is_current(&info->Header,
                                  tu_wddm_sizeof<VIOGPU_WDDM_CONTEXT_INFO>()) ||
       info->Opcode != VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO ||
       info->Flags != VIOGPU_WDDM_ESCAPE_FLAGS_NONE ||
       info->ExpectedResetGeneration != expected_reset_generation || info->VaStart == 0 ||
       info->VaSize == 0 || info->ResetGeneration != expected_reset_generation ||
       info->ContextId == 0 || (info->VaStart & 4095) != 0 || (info->VaSize & 4095) != 0 ||
       info->VaSize > UINT64_MAX - info->VaStart)
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
