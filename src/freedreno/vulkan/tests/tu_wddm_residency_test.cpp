/* SPDX-License-Identifier: MIT */
/* Production WDDM transport against a fake D3DKMT layer that enforces the
 * WDDM 2.0 residency contract (display/residency-overview.md): Render with a
 * non-resident allocation, or before its paging fence, is rejected. */
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "tu_wddm_abi.h"
#include "tu_wddm_residency.h"

/* ---- Minimal D3DKMT surface with the SDK field names production uses ---- */
using NTSTATUS = int32_t;
using D3DKMT_HANDLE = uint32_t;
using UINT = unsigned int;
using ULONG = unsigned long;
using BYTE = unsigned char;
using LONG64 = int64_t;
using ULONGLONG = uint64_t;
using HANDLE = void *;
#define NT_SUCCESS(status) ((status) >= 0)
#define APIENTRY
struct LUID { uint32_t LowPart; int32_t HighPart; };
enum D3DKMT_DRIVERVERSION { KMT_DRIVERVERSION_WDDM_1_2 = 1200, KMT_DRIVERVERSION_WDDM_2_0 = 2000 };
enum KMTQUERYADAPTERINFOTYPE { KMTQAITYPE_UMDRIVERPRIVATE = 0, KMTQAITYPE_DRIVERVERSION = 13 };
enum D3DKMT_DEVICESTATE_TYPE { D3DKMT_DEVICESTATE_EXECUTION = 1 };
enum D3DKMT_DEVICEEXECUTION_STATE { D3DKMT_DEVICEEXECUTION_ACTIVE = 1, D3DKMT_DEVICEEXECUTION_RESET = 2 };
enum D3DDDI_PAGINGQUEUE_PRIORITY { D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL = 0 };
struct D3DKMT_QUERYADAPTERINFO { D3DKMT_HANDLE hAdapter; KMTQUERYADAPTERINFOTYPE Type; void *pPrivateDriverData; UINT PrivateDriverDataSize; };
struct D3DKMT_OPENADAPTERFROMLUID { LUID AdapterLuid; D3DKMT_HANDLE hAdapter; };
struct D3DKMT_CLOSEADAPTER { D3DKMT_HANDLE hAdapter; };
struct D3DDDI_ALLOCATIONLIST { D3DKMT_HANDLE hAllocation; UINT WriteOperation; };
struct D3DDDI_PATCHLOCATIONLIST { UINT AllocationIndex; UINT SlotId; UINT AllocationOffset; UINT PatchOffset; };
struct D3DKMT_CREATEDEVICE {
   D3DKMT_HANDLE hAdapter; D3DKMT_HANDLE hDevice; void *pCommandBuffer; UINT CommandBufferSize;
   D3DDDI_ALLOCATIONLIST *pAllocationList; UINT AllocationListSize;
   D3DDDI_PATCHLOCATIONLIST *pPatchLocationList; UINT PatchLocationListSize;
};
struct D3DKMT_DESTROYDEVICE { D3DKMT_HANDLE hDevice; };
struct D3DKMT_GETDEVICESTATE { D3DKMT_HANDLE hDevice; D3DKMT_DEVICESTATE_TYPE StateType; D3DKMT_DEVICEEXECUTION_STATE ExecutionState; };
struct D3DKMT_CREATEPAGINGQUEUE {
   D3DKMT_HANDLE hDevice; D3DDDI_PAGINGQUEUE_PRIORITY Priority; D3DKMT_HANDLE hPagingQueue;
   D3DKMT_HANDLE hSyncObject; void *FenceValueCPUVirtualAddress; UINT PhysicalAdapterIndex;
};
struct D3DDDI_DESTROYPAGINGQUEUE { D3DKMT_HANDLE hPagingQueue; };
struct D3DDDI_MAKERESIDENT {
   D3DKMT_HANDLE hPagingQueue; UINT NumAllocations; const D3DKMT_HANDLE *AllocationList;
   const UINT *PriorityList;
   struct { UINT CantTrimFurther : 1; UINT MustSucceed : 1; UINT Reserved : 30; } Flags;
   uint64_t PagingFenceValue; uint64_t NumBytesToTrim;
};
struct D3DKMT_EVICT {
   D3DKMT_HANDLE hDevice; UINT NumAllocations; const D3DKMT_HANDLE *AllocationList;
   struct { UINT Value; } Flags; uint64_t NumBytesToTrim;
};
struct D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU {
   D3DKMT_HANDLE hDevice; UINT ObjectCount; const D3DKMT_HANDLE *ObjectHandleArray;
   const uint64_t *FenceValueArray; HANDLE hAsyncEvent; struct { UINT Value; } Flags;
};
struct D3DDDI_ALLOCATIONINFO { D3DKMT_HANDLE hAllocation; void *pPrivateDriverData; UINT PrivateDriverDataSize; };
struct D3DKMT_CREATEALLOCATION {
   D3DKMT_HANDLE hDevice; UINT NumAllocations; D3DDDI_ALLOCATIONINFO *pAllocationInfo;
   struct { UINT NonSecure; } Flags;
};
struct D3DKMT_DESTROYALLOCATION2 {
   D3DKMT_HANDLE hDevice; const D3DKMT_HANDLE *phAllocationList; UINT AllocationCount;
   struct { UINT AssumeNotInUse; } Flags;
};
struct D3DKMT_RENDER {
   D3DKMT_HANDLE hContext; UINT CommandOffset; UINT CommandLength; UINT AllocationCount; UINT PatchLocationCount;
   void *pNewCommandBuffer; UINT NewCommandBufferSize;
   D3DDDI_ALLOCATIONLIST *pNewAllocationList; UINT NewAllocationListSize;
   D3DDDI_PATCHLOCATIONLIST *pNewPatchLocationList; UINT NewPatchLocationListSize;
};
struct tu_wddm_dispatch {
   NTSTATUS (*OpenAdapterFromLuid)(D3DKMT_OPENADAPTERFROMLUID *);
   NTSTATUS (*CloseAdapter)(const D3DKMT_CLOSEADAPTER *);
   NTSTATUS (*QueryAdapterInfo)(const D3DKMT_QUERYADAPTERINFO *);
   NTSTATUS (*CreateDevice)(D3DKMT_CREATEDEVICE *);
   NTSTATUS (*DestroyDevice)(const D3DKMT_DESTROYDEVICE *);
   NTSTATUS (*CreateAllocation)(D3DKMT_CREATEALLOCATION *);
   NTSTATUS (*DestroyAllocation2)(const D3DKMT_DESTROYALLOCATION2 *);
   NTSTATUS (*Render)(D3DKMT_RENDER *);
   NTSTATUS (*GetDeviceState)(D3DKMT_GETDEVICESTATE *);
   NTSTATUS (*CreatePagingQueue)(D3DKMT_CREATEPAGINGQUEUE *);
   NTSTATUS (*DestroyPagingQueue)(D3DDDI_DESTROYPAGINGQUEUE *);
   NTSTATUS (*MakeResident)(D3DDDI_MAKERESIDENT *);
   NTSTATUS (*Evict)(D3DKMT_EVICT *);
   NTSTATUS (*WaitForSynchronizationObjectFromCpu)(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *);
};

