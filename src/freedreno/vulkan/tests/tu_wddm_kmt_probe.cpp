/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Direct ARM64 D3DKMT bring-up probe for the DroidVM Turnip private endpoint.
 * It does not load Vulkan or a D3D UMD.  It exercises the bounded KMT
 * allocation lock/unlock lifecycle after Context/VA bring-up. With the
 * explicit --submit-nop argument it additionally submits one CP_NOP through
 * the Native Context path and waits for the private fence endpoint. Every
 * acquired KMT handle is closed before exit, including adapters rejected by
 * the private ABI query. The explicit --stress-lifecycle argument repeats the
 * allocation lock/unlock/destroy cycle 10,000 times at one requested IOVA to
 * expose retained KMT owners and requested-IOVA leaks without making the
 * default bring-up probe invasive.
 */

#include "../tu_knl_wddm.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

enum : uint32_t {
   kMsmCcmdGemSubmit = 7,
   kMsmPipe3d0 = 0x10,
   kMsmSubmitNoImplicit = 0x80000000U,
   kMsmSubmitBoRead = 0x0001,
   kMsmSubmitBoWrite = 0x0002,
   kMsmSubmitBoNoImplicit = 0x0008,
   kMsmSubmitCmdBuf = 0x0001,
};

#pragma pack(push, 1)
struct test_msm_submit_request {
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

struct test_msm_submit_bo {
   uint32_t flags;
   uint32_t handle;
   uint64_t presumed;
};

struct test_msm_submit_command {
   uint32_t type;
   uint32_t submit_index;
   uint32_t submit_offset;
   uint32_t size;
   uint32_t padding;
   uint32_t relocation_count;
   uint64_t iova;
};

struct test_msm_submit_one_bo {
   test_msm_submit_request request;
   test_msm_submit_bo bo;
   test_msm_submit_command command;
};
#pragma pack(pop)

static_assert(sizeof(test_msm_submit_request) == 36, "MSM request fixture drift");
static_assert(sizeof(test_msm_submit_bo) == 16, "MSM BO fixture drift");
static_assert(sizeof(test_msm_submit_command) == 32, "MSM command fixture drift");
static_assert(sizeof(test_msm_submit_one_bo) == 84, "MSM submit fixture drift");

/* pm4_pkt7_hdr(CP_NOP, 0): a one-dword packet with no payload.  Keep this
 * probe independent of generated register headers while retaining the exact
 * opcode- and parity-bit encoding used by Turnip. */
constexpr uint32_t
pm4_odd_parity_bit_constexpr(uint32_t value)
{
   value ^= value >> 16;
   value ^= value >> 8;
   value ^= value >> 4;
   value &= 0xfU;
   return (~0x6996U >> value) & 1U;
}

constexpr uint32_t
pm4_pkt7_hdr_constexpr(uint8_t opcode, uint16_t count)
{
   return 0x70000000U | static_cast<uint32_t>(count) |
          (pm4_odd_parity_bit_constexpr(count) << 15) |
          ((static_cast<uint32_t>(opcode) & 0x7fU) << 16) |
          (pm4_odd_parity_bit_constexpr(opcode) << 23);
}

constexpr uint32_t kCpNop = pm4_pkt7_hdr_constexpr(0x10U, 0U);
static_assert(kCpNop == 0x70108000U, "CP_NOP packet encoding drift");
static_assert(((kCpNop >> 16) & 0x7fU) == 0x10U, "CP_NOP opcode drift");
static_assert(((kCpNop >> 15) & 1U) == pm4_odd_parity_bit_constexpr(0U),
              "CP_NOP count parity drift");
static_assert(((kCpNop >> 23) & 1U) == pm4_odd_parity_bit_constexpr(0x10U),
              "CP_NOP opcode parity drift");

constexpr uint32_t kLifecycleIterations = 10000;

enum probe_stage {
   kProbeStageEnumeration,
   kProbeStageOpenAdapter,
   kProbeStageCreateDevice,
   kProbeStageCreateContext,
   kProbeStageAllocation,
};

const char *
probe_stage_name(probe_stage stage)
{
   switch (stage) {
   case kProbeStageEnumeration:
      return "enumeration";
   case kProbeStageOpenAdapter:
      return "open";
   case kProbeStageCreateDevice:
      return "device";
   case kProbeStageCreateContext:
      return "context";
   case kProbeStageAllocation:
      return "allocation";
   }

   return "unknown";
}

bool
parse_probe_stage(const char *argument, probe_stage *stage)
{
   static const char prefix[] = "--stage=";
   if (argument == nullptr || stage == nullptr || strncmp(argument, prefix, sizeof(prefix) - 1) != 0)
      return false;

   const char *value = argument + sizeof(prefix) - 1;
   if (strcmp(value, "enumeration") == 0)
      *stage = kProbeStageEnumeration;
   else if (strcmp(value, "open") == 0)
      *stage = kProbeStageOpenAdapter;
   else if (strcmp(value, "device") == 0)
      *stage = kProbeStageCreateDevice;
   else if (strcmp(value, "context") == 0)
      *stage = kProbeStageCreateContext;
   else if (strcmp(value, "allocation") == 0)
      *stage = kProbeStageAllocation;
   else
      return false;

   return true;
}

bool
fence_reached(uint32_t completed, uint32_t target)
{
   return completed == target || static_cast<int32_t>(completed - target) > 0;
}

void
print_status(const char *operation, NTSTATUS status)
{
   printf("%s: status=0x%08lx\n", operation, static_cast<unsigned long>(status));
}

void
print_adapter_info(const VIOGPU_WDDM_ADAPTER_INFO *info)
{
   printf("    header: magic=0x%08x version=%u size=%u reserved=0x%08x\n", info->Header.Magic, info->Header.Version,
          info->Header.Size, info->Header.Reserved);
   printf("    capabilities=0x%016llx reset=%llu\n", static_cast<unsigned long long>(info->Capabilities),
          static_cast<unsigned long long>(info->ResetGeneration));
   printf("    msm=%u.%u.%u gpu=%u chip=0x%016llx\n", info->MsmMajorVersion, info->MsmMinorVersion,
          info->MsmPatchVersion, info->GpuId, static_cast<unsigned long long>(info->ChipId));
   printf("    gmem: size=%u base=0x%016llx highest-bank-bit=%u "
          "priorities=%u\n",
          info->GmemSize, static_cast<unsigned long long>(info->GmemBase), info->HighestBankBit, info->PriorityCount);
   printf("    coherent=%u ubwc=0x%016llx macrotile=0x%016llx\n", info->HasCachedCoherentMemory,
          static_cast<unsigned long long>(info->UbwcSwizzle), static_cast<unsigned long long>(info->MacrotileMode));
   printf("    uche-trap=0x%016llx ray-tracing=%u max-frequency=%u "
          "reserved=[0x%016llx,0x%016llx]\n",
          static_cast<unsigned long long>(info->UcheTrapBase), info->HasRayTracing, info->MaxFrequency,
          static_cast<unsigned long long>(info->Reserved[0]), static_cast<unsigned long long>(info->Reserved[1]));
}

NTSTATUS
query_private_info(tu_wddm_dispatch *dispatch, D3DKMT_HANDLE adapter, VIOGPU_WDDM_ADAPTER_INFO *info)
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
run_submit_nop_probe(tu_wddm_dispatch *dispatch,
                     const D3DKMT_ADAPTERINFO *enumerated,
                     D3DKMT_HANDLE adapter_handle,
                     D3DKMT_HANDLE device_handle,
                     D3DKMT_HANDLE context_handle,
                     const D3DKMT_CREATECONTEXT *context_create,
                     const VIOGPU_WDDM_ADAPTER_INFO *adapter_info,
                     const VIOGPU_WDDM_CONTEXT_INFO *context_info)
{
   if (dispatch == nullptr || enumerated == nullptr || adapter_info == nullptr || context_info == nullptr ||
       context_create == nullptr || adapter_handle == 0 || device_handle == 0 || context_handle == 0 ||
       context_create->pCommandBuffer == nullptr || context_create->pAllocationList == nullptr ||
       context_create->pPatchLocationList == nullptr)
      return false;

   tu_wddm_runtime runtime = {};
   runtime.dispatch = *dispatch;
   tu_wddm_device device = {};
   device.adapter.runtime = &runtime;
   device.adapter.luid = enumerated->AdapterLuid;
   device.adapter.handle = adapter_handle;
   device.adapter.private_info = *adapter_info;
   device.handle = device_handle;

   tu_wddm_context context = {};
   context.device = &device;
   context.handle = context_handle;
   context.command_buffer = context_create->pCommandBuffer;
   context.command_buffer_size = context_create->CommandBufferSize;
   context.allocation_list = context_create->pAllocationList;
   context.allocation_list_size = context_create->AllocationListSize;
   context.patch_location_list = context_create->pPatchLocationList;
   context.patch_location_list_size = context_create->PatchLocationListSize;
   context.info = *context_info;

   tu_wddm_allocation_desc desc = {};
   desc.size = 4096;
   desc.alignment = 4096;
   desc.requested_iova = context_info->VaStart;
   desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;

   tu_wddm_allocation allocation = {};
   printf("  Submit probe CreateAllocation: begin\n");
   bool created = tu_wddm_allocation_create(&context, &desc, &allocation);
   printf("  Submit probe CreateAllocation: success=%u handle=0x%08x\n", static_cast<unsigned>(created),
          allocation.handle);
   if (!created && allocation.handle == 0)
      return false;

   bool prepared = false;
   void *map = nullptr;
   if (created) {
      printf("  Submit probe Lock command BO: begin\n");
   }
   if (created && tu_wddm_allocation_lock(&allocation, &map) && map != nullptr) {
      memset(map, 0, 4096);
      memcpy(map, &kCpNop, sizeof(kCpNop));
      prepared = tu_wddm_allocation_unlock(&allocation);
      if (!prepared)
         prepared = tu_wddm_allocation_unlock(&allocation);
   }
   printf("  Submit probe command BO prepared=%u\n", static_cast<unsigned>(prepared));

   bool submitted = false;
   bool completed = false;
   if (prepared) {
      test_msm_submit_one_bo submit = {};
      submit.request.command = kMsmCcmdGemSubmit;
      submit.request.length = sizeof(submit);
      submit.request.sequence = 1;
      submit.request.flags = kMsmPipe3d0 | kMsmSubmitNoImplicit;
      submit.request.queue_id = context_info->SubmitQueueId;
      submit.request.bo_count = 1;
      submit.request.command_count = 1;
      submit.request.fence = 1;
      submit.bo.flags = kMsmSubmitBoRead | kMsmSubmitBoWrite | kMsmSubmitBoNoImplicit;
      submit.command.type = kMsmSubmitCmdBuf;
      submit.command.submit_index = 0;
      submit.command.submit_offset = 0;
      submit.command.size = sizeof(kCpNop);

      tu_wddm_render_reference reference = {};
      reference.allocation = &allocation;
      reference.flags = VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE;
      reference.allocation_offset = 0;
      reference.length = 4096;
      reference.patch_offset =
         static_cast<uint32_t>(offsetof(test_msm_submit_one_bo, bo) + offsetof(test_msm_submit_bo, presumed));

      printf("  Submit probe Render(NOP): begin\n");
      submitted = tu_wddm_context_render(&context, &submit, sizeof(submit), &reference, 1);
      printf("  Submit probe Render(NOP): success=%u fence=%u\n", static_cast<unsigned>(submitted),
             context.last_submitted_fence);
      if (submitted) {
         completed = tu_wddm_context_wait_fence(&context, submit.request.fence,
                                                UINT64_C(5) * UINT64_C(1000) * UINT64_C(1000) * UINT64_C(1000));
         uint32_t completed_fence = 0;
         bool queried = tu_wddm_context_get_completed_fence(&context, &completed_fence);
         printf("  Submit probe fence: completed=%u query=%u value=%u\n", static_cast<unsigned>(completed),
                static_cast<unsigned>(queried), completed_fence);
         completed = completed && queried && fence_reached(completed_fence, submit.request.fence);
      }
   }

   bool cleaned = true;
   if (allocation.handle != 0 && allocation.locked)
      cleaned = tu_wddm_allocation_unlock(&allocation);
   if (allocation.handle != 0 && !allocation.locked) {
      bool destroyed = tu_wddm_allocation_destroy(&allocation);
      if (!destroyed)
         destroyed = tu_wddm_allocation_destroy(&allocation);
      cleaned = cleaned && destroyed;
   } else if (allocation.handle != 0) {
      cleaned = false;
   }
   printf("  Submit probe allocation cleanup=%u\n", static_cast<unsigned>(cleaned));
   return prepared && submitted && completed && cleaned && allocation.handle == 0;
}

bool
run_allocation_lifecycle_probe(tu_wddm_context *context)
{
   if (context == nullptr || context->info.VaStart == 0 || context->info.VaSize < 4096)
      return false;

   tu_wddm_allocation_desc desc = {};
   desc.size = 4096;
   desc.alignment = 4096;
   desc.requested_iova = context->info.VaStart;
   desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;

   printf("  Stress lifecycle: begin iterations=%u iova=0x%llx\n", kLifecycleIterations,
          static_cast<unsigned long long>(desc.requested_iova));
   for (uint32_t iteration = 0; iteration < kLifecycleIterations; iteration++) {
      tu_wddm_allocation allocation = {};
      if (!tu_wddm_allocation_create(context, &desc, &allocation)) {
         printf("  Stress lifecycle: CreateAllocation failed iteration=%u status=0x%08x handle=0x%08x\n", iteration,
                allocation.last_create_status, allocation.handle);
         if (allocation.handle != 0)
            (void) tu_wddm_allocation_destroy(&allocation);
         return false;
      }

      void *map = nullptr;
      bool locked = tu_wddm_allocation_lock(&allocation, &map);
      bool unlocked = false;
      if (locked && map != nullptr) {
         static_cast<unsigned char *>(map)[0] = static_cast<unsigned char>(iteration);
         unlocked = tu_wddm_allocation_unlock(&allocation);
         if (!unlocked)
            unlocked = tu_wddm_allocation_unlock(&allocation);
      }

      bool destroyed = true;
      if (allocation.handle != 0 && allocation.locked)
         destroyed = tu_wddm_allocation_unlock(&allocation);
      if (allocation.handle != 0 && !allocation.locked) {
         destroyed = tu_wddm_allocation_destroy(&allocation);
         if (!destroyed)
            destroyed = tu_wddm_allocation_destroy(&allocation);
      } else if (allocation.handle != 0) {
         destroyed = false;
      }

      if (!locked || map == nullptr || !unlocked || !destroyed || allocation.handle != 0) {
         printf("  Stress lifecycle: failed iteration=%u lock=%u unlock=%u destroy=%u destroy_status=0x%08x handle=0x%08x\n", iteration,
                static_cast<unsigned>(locked), static_cast<unsigned>(unlocked), static_cast<unsigned>(destroyed),
                allocation.last_destroy_status, allocation.handle);
         return false;
      }

      if ((iteration + 1) % 1000 == 0)
         printf("  Stress lifecycle: completed=%u\n", iteration + 1);
   }

   printf("  Stress lifecycle: passed iterations=%u\n", kLifecycleIterations);
   return true;
}

bool
run_context_lifecycle_probe(tu_wddm_dispatch *dispatch,
                            D3DKMT_HANDLE adapter_handle,
                            D3DKMT_HANDLE device_handle,
                            const VIOGPU_WDDM_ADAPTER_INFO *adapter_info)
{
   if (dispatch == nullptr || adapter_info == nullptr || adapter_handle == 0 || device_handle == 0)
      return false;

   printf("  Stress context lifecycle: begin iterations=%u\n", kLifecycleIterations);
   for (uint32_t iteration = 0; iteration < kLifecycleIterations; iteration++) {
      VIOGPU_WDDM_CONTEXT_CREATE private_data = {};
      private_data.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      private_data.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      private_data.Header.Size = static_cast<uint32_t>(sizeof(private_data));
      private_data.ExpectedResetGeneration = adapter_info->ResetGeneration;
      private_data.Flags = VIOGPU_WDDM_CONTEXT_FLAGS_NONE;

      D3DKMT_CREATECONTEXT create = {};
      create.hDevice = device_handle;
      create.NodeOrdinal = 0;
      create.EngineAffinity = 1;
      create.pPrivateDriverData = &private_data;
      create.PrivateDriverDataSize = static_cast<UINT>(sizeof(private_data));
      create.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
      NTSTATUS status = dispatch->CreateContext(&create);
      if (!NT_SUCCESS(status) || create.hContext == 0) {
         printf("  Stress context lifecycle: CreateContext failed iteration=%u status=0x%08lx handle=0x%08x\n",
                iteration, static_cast<unsigned long>(status), create.hContext);
         return false;
      }

      VIOGPU_WDDM_CONTEXT_INFO context_info = {};
      context_info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      context_info.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      context_info.Header.Size = static_cast<uint32_t>(sizeof(context_info));
      context_info.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
      context_info.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
      context_info.ExpectedResetGeneration = adapter_info->ResetGeneration;

      D3DKMT_ESCAPE escape = {};
      escape.hAdapter = adapter_handle;
      escape.hDevice = device_handle;
      escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
      escape.pPrivateDriverData = &context_info;
      escape.PrivateDriverDataSize = static_cast<UINT>(sizeof(context_info));
      escape.hContext = create.hContext;
      status = dispatch->Escape(&escape);
      const bool queried =
         NT_SUCCESS(status) && tu_wddm_validate_context_info(&context_info, adapter_info->ResetGeneration);

      D3DKMT_DESTROYCONTEXT destroy = {};
      destroy.hContext = create.hContext;
      NTSTATUS destroy_status = dispatch->DestroyContext(&destroy);
      if (!NT_SUCCESS(destroy_status)) {
         destroy_status = dispatch->DestroyContext(&destroy);
      }

      if (!queried || !NT_SUCCESS(destroy_status)) {
         printf("  Stress context lifecycle: failed iteration=%u query=%u query_status=0x%08lx "
                "destroy_status=0x%08lx\n",
                iteration, static_cast<unsigned>(queried), static_cast<unsigned long>(status),
                static_cast<unsigned long>(destroy_status));
         return false;
      }

      if ((iteration + 1) % 1000 == 0)
         printf("  Stress context lifecycle: completed=%u\n", iteration + 1);
   }

   printf("  Stress context lifecycle: passed iterations=%u\n", kLifecycleIterations);
   return true;
}

bool
probe_adapter(tu_wddm_dispatch *dispatch,
              const D3DKMT_ADAPTERINFO *enumerated,
              probe_stage stage,
              bool submit_nop,
              bool stress_lifecycle)
{
   VIOGPU_WDDM_ADAPTER_INFO enumerated_info = {};
   printf("  QueryAdapterInfo(enum): begin\n");
   NTSTATUS status = query_private_info(dispatch, enumerated->hAdapter, &enumerated_info);
   printf("  QueryAdapterInfo(enum): status=0x%08lx valid=%u\n", static_cast<unsigned long>(status),
          static_cast<unsigned>(NT_SUCCESS(status) && tu_wddm_validate_adapter_info(&enumerated_info)));
   print_adapter_info(&enumerated_info);
   if (!NT_SUCCESS(status) || !tu_wddm_validate_adapter_info(&enumerated_info))
      return false;

   D3DKMT_OPENADAPTERFROMLUID open = {};
   open.AdapterLuid = enumerated->AdapterLuid;
   printf("  OpenAdapterFromLuid: begin\n");
   status = dispatch->OpenAdapterFromLuid(&open);
   printf("  OpenAdapterFromLuid: status=0x%08lx handle=0x%08x\n", static_cast<unsigned long>(status), open.hAdapter);
   if (!NT_SUCCESS(status) || open.hAdapter == 0)
      return false;

   bool ready = false;
   D3DKMT_HANDLE opened_adapter = open.hAdapter;
   D3DKMT_HANDLE device_handle = 0;
   D3DKMT_HANDLE context_handle = 0;
   D3DKMT_CREATECONTEXT context_create = {};
   VIOGPU_WDDM_CONTEXT_INFO context_info = {};

   VIOGPU_WDDM_ADAPTER_INFO opened_info = {};
   printf("  QueryAdapterInfo(open): begin\n");
   status = query_private_info(dispatch, opened_adapter, &opened_info);
   printf(
      "  QueryAdapterInfo(open): status=0x%08lx valid=%u exact=%u\n", static_cast<unsigned long>(status),
      static_cast<unsigned>(NT_SUCCESS(status) && tu_wddm_validate_adapter_info(&opened_info)),
      static_cast<unsigned>(NT_SUCCESS(status) && memcmp(&opened_info, &enumerated_info, sizeof(opened_info)) == 0));
   print_adapter_info(&opened_info);
   if (!NT_SUCCESS(status) || !tu_wddm_validate_adapter_info(&opened_info) ||
       memcmp(&opened_info, &enumerated_info, sizeof(opened_info)) != 0)
      goto cleanup;
   if (stage == kProbeStageOpenAdapter) {
      ready = true;
      goto cleanup;
   }

   {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = opened_adapter;
      printf("  CreateDevice: begin\n");
      status = dispatch->CreateDevice(&create);
      printf("  CreateDevice: status=0x%08lx handle=0x%08x command=%u "
             "allocations=%u patches=%u\n",
             static_cast<unsigned long>(status), create.hDevice, create.CommandBufferSize, create.AllocationListSize,
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
      printf("  GetDeviceState: status=0x%08lx execution=%u\n", static_cast<unsigned long>(status),
             static_cast<unsigned>(state.ExecutionState));
      if (!NT_SUCCESS(status) || state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE)
         goto cleanup;
   }
   if (stage == kProbeStageCreateDevice) {
      ready = true;
      goto cleanup;
   }

   {
      VIOGPU_WDDM_CONTEXT_CREATE private_data = {};
      private_data.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      private_data.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      private_data.Header.Size = static_cast<uint32_t>(sizeof(private_data));
      private_data.ExpectedResetGeneration = opened_info.ResetGeneration;
      private_data.Flags = VIOGPU_WDDM_CONTEXT_FLAGS_NONE;

      context_create.hDevice = device_handle;
      context_create.NodeOrdinal = 0;
      context_create.EngineAffinity = 1;
      context_create.pPrivateDriverData = &private_data;
      context_create.PrivateDriverDataSize = static_cast<UINT>(sizeof(private_data));
      context_create.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
      printf("  CreateContext: begin\n");
      status = dispatch->CreateContext(&context_create);
      printf("  CreateContext: status=0x%08lx handle=0x%08x command=%u "
             "allocations=%u patches=%u\n",
             static_cast<unsigned long>(status), context_create.hContext, context_create.CommandBufferSize,
             context_create.AllocationListSize, context_create.PatchLocationListSize);
      if (!NT_SUCCESS(status) || context_create.hContext == 0)
         goto cleanup;
      context_handle = context_create.hContext;
   }
   if (stage == kProbeStageCreateContext) {
      ready = true;
      goto cleanup;
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
             static_cast<unsigned>(NT_SUCCESS(status) &&
                                   tu_wddm_validate_context_info(&context_info, opened_info.ResetGeneration)),
             static_cast<unsigned long long>(context_info.VaStart),
             static_cast<unsigned long long>(context_info.VaSize),
             static_cast<unsigned long long>(context_info.ResetGeneration), context_info.ContextId,
             context_info.SubmitQueueId);
      ready = NT_SUCCESS(status) && tu_wddm_validate_context_info(&context_info, opened_info.ResetGeneration);
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
      desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;

      tu_wddm_allocation allocation = {};
      printf("  CreateAllocation(native 4KiB): begin\n");
      const bool created = tu_wddm_allocation_create(&context, &desc, &allocation);
      printf("  CreateAllocation(native 4KiB): success=%u handle=0x%08x\n", static_cast<unsigned>(created),
             allocation.handle);
      bool allocation_ready = created;
      bool locked = false;
      bool unlocked = false;
      if (created) {
         void *map = nullptr;
         printf("  Lock(native 4KiB): begin\n");
         locked = tu_wddm_allocation_lock(&allocation, &map);
         printf("  Lock(native 4KiB): success=%u map=%p\n", static_cast<unsigned>(locked), map);
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
            printf("  Unlock(native 4KiB): success=%u\n", static_cast<unsigned>(unlocked));
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
         printf("  DestroyAllocation(native 4KiB): success=%u\n", static_cast<unsigned>(destroyed));
         allocation_clean = allocation_clean && destroyed;
      } else if (allocation.handle != 0) {
         allocation_clean = false;
      }
      ready = allocation_ready && allocation_clean && allocation.handle == 0;

      if (ready && stress_lifecycle) {
         /* Run the same fixed-IOVA allocation cycle on both sides of the
          * context-only stress.  A failure only after context churn points to
          * registration corruption; an early failure is allocation teardown. */
         const bool allocation_before_context = run_allocation_lifecycle_probe(&context);
         const bool context_lifecycle =
            run_context_lifecycle_probe(dispatch, opened_adapter, device_handle, &opened_info);
         const bool allocation_after_context = run_allocation_lifecycle_probe(&context);
         printf("  Stress lifecycle summary: allocation_before=%u context_only=%u allocation_after=%u\n",
                static_cast<unsigned>(allocation_before_context), static_cast<unsigned>(context_lifecycle),
                static_cast<unsigned>(allocation_after_context));
         ready = allocation_before_context && context_lifecycle && allocation_after_context;
      }

      if (ready && submit_nop) {
         ready = run_submit_nop_probe(dispatch, enumerated, opened_adapter, device_handle, context_handle,
                                      &context_create, &opened_info, &context_info);
      }
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
main(int argc, char **argv)
{
   /* KMT calls can each wait for a bounded host response.  Keep every phase
    * visible when the probe is run through SSH, even before process exit. */
   (void) setvbuf(stdout, nullptr, _IONBF, 0);
   (void) setvbuf(stderr, nullptr, _IONBF, 0);
   bool submit_nop = false;
   bool stress_lifecycle = false;
   probe_stage stage = kProbeStageAllocation;
   for (int argument = 1; argument < argc; argument++) {
      if (strcmp(argv[argument], "--submit-nop") == 0)
         submit_nop = true;
      else if (strcmp(argv[argument], "--stress-lifecycle") == 0)
         stress_lifecycle = true;
      else if (parse_probe_stage(argv[argument], &stage))
         continue;
      else {
         fprintf(stderr,
                 "usage: %s [--stage=enumeration|open|device|context|allocation] "
                 "[--submit-nop] [--stress-lifecycle]\n",
                 argv[0]);
         return 2;
      }
   }
   if (stage != kProbeStageAllocation && (submit_nop || stress_lifecycle)) {
      fprintf(stderr, "submit and stress probes require --stage=allocation\n");
      return 2;
   }
   printf("tu WDDM KMT probe: begin stage=%s submit_nop=%u stress_lifecycle=%u\n", probe_stage_name(stage),
          static_cast<unsigned>(submit_nop), static_cast<unsigned>(stress_lifecycle));

   tu_wddm_dispatch dispatch = {};
   if (!tu_wddm_dispatch_init(&dispatch)) {
      fprintf(stderr, "tu WDDM KMT probe: thunk initialization failed\n");
      return 1;
   }

   D3DKMT_ENUMADAPTERS2 enumeration = {};
   printf("EnumAdapters2(count): begin\n");
   NTSTATUS status = dispatch.EnumAdapters2(&enumeration);
   printf("EnumAdapters2(count): status=0x%08lx capacity=%lu\n", static_cast<unsigned long>(status),
          enumeration.NumAdapters);
   if (!NT_SUCCESS(status) || enumeration.NumAdapters == 0 ||
       enumeration.NumAdapters > SIZE_MAX / sizeof(D3DKMT_ADAPTERINFO)) {
      tu_wddm_dispatch_finish(&dispatch);
      return 1;
   }

   const ULONG capacity = enumeration.NumAdapters;
   D3DKMT_ADAPTERINFO *adapters = static_cast<D3DKMT_ADAPTERINFO *>(calloc(capacity, sizeof(*adapters)));
   if (adapters == NULL) {
      tu_wddm_dispatch_finish(&dispatch);
      return 1;
   }

   enumeration.NumAdapters = capacity;
   enumeration.pAdapters = adapters;
   printf("EnumAdapters2(fill): begin\n");
   status = dispatch.EnumAdapters2(&enumeration);
   printf("EnumAdapters2(fill): status=0x%08lx count=%lu\n", static_cast<unsigned long>(status),
          enumeration.NumAdapters);

   bool ready = stage == kProbeStageEnumeration && NT_SUCCESS(status) && enumeration.NumAdapters > 0 &&
                enumeration.NumAdapters <= capacity;
   if (NT_SUCCESS(status) && enumeration.NumAdapters <= capacity) {
      for (ULONG index = 0; index < enumeration.NumAdapters; index++) {
         printf("adapter[%lu]: handle=0x%08x luid=%08lx:%08lx sources=%lu\n", index, adapters[index].hAdapter,
                static_cast<unsigned long>(adapters[index].AdapterLuid.HighPart),
                static_cast<unsigned long>(adapters[index].AdapterLuid.LowPart), adapters[index].NumOfSources);
         if (stage != kProbeStageEnumeration && adapters[index].hAdapter != 0 &&
             probe_adapter(&dispatch, &adapters[index], stage, submit_nop, stress_lifecycle))
            ready = true;
      }
   }

   const ULONG close_count = NT_SUCCESS(status) && enumeration.NumAdapters <= capacity ? enumeration.NumAdapters : 0;
   for (ULONG index = 0; index < close_count; index++)
      close_adapter(&dispatch, &adapters[index].hAdapter);

   free(adapters);
   tu_wddm_dispatch_finish(&dispatch);
   if (!ready) {
      fprintf(stderr, "tu WDDM KMT probe: no adapter completed stage=%s\n", probe_stage_name(stage));
      return 1;
   }

   printf("tu WDDM KMT probe passed stage=%s\n", probe_stage_name(stage));
   return 0;
}
