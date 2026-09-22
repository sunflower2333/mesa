// SPDX-License-Identifier: MIT
// D3D10/11 UMD residency production code against fake runtime callbacks that
// enforce the WDDM 2.0 device residency list (display/residency-overview.md).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "tu_wddm_abi.h"
#include "tu_wddm_residency.h"

using UINT = unsigned;
using ULONG = unsigned long;
using UINT64 = uint64_t;
using HRESULT = int32_t;
using NTSTATUS = int32_t;
using D3DKMT_HANDLE = uint32_t;
using HANDLE = void *;
#define APIENTRY
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005u)
#define E_PENDING ((HRESULT)0x8000000Au)
#define E_OUTOFMEMORY ((HRESULT)0x8007000Eu)
#define E_INVALIDARG ((HRESULT)0x80070057u)
#define E_NOTIMPL ((HRESULT)0x80004001u)
#define E_UNEXPECTED ((HRESULT)0x8000FFFFu)
#define DXGI_DDI_ERR_UNSUPPORTED ((HRESULT)0x887B0002u)
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
#define DebugPrintf(...) ((void)0)
struct LUID { uint32_t LowPart; int32_t HighPart; };
enum DXGI_FORMAT { DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_R10G10B10A2_UNORM = 24, DXGI_FORMAT_R8G8B8A8_UNORM = 28,
                   DXGI_FORMAT_B8G8R8A8_UNORM = 87 };