/* Real atomics so the lock-free paging-fence bookkeeping is exercised. */
static LONG64 InterlockedCompareExchange64(LONG64 volatile *target, LONG64 exchange, LONG64 comparand)
{
   return __sync_val_compare_and_swap(target, comparand, exchange);
}
static void Sleep(uint32_t) {}
static void tu_wddm_diag(const char *, ...) {}
template <typename T> static constexpr uint32_t tu_wddm_sizeof() { return static_cast<uint32_t>(sizeof(T)); }

// PRODUCTION_CONSTANTS

// PRODUCTION_STRUCTS

/* Public transport prototypes, as declared by tu_knl_wddm.h. */
bool tu_wddm_adapter_close(tu_wddm_adapter *adapter);
bool tu_wddm_device_close(tu_wddm_device *device);
bool tu_wddm_device_execution_active(tu_wddm_device *device);
bool tu_wddm_device_requires_residency(const tu_wddm_device *device);
bool tu_wddm_device_residency_init(tu_wddm_device *device);
bool tu_wddm_device_residency_finish(tu_wddm_device *device);
bool tu_wddm_device_wait_paging_fence(tu_wddm_device *device);

static unsigned advisory_waits;
static bool tu_wddm_context_wait_submissions(tu_wddm_context *, uint64_t)
{
   advisory_waits++; /* advisory in production; orthogonal to residency */
   return true;
}

// PRODUCTION_FUNCTIONS

/* ---------------------------------- fake KMT --------------------------------- */
constexpr D3DKMT_HANDLE kAdapter = 1, kDevice = 2, kContext = 3, kQueue = 0x50, kSync = 0x51;
constexpr NTSTATUS kSuccess = 0, kPending = 0x103, kNoMemory = static_cast<NTSTATUS>(0xc0000017u),
                   kNoVideoMemory = static_cast<NTSTATUS>(0xc01e0100u),
                   kInvalid = static_cast<NTSTATUS>(0xc000000du), kTimeout = 0x102,
                   kNotResident = static_cast<NTSTATUS>(0xc01e0005u);

struct scripted_residency { NTSTATUS status; uint64_t fence; uint64_t trim; };

