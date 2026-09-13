/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Residency.cpp --
 *    WDDM 2.0 residency for the kernel allocations of this D3D10/11 UMD.
 *
 * Contract (d3dumddi pfnMakeResidentCb / pfnEvictCb / pfnCreatePagingQueueCb /
 * pfnWaitForSynchronizationObjectFromCpuCb, display/residency-overview.md):
 *
 *  - Gate.  An adapter is WDDM 2.0 when dxgkrnl reports KMTQAITYPE_DRIVERVERSION
 *    >= KMT_DRIVERVERSION_WDDM_2_0 for an adapter that answers the DroidVM
 *    private ABI.  Otherwise no residency callback is ever made and the WDDM
 *    1.x allocation-list residency stays in charge.
 *  - Allocations.  Every kernel allocation this device creates (shared,
 *    present and primary resources, private staging) or opens holds exactly
 *    one residency reference from creation/open until destruction.
 *  - Paging queue.  One per device, created on first use, destroyed in
 *    DestroyDevice.  E_PENDING records PagingFenceValue; RenderCb, PresentCb
 *    and SetDisplayModeCb are issued only after the fence is observed.
 *  - Budget.  E_OUTOFMEMORY trims idle private staging of other shared
 *    resources (recreated on demand by EnsureSharedCopy) and retries in the
 *    bounded loop of tu_wddm_residency.h, finishing with CantTrimFurther.
 *  - Destroy.  Evict once, then deallocate.  DestroyResource cannot report an
 *    error, and destroying the allocation removes it from the residency list,
 *    so a failed Evict is logged and the handle is still released.
 *  - TrimResidencySet is a DXGI1_4_DDI_BASE_FUNCTIONS entry that the runtime
 *    fills only for Interface >= D3DWDDM2_0_DDI_INTERFACE_VERSION
 *    (IS_DXGI1_4_BASE_FUNCTIONS).  This frontend negotiates at most
 *    D3D11_0_7_DDI_INTERFACE_VERSION, so the runtime never asks it to trim;
 *    budget pressure reaches it only through MakeResident failures.
 */

#include "Residency.h"

#include "Debug.h"
#include "State.h"
#include "tu_wddm_abi.h"
#include "tu_wddm_residency.h"

#include <stdlib.h>

struct ResidencyKmt
{
   PFND3DKMT_ENUMADAPTERS2 EnumAdapters2;
   PFND3DKMT_QUERYADAPTERINFO QueryAdapterInfo;
   PFND3DKMT_CLOSEADAPTER CloseAdapter;
};

static bool
ResidencyAdapterClaimsDroidVmAbi(const ResidencyKmt *kmt, D3DKMT_HANDLE adapter)
{
   VIOGPU_WDDM_ADAPTER_INFO info;
   memset(&info, 0, sizeof info);
   D3DKMT_QUERYADAPTERINFO query;
   memset(&query, 0, sizeof query);
   query.hAdapter = adapter;
   query.Type = KMTQAITYPE_UMDRIVERPRIVATE;
   query.pPrivateDriverData = &info;
   query.PrivateDriverDataSize = sizeof info;
   return kmt->QueryAdapterInfo(&query) == 0 &&
          info.Header.Magic == VIOGPU_WDDM_ABI_MAGIC &&
          info.Header.Version == VIOGPU_WDDM_ABI_VERSION &&
          info.Header.Size == sizeof info;
}

/* The UMD is registered only by the viogpu package, and every DroidVM adapter
 * of a system runs that one KMD, so the DroidVM adapters share one driver
 * model.  The driver-version query is answered by dxgkrnl; the private ABI
 * query that reaches the miniport is issued only for WDDM 2.0 adapters. */
static UINT
ResidencyEnumerateDriverVersion(const ResidencyKmt *kmt)
{
   if (kmt == NULL || kmt->EnumAdapters2 == NULL || kmt->QueryAdapterInfo == NULL ||
       kmt->CloseAdapter == NULL)
      return 0;

   D3DKMT_ENUMADAPTERS2 enumeration;
   memset(&enumeration, 0, sizeof enumeration);
   if (kmt->EnumAdapters2(&enumeration) != 0 || enumeration.NumAdapters == 0 ||
       enumeration.NumAdapters > 64)
      return 0;

   const ULONG capacity = enumeration.NumAdapters;
   D3DKMT_ADAPTERINFO *adapters =
      static_cast<D3DKMT_ADAPTERINFO *>(calloc(capacity, sizeof *adapters));
   if (adapters == NULL)
      return 0;

   enumeration.NumAdapters = capacity;
   enumeration.pAdapters = adapters;
   const NTSTATUS status = kmt->EnumAdapters2(&enumeration);
   const ULONG count = status == 0 && enumeration.NumAdapters <= capacity
                          ? enumeration.NumAdapters : 0;

   UINT driver_version = 0;
   for (ULONG i = 0; i < count; ++i) {
      if (adapters[i].hAdapter == 0)
         continue;
      UINT version = 0;
      D3DKMT_QUERYADAPTERINFO query;
      memset(&query, 0, sizeof query);
      query.hAdapter = adapters[i].hAdapter;
      query.Type = KMTQAITYPE_DRIVERVERSION;
      query.pPrivateDriverData = &version;
      query.PrivateDriverDataSize = sizeof version;
      if (driver_version == 0 && kmt->QueryAdapterInfo(&query) == 0 &&
          version >= TU_WDDM_DRIVER_VERSION_WDDM_2_0 &&
          ResidencyAdapterClaimsDroidVmAbi(kmt, adapters[i].hAdapter))
         driver_version = version;

      D3DKMT_CLOSEADAPTER close;
      memset(&close, 0, sizeof close);
      close.hAdapter = adapters[i].hAdapter;
      (void)kmt->CloseAdapter(&close);
   }

   free(adapters);
   return driver_version;
}