enum KMTQUERYADAPTERINFOTYPE { KMTQAITYPE_UMDRIVERPRIVATE = 0, KMTQAITYPE_DRIVERVERSION = 13 };
enum D3DDDI_PAGINGQUEUE_PRIORITY { D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL = 0 };
struct D3DKMT_ADAPTERINFO { D3DKMT_HANDLE hAdapter; LUID AdapterLuid; ULONG NumOfSources; int bPrecisePresentRegionsPreferred; };
struct D3DKMT_ENUMADAPTERS2 { ULONG NumAdapters; D3DKMT_ADAPTERINFO *pAdapters; };
struct D3DKMT_QUERYADAPTERINFO { D3DKMT_HANDLE hAdapter; KMTQUERYADAPTERINFOTYPE Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; };
struct D3DKMT_CLOSEADAPTER { D3DKMT_HANDLE hAdapter; };
using PFND3DKMT_ENUMADAPTERS2 = NTSTATUS (*)(const D3DKMT_ENUMADAPTERS2 *);
using PFND3DKMT_QUERYADAPTERINFO = NTSTATUS (*)(const D3DKMT_QUERYADAPTERINFO *);
using PFND3DKMT_CLOSEADAPTER = NTSTATUS (*)(const D3DKMT_CLOSEADAPTER *);
struct D3DDDI_ALLOCATIONLIST { D3DKMT_HANDLE hAllocation; UINT WriteOperation; };
struct D3DDDI_PATCHLOCATIONLIST { UINT AllocationIndex, SlotId, AllocationOffset, PatchOffset; };
struct D3DDDI_ALLOCATIONINFO { D3DKMT_HANDLE hAllocation; const void *pSystem; const void *pPrivateDriverData; UINT PrivateDriverDataSize; };
struct D3DDDICB_ALLOCATE { const void *pPrivateDriverData; UINT PrivateDriverDataSize; HANDLE hResource; D3DKMT_HANDLE hKMResource; UINT NumAllocations; D3DDDI_ALLOCATIONINFO *pAllocationInfo; };
struct D3DDDICB_DEALLOCATE { HANDLE hResource; UINT NumAllocations; const D3DKMT_HANDLE *HandleList; };
struct D3DDDICB_CREATECONTEXT {
   UINT NodeOrdinal, EngineAffinity; HANDLE hContext; void *pCommandBuffer; UINT CommandBufferSize;
   D3DDDI_ALLOCATIONLIST *pAllocationList; UINT AllocationListSize;
   D3DDDI_PATCHLOCATIONLIST *pPatchLocationList; UINT PatchLocationListSize;
};
struct D3DDDICB_DESTROYCONTEXT { HANDLE hContext; };
struct D3DDDICB_RENDER {
   UINT CommandLength, CommandOffset, NumAllocations, NumPatchLocations; void *pNewCommandBuffer;
   UINT NewCommandBufferSize; D3DDDI_ALLOCATIONLIST *pNewAllocationList; UINT NewAllocationListSize;
   D3DDDI_PATCHLOCATIONLIST *pNewPatchLocationList; UINT NewPatchLocationListSize; HANDLE hContext;
};
struct D3DDDICB_LOCK { D3DKMT_HANDLE hAllocation; void *pData; };
struct D3DDDICB_UNLOCK { UINT NumAllocations; const D3DKMT_HANDLE *phAllocations; };
struct D3DDDICB_CREATEPAGINGQUEUE {
   D3DDDI_PAGINGQUEUE_PRIORITY Priority; D3DKMT_HANDLE hPagingQueue; D3DKMT_HANDLE hSyncObject;
   void *FenceValueCPUVirtualAddress; UINT PhysicalAdapterIndex;
};
struct D3DDDI_DESTROYPAGINGQUEUE { D3DKMT_HANDLE hPagingQueue; };
struct D3DDDI_MAKERESIDENT {
   D3DKMT_HANDLE hPagingQueue; UINT NumAllocations; const D3DKMT_HANDLE *AllocationList; const UINT *PriorityList;
   struct { UINT CantTrimFurther : 1; UINT MustSucceed : 1; UINT Reserved : 30; } Flags;
   UINT64 PagingFenceValue; UINT64 NumBytesToTrim;
};
struct D3DDDICB_EVICT { UINT NumAllocations; const D3DKMT_HANDLE *AllocationList; struct { UINT Value; } Flags; UINT64 NumBytesToTrim; };
struct D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU {
   UINT ObjectCount; const D3DKMT_HANDLE *ObjectHandleArray; const UINT64 *FenceValueArray;
   HANDLE hAsyncEvent; struct { UINT Value; } Flags;
};
struct D3DDDI_DEVICECALLBACKS {
   HRESULT (*pfnAllocateCb)(HANDLE, D3DDDICB_ALLOCATE *);
   HRESULT (*pfnDeallocateCb)(HANDLE, const D3DDDICB_DEALLOCATE *);
   HRESULT (*pfnRenderCb)(HANDLE, D3DDDICB_RENDER *);
   HRESULT (*pfnLockCb)(HANDLE, D3DDDICB_LOCK *);
   HRESULT (*pfnUnlockCb)(HANDLE, const D3DDDICB_UNLOCK *);
   HRESULT (*pfnCreateContextCb)(HANDLE, D3DDDICB_CREATECONTEXT *);
   HRESULT (*pfnDestroyContextCb)(HANDLE, const D3DDDICB_DESTROYCONTEXT *);
   HRESULT (*pfnMakeResidentCb)(HANDLE, D3DDDI_MAKERESIDENT *);
   HRESULT (*pfnEvictCb)(HANDLE, D3DDDICB_EVICT *);
   HRESULT (*pfnWaitForSynchronizationObjectFromCpuCb)(HANDLE, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *);
   HRESULT (*pfnCreatePagingQueueCb)(HANDLE, D3DDDICB_CREATEPAGINGQUEUE *);
   HRESULT (*pfnDestroyPagingQueueCb)(HANDLE, const D3DDDI_DESTROYPAGINGQUEUE *);
};
struct pipe_resource { unsigned width0, height0; };
struct Resource {
   D3DKMT_HANDLE hKMResource;
   D3DKMT_HANDLE hAllocation;
   HANDLE hRTResourceHandle;
   Resource *shared_next;
   UINT shared_pitch;
   D3DKMT_HANDLE shared_staging_allocation;
   bool allocation_lockable;
   bool native_host_backing;
   bool allocation_resident;
   bool staging_resident;
   bool staging_idle;
   UINT64 shared_staging_size;
   DXGI_FORMAT Format;
   pipe_resource *resource;
};
struct Device {
   HANDLE hDevice;
   D3DDDI_DEVICECALLBACKS KTCallbacks;
   Resource *shared_resources;
   D3DDDICB_CREATECONTEXT shared_copy_context;
   UINT kmt_driver_version;
   D3DKMT_HANDLE paging_queue;
   D3DKMT_HANDLE paging_queue_sync_object;
   const UINT64 *paging_fence_cpu_address;
   UINT64 pending_paging_fence;
   HRESULT last_residency_hr;
   UINT residency_attempts;
};
static void LogSharedCopyFailure(const char *, HRESULT) {}
UINT64 ResidencyTrimStaging(Device *device, Resource *keep, UINT64 bytes_to_trim);

// PRODUCTION_STRUCTS

// PRODUCTION_FUNCTIONS

