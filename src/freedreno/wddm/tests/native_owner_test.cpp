/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Execute the real native owner + transport against an explicit fake OS boundary.
 */
#include "freedreno_wddm_native_private.h"
#include "freedreno_wddm_packet.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <thread>

static unsigned checks;
/* Assertions remain enabled in release builds and stop at the first violation. */
static void check(bool value, const char *text, int line)
{
   checks++;
   if (!value) {
      std::fprintf(stderr, "FAIL native owner line %d: %s\n", line, text);
      std::exit(1);
   }
}
#define CHECK(x) check(!!(x), #x, __LINE__)
static constexpr NTSTATUS success = 0;
static constexpr NTSTATUS invalid = static_cast<NTSTATUS>(0xc000000dL);
static constexpr NTSTATUS busy = static_cast<NTSTATUS>(0xc01e0102L);
static constexpr uint64_t va_start = UINT64_C(0x100000000);
struct allocation_record {
   VIOGPU_WDDM_ALLOCATION_INFO info;
   std::vector<uint8_t> bytes;
   bool locked = false;
};
struct fixture {
   tu_wddm_runtime runtime = {};
   uint8_t commands[2][65536] = {};
   D3DDDI_ALLOCATIONLIST allocations[2][1024] = {};
   D3DDDI_PATCHLOCATIONLIST patches[2][1024] = {};
   std::map<D3DKMT_HANDLE, allocation_record> owners;
   uint32_t next_handle = 4, completed = 0, accepted = 0;
   unsigned render_calls = 0, destroy_calls = 0, context_destroy_calls = 0;
   unsigned expected_residents = 0, buffer_index = 0;
   bool fail_create = false, fail_render = false, malformed_replacement = false;
   bool destroy_busy = false, unlock_fail = false, reset = false;
   bool context_busy = false, immediate_complete = false;
};
static fixture *os;