struct fake_kmt {
   uint32_t driver_version = 1200;
   NTSTATUS version_status = kSuccess, create_queue_status = kSuccess, destroy_queue_status = kSuccess;
   NTSTATUS evict_status = kSuccess, wait_status = kSuccess, destroy_status = kSuccess;
   unsigned evict_failures = 0, destroy_failures = 0;
   std::vector<std::string> calls;
   std::deque<scripted_residency> script;
   std::vector<bool> make_resident_cant_trim;
   std::set<D3DKMT_HANDLE> live;
   std::map<D3DKMT_HANDLE, int> refs;
   std::map<D3DKMT_HANDLE, uint64_t> paging_fence_of;
   uint64_t cpu_fence = 0;
   D3DKMT_HANDLE next_allocation = 0x100;
   bool queue_live = false, device_live = false, adapter_open = false;
   unsigned non_resident_renders = 0, unpaged_renders = 0, destroyed_resident = 0;
   unsigned evict_without_reference = 0, destroy_device_with_queue = 0;
   BYTE command[TU_WDDM_MAX_RENDER_COMMAND_SIZE];
   D3DDDI_ALLOCATIONLIST allocations[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   D3DDDI_PATCHLOCATIONLIST patches[TU_WDDM_MAX_RENDER_ALLOCATIONS];

   bool wddm2() const { return driver_version >= 2000; }
   unsigned count(const std::string &prefix) const
   {
      return static_cast<unsigned>(std::count_if(calls.begin(), calls.end(), [&](const std::string &c) {
         return c.compare(0, prefix.size(), prefix) == 0;
      }));
   }
   unsigned residency_calls() const
   {
      return count("CreatePagingQueue") + count("DestroyPagingQueue") + count("MakeResident") +
             count("Evict") + count("WaitForSynchronizationObjectFromCpu");
   }
   long index_of(const std::string &call) const
   {
      auto it = std::find(calls.begin(), calls.end(), call);
      return it == calls.end() ? -1 : static_cast<long>(it - calls.begin());
   }
};
static fake_kmt *kmt;

static std::string with_handle(const char *name, uint64_t value)
{
   char buffer[96];
   snprintf(buffer, sizeof(buffer), "%s:%llx", name, static_cast<unsigned long long>(value));
   return buffer;
}

static VIOGPU_WDDM_ADAPTER_INFO valid_adapter_info()
{
   VIOGPU_WDDM_ADAPTER_INFO info = {};
   info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
   info.Header.Size = sizeof(info);
   info.ResetGeneration = 7;
   info.MsmMajorVersion = 1;
   info.MsmMinorVersion = 9;
   info.GpuId = 1;
   info.ChipId = 1;
   info.GmemSize = 4096;
   info.PriorityCount = 1;
   return info;
}

static NTSTATUS fake_open(D3DKMT_OPENADAPTERFROMLUID *open)
{
   kmt->calls.push_back("OpenAdapterFromLuid");
   open->hAdapter = kAdapter;
   kmt->adapter_open = true;
   return kSuccess;
}
static NTSTATUS fake_close(const D3DKMT_CLOSEADAPTER *close)
{
   kmt->calls.push_back("CloseAdapter");
   kmt->adapter_open = close->hAdapter != kAdapter;
   return kSuccess;
}
static NTSTATUS fake_query(const D3DKMT_QUERYADAPTERINFO *query)
{
   if (query->Type == KMTQAITYPE_UMDRIVERPRIVATE) {
      kmt->calls.push_back("QueryAdapterInfo(UMDRIVERPRIVATE)");
      if (query->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_ADAPTER_INFO))
         return kInvalid;
      VIOGPU_WDDM_ADAPTER_INFO info = valid_adapter_info();
      memcpy(query->pPrivateDriverData, &info, sizeof(info));
      return kSuccess;
   }
   kmt->calls.push_back("QueryAdapterInfo(DRIVERVERSION)");
   if (query->Type != KMTQAITYPE_DRIVERVERSION || query->PrivateDriverDataSize != sizeof(uint32_t))
      return kInvalid;
   memcpy(query->pPrivateDriverData, &kmt->driver_version, sizeof(uint32_t));
   return kmt->version_status;
}
static NTSTATUS fake_create_device(D3DKMT_CREATEDEVICE *create)
{
   kmt->calls.push_back("CreateDevice");
   create->hDevice = kDevice;
   create->pCommandBuffer = kmt->command;
   create->CommandBufferSize = sizeof(kmt->command);
   create->pAllocationList = kmt->allocations;
   create->AllocationListSize = TU_WDDM_MAX_RENDER_ALLOCATIONS;
   create->pPatchLocationList = kmt->patches;
   create->PatchLocationListSize = TU_WDDM_MAX_RENDER_ALLOCATIONS;
   kmt->device_live = true;
   return kSuccess;
}
static NTSTATUS fake_destroy_device(const D3DKMT_DESTROYDEVICE *)
{
   kmt->calls.push_back("DestroyDevice");
   kmt->destroy_device_with_queue += kmt->queue_live;
   kmt->queue_live = false; /* the device implicitly destroys its queues */
   kmt->device_live = false;
   return kSuccess;
}
static NTSTATUS fake_state(D3DKMT_GETDEVICESTATE *state)
{
   state->ExecutionState = D3DKMT_DEVICEEXECUTION_ACTIVE;
   return kSuccess;
}
static NTSTATUS fake_create_queue(D3DKMT_CREATEPAGINGQUEUE *create)
{
   kmt->calls.push_back("CreatePagingQueue");
   if (create->hDevice != kDevice || create->Priority != D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL)
      return kInvalid;
   if (kmt->create_queue_status != kSuccess)
      return kmt->create_queue_status;
   create->hPagingQueue = kQueue;
   create->hSyncObject = kSync;
   create->FenceValueCPUVirtualAddress = &kmt->cpu_fence;
   kmt->queue_live = true;
   return kSuccess;
}
static NTSTATUS fake_destroy_queue(D3DDDI_DESTROYPAGINGQUEUE *destroy)
{
   kmt->calls.push_back("DestroyPagingQueue");
   if (destroy->hPagingQueue != kQueue || !kmt->queue_live)
      return kInvalid;
   kmt->queue_live = false;
   return kmt->destroy_queue_status;
}
static NTSTATUS fake_create_allocation(D3DKMT_CREATEALLOCATION *create)
{
   create->pAllocationInfo->hAllocation = kmt->next_allocation++;
   kmt->live.insert(create->pAllocationInfo->hAllocation);
   kmt->calls.push_back(with_handle("CreateAllocation", create->pAllocationInfo->hAllocation));
   return kSuccess;
}
static NTSTATUS fake_destroy_allocation(const D3DKMT_DESTROYALLOCATION2 *destroy)
{
   const D3DKMT_HANDLE handle = destroy->phAllocationList[0];
   kmt->calls.push_back(with_handle("DestroyAllocation2", handle));
   if (kmt->destroy_failures != 0) {
      kmt->destroy_failures--;
      return kmt->destroy_status;
   }
   kmt->destroyed_resident += kmt->refs[handle] != 0;
   kmt->refs.erase(handle);
   kmt->live.erase(handle);
   return kSuccess;
}
static NTSTATUS fake_make_resident(D3DDDI_MAKERESIDENT *request)
{
   const D3DKMT_HANDLE handle = request->AllocationList[0];
   kmt->calls.push_back(with_handle("MakeResident", handle));
   kmt->make_resident_cant_trim.push_back(request->Flags.CantTrimFurther != 0);
   if (!kmt->queue_live || request->hPagingQueue != kQueue || request->NumAllocations != 1 ||
       !kmt->live.count(handle) || request->Flags.MustSucceed)
      return kInvalid;
   scripted_residency next = {kSuccess, 0, 0};
   if (!kmt->script.empty()) {
      next = kmt->script.front();
      kmt->script.pop_front();
   }
   request->PagingFenceValue = next.fence;
   request->NumBytesToTrim = next.trim;
   if (next.status == kSuccess || next.status == kPending) {
      kmt->refs[handle]++;
      kmt->paging_fence_of[handle] = next.status == kPending ? next.fence : 0;
   }
   return next.status;
}
static NTSTATUS fake_evict(D3DKMT_EVICT *evict)
{
   const D3DKMT_HANDLE handle = evict->AllocationList[0];
   kmt->calls.push_back(with_handle("Evict", handle));
   if (evict->hDevice != kDevice || evict->NumAllocations != 1 || evict->Flags.Value != 0)
      return kInvalid;
   if (kmt->evict_failures != 0) {
      kmt->evict_failures--;
      return kmt->evict_status;
   }
   if (kmt->refs[handle] == 0) {
      kmt->evict_without_reference++;
      return kInvalid;
   }
   kmt->refs[handle]--;
   return kSuccess;
}
static NTSTATUS fake_wait(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *wait)
{
   kmt->calls.push_back(with_handle("WaitForSynchronizationObjectFromCpu", wait->FenceValueArray[0]));
   if (wait->hDevice != kDevice || wait->ObjectCount != 1 || wait->ObjectHandleArray[0] != kSync ||
       wait->hAsyncEvent != nullptr || wait->Flags.Value != 0)
      return kInvalid;
   if (kmt->wait_status != kSuccess)
      return kmt->wait_status;
   kmt->cpu_fence = std::max(kmt->cpu_fence, wait->FenceValueArray[0]);
   return kSuccess;
}
static NTSTATUS fake_render(D3DKMT_RENDER *render)
{
   kmt->calls.push_back("Render");
   /* dxgkrnl on WDDM 2.0: "Failed to reference DMA buffer: Allocation is not
    * requested to be resident" rejects the submission. */
   for (UINT i = 0; i < render->AllocationCount; i++) {
      const D3DKMT_HANDLE handle = render->pNewAllocationList[i].hAllocation;
      if (!kmt->wddm2())
         continue;
      if (kmt->refs[handle] == 0) {
         kmt->non_resident_renders++;
         return kNotResident;
      }
      if (kmt->paging_fence_of[handle] > kmt->cpu_fence) {
         kmt->unpaged_renders++;
         return kNotResident;
      }
   }
   return kSuccess;
}