/* ------------------------------ fake runtime ------------------------------ */
static const HANDLE kRuntimeDevice = reinterpret_cast<HANDLE>(0x7000);
constexpr D3DKMT_HANDLE kQueue = 0x50, kSync = 0x51;
struct scripted { HRESULT hr; UINT64 fence; UINT64 trim; };
struct fake_runtime {
   std::vector<std::string> calls;
   std::set<D3DKMT_HANDLE> live;
   std::map<D3DKMT_HANDLE, int> refs;
   std::map<D3DKMT_HANDLE, UINT64> fence_of;
   std::map<HANDLE, D3DKMT_HANDLE> resource_allocation;
   std::deque<scripted> script;
   std::vector<bool> cant_trim;
   UINT64 cpu_fence = 0;
   D3DKMT_HANDLE next_allocation = 0x200;
   HRESULT evict_hr = S_OK, wait_hr = S_OK, queue_hr = S_OK, dealloc_hr = S_OK;
   unsigned evict_failures = 0, non_resident_renders = 0, unpaged_renders = 0, evict_without_reference = 0,
            destroyed_resident = 0, renders = 0;
   bool queue_live = false;
   D3DDDI_ALLOCATIONLIST allocation_list[8];
   D3DDDI_PATCHLOCATIONLIST patch_list[8];
   uint8_t command[256];

   unsigned count(const std::string &prefix) const
   {
      return static_cast<unsigned>(std::count_if(calls.begin(), calls.end(), [&](const std::string &c) {
         return c.compare(0, prefix.size(), prefix) == 0;
      }));
   }
   unsigned residency_calls() const
   {
      return count("CreatePagingQueueCb") + count("DestroyPagingQueueCb") + count("MakeResidentCb") +
             count("EvictCb") + count("WaitForSynchronizationObjectFromCpuCb");
   }
   long index_of(const std::string &call) const
   {
      auto it = std::find(calls.begin(), calls.end(), call);
      return it == calls.end() ? -1 : static_cast<long>(it - calls.begin());
   }
   bool wddm2 = false;
};
static fake_runtime *rt;
static std::string named(const char *name, uint64_t value)
{
   char text[80];
   snprintf(text, sizeof(text), "%s:%llx", name, static_cast<unsigned long long>(value));
   return text;
}