UINT
ResidencyQueryAdapterDriverVersion(void)
{
   /* Resolve the system thunks directly.  This DLL exports D3DKMT* software
    * stubs of its own (D3DKMT.cpp), so the thunks must never be linked by name. */
   HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (gdi == NULL)
      return 0;

   ResidencyKmt kmt;
   kmt.EnumAdapters2 =
      reinterpret_cast<PFND3DKMT_ENUMADAPTERS2>(GetProcAddress(gdi, "D3DKMTEnumAdapters2"));
   kmt.QueryAdapterInfo =
      reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
   kmt.CloseAdapter =
      reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
   const UINT driver_version = ResidencyEnumerateDriverVersion(&kmt);
   FreeLibrary(gdi);
   DebugPrintf("Residency: DroidVM adapter driver_version=%u residency=%u\n",
               driver_version, driver_version >= TU_WDDM_DRIVER_VERSION_WDDM_2_0 ? 1u : 0u);
   return driver_version;
}

bool
ResidencyRequired(const Device *device)
{
   return device != NULL && device->kmt_driver_version >= TU_WDDM_DRIVER_VERSION_WDDM_2_0;
}

static HRESULT
ResidencyEnsurePagingQueue(Device *device)
{
   if (device->paging_queue != 0)
      return S_OK;
   if (!device->KTCallbacks.pfnCreatePagingQueueCb || !device->KTCallbacks.pfnDestroyPagingQueueCb ||
       !device->KTCallbacks.pfnMakeResidentCb || !device->KTCallbacks.pfnEvictCb ||
       !device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb)
      return E_NOTIMPL;

   D3DDDICB_CREATEPAGINGQUEUE create;
   memset(&create, 0, sizeof create);
   create.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
   create.PhysicalAdapterIndex = 0;
   HRESULT hr = device->KTCallbacks.pfnCreatePagingQueueCb(device->hDevice, &create);
   device->last_residency_hr = hr;
   if (hr != S_OK || create.hPagingQueue == 0 || create.hSyncObject == 0) {
      DebugPrintf("Residency: CreatePagingQueueCb hr=0x%08lx queue=0x%x sync=0x%x\n",
                  (unsigned long)hr, create.hPagingQueue, create.hSyncObject);
      if (create.hPagingQueue != 0) {
         D3DDDI_DESTROYPAGINGQUEUE destroy;
         memset(&destroy, 0, sizeof destroy);
         destroy.hPagingQueue = create.hPagingQueue;
         (void)device->KTCallbacks.pfnDestroyPagingQueueCb(device->hDevice, &destroy);
      }
      return FAILED(hr) ? hr : E_FAIL;
   }

   device->paging_queue = create.hPagingQueue;
   device->paging_queue_sync_object = create.hSyncObject;
   device->paging_fence_cpu_address =
      static_cast<const UINT64 *>(create.FenceValueCPUVirtualAddress);
   return S_OK;
}

struct ResidencyRequest
{
   Device *device;
   Resource *keep;
   D3DKMT_HANDLE allocation;
   HRESULT hr;
};

static enum tu_wddm_residency_outcome
ResidencyAttempt(void *data, bool cant_trim_further, uint64_t *paging_fence,
                 uint64_t *bytes_to_trim)
{
   ResidencyRequest *request = static_cast<ResidencyRequest *>(data);
   Device *device = request->device;
   const D3DKMT_HANDLE handle = request->allocation;

   D3DDDI_MAKERESIDENT make;
   memset(&make, 0, sizeof make);
   make.hPagingQueue = device->paging_queue;
   make.NumAllocations = 1;
   make.AllocationList = &handle;
   make.PriorityList = NULL;
   make.Flags.CantTrimFurther = cant_trim_further ? 1 : 0;
   const HRESULT hr = device->KTCallbacks.pfnMakeResidentCb(device->hDevice, &make);
   request->hr = hr;
   *paging_fence = make.PagingFenceValue;
   *bytes_to_trim = make.NumBytesToTrim;
   return tu_wddm_residency_classify_hresult(static_cast<uint32_t>(hr));
}