static tu_wddm_runtime make_runtime()
{
   tu_wddm_runtime runtime = {};
   runtime.dispatch.OpenAdapterFromLuid = fake_open;
   runtime.dispatch.CloseAdapter = fake_close;
   runtime.dispatch.QueryAdapterInfo = fake_query;
   runtime.dispatch.CreateDevice = fake_create_device;
   runtime.dispatch.DestroyDevice = fake_destroy_device;
   runtime.dispatch.CreateAllocation = fake_create_allocation;
   runtime.dispatch.DestroyAllocation2 = fake_destroy_allocation;
   runtime.dispatch.Render = fake_render;
   runtime.dispatch.GetDeviceState = fake_state;
   runtime.dispatch.CreatePagingQueue = fake_create_queue;
   runtime.dispatch.DestroyPagingQueue = fake_destroy_queue;
   runtime.dispatch.MakeResident = fake_make_resident;
   runtime.dispatch.Evict = fake_evict;
   runtime.dispatch.WaitForSynchronizationObjectFromCpu = fake_wait;
   return runtime;
}

/* ------------------------------ fixture helpers ------------------------------ */
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
   fake_kmt fake;
   tu_wddm_runtime runtime = make_runtime();
   tu_wddm_device device = {};
   tu_wddm_context context = {};
   uint32_t fence = 0;

   explicit session(uint32_t driver_version) { fake.driver_version = driver_version; kmt = &fake; }
   ~session() { kmt = nullptr; }

   bool open()
   {
      tu_wddm_adapter_info identity = {};
      identity.luid.LowPart = 0x1234;
      if (!tu_wddm_device_open(&runtime, &identity, &device))
         return false;
      context.device = &device;
      context.handle = kContext;
      context.command_buffer = device.command_buffer;
      context.command_buffer_size = device.command_buffer_size;
      context.allocation_list = device.allocation_list;
      context.allocation_list_size = device.allocation_list_size;
      context.patch_location_list = device.patch_location_list;
      context.patch_location_list_size = device.patch_location_list_size;
      VIOGPU_WDDM_CONTEXT_INFO &info = context.info;
      tu_wddm_init_header(&info.Header, sizeof(info));
      info.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
      info.ExpectedResetGeneration = info.ResetGeneration = device.adapter.private_info.ResetGeneration;
      info.VaStart = UINT64_C(0x100000000);
      info.VaSize = UINT64_C(0x01000000);
      info.ContextId = 11;
      info.SubmitQueueId = 17;
      return true;
   }

   bool allocate(tu_wddm_allocation *allocation, unsigned page)
   {
      tu_wddm_allocation_desc desc = {};
      desc.size = 4096;
      desc.alignment = 4096;
      desc.requested_iova = context.info.VaStart + page * UINT64_C(4096);
      desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
      return tu_wddm_allocation_create(&context, &desc, allocation);
   }

   /* One native submit whose BO table names every allocation given. */
   bool render(const std::vector<tu_wddm_allocation *> &allocations)
   {
      const uint32_t count = static_cast<uint32_t>(allocations.size());
      std::vector<uint8_t> packet(sizeof(tu_wddm_msm_submit_request) + count * sizeof(tu_wddm_msm_submit_bo) +
                                  sizeof(tu_wddm_msm_submit_command));
      tu_wddm_msm_submit_request request = {};
      request.command = TU_WDDM_MSM_CCMD_GEM_SUBMIT;
      request.length = static_cast<uint32_t>(packet.size());
      request.sequence = request.fence = ++fence;
      request.flags = TU_WDDM_MSM_PIPE_3D0 | TU_WDDM_MSM_SUBMIT_NO_IMPLICIT;
      request.queue_id = context.info.SubmitQueueId;
      request.bo_count = count;
      request.command_count = 1;
      memcpy(packet.data(), &request, sizeof(request));
      std::vector<tu_wddm_render_reference> references(count);
      for (uint32_t i = 0; i < count; i++) {
         tu_wddm_msm_submit_bo bo = {TU_WDDM_MSM_SUBMIT_BO_READ | TU_WDDM_MSM_SUBMIT_BO_WRITE, 0, 0};
         const size_t offset = sizeof(request) + i * sizeof(bo);
         memcpy(packet.data() + offset, &bo, sizeof(bo));
         references[i].allocation = allocations[i];
         references[i].flags = VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE;
         references[i].length = 4096;
         references[i].patch_offset = static_cast<uint32_t>(offset + offsetof(tu_wddm_msm_submit_bo, presumed));
      }
      tu_wddm_msm_submit_command command = {TU_WDDM_MSM_SUBMIT_CMD_BUF, 0, 0, 4, 0, 0, 0};
      memcpy(packet.data() + sizeof(request) + count * sizeof(tu_wddm_msm_submit_bo), &command, sizeof(command));
      return tu_wddm_context_render(&context, packet.data(), static_cast<uint32_t>(packet.size()),
                                    references.data(), count);
   }
};