static HRESULT cb_allocate(HANDLE, D3DDDICB_ALLOCATE *allocate)
{
   const D3DKMT_HANDLE handle = rt->next_allocation++;
   allocate->pAllocationInfo[0].hAllocation = handle;
   if (allocate->hResource)
      rt->resource_allocation[allocate->hResource] = handle;
   rt->live.insert(handle);
   rt->calls.push_back(named("AllocateCb", handle));
   return S_OK;
}
static HRESULT cb_deallocate(HANDLE, const D3DDDICB_DEALLOCATE *deallocate)
{
   D3DKMT_HANDLE handle = deallocate->hResource ? rt->resource_allocation[deallocate->hResource]
                                                : deallocate->HandleList[0];
   rt->calls.push_back(named("DeallocateCb", handle));
   if (FAILED(rt->dealloc_hr))
      return rt->dealloc_hr;
   rt->destroyed_resident += rt->refs[handle] != 0;
   rt->refs.erase(handle);
   rt->live.erase(handle);
   return S_OK;
}
static HRESULT cb_render(HANDLE, D3DDDICB_RENDER *render)
{
   rt->calls.push_back("RenderCb");
   rt->renders++;
   for (UINT i = 0; i < render->NumAllocations; i++) {
      const D3DKMT_HANDLE handle = rt->allocation_list[i].hAllocation;
      if (!rt->wddm2)
         continue;
      if (rt->refs[handle] == 0) {
         rt->non_resident_renders++;
         return E_INVALIDARG;
      }
      if (rt->fence_of[handle] > rt->cpu_fence) {
         rt->unpaged_renders++;
         return E_INVALIDARG;
      }
   }
   render->pNewCommandBuffer = rt->command;
   render->pNewAllocationList = rt->allocation_list;
   render->pNewPatchLocationList = rt->patch_list;
   return S_OK;
}
static HRESULT cb_lock(HANDLE, D3DDDICB_LOCK *) { return S_OK; }
static HRESULT cb_unlock(HANDLE, const D3DDDICB_UNLOCK *) { return S_OK; }
static HRESULT cb_create_context(HANDLE, D3DDDICB_CREATECONTEXT *create)
{
   rt->calls.push_back("CreateContextCb");
   create->hContext = reinterpret_cast<HANDLE>(0x9000);
   create->pCommandBuffer = rt->command;
   create->CommandBufferSize = sizeof(rt->command);
   create->pAllocationList = rt->allocation_list;
   create->AllocationListSize = 8;
   create->pPatchLocationList = rt->patch_list;
   create->PatchLocationListSize = 8;
   return S_OK;
}
static HRESULT cb_destroy_context(HANDLE, const D3DDDICB_DESTROYCONTEXT *) { return S_OK; }
static HRESULT cb_create_queue(HANDLE device, D3DDDICB_CREATEPAGINGQUEUE *create)
{
   rt->calls.push_back("CreatePagingQueueCb");
   if (device != kRuntimeDevice || create->Priority != D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL)
      return E_INVALIDARG;
   if (FAILED(rt->queue_hr))
      return rt->queue_hr;
   create->hPagingQueue = kQueue;
   create->hSyncObject = kSync;
   create->FenceValueCPUVirtualAddress = &rt->cpu_fence;
   rt->queue_live = true;
   return S_OK;
}
static HRESULT cb_destroy_queue(HANDLE, const D3DDDI_DESTROYPAGINGQUEUE *destroy)
{
   rt->calls.push_back("DestroyPagingQueueCb");
   rt->queue_live = false;
   return destroy->hPagingQueue == kQueue ? S_OK : E_INVALIDARG;
}
static HRESULT cb_make_resident(HANDLE, D3DDDI_MAKERESIDENT *request)
{
   const D3DKMT_HANDLE handle = request->AllocationList[0];
   rt->calls.push_back(named("MakeResidentCb", handle));
   rt->cant_trim.push_back(request->Flags.CantTrimFurther != 0);
   if (!rt->queue_live || request->hPagingQueue != kQueue || request->NumAllocations != 1 || !rt->live.count(handle))
      return E_INVALIDARG;
   scripted next = {S_OK, 0, 0};
   if (!rt->script.empty()) {
      next = rt->script.front();
      rt->script.pop_front();
   }
   request->PagingFenceValue = next.fence;
   request->NumBytesToTrim = next.trim;
   if (next.hr == S_OK || next.hr == E_PENDING) {
      rt->refs[handle]++;
      rt->fence_of[handle] = next.hr == E_PENDING ? next.fence : 0;
   }
   return next.hr;
}
static HRESULT cb_evict(HANDLE, D3DDDICB_EVICT *evict)
{
   const D3DKMT_HANDLE handle = evict->AllocationList[0];
   rt->calls.push_back(named("EvictCb", handle));
   if (rt->evict_failures) {
      rt->evict_failures--;
      return rt->evict_hr;
   }
   if (rt->refs[handle] == 0) {
      rt->evict_without_reference++;
      return E_INVALIDARG;
   }
   rt->refs[handle]--;
   return S_OK;
}
static HRESULT cb_wait(HANDLE, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *wait)
{
   rt->calls.push_back(named("WaitForSynchronizationObjectFromCpuCb", wait->FenceValueArray[0]));
   if (wait->ObjectCount != 1 || wait->ObjectHandleArray[0] != kSync || wait->hAsyncEvent)
      return E_INVALIDARG;
   if (FAILED(rt->wait_hr))
      return rt->wait_hr;
   rt->cpu_fence = std::max(rt->cpu_fence, wait->FenceValueArray[0]);
   return S_OK;
}

/* Fake gdi32 KMT enumeration for the driver-model gate. */
struct fake_adapter { UINT version; bool droidvm; };
static std::vector<fake_adapter> adapters;
static unsigned version_queries, private_queries, closes;
static NTSTATUS kmt_enum(const D3DKMT_ENUMADAPTERS2 *input)
{
   D3DKMT_ENUMADAPTERS2 *enumeration = const_cast<D3DKMT_ENUMADAPTERS2 *>(input);
   if (enumeration->pAdapters) {
      for (size_t i = 0; i < adapters.size() && i < enumeration->NumAdapters; i++)
         enumeration->pAdapters[i].hAdapter = static_cast<D3DKMT_HANDLE>(0x40 + i);
   }
   enumeration->NumAdapters = static_cast<ULONG>(adapters.size());
   return 0;
}
static NTSTATUS kmt_query(const D3DKMT_QUERYADAPTERINFO *query)
{
   const fake_adapter &adapter = adapters.at(query->hAdapter - 0x40);
   if (query->Type == KMTQAITYPE_DRIVERVERSION) {
      version_queries++;
      memcpy(query->pPrivateDriverData, &adapter.version, sizeof(UINT));
      return 0;
   }
   private_queries++;
   if (!adapter.droidvm)
      return static_cast<NTSTATUS>(0xc01e0009u);
   VIOGPU_WDDM_ADAPTER_INFO info = {};
   info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
   info.Header.Size = sizeof(info);
   memcpy(query->pPrivateDriverData, &info, sizeof(info));
   return query->PrivateDriverDataSize == sizeof(info) ? 0 : static_cast<NTSTATUS>(0xc000000du);
}
static NTSTATUS kmt_close(const D3DKMT_CLOSEADAPTER *) { closes++; return 0; }