static uint64_t
ResidencyTrim(void *data, uint64_t bytes_to_trim)
{
   ResidencyRequest *request = static_cast<ResidencyRequest *>(data);
   return ResidencyTrimStaging(request->device, request->keep, bytes_to_trim);
}

HRESULT
ResidencyMakeResident(Device *device, Resource *owner, D3DKMT_HANDLE allocation, bool *resident)
{
   if (device == NULL || resident == NULL || allocation == 0)
      return E_INVALIDARG;
   if (*resident || !ResidencyRequired(device))
      return S_OK;

   HRESULT hr = ResidencyEnsurePagingQueue(device);
   if (FAILED(hr))
      return hr;

   ResidencyRequest request = { device, owner, allocation, S_OK };
   uint64_t paging_fence = 0;
   uint32_t attempts = 0;
   const enum tu_wddm_residency_outcome outcome = tu_wddm_make_resident_bounded(
      ResidencyAttempt, ResidencyTrim, &request, &paging_fence, &attempts);
   device->residency_attempts = attempts;
   device->last_residency_hr = request.hr;

   switch (outcome) {
   case TU_WDDM_RESIDENCY_PENDING:
      /* One paging queue: its fence is monotonic, keep the highest target. */
      if (paging_fence > device->pending_paging_fence)
         device->pending_paging_fence = paging_fence;
      *resident = true;
      return S_OK;
   case TU_WDDM_RESIDENCY_RESIDENT:
      *resident = true;
      return S_OK;
   case TU_WDDM_RESIDENCY_OVER_BUDGET:
      DebugPrintf("Residency: allocation=0x%x over budget after %u attempts hr=0x%08lx\n",
                  allocation, attempts, (unsigned long)request.hr);
      return E_OUTOFMEMORY;
   default:
      DebugPrintf("Residency: allocation=0x%x MakeResidentCb hr=0x%08lx\n", allocation,
                  (unsigned long)request.hr);
      return FAILED(request.hr) ? request.hr : E_FAIL;
   }
}

HRESULT
ResidencyEvict(Device *device, D3DKMT_HANDLE allocation, bool *resident)
{
   if (device == NULL || resident == NULL)
      return E_INVALIDARG;
   if (!*resident)
      return S_OK;
   if (allocation == 0 || !device->KTCallbacks.pfnEvictCb)
      return E_UNEXPECTED;

   const D3DKMT_HANDLE handle = allocation;
   D3DDDICB_EVICT evict;
   memset(&evict, 0, sizeof evict);
   evict.NumAllocations = 1;
   evict.AllocationList = &handle;
   /* EvictOnlyIfNecessary clear: callers are about to release the handle. */
   evict.Flags.Value = 0;
   const HRESULT hr = device->KTCallbacks.pfnEvictCb(device->hDevice, &evict);
   device->last_residency_hr = hr;
   if (FAILED(hr)) {
      DebugPrintf("Residency: EvictCb allocation=0x%x hr=0x%08lx\n", allocation, (unsigned long)hr);
      return hr;
   }
   *resident = false;
   return S_OK;
}

HRESULT
ResidencyPrepareSubmission(Device *device)
{
   if (device == NULL)
      return E_INVALIDARG;
   const UINT64 fence = device->pending_paging_fence;
   if (fence == 0)
      return S_OK;
   if (device->paging_queue == 0 || device->paging_queue_sync_object == 0 ||
       !device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb)
      return E_UNEXPECTED;

   /* FenceValueCPUVirtualAddress is the queue fence's read-only CPU view. */
   if (device->paging_fence_cpu_address == NULL || *device->paging_fence_cpu_address < fence) {
      const D3DKMT_HANDLE object = device->paging_queue_sync_object;
      const UINT64 value = fence;
      D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
      memset(&wait, 0, sizeof wait);
      wait.ObjectCount = 1;
      wait.ObjectHandleArray = &object;
      wait.FenceValueArray = &value;
      /* NULL: return only once the paging operation has completed. */
      wait.hAsyncEvent = NULL;
      wait.Flags.Value = 0;
      const HRESULT hr =
         device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb(device->hDevice, &wait);
      device->last_residency_hr = hr;
      if (FAILED(hr)) {
         DebugPrintf("Residency: paging fence %llu wait hr=0x%08lx\n",
                     (unsigned long long)fence, (unsigned long)hr);
         return hr;
      }
   }

   device->pending_paging_fence = 0;
   return S_OK;
}