/* ---------------------------------- scenarios -------------------------------- */
static void test_wddm1_issues_no_residency_calls()
{
   session s(1200);
   check(s.open(), "WDDM 1.x device opens");
   tu_wddm_allocation a = {}, b = {};
   check(s.allocate(&a, 0) && s.allocate(&b, 1), "WDDM 1.x allocations are created");
   check(s.render({&a, &b}) && s.render({&a}), "WDDM 1.x renders are accepted");
   check(tu_wddm_allocation_destroy(&a) && tu_wddm_allocation_destroy(&b), "WDDM 1.x allocations destroy");
   check(tu_wddm_device_close(&s.device), "WDDM 1.x device closes");
   const std::vector<std::string> expected = {
      "OpenAdapterFromLuid", "QueryAdapterInfo(UMDRIVERPRIVATE)", "QueryAdapterInfo(DRIVERVERSION)",
      "CreateDevice", "CreateAllocation:100", "CreateAllocation:101", "Render", "Render",
      "DestroyAllocation2:100", "DestroyAllocation2:101", "DestroyDevice", "CloseAdapter"};
   check(s.fake.residency_calls() == 0, "WDDM 1.x adapter issues no residency or paging call");
   check(s.fake.calls == expected, "WDDM 1.x call sequence is the legacy sequence plus one version query");
   check(!a.resident && !b.resident && s.device.paging_queue == 0, "WDDM 1.x keeps residency state empty");
}