/* -------------------------------- fixture -------------------------------- */
static unsigned checks, failures;
static void check(bool ok, const char *label)
{
   checks++;
   if (!ok) {
      failures++;
      printf("FAIL %s\n", label);
   }
}

struct session {
   fake_runtime fake;
   Device device = {};
   pipe_resource texture = {64, 32};
   std::deque<Resource> resources;

   explicit session(UINT driver_version)
   {
      rt = &fake;
      fake.wddm2 = driver_version >= 2000;
      device.hDevice = kRuntimeDevice;
      device.kmt_driver_version = driver_version;
      D3DDDI_DEVICECALLBACKS &cb = device.KTCallbacks;
      cb.pfnAllocateCb = cb_allocate;
      cb.pfnDeallocateCb = cb_deallocate;
      cb.pfnRenderCb = cb_render;
      cb.pfnLockCb = cb_lock;
      cb.pfnUnlockCb = cb_unlock;
      cb.pfnCreateContextCb = cb_create_context;
      cb.pfnDestroyContextCb = cb_destroy_context;
      cb.pfnMakeResidentCb = cb_make_resident;
      cb.pfnEvictCb = cb_evict;
      cb.pfnWaitForSynchronizationObjectFromCpuCb = cb_wait;
      cb.pfnCreatePagingQueueCb = cb_create_queue;
      cb.pfnDestroyPagingQueueCb = cb_destroy_queue;
   }
   ~session() { rt = nullptr; }

   /* The kernel-allocation part of CreateResource: AllocateCb, then admit. */
   Resource *create(bool lockable, HRESULT *result = nullptr)
   {
      resources.emplace_back();
      Resource *resource = &resources.back();
      resource->resource = &texture;
      resource->Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      resource->shared_pitch = texture.width0 * 4;
      resource->allocation_lockable = lockable;
      resource->hRTResourceHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1000 + resources.size()));
      D3DDDI_ALLOCATIONINFO info = {};
      D3DDDICB_ALLOCATE allocate = {};
      allocate.hResource = resource->hRTResourceHandle;
      allocate.NumAllocations = 1;
      allocate.pAllocationInfo = &info;
      device.KTCallbacks.pfnAllocateCb(device.hDevice, &allocate);
      resource->hAllocation = info.hAllocation;
      HRESULT hr = ResidencyAdmitCreatedAllocation(&device, resource);
      if (result)
         *result = hr;
      if (SUCCEEDED(hr)) {
         resource->shared_next = device.shared_resources;
         device.shared_resources = resource;
      }
      return resource;
   }

   HRESULT transfer(Resource *resource, bool publish)
   {
      HRESULT hr = EnsureSharedCopy(&device, resource);
      if (SUCCEEDED(hr))
         hr = SubmitSharedCopy(&device, resource, publish);
      if (SUCCEEDED(hr))
         resource->staging_idle = true; /* TransferSharedResource after its completion wait */
      return hr;
   }

   void unlink(Resource *resource)
   {
      for (Resource **entry = &device.shared_resources; *entry; entry = &(*entry)->shared_next) {
         if (*entry == resource) {
            *entry = resource->shared_next;
            return;
         }
      }
   }
};

static void test_driver_model_gate()
{
   ResidencyKmt kmt = {kmt_enum, kmt_query, kmt_close};
   adapters = {{1300, false}, {2000, true}, {2100, false}};
   version_queries = private_queries = closes = 0;
   check(ResidencyEnumerateDriverVersion(&kmt) == 2000 && version_queries == 2 && private_queries == 1 &&
         closes == 3, "a WDDM 2.0 DroidVM adapter selects residency and every handle is closed");
   adapters = {{1300, false}, {1200, true}};
   version_queries = private_queries = closes = 0;
   check(ResidencyEnumerateDriverVersion(&kmt) == 0 && version_queries == 2 && private_queries == 0 && closes == 2,
         "WDDM 1.x DroidVM adapter keeps residency off without reaching the miniport");
   adapters = {{2600, false}};
   version_queries = private_queries = closes = 0;
   check(ResidencyEnumerateDriverVersion(&kmt) == 0 && private_queries == 1 && closes == 1,
         "a WDDM 2.x adapter without the DroidVM ABI does not enable residency");
   ResidencyKmt missing = {nullptr, kmt_query, kmt_close};
   check(ResidencyEnumerateDriverVersion(&missing) == 0, "missing gdi32 thunks keep the WDDM 1.x path");
}