void
ResidencyDestroyDevice(Device *device)
{
   if (device == NULL || device->paging_queue == 0)
      return;
   if (device->KTCallbacks.pfnDestroyPagingQueueCb) {
      D3DDDI_DESTROYPAGINGQUEUE destroy;
      memset(&destroy, 0, sizeof destroy);
      destroy.hPagingQueue = device->paging_queue;
      const HRESULT hr = device->KTCallbacks.pfnDestroyPagingQueueCb(device->hDevice, &destroy);
      device->last_residency_hr = hr;
      if (FAILED(hr))
         DebugPrintf("Residency: DestroyPagingQueueCb hr=0x%08lx\n", (unsigned long)hr);
   }
   /* The runtime device owns the queue either way; never keep its CPU view. */
   device->paging_queue = 0;
   device->paging_queue_sync_object = 0;
   device->paging_fence_cpu_address = NULL;
   device->pending_paging_fence = 0;
}

HRESULT
ResidencyAdmitCreatedAllocation(Device *device, Resource *resource)
{
   const HRESULT hr = ResidencyMakeResident(device, resource, resource->hAllocation,
                                            &resource->allocation_resident);
   if (SUCCEEDED(hr))
      return S_OK;

   /* MakeResident failures take no reference: release the new allocation so
    * the resource is refused instead of reaching a submission non-resident. */
   D3DDDICB_DEALLOCATE deallocate;
   memset(&deallocate, 0, sizeof deallocate);
   deallocate.hResource = resource->hRTResourceHandle;
   const HRESULT release = device->KTCallbacks.pfnDeallocateCb(device->hDevice, &deallocate);
   if (FAILED(release))
      DebugPrintf("Residency: rollback DeallocateCb hr=0x%08lx\n", (unsigned long)release);
   resource->hAllocation = 0;
   resource->hKMResource = 0;
   resource->hRTResourceHandle = NULL;
   resource->allocation_resident = false;
   return hr;
}

static HRESULT
ReleaseStagingAllocation(Device *device, Resource *resource, bool force)
{
   if (resource->shared_staging_allocation == 0)
      return S_OK;

   HRESULT hr = ResidencyEvict(device, resource->shared_staging_allocation,
                               &resource->staging_resident);
   if (FAILED(hr) && !force)
      return hr;

   D3DDDICB_DEALLOCATE deallocate;
   memset(&deallocate, 0, sizeof deallocate);
   deallocate.NumAllocations = 1;
   deallocate.HandleList = &resource->shared_staging_allocation;
   hr = device->KTCallbacks.pfnDeallocateCb(device->hDevice, &deallocate);
   if (FAILED(hr)) {
      DebugPrintf("Residency: staging DeallocateCb hr=0x%08lx\n", (unsigned long)hr);
      if (!force)
         return hr;
   }
   resource->shared_staging_allocation = 0;
   resource->shared_staging_size = 0;
   resource->staging_resident = false;
   resource->staging_idle = true;
   return S_OK;
}

UINT64
ResidencyTrimStaging(Device *device, Resource *keep, UINT64 bytes_to_trim)
{
   UINT64 trimmed = 0;
   for (Resource *resource = device->shared_resources; resource != NULL;
        resource = resource->shared_next) {
      if (resource == keep || resource->shared_staging_allocation == 0 ||
          !resource->staging_idle)
         continue;
      const UINT64 size = resource->shared_staging_size;
      if (FAILED(ReleaseStagingAllocation(device, resource, false)))
         continue;
      trimmed += size != 0 ? size : 4096;
      if (bytes_to_trim != 0 && trimmed >= bytes_to_trim)
         break;
   }
   return trimmed;
}

void
ReleaseResourceAllocations(Device *device, Resource *resource)
{
   (void)ReleaseStagingAllocation(device, resource, true);

   if (resource->hAllocation == 0)
      return;

   /* Evict exactly once.  The runtime frees the resource memory after this
    * returns, and destroying the allocation drops any reference Evict left. */
   (void)ResidencyEvict(device, resource->hAllocation, &resource->allocation_resident);
   resource->allocation_resident = false;

   /* Only free an allocation this device created. An opened resource holds a
    * view of another process's allocation and must not deallocate it. */
   if (resource->hRTResourceHandle != NULL) {
      D3DDDICB_DEALLOCATE deallocate;
      memset(&deallocate, 0, sizeof deallocate);
      /* With a resource handle set, the runtime frees the resource and every
       * allocation under it, so no handle list is passed. */
      deallocate.hResource = resource->hRTResourceHandle;
      deallocate.NumAllocations = 0;
      deallocate.HandleList = NULL;
      device->KTCallbacks.pfnDeallocateCb(device->hDevice, &deallocate);
      resource->hAllocation = 0;
      resource->hKMResource = 0;
      resource->hRTResourceHandle = NULL;
   }
}