/* Initialize only the ABI header; the caller zeros the whole wire structure. */
static void header(VIOGPU_WDDM_ABI_HEADER &h, uint32_t size)
{
   h.Magic = VIOGPU_WDDM_ABI_MAGIC;
   h.Version = VIOGPU_WDDM_ABI_VERSION;
   h.Size = size;
}
/* Verify that exact-LUID selection reaches the KMT adapter API. */
static NTSTATUS APIENTRY open_adapter(D3DKMT_OPENADAPTERFROMLUID *arg)
{
   CHECK(arg->AdapterLuid.LowPart == 42 && arg->AdapterLuid.HighPart == 7);
   arg->hAdapter = 1;
   return success;
}
/* Supply the actual private ABI, not a pretend DRM fd or GPU driver. */
static NTSTATUS APIENTRY query_adapter(const D3DKMT_QUERYADAPTERINFO *arg)
{
   CHECK(arg->hAdapter == 1 && arg->Type == KMTQAITYPE_UMDRIVERPRIVATE);
   CHECK(arg->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_ADAPTER_INFO));
   VIOGPU_WDDM_ADAPTER_INFO info = {};
   header(info.Header, sizeof(info));
   info.ResetGeneration = 9;
   info.MsmMajorVersion = 1;
   info.MsmMinorVersion = 9;
   info.GpuId = 630;
   info.ChipId = UINT64_C(0x06030001);
   info.GmemSize = 1024 * 1024;
   info.PriorityCount = 1;
   std::memcpy(arg->pPrivateDriverData, &info, sizeof(info));
   return success;
}
/* Supply real-shaped KMT command/allocation/patch storage. */
static NTSTATUS APIENTRY create_device(D3DKMT_CREATEDEVICE *arg)
{
   CHECK(arg->hAdapter == 1);
   arg->hDevice = 2;
   arg->pCommandBuffer = os->commands[0];
   arg->CommandBufferSize = sizeof(os->commands[0]);
   arg->pAllocationList = os->allocations[0];
   arg->AllocationListSize = 1024;
   arg->pPatchLocationList = os->patches[0];
   arg->PatchLocationListSize = 1024;
   return success;
}
/* Native clients must advertise OpenGL, not silently create Vulkan contexts. */
static NTSTATUS APIENTRY create_context(D3DKMT_CREATECONTEXT *arg)
{
   CHECK(arg->hDevice == 2);
   CHECK(arg->ClientHint == D3DKMT_CLIENTHINT_OPENGL);
   arg->hContext = 3;
   arg->pCommandBuffer = os->commands[0];
   arg->CommandBufferSize = sizeof(os->commands[0]);
   arg->pAllocationList = os->allocations[0];
   arg->AllocationListSize = 1024;
   arg->pPatchLocationList = os->patches[0];
   arg->PatchLocationListSize = 1024;
   return success;
}
/* Return independent context identity or independently controlled completion. */
static NTSTATUS APIENTRY escape(const D3DKMT_ESCAPE *arg)
{
   CHECK(arg->hAdapter == 1 && arg->hDevice == 2 && arg->hContext == 3);
   if (arg->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_CONTEXT_INFO)) {
      auto *info = static_cast<VIOGPU_WDDM_CONTEXT_INFO *>(arg->pPrivateDriverData);
      CHECK(info->ExpectedResetGeneration == 9);
      info->VaStart = va_start;
      info->VaSize = UINT64_C(4) * 1024 * 1024;
      info->ResetGeneration = 9;
      info->ContextId = 11;
      info->SubmitQueueId = 17;
   } else {
      CHECK(arg->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_FENCE_INFO));
      auto *info = static_cast<VIOGPU_WDDM_FENCE_INFO *>(arg->pPrivateDriverData);
      CHECK(info->ExpectedResetGeneration == 9);
      info->ResetGeneration = os->reset ? 10 : 9;
      info->ContextId = 11;
      info->CompletedFence = os->completed;
   }
   return success;
}
/* Fail execution checks independently from a fence that appears complete. */
static NTSTATUS APIENTRY device_state(D3DKMT_GETDEVICESTATE *arg)
{
   CHECK(arg->hDevice == 2);
   arg->ExecutionState = os->reset ? D3DKMT_DEVICEEXECUTION_RESET : D3DKMT_DEVICEEXECUTION_ACTIVE;
   return success;
}
/* Enforce address exclusivity even for failed creates that returned a handle. */
static NTSTATUS APIENTRY create_allocation(D3DKMT_CREATEALLOCATION *arg)
{
   CHECK(arg->hDevice == 2 && arg->NumAllocations == 1);
   auto *in = static_cast<VIOGPU_WDDM_ALLOCATION_INFO *>(arg->pAllocationInfo->pPrivateDriverData);
   CHECK(in->ExpectedResetGeneration == 9 && in->ContextId == 11);
   CHECK(in->RequestedIova >= va_start && in->Size != 0);
   for (const auto &item : os->owners) {
      const auto &old = item.second.info;
      CHECK(in->RequestedIova + in->Size <= old.RequestedIova ||
            old.RequestedIova + old.Size <= in->RequestedIova);
   }
   auto &record = os->owners[os->next_handle];
   record.info = *in;
   record.bytes.resize(static_cast<size_t>(in->Size));
   arg->pAllocationInfo->hAllocation = os->next_handle++;
   return os->fail_create ? invalid : success;
}
/* A completed private fence does not waive VidMm's independent busy check. */
static NTSTATUS APIENTRY destroy_allocation(const D3DKMT_DESTROYALLOCATION2 *arg)
{
   CHECK(arg->hDevice == 2 && arg->AllocationCount == 1);
   CHECK(!arg->Flags.AssumeNotInUse);
   os->destroy_calls++;
   if (os->destroy_busy)
      return busy;
   auto it = os->owners.find(*arg->phAllocationList);
   CHECK(it != os->owners.end() && !it->second.locked);
   os->owners.erase(it);
   return success;
}
/* Model a stable CPU map, including for GPU-readonly IB allocations. */
static NTSTATUS APIENTRY lock_allocation(D3DKMT_LOCK *arg)
{
   auto it = os->owners.find(arg->hAllocation);
   CHECK(it != os->owners.end() && !it->second.locked);
   CHECK(!arg->Flags.ReadOnly);
   it->second.locked = true;
   arg->pData = it->second.bytes.data();
   return success;
}
/* Failed unlock must leave the map, handle, and GPU address owned. */
static NTSTATUS APIENTRY unlock_allocation(const D3DKMT_UNLOCK *arg)
{
   CHECK(arg->NumAllocations == 1);
   auto it = os->owners.find(*arg->phAllocations);
   CHECK(it != os->owners.end() && it->second.locked);
   if (os->unlock_fail)
      return invalid;
   it->second.locked = false;
   return success;
}
/* Decode the real outer/inner packet and consume KMT's replacement buffers. */
static NTSTATUS APIENTRY render(D3DKMT_RENDER *arg)
{
   CHECK(arg->hContext == 3 && arg->CommandLength <= 65536);
   auto *base = static_cast<uint8_t *>(arg->pNewCommandBuffer);
   CHECK(base == os->commands[os->buffer_index]);
   auto *outer = reinterpret_cast<VIOGPU_WDDM_RENDER_COMMAND *>(base);
   CHECK(outer->ExpectedResetGeneration == 9);
   auto *req = reinterpret_cast<tu_wddm_msm_submit_request *>(base + outer->CommandStreamOffset);
   CHECK(req->queue_id == 17 && req->fence != 0);
   CHECK(req->bo_count == arg->AllocationCount);
   if (os->expected_residents)
      CHECK(req->bo_count == os->expected_residents);
   auto *bos = reinterpret_cast<tu_wddm_msm_submit_bo *>(req + 1);
   auto *cmds = reinterpret_cast<tu_wddm_msm_submit_command *>(bos + req->bo_count);
   for (uint32_t i = 0; i < req->bo_count; i++) {
      CHECK(bos[i].handle == 0 && bos[i].presumed == 0);
      CHECK(os->owners.count(os->allocations[os->buffer_index][i].hAllocation) == 1);
   }
   for (uint32_t i = 0; i < req->command_count; i++)
      CHECK(cmds[i].submit_index < req->bo_count && cmds[i].iova == 0);
   os->render_calls++;
   if (!os->fail_render) {
      os->accepted = req->fence;
      if (os->immediate_complete)
         os->completed = req->fence;
   }
   os->buffer_index ^= 1;
   arg->pNewCommandBuffer = os->malformed_replacement ? NULL : os->commands[os->buffer_index];
   arg->pNewAllocationList = os->allocations[os->buffer_index];
   arg->pNewPatchLocationList = os->patches[os->buffer_index];
   return os->fail_render ? invalid : success;
}
/* Refuse parent teardown before every allocation's authoritative destroy. */
static NTSTATUS APIENTRY destroy_context(const D3DKMT_DESTROYCONTEXT *arg)
{
   CHECK(arg->hContext == 3 && os->owners.empty());
   os->context_destroy_calls++;
   return os->context_busy ? invalid : success;
}
/* Parent teardown is reached only after the child context is released. */
static NTSTATUS APIENTRY destroy_device(const D3DKMT_DESTROYDEVICE *arg)
{
   CHECK(arg->hDevice == 2 && os->context_destroy_calls > 0);
   return success;
}
/* Close the sole fake adapter. */
static NTSTATUS APIENTRY close_adapter(const D3DKMT_CLOSEADAPTER *arg)
{
   CHECK(arg->hAdapter == 1);
   return success;
}
/* Build the borrowed OS boundary without loading a real GPU/runtime. */
static fd_wddm_native_device *open_fixture(fixture &f)
{
   os = &f;
   auto &d = f.runtime.dispatch;
   d.OpenAdapterFromLuid = open_adapter; d.QueryAdapterInfo = query_adapter;
   d.CreateDevice = create_device; d.CreateContext = create_context;
   d.Escape = escape; d.GetDeviceState = device_state;
   d.CreateAllocation = create_allocation; d.DestroyAllocation2 = destroy_allocation;
   d.Lock = lock_allocation; d.Unlock = unlock_allocation; d.Render = render;
   d.DestroyContext = destroy_context; d.DestroyDevice = destroy_device;
   d.CloseAdapter = close_adapter;
   fd_wddm_luid luid = {42, 7};
   auto *dev = fd_wddm_native_open_with_runtime(&f.runtime, &luid);
   CHECK(dev != NULL);
   return dev;
}
/* Exercise all-live residency, stable mapping, pending waits, busy and VA reuse. */
static void test_owner()
{
   fixture f; auto *dev = open_fixture(f);
   fd_wddm_native_info info = {};
   CHECK(fd_wddm_native_get_info(dev, &info) && info.gpu_id == 630);
   auto *ib = fd_wddm_native_bo_create(dev, 4096, FD_WDDM_NATIVE_CPU_VISIBLE | FD_WDDM_NATIVE_GPU_READONLY);
   auto *unreferenced = fd_wddm_native_bo_create(dev, 4096, 0);
   CHECK(ib && unreferenced && fd_wddm_native_bo_iova(ib) == va_start);
   CHECK(fd_wddm_native_bo_map(ib) != NULL);
   CHECK(fd_wddm_native_bo_map(ib) == fd_wddm_native_bo_map(ib));
   CHECK(!fd_wddm_native_close(&dev));
   fd_wddm_native_command cmd = {ib, 0, 4};
   uint64_t serial = 99;
   f.expected_residents = 2;
   CHECK(fd_wddm_native_submit(dev, &cmd, 1, &serial) == FD_WDDM_NATIVE_OK && serial == 1);
   CHECK(fd_wddm_native_wait(dev, serial, 0) == FD_WDDM_NATIVE_PENDING);
   CHECK(fd_wddm_native_bo_wait(ib, 0) == FD_WDDM_NATIVE_PENDING);
   CHECK(fd_wddm_native_wait(dev, 2, 0) == FD_WDDM_NATIVE_INVALID);
   const uint64_t old = fd_wddm_native_bo_iova(ib);
   fd_wddm_native_bo_release(&ib);
   CHECK(!ib && f.destroy_calls == 0);
   f.completed = 1; f.destroy_busy = true;
   auto *third = fd_wddm_native_bo_create(dev, 4096, 0);
   CHECK(third && fd_wddm_native_bo_iova(third) != old && f.destroy_calls > 0);
   f.destroy_busy = false;
   auto *reused = fd_wddm_native_bo_create(dev, 4096, 0);
   CHECK(reused && fd_wddm_native_bo_iova(reused) == old);
   fd_wddm_native_bo_release(&unreferenced);
   fd_wddm_native_bo_release(&third);
   fd_wddm_native_bo_release(&reused);
   CHECK(fd_wddm_native_close(&dev) && dev == NULL && f.owners.empty());
}
/* Partial-create and failed unlock retain their exact address until a later success. */
static void test_rollback()
{
   fixture f; auto *dev = open_fixture(f);
   f.fail_create = true; f.destroy_busy = true;
   CHECK(!fd_wddm_native_bo_create(dev, 4096, 0));
   f.fail_create = false;
   auto *bo = fd_wddm_native_bo_create(dev, 4096, FD_WDDM_NATIVE_CPU_VISIBLE);
   CHECK(bo && fd_wddm_native_bo_iova(bo) == va_start + 4096);
   CHECK(fd_wddm_native_bo_map(bo));
   f.unlock_fail = true;
   fd_wddm_native_bo_release(&bo);
   CHECK(!fd_wddm_native_close(&dev) && dev && f.owners.size() == 2);
   CHECK(f.context_destroy_calls == 0);
   f.destroy_busy = false; f.unlock_fail = false;
   f.context_busy = true;
   CHECK(!fd_wddm_native_close(&dev) && dev && f.owners.empty());
   fd_wddm_native_quarantine(&dev);
   CHECK(!dev && !fd_wddm_native_reap_quarantine());
   f.context_busy = false;
   CHECK(fd_wddm_native_reap_quarantine());
}
/* Reject malformed input before Render, and record even a malformed successful transfer. */
static void test_failures()
{
   for (int kind = 0; kind != 4; kind++) {
      fixture f; auto *dev = open_fixture(f);
      auto *bo = fd_wddm_native_bo_create(dev, 4096, 0);
      CHECK(bo);
      fd_wddm_native_command cmd = {bo, 4092, 8}; uint64_t serial = 99;
      CHECK(fd_wddm_native_submit(dev, &cmd, 1, &serial) == FD_WDDM_NATIVE_INVALID);
      CHECK(serial == 0 && f.render_calls == 0);
      cmd.size = 4;
      if (kind == 0) f.fail_render = true;
      if (kind == 1) f.malformed_replacement = true;
      if (kind == 2) f.reset = true;
      if (kind == 3) f.completed = 1; /* Impossible future completion. */
      CHECK(fd_wddm_native_submit(dev, &cmd, 1, &serial) == FD_WDDM_NATIVE_LOST);
      CHECK(serial == (kind == 1 ? 1u : 0u));
      fd_wddm_native_info info = {};
      CHECK(!fd_wddm_native_get_info(dev, &info));
      fd_wddm_native_bo_release(&bo);
      CHECK(fd_wddm_native_close(&dev) && f.owners.empty());
   }
}
/* Concurrent clients share one serialized KMT context, not cross-context BO aliases. */
static void test_concurrent()
{
   fixture f; auto *dev = open_fixture(f); f.immediate_complete = true;
   auto job = [dev]() {
      for (unsigned i = 0; i < 200; i++) {
         auto *bo = fd_wddm_native_bo_create(dev, 4096, 0);
         /* Other assertions run inside the core lock; do not race the test counter. */
         if (!bo) std::abort();
         fd_wddm_native_command cmd = {bo, 0, 4}; uint64_t serial;
         if (fd_wddm_native_submit(dev, &cmd, 1, &serial) != FD_WDDM_NATIVE_OK)
            std::abort();
         fd_wddm_native_bo_release(&bo);
      }
   };
   std::thread a(job), b(job), c(job), d(job);
   a.join(); b.join(); c.join(); d.join();
   CHECK(f.render_calls == 800 && f.accepted == 800);
   CHECK(fd_wddm_native_close(&dev) && f.owners.empty());
}
/* Deliberately do not load a DLL: injected dispatch is the only test OS boundary. */
extern "C" bool tu_wddm_dispatch_init(tu_wddm_dispatch *) { return false; }
/* Borrowed fake dispatch owns no module. */
extern "C" void tu_wddm_dispatch_finish(tu_wddm_dispatch *) {}
/* Execute the real owner and real packet/transport code; never claim GPU execution. */
int main()
{
   test_owner(); test_rollback(); test_failures(); test_concurrent();
   std::printf("PASS native owner: %u assertions, 800 concurrent submissions; fake OS, no GPU\n", checks);
   return 0;
}