static void test_wddm1_issues_no_residency_callback()
{
   session s(1200);
   Resource *a = s.create(false);
   Resource *b = s.create(true);
   check(s.transfer(a, false) == S_OK && s.transfer(a, true) == S_OK, "WDDM 1.x transfers");
   check(ResidencyPrepareSubmission(&s.device) == S_OK, "WDDM 1.x present preparation");
   s.unlink(a);
   ReleaseResourceAllocations(&s.device, a);
   s.unlink(b);
   ReleaseResourceAllocations(&s.device, b);
   ResidencyDestroyDevice(&s.device);
   check(s.fake.residency_calls() == 0, "WDDM 1.x adapter issues no residency callback");
   const std::vector<std::string> legacy = {"AllocateCb:200", "AllocateCb:201", "CreateContextCb",
                                            "AllocateCb:202", "RenderCb", "RenderCb", "DeallocateCb:202",
                                            "DeallocateCb:200", "DeallocateCb:201"};
   check(s.fake.calls == legacy, "WDDM 1.x callback sequence is unchanged");
   check(s.fake.live.empty() && !a->allocation_resident && !a->staging_resident, "WDDM 1.x releases every allocation");
}

static void test_wddm2_allocations_resident_before_use()
{
   session s(2000);
   Resource *a = s.create(false);
   Resource *b = s.create(false);
   check(s.fake.count("CreatePagingQueueCb") == 1 && s.fake.index_of("CreatePagingQueueCb") == 1 &&
         s.fake.index_of("MakeResidentCb:200") == 2 && s.fake.index_of("MakeResidentCb:201") == 4 &&
         a->allocation_resident && b->allocation_resident,
         "WDDM 2.0 makes each created allocation resident, with one lazily created paging queue");
   check(s.transfer(a, false) == S_OK && s.transfer(b, true) == S_OK && a->staging_resident && b->staging_resident,
         "WDDM 2.0 transfers with resident private staging");
   check(s.fake.index_of("MakeResidentCb:202") == s.fake.index_of("AllocateCb:202") + 1 &&
         s.fake.non_resident_renders == 0 && s.fake.renders == 2,
         "WDDM 2.0 RenderCb referenced a non-resident allocation");

   /* An opened allocation takes this device's own reference; destroy never deallocates it. */
   Resource opened = {};
   opened.hAllocation = s.fake.next_allocation++;
   s.fake.live.insert(opened.hAllocation);
   check(ResidencyMakeResident(&s.device, &opened, opened.hAllocation, &opened.allocation_resident) == S_OK &&
         opened.allocation_resident, "opened allocation is made resident");
   ReleaseResourceAllocations(&s.device, &opened);
   check(s.fake.count(named("EvictCb", opened.hAllocation)) == 1 &&
         s.fake.count(named("DeallocateCb", opened.hAllocation)) == 0 && !opened.allocation_resident,
         "opened allocation is evicted and not deallocated");
   s.fake.live.erase(opened.hAllocation);

   s.unlink(a);
   ReleaseResourceAllocations(&s.device, a);
   s.unlink(b);
   ReleaseResourceAllocations(&s.device, b);
   check(s.fake.destroyed_resident == 0 && s.fake.evict_without_reference == 0 && s.fake.count("EvictCb") == 5,
         "destroy evicts each reference exactly once");
   ReleaseResourceAllocations(&s.device, a);
   check(s.fake.count("EvictCb") == 5 && s.fake.count("DeallocateCb") == 4, "destroy evicts each reference exactly once");
   ResidencyDestroyDevice(&s.device);
   check(s.fake.count("DestroyPagingQueueCb") == 1 && s.device.paging_queue == 0 && s.fake.live.empty(),
         "DestroyDevice releases the paging queue");
}

static void test_native_host_copy_refused()
{
   for (bool lockable : {false, true}) {
      session s(2000);
      Resource *resource = s.create(lockable);
      resource->native_host_backing = true;
      const auto calls = s.fake.calls;
      for (bool publish : {false, true}) {
         check(s.transfer(resource, publish) == DXGI_DDI_ERR_UNSUPPORTED &&
               s.fake.calls == calls && !resource->shared_staging_allocation,
               "native HostSurface refuses CPU copy before runtime callbacks");
      }
      s.unlink(resource);
      ReleaseResourceAllocations(&s.device, resource);
      ResidencyDestroyDevice(&s.device);
   }
}