static void test_wddm2_residency_lifecycle()
{
   session s(2000);
   check(s.open(), "WDDM 2.0 device opens");
   check(s.fake.index_of("CreateDevice") >= 0 &&
         s.fake.index_of("CreatePagingQueue") == s.fake.index_of("CreateDevice") + 1 &&
         s.device.paging_queue == kQueue && s.device.paging_queue_sync_object == kSync,
         "WDDM 2.0 device creates its paging queue right after CreateDevice");
   tu_wddm_allocation a = {}, b = {}, c = {};
   check(s.allocate(&a, 0) && s.allocate(&b, 1) && s.allocate(&c, 2), "WDDM 2.0 allocations are created");
   check(s.fake.index_of("MakeResident:100") == s.fake.index_of("CreateAllocation:100") + 1 &&
         s.fake.index_of("MakeResident:102") == s.fake.index_of("CreateAllocation:102") + 1 &&
         a.resident && b.resident && c.resident,
         "WDDM 2.0 makes every allocation resident as part of creation");
   check(s.render({&a, &b, &c}) && s.render({&b}), "WDDM 2.0 renders are accepted");
   check(s.fake.non_resident_renders == 0, "WDDM 2.0 Render referenced a non-resident allocation");
   check(s.fake.count("WaitForSynchronizationObjectFromCpu") == 0, "completed MakeResident needs no fence wait");
   for (tu_wddm_allocation *allocation : {&a, &b, &c})
      check(tu_wddm_allocation_destroy(allocation) && allocation->handle == 0 && !allocation->resident,
            "WDDM 2.0 allocation destroys");
   check(s.fake.count("Evict") == 3 && s.fake.evict_without_reference == 0 && s.fake.destroyed_resident == 0 &&
         s.fake.index_of("Evict:101") + 1 == s.fake.index_of("DestroyAllocation2:101"),
         "destroy evicts exactly once before DestroyAllocation2");
   check(tu_wddm_device_close(&s.device), "WDDM 2.0 device closes");
   check(s.fake.index_of("DestroyPagingQueue") + 1 == s.fake.index_of("DestroyDevice") &&
         s.fake.destroy_device_with_queue == 0 && s.device.paging_queue == 0,
         "paging queue is destroyed immediately before DestroyDevice");
   check(!s.fake.adapter_open && s.fake.live.empty(), "WDDM 2.0 teardown releases every KMT owner");
}

static void test_pending_waits_for_paging_fence()
{
   session s(2000);
   check(s.open(), "pending: device opens");
   s.fake.script = {{kPending, 7, 0}};
   tu_wddm_allocation a = {}, b = {};
   check(s.allocate(&a, 0) && a.resident && s.device.pending_paging_fence == 7,
         "STATUS_PENDING is a residency reference with a recorded paging fence");
   check(s.fake.count("WaitForSynchronizationObjectFromCpu") == 0, "creation does not block on paging");
   check(s.render({&a}), "pending: render succeeds once paged in");
   const long wait = s.fake.index_of("WaitForSynchronizationObjectFromCpu:7");
   check(wait >= 0 && wait < s.fake.index_of("Render") && s.fake.unpaged_renders == 0,
         "WDDM 2.0 Render issued before its paging fence completed");
   check(s.device.pending_paging_fence == 0 && s.render({&a}) &&
         s.fake.count("WaitForSynchronizationObjectFromCpu") == 1,
         "an observed paging fence is not waited for again");

   /* The CPU-visible monitored fence already covers the target: no KMT wait. */
   s.fake.script = {{kPending, 9, 0}};
   s.fake.cpu_fence = 12;
   check(s.allocate(&b, 1) && s.render({&a, &b}) && s.fake.count("WaitForSynchronizationObjectFromCpu") == 1 &&
         s.device.pending_paging_fence == 0, "completed CPU fence view skips the wait");

   /* A pending MakeResident with fence zero already completed. */
   tu_wddm_allocation c = {};
   s.fake.script = {{kPending, 0, 0}};
   check(s.allocate(&c, 2) && c.resident && s.device.pending_paging_fence == 0, "zero paging fence is not pending");

   /* A failed paging wait fails the submission closed and stays pending. */
   tu_wddm_allocation d = {};
   s.fake.script = {{kPending, 20, 0}};
   s.fake.wait_status = kInvalid;
   const unsigned renders = s.fake.count("Render");
   check(s.allocate(&d, 3) && !s.render({&a, &b, &c, &d}) && s.fake.count("Render") == renders &&
         s.device.pending_paging_fence == 20, "failed paging-fence wait rejects Render without submitting");
   s.fake.wait_status = kSuccess;
   check(s.render({&a, &b, &c, &d}) && s.fake.unpaged_renders == 0, "paging-fence wait retries on the next submit");
   for (tu_wddm_allocation *allocation : {&a, &b, &c, &d})
      check(tu_wddm_allocation_destroy(allocation), "pending: destroy");
   check(tu_wddm_device_close(&s.device) && s.fake.count("Evict") == 4, "pending: teardown");
}

static void test_over_budget_is_bounded()
{
   session s(2000);
   check(s.open(), "budget: device opens");
   tu_wddm_allocation a = {};
   s.fake.script = {{kNoMemory, 0, 1 << 20}, {kSuccess, 0, 0}};
   check(s.allocate(&a, 0) && a.resident && s.fake.count("MakeResident") == 2 &&
         s.fake.make_resident_cant_trim == std::vector<bool>({false, true}) && a.residency_attempt_count == 2,
         "over budget without trimmable residency makes exactly one CantTrimFurther retry");

   tu_wddm_allocation b = {};
   s.fake.make_resident_cant_trim.clear();
   s.fake.script = {{kNoVideoMemory, 0, 4096}, {kNoMemory, 0, 4096}, {kSuccess, 0, 0}};
   const unsigned destroys = s.fake.count("DestroyAllocation2");
   check(!s.allocate(&b, 1) && b.handle == 0 && !b.resident &&
         b.last_create_status == static_cast<uint32_t>(kNoMemory) &&
         s.fake.make_resident_cant_trim == std::vector<bool>({false, true}) &&
         s.fake.count("DestroyAllocation2") == destroys + 1 && s.fake.count("Evict") == 0 &&
         s.fake.live.size() == 1,
         "over budget without trimmable residency makes exactly one CantTrimFurther retry");
   check(s.fake.script.size() == 1, "over budget stops after its final CantTrimFurther attempt");
   s.fake.script.clear();

   tu_wddm_allocation c = {};
   s.fake.script = {{kInvalid, 0, 0}};
   const unsigned attempts = s.fake.count("MakeResident");
   check(!s.allocate(&c, 2) && s.fake.count("MakeResident") == attempts + 1 &&
         c.last_create_status == static_cast<uint32_t>(kInvalid) && s.fake.live.size() == 1,
         "an unexpected MakeResident failure is not retried and releases the handle");

   tu_wddm_allocation d = {};
   s.fake.script = {{kTimeout, 0, 0}};
   check(!s.allocate(&d, 3) && s.fake.live.size() == 1 && s.fake.refs[0x103] == 0,
         "an undocumented success code fails closed");

   /* Compensating destroy failure keeps a non-resident owner; teardown needs no Evict. */
   tu_wddm_allocation e = {};
   s.fake.script = {{kInvalid, 0, 0}};
   s.fake.destroy_failures = 1;
   s.fake.destroy_status = kInvalid;
   const unsigned evicts = s.fake.count("Evict");
   check(!s.allocate(&e, 4) && e.handle != 0 && !e.resident, "failed rollback retains a non-resident owner");
   check(tu_wddm_allocation_destroy(&e) && s.fake.count("Evict") == evicts, "non-resident owner destroys without Evict");
   check(tu_wddm_allocation_destroy(&a) && tu_wddm_device_close(&s.device) && s.fake.live.empty(), "budget: teardown");
}