static void test_pending_waits_before_render()
{
   session s(2000);
   s.fake.script = {{E_PENDING, 7, 0}};
   Resource *a = s.create(false);
   check(a->allocation_resident && s.device.pending_paging_fence == 7 &&
         s.fake.count("WaitForSynchronizationObjectFromCpuCb") == 0,
         "E_PENDING is a residency reference with a recorded paging fence");
   s.fake.script = {{E_PENDING, 9, 0}};
   check(s.transfer(a, false) == S_OK, "pending transfer succeeds");
   const long wait = s.fake.index_of("WaitForSynchronizationObjectFromCpuCb:9");
   check(wait >= 0 && wait < s.fake.index_of("RenderCb") && s.fake.unpaged_renders == 0,
         "WDDM 2.0 RenderCb issued before its paging fence completed");
   check(s.device.pending_paging_fence == 0 && ResidencyPrepareSubmission(&s.device) == S_OK &&
         s.fake.count("WaitForSynchronizationObjectFromCpuCb") == 1, "observed paging fence is not waited again");

   Resource *b = s.create(false);
   s.fake.script = {{E_PENDING, 20, 0}};
   check(s.transfer(b, false) == S_OK, "second transfer");
   s.fake.script = {{E_PENDING, 30, 0}};
   Resource *c = s.create(false);
   s.fake.cpu_fence = 31;
   check(ResidencyPrepareSubmission(&s.device) == S_OK && s.fake.count("WaitForSynchronizationObjectFromCpuCb") == 2,
         "completed CPU fence view skips the wait");

   s.fake.script = {{E_PENDING, 40, 0}};
   Resource *d = s.create(false);
   s.fake.wait_hr = E_INVALIDARG;
   const unsigned renders = s.fake.renders;
   d->shared_staging_allocation = 0;
   check(FAILED(s.transfer(d, false)) && s.fake.renders == renders && s.device.pending_paging_fence == 40,
         "failed paging wait prevents RenderCb");
   s.fake.wait_hr = S_OK;
   check(s.transfer(d, false) == S_OK && s.fake.unpaged_renders == 0, "paging wait retries on the next submission");
   for (Resource *resource : {a, b, c, d}) {
      s.unlink(resource);
      ReleaseResourceAllocations(&s.device, resource);
   }
   ResidencyDestroyDevice(&s.device);
   check(s.fake.live.empty() && s.fake.destroyed_resident == 0, "pending: teardown");
}

static void test_over_budget_trims_idle_staging()
{
   session s(2000);
   Resource *a = s.create(false), *b = s.create(false), *c = s.create(false), *d = s.create(false);
   check(s.transfer(a, false) == S_OK && s.transfer(b, false) == S_OK && s.transfer(c, false) == S_OK,
         "budget: staging exists");
   b->staging_idle = false; /* its scheduled copy was never observed complete */
   const D3DKMT_HANDLE a_staging = a->shared_staging_allocation, b_staging = b->shared_staging_allocation,
                       c_staging = c->shared_staging_allocation;
   s.fake.cant_trim.clear();
   /* shared_resources is most-recent first: d (the request), c (idle), b (busy), a (idle).
    * One 8 KiB staging release satisfies NumBytesToTrim, so trimming stops there. */
   s.fake.script = {{E_OUTOFMEMORY, 0, 4096}, {S_OK, 0, 0}};
   check(s.transfer(d, false) == S_OK && d->staging_resident &&
         s.fake.cant_trim == std::vector<bool>({false, false}) && c->shared_staging_allocation == 0 &&
         a->shared_staging_allocation == a_staging && b->shared_staging_allocation == b_staging &&
         s.fake.count(named("EvictCb", c_staging)) == 1 && s.fake.count(named("DeallocateCb", c_staging)) == 1 &&
         !s.fake.live.count(c_staging),
         "out-of-memory trims idle staging and retries");

   check(s.transfer(c, false) == S_OK && c->shared_staging_allocation != 0 && c->staging_resident,
         "trimmed staging is recreated and made resident on next use");

   /* Nothing idle is left to trim: one final CantTrimFurther attempt, then refusal. */
   for (Resource *resource : {a, b, c, d})
      resource->staging_idle = false;
   s.fake.cant_trim.clear();
   const unsigned evictions = s.fake.count("EvictCb");
   s.fake.script = {{E_OUTOFMEMORY, 0, 1}, {E_OUTOFMEMORY, 0, 1}, {S_OK, 0, 0}};
   HRESULT hr = S_OK;
   Resource *e = s.create(false, &hr);
   check(hr == E_OUTOFMEMORY && e->hAllocation == 0 && !e->allocation_resident &&
         s.fake.cant_trim == std::vector<bool>({false, true}) && s.fake.count("EvictCb") == evictions &&
         s.fake.script.size() == 1,
         "over budget with nothing idle ends with one CantTrimFurther attempt");
   s.fake.script.clear();

   for (Resource *resource : {a, b, c, d}) {
      s.unlink(resource);
      ReleaseResourceAllocations(&s.device, resource);
   }
   ResidencyDestroyDevice(&s.device);
   check(s.fake.live.empty() && s.fake.destroyed_resident == 0 && s.fake.evict_without_reference == 0,
         "budget: teardown releases every allocation after exactly one Evict each");
}