static void test_destroy_evicts_exactly_once()
{
   session s(2000);
   check(s.open(), "evict: device opens");
   tu_wddm_allocation a = {}, b = {};
   check(s.allocate(&a, 0) && s.allocate(&b, 1), "evict: allocations");

   /* A failed Evict (e.g. removed device) does not strand the owner:
    * destroying the allocation drops the reference Evict could not. */
   s.fake.evict_failures = 1;
   s.fake.evict_status = static_cast<NTSTATUS>(0xc00002b6u);
   check(tu_wddm_allocation_destroy(&a) && a.handle == 0 && s.fake.count("Evict:100") == 1 &&
         s.fake.count("DestroyAllocation2:100") == 1 && s.fake.destroyed_resident == 1 && s.fake.live.size() == 1,
         "failed Evict is attempted once and the allocation is still destroyed");
   s.fake.destroyed_resident = 0;

   /* Evict failure then destroy failure: the retry evicts again (the reference is still held). */
   tu_wddm_allocation c = {};
   check(s.allocate(&c, 2), "evict: third allocation");
   s.fake.evict_failures = 1;
   s.fake.evict_status = kInvalid;
   s.fake.destroy_failures = 1;
   s.fake.destroy_status = kInvalid;
   check(!tu_wddm_allocation_destroy(&c) && c.handle == 0x102 && c.resident, "destroy failure keeps the owner");
   check(tu_wddm_allocation_destroy(&c) && c.handle == 0 && s.fake.count("Evict:102") == 2 &&
         s.fake.evict_without_reference == 0 && s.fake.destroyed_resident == 0,
         "a retry after a failed Evict and destroy evicts the still-held reference once");

   s.fake.destroy_failures = 1;
   s.fake.destroy_status = kInvalid;
   check(!tu_wddm_allocation_destroy(&b) && b.handle == 0x101 && !b.resident, "destroy failure after Evict keeps the owner");
   check(tu_wddm_allocation_destroy(&b) && b.handle == 0, "retry after destroy failure succeeds");
   check(s.fake.count("Evict:101") == 1 && s.fake.evict_without_reference == 0 && s.fake.destroyed_resident == 0,
         "destroy evicts exactly once before DestroyAllocation2");
   check(tu_wddm_device_close(&s.device), "evict: teardown");
}

static void test_open_failures_fail_closed()
{
   {
      session s(2000);
      s.fake.version_status = kInvalid;
      check(!s.open() && s.fake.count("CreateDevice") == 0 && !s.fake.adapter_open,
            "unknown driver model rejects the adapter and closes it");
   }
   {
      session s(900);
      check(!s.open() && !s.fake.adapter_open, "an out-of-range driver model is rejected");
   }
   {
      session s(2000);
      s.runtime.dispatch.MakeResident = nullptr;
      check(!s.open() && s.fake.count("CreatePagingQueue") == 0 && s.fake.count("DestroyDevice") == 1 &&
            !s.fake.adapter_open, "WDDM 2.0 without residency thunks is not opened");
   }
   {
      session s(1200);
      s.runtime.dispatch.MakeResident = nullptr;
      s.runtime.dispatch.CreatePagingQueue = nullptr;
      check(s.open() && tu_wddm_device_close(&s.device), "WDDM 1.x does not need residency thunks");
   }
   {
      session s(2000);
      s.fake.create_queue_status = kInvalid;
      check(!s.open() && s.fake.count("DestroyDevice") == 1 && !s.fake.adapter_open,
            "paging queue failure releases the device");
   }
}

static uint64_t trim_budget;
static unsigned trim_calls;
static std::vector<bool> loop_flags;
static std::deque<tu_wddm_residency_outcome> loop_script;
static tu_wddm_residency_outcome loop_attempt(void *, bool cant, uint64_t *fence, uint64_t *trim)
{
   loop_flags.push_back(cant);
   *fence = 5;
   *trim = 4096;
   tu_wddm_residency_outcome next = TU_WDDM_RESIDENCY_OVER_BUDGET;
   if (!loop_script.empty()) {
      next = loop_script.front();
      loop_script.pop_front();
   }
   return next;
}
static uint64_t loop_trim(void *, uint64_t bytes)
{
   trim_calls++;
   if (trim_budget == 0)
      return 0;
   trim_budget -= 1;
   return bytes;
}

static void test_shared_trim_loop()
{
   check(tu_wddm_residency_classify_ntstatus(0) == TU_WDDM_RESIDENCY_RESIDENT &&
         tu_wddm_residency_classify_ntstatus(0x103) == TU_WDDM_RESIDENCY_PENDING &&
         tu_wddm_residency_classify_ntstatus(0xc0000017u) == TU_WDDM_RESIDENCY_OVER_BUDGET &&
         tu_wddm_residency_classify_ntstatus(0xc01e0100u) == TU_WDDM_RESIDENCY_OVER_BUDGET &&
         tu_wddm_residency_classify_ntstatus(0x102) == TU_WDDM_RESIDENCY_FAILED &&
         tu_wddm_residency_classify_hresult(0) == TU_WDDM_RESIDENCY_RESIDENT &&
         tu_wddm_residency_classify_hresult(0x8000000au) == TU_WDDM_RESIDENCY_PENDING &&
         tu_wddm_residency_classify_hresult(0x8007000eu) == TU_WDDM_RESIDENCY_OVER_BUDGET &&
         tu_wddm_residency_classify_hresult(1) == TU_WDDM_RESIDENCY_FAILED,
         "documented residency status classification");

   uint64_t fence = 0;
   uint32_t attempts = 0;
   /* Trimming that keeps making progress is still bounded, and the last
    * permitted call always carries CantTrimFurther. */
   loop_flags.clear(); loop_script.clear(); trim_calls = 0; trim_budget = 100;
   check(tu_wddm_make_resident_bounded(loop_attempt, loop_trim, nullptr, &fence, &attempts) ==
            TU_WDDM_RESIDENCY_OVER_BUDGET && attempts == TU_WDDM_RESIDENCY_MAX_ATTEMPTS &&
         loop_flags.size() == TU_WDDM_RESIDENCY_MAX_ATTEMPTS && loop_flags.back() &&
         std::count(loop_flags.begin(), loop_flags.end(), true) == 1 && trim_calls == 7,
         "out-of-memory trims and retries in a bounded loop");
   loop_flags.clear(); trim_calls = 0; trim_budget = 2;
   loop_script = {TU_WDDM_RESIDENCY_OVER_BUDGET, TU_WDDM_RESIDENCY_OVER_BUDGET, TU_WDDM_RESIDENCY_OVER_BUDGET,
                  TU_WDDM_RESIDENCY_RESIDENT};
   check(tu_wddm_make_resident_bounded(loop_attempt, loop_trim, nullptr, &fence, &attempts) ==
            TU_WDDM_RESIDENCY_RESIDENT && attempts == 4 && trim_calls == 3 &&
         loop_flags == std::vector<bool>({false, false, false, true}),
         "trim progress is followed by the final CantTrimFurther attempt");
   loop_flags.clear(); trim_calls = 0;
   loop_script = {TU_WDDM_RESIDENCY_PENDING};
   check(tu_wddm_make_resident_bounded(loop_attempt, loop_trim, nullptr, &fence, &attempts) ==
            TU_WDDM_RESIDENCY_PENDING && fence == 5 && attempts == 1 && trim_calls == 0,
         "pending returns its paging fence without trimming");
   loop_script = {TU_WDDM_RESIDENCY_FAILED};
   check(tu_wddm_make_resident_bounded(loop_attempt, loop_trim, nullptr, &fence, &attempts) ==
            TU_WDDM_RESIDENCY_FAILED && attempts == 1, "unexpected failure is not retried");
   check(tu_wddm_make_resident_bounded(nullptr, loop_trim, nullptr, &fence, &attempts) ==
            TU_WDDM_RESIDENCY_FAILED && attempts == 0, "missing attempt callback fails closed");
}

static void test_concurrent_paging_fence_bookkeeping()
{
   session s(2000);
   check(s.open(), "concurrency: device opens");
   std::vector<std::thread> threads;
   for (unsigned t = 0; t < 8; t++)
      threads.emplace_back([&, t]() {
         for (uint64_t i = 1; i <= 5000; i++)
            tu_wddm_device_note_paging_fence(&s.device, i * 8 + t);
      });
   for (std::thread &thread : threads)
      thread.join();
   check(s.device.pending_paging_fence == 5000 * 8 + 7, "concurrent paging fences keep the maximum");
   s.fake.cpu_fence = UINT64_MAX;
   check(tu_wddm_device_wait_paging_fence(&s.device) && s.device.pending_paging_fence == 0,
         "observed maximum clears the pending fence");
   tu_wddm_device_note_paging_fence(&s.device, 3);
   check(s.device.pending_paging_fence == 3, "a new pending fence is recorded after clearing");
   check(tu_wddm_device_close(&s.device), "concurrency: teardown");
}

int main()
{
   test_wddm1_issues_no_residency_calls();
   test_wddm2_residency_lifecycle();
   test_pending_waits_for_paging_fence();
   test_over_budget_is_bounded();
   test_destroy_evicts_exactly_once();
   test_open_failures_fail_closed();
   test_shared_trim_loop();
   test_concurrent_paging_fence_bookkeeping();
   printf("%s Turnip WDDM residency: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
   return failures ? 1 : 0;
}