static void test_trim_loop_is_bounded()
{
   session s(2000);
   std::vector<Resource *> idle;
   for (unsigned i = 0; i < 10; i++) {
      idle.push_back(s.create(false));
      check(s.transfer(idle.back(), false) == S_OK, "bounded: staging");
   }
   s.fake.cant_trim.clear();
   for (unsigned i = 0; i < TU_WDDM_RESIDENCY_MAX_ATTEMPTS; i++)
      s.fake.script.push_back({E_OUTOFMEMORY, 0, 1});
   s.fake.script.push_back({S_OK, 0, 0});
   HRESULT hr = S_OK;
   Resource *request = s.create(false, &hr);
   const unsigned remaining = static_cast<unsigned>(std::count_if(idle.begin(), idle.end(), [](Resource *r) {
      return r->shared_staging_allocation != 0;
   }));
   check(hr == E_OUTOFMEMORY && request->hAllocation == 0 &&
         s.fake.cant_trim.size() == TU_WDDM_RESIDENCY_MAX_ATTEMPTS && s.fake.cant_trim.back() &&
         std::count(s.fake.cant_trim.begin(), s.fake.cant_trim.end(), true) == 1 &&
         remaining == 10 - (TU_WDDM_RESIDENCY_MAX_ATTEMPTS - 1) && s.fake.script.size() == 1,
         "out-of-memory trims and retries in a bounded loop");
   s.fake.script.clear();
   for (Resource *resource : idle) {
      s.unlink(resource);
      ReleaseResourceAllocations(&s.device, resource);
   }
   ResidencyDestroyDevice(&s.device);
   check(s.fake.live.empty() && s.fake.destroyed_resident == 0, "bounded: teardown");
}

static void test_failures_fail_closed()
{
   {
      session s(2000);
      s.fake.script = {{E_INVALIDARG, 0, 0}};
      HRESULT hr = S_OK;
      Resource *a = s.create(false, &hr);
      check(hr == E_INVALIDARG && a->hAllocation == 0 && a->hRTResourceHandle == nullptr &&
            s.fake.count("MakeResidentCb") == 1 && s.fake.count("EvictCb") == 0 && s.fake.live.empty(),
            "failed MakeResident releases the new allocation without Evict or retry");
   }
   {
      session s(2000);
      s.fake.queue_hr = E_OUTOFMEMORY;
      HRESULT hr = S_OK;
      s.create(false, &hr);
      check(hr == E_OUTOFMEMORY && s.fake.count("MakeResidentCb") == 0 && s.fake.live.empty(),
            "paging queue failure refuses the allocation");
   }
   {
      session s(2000);
      s.device.KTCallbacks.pfnMakeResidentCb = nullptr;
      HRESULT hr = S_OK;
      s.create(false, &hr);
      check(FAILED(hr) && s.fake.residency_calls() == 0 && s.fake.live.empty(),
            "a runtime without residency callbacks refuses WDDM 2.0 allocations");
   }
   {
      session s(2000);
      Resource *a = s.create(false);
      check(s.transfer(a, false) == S_OK, "evict failure: transfer");
      s.fake.evict_failures = 2;
      s.fake.evict_hr = E_INVALIDARG;
      s.unlink(a);
      ReleaseResourceAllocations(&s.device, a);
      check(s.fake.count("EvictCb") == 2 && s.fake.count("DeallocateCb") == 2 && s.fake.live.empty() &&
            !a->allocation_resident && !a->staging_resident,
            "destroy still releases allocations when Evict fails, attempting it once each");
      ResidencyDestroyDevice(&s.device);
   }
}

int main()
{
   test_driver_model_gate();
   test_wddm1_issues_no_residency_callback();
   test_wddm2_allocations_resident_before_use();
   test_native_host_copy_refused();
   test_pending_waits_before_render();
   test_over_budget_trims_idle_staging();
   test_trim_loop_is_bounded();
   test_failures_fail_closed();
   printf("%s D3D10 UMD residency: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
   return failures ? 1 : 0;
}
