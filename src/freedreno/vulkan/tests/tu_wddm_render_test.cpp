/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#include "../tu_knl_wddm.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint64_t kResetGeneration = 7;
constexpr uint32_t kContextId = 11;
constexpr uint32_t kSubmitQueueId = 17;
constexpr uint64_t kVaStart = UINT64_C(0x100000000);
constexpr uint64_t kVaSize = UINT64_C(0x01000000);
constexpr D3DKMT_HANDLE kAdapterHandle = 1;
constexpr D3DKMT_HANDLE kDeviceHandle = 2;
constexpr D3DKMT_HANDLE kContextHandle = 3;
constexpr D3DKMT_HANDLE kAllocationHandle = 4;
constexpr NTSTATUS kStatusSuccess = static_cast<NTSTATUS>(0);
constexpr NTSTATUS kStatusInvalidParameter = static_cast<NTSTATUS>(-1073741811L);

enum : uint32_t {
   TEST_MSM_CCMD_GEM_SUBMIT = 7,
   TEST_MSM_PIPE_3D0 = 0x10,
   TEST_MSM_SUBMIT_NO_IMPLICIT = 0x80000000,
   TEST_MSM_SUBMIT_BO_READ = 0x0001,
   TEST_MSM_SUBMIT_BO_WRITE = 0x0002,
   TEST_MSM_SUBMIT_CMD_BUF = 0x0001,
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

struct test_msm_submit_two_bos {
   test_msm_submit_request request;
   test_msm_submit_bo bos[2];
   test_msm_submit_command command;
};
#pragma pack(pop)

static_assert(sizeof(test_msm_submit_request) == 36, "MSM request fixture drift");
static_assert(sizeof(test_msm_submit_bo) == 16, "MSM BO fixture drift");
static_assert(sizeof(test_msm_submit_command) == 32, "MSM command fixture drift");
static_assert(offsetof(test_msm_submit_one_bo, bo) + offsetof(test_msm_submit_bo, presumed) == 44,
              "first MSM presumed patch offset drift");
static_assert(offsetof(test_msm_submit_two_bos, bos) + sizeof(test_msm_submit_bo) +
                 offsetof(test_msm_submit_bo, presumed) == 60,
              "second MSM presumed patch offset drift");

int failures;

void
check(bool condition, const char *expression, int line)
{
   if (!condition) {
      fprintf(stderr, "line %d: check failed: %s\n", line, expression);
      failures++;
   }
}

#define CHECK(expression) check(!!(expression), #expression, __LINE__)

VIOGPU_WDDM_ADAPTER_INFO
valid_adapter_info()
{
   VIOGPU_WDDM_ADAPTER_INFO info = {};
   info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
   info.Header.Version = VIOGPU_WDDM_ABI_VERSION;
   info.Header.Size = static_cast<uint32_t>(sizeof(info));
   info.ResetGeneration = 1;
   info.MsmMajorVersion = 1;
   info.MsmMinorVersion = 9;
   info.GpuId = 1;
   info.ChipId = 1;
   info.GmemSize = 4096;
   info.PriorityCount = 1;
   return info;
}

void
test_priority_contract()
{
   VIOGPU_WDDM_ADAPTER_INFO info = valid_adapter_info();
   CHECK(tu_wddm_validate_adapter_info(&info));

   info.PriorityCount = 0;
   CHECK(!tu_wddm_validate_adapter_info(&info));
   info.PriorityCount = 2;
   CHECK(!tu_wddm_validate_adapter_info(&info));

   CHECK(tu_wddm_submitqueue_priority_is_supported(0));
   CHECK(!tu_wddm_submitqueue_priority_is_supported(1));
   CHECK(!tu_wddm_submitqueue_priority_is_supported(-1));
}

void
test_heap_size_contract()
{
   uint64_t heap_size = UINT64_MAX;

   CHECK(!tu_wddm_select_heap_size(0, kVaSize, &heap_size));
   CHECK(heap_size == 0);

   heap_size = UINT64_MAX;
   CHECK(!tu_wddm_select_heap_size(UINT64_C(0x1001), kVaSize, &heap_size));
   CHECK(heap_size == 0);

   heap_size = 0;
   CHECK(tu_wddm_select_heap_size(UINT64_C(0x02000000), kVaSize, &heap_size));
   CHECK(heap_size == kVaSize);

   heap_size = 0;
   CHECK(tu_wddm_select_heap_size(UINT64_C(0x00400000), kVaSize, &heap_size));
   CHECK(heap_size == UINT64_C(0x00400000));

   CHECK(!tu_wddm_select_heap_size(UINT64_C(0x00400000), 0, &heap_size));
   CHECK(!tu_wddm_select_heap_size(UINT64_C(0x00400000), UINT64_C(0x1001), &heap_size));
   CHECK(!tu_wddm_select_heap_size(UINT64_C(0x00400000), kVaSize, NULL));
}

struct test_fixture {
   tu_wddm_runtime runtime;
   tu_wddm_device device;
   tu_wddm_context context;
   BYTE command_buffer[TU_WDDM_MAX_RENDER_COMMAND_SIZE];
   D3DDDI_ALLOCATIONLIST allocation_list[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   D3DDDI_PATCHLOCATIONLIST patch_list[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   BYTE next_command_buffer[TU_WDDM_MAX_RENDER_COMMAND_SIZE];
   D3DDDI_ALLOCATIONLIST next_allocation_list[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   D3DDDI_PATCHLOCATIONLIST next_patch_list[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   BYTE allocation_map[4096];
   VIOGPU_WDDM_ALLOCATION_INFO created_private_info;
   NTSTATUS render_status;
   NTSTATUS escape_status;
   NTSTATUS create_allocation_status;
   NTSTATUS destroy_allocation_status;
   NTSTATUS unlock_status;
   D3DKMT_HANDLE next_allocation_handle;
   uint32_t expected_allocation_count;
   uint32_t next_command_buffer_size;
   uint32_t next_allocation_list_size;
   uint32_t next_patch_list_size;
   unsigned create_calls;
   unsigned destroy_allocation_calls;
   unsigned lock_calls;
   unsigned unlock_calls;
   unsigned render_calls;
   unsigned escape_calls;
   uint32_t completed_fence;
   uint32_t returned_context_id;
   uint32_t expected_reference_flags;
   uint32_t expected_submit_bo_flags;
   bool lock_returns_null_data;
   bool render_returns_null_replacements;
   bool render_returns_partial_replacements;
   bool render_returns_oversized_replacements;
   bool render_leaves_replacements_untouched;
};

test_fixture *current_fixture;

void
init_header(VIOGPU_WDDM_ABI_HEADER *header, uint32_t size)
{
   memset(header, 0, size);
   header->Magic = VIOGPU_WDDM_ABI_MAGIC;
   header->Version = VIOGPU_WDDM_ABI_VERSION;
   header->Size = size;
}

NTSTATUS APIENTRY
fake_create_allocation(D3DKMT_CREATEALLOCATION *create)
{
   test_fixture *fixture = current_fixture;
   CHECK(fixture != NULL);
   CHECK(create != NULL);
   if (fixture == NULL || create == NULL)
      return kStatusInvalidParameter;

   fixture->create_calls++;
   CHECK(create->hDevice == kDeviceHandle);
   CHECK(create->NumAllocations == 1);
   CHECK(create->pAllocationInfo != NULL);
   CHECK(create->Flags.NonSecure == 1);
   if (create->NumAllocations != 1 || create->pAllocationInfo == NULL)
      return kStatusInvalidParameter;

   D3DDDI_ALLOCATIONINFO *allocation_info = &create->pAllocationInfo[0];
   CHECK(allocation_info->pPrivateDriverData != NULL);
   CHECK(allocation_info->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_ALLOCATION_INFO));
   if (allocation_info->pPrivateDriverData == NULL ||
       allocation_info->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_ALLOCATION_INFO))
      return kStatusInvalidParameter;

   memcpy(&fixture->created_private_info, allocation_info->pPrivateDriverData, sizeof(fixture->created_private_info));
   allocation_info->hAllocation = fixture->next_allocation_handle++;
   return fixture->create_allocation_status;
}

NTSTATUS APIENTRY
fake_destroy_allocation(const D3DKMT_DESTROYALLOCATION *destroy)
{
   test_fixture *fixture = current_fixture;
   CHECK(fixture != NULL);
   CHECK(destroy != NULL);
   if (fixture == NULL || destroy == NULL)
      return kStatusInvalidParameter;

   fixture->destroy_allocation_calls++;
   CHECK(destroy->hDevice == kDeviceHandle);
   CHECK(destroy->AllocationCount == 1);
   CHECK(destroy->phAllocationList != NULL);
   if (destroy->AllocationCount == 1 && destroy->phAllocationList != NULL)
      CHECK(destroy->phAllocationList[0] == kAllocationHandle);
   return fixture->destroy_allocation_status;
}

NTSTATUS APIENTRY
fake_lock(D3DKMT_LOCK *lock)
{
   test_fixture *fixture = current_fixture;
   CHECK(fixture != NULL);
   CHECK(lock != NULL);
   if (fixture == NULL || lock == NULL)
      return kStatusInvalidParameter;

   fixture->lock_calls++;
   CHECK(lock->hDevice == kDeviceHandle);
   CHECK(lock->hAllocation == kAllocationHandle);
   CHECK(lock->NumPages == 0);
   CHECK(lock->pPages == NULL);
   CHECK(lock->Flags.ReadOnly == 0);
   CHECK(lock->Flags.LockEntire == 1);
   CHECK((lock->Flags.Value & ~UINT32_C(0x10)) == 0);
   if (!fixture->lock_returns_null_data)
      lock->pData = fixture->allocation_map;
   return kStatusSuccess;
}

NTSTATUS APIENTRY
fake_unlock(const D3DKMT_UNLOCK *unlock)
{
   test_fixture *fixture = current_fixture;
   CHECK(fixture != NULL);
   CHECK(unlock != NULL);
   if (fixture == NULL || unlock == NULL)
      return kStatusInvalidParameter;

   fixture->unlock_calls++;
   CHECK(unlock->hDevice == kDeviceHandle);
   CHECK(unlock->NumAllocations == 1);
   CHECK(unlock->phAllocations != NULL);
   if (unlock->NumAllocations == 1 && unlock->phAllocations != NULL)
      CHECK(unlock->phAllocations[0] == kAllocationHandle);
   return fixture->unlock_status;
}

void
check_render_packet(const D3DKMT_RENDER *render)
{
   test_fixture *fixture = current_fixture;
   CHECK(render->hContext == kContextHandle);
   CHECK(render->CommandOffset == 0);
   CHECK(render->AllocationCount == 1);
   CHECK(render->PatchLocationCount == 1);
   CHECK(render->pNewCommandBuffer == fixture->context.command_buffer);
   CHECK(render->pNewAllocationList == fixture->context.allocation_list);
   CHECK(render->pNewPatchLocationList == fixture->context.patch_location_list);
   CHECK(render->NewCommandBufferSize == sizeof(fixture->command_buffer));
   CHECK(render->NewAllocationListSize == TU_WDDM_MAX_RENDER_ALLOCATIONS);
   CHECK(render->NewPatchLocationListSize == TU_WDDM_MAX_RENDER_ALLOCATIONS);
   CHECK(render->Flags.ResizeCommandBuffer == 0);
   CHECK(render->Flags.ResizeAllocationList == 0);
   CHECK(render->Flags.ResizePatchLocationList == 0);
   CHECK(render->Flags.NullRendering == 0);
   CHECK(render->Flags.PresentRedirected == 0);
   CHECK(render->Flags.RenderKm == 0);
   CHECK(render->Flags.RenderKmReadback == 0);
   CHECK(render->Flags.Reserved == 0);

   const BYTE *packet = static_cast<const BYTE *>(fixture->context.command_buffer);
   const VIOGPU_WDDM_RENDER_COMMAND *header = reinterpret_cast<const VIOGPU_WDDM_RENDER_COMMAND *>(packet);
   const uint32_t command_offset = sizeof(*header) + sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE);
   CHECK(render->CommandLength == command_offset + sizeof(test_msm_submit_one_bo));
   CHECK(header->Header.Magic == VIOGPU_WDDM_ABI_MAGIC);
   CHECK(header->Header.Version == VIOGPU_WDDM_ABI_VERSION);
   CHECK(header->Header.Size == render->CommandLength);
   CHECK(header->Opcode == VIOGPU_WDDM_RENDER_NATIVE_SUBMIT);
   CHECK(header->Flags == VIOGPU_WDDM_RENDER_FLAGS_NONE);
   CHECK(header->ExpectedResetGeneration == kResetGeneration);
   CHECK(header->AllocationReferencesOffset == sizeof(*header));
   CHECK(header->AllocationReferenceCount == 1);
   CHECK(header->CommandStreamOffset == command_offset);
   CHECK(header->CommandStreamSize == sizeof(test_msm_submit_one_bo));

   const VIOGPU_WDDM_ALLOCATION_REFERENCE *reference =
      reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(packet + sizeof(*header));
   CHECK(reference->AllocationIndex == 0);
   CHECK(reference->Flags == fixture->expected_reference_flags);
   CHECK(reference->AllocationOffset == 0);
   CHECK(reference->Length == 4096);
   CHECK(reference->PatchOffset == 44);
   CHECK(reference->Reserved == 0);

   test_msm_submit_one_bo submit = {};
   memcpy(&submit, packet + command_offset, sizeof(submit));
   CHECK(submit.request.command == TEST_MSM_CCMD_GEM_SUBMIT);
   CHECK(submit.request.length == sizeof(submit));
   CHECK(submit.bo.flags == fixture->expected_submit_bo_flags);
   CHECK(submit.bo.handle == 0);
   CHECK(submit.bo.presumed == 0);

   CHECK(fixture->context.allocation_list[0].hAllocation == kAllocationHandle);
   CHECK(fixture->context.allocation_list[0].WriteOperation ==
         ((fixture->expected_reference_flags & VIOGPU_WDDM_REFERENCE_WRITE) != 0));
   CHECK(fixture->context.patch_location_list[0].AllocationIndex == 0);
   CHECK(fixture->context.patch_location_list[0].SlotId == 0);
   CHECK(fixture->context.patch_location_list[0].Reserved == 0);
   CHECK(fixture->context.patch_location_list[0].AllocationOffset == 0);
   CHECK(fixture->context.patch_location_list[0].PatchOffset == command_offset + 44);
}

void
check_two_bo_render_packet(const D3DKMT_RENDER *render)
{
   test_fixture *fixture = current_fixture;
   CHECK(render->hContext == kContextHandle);
   CHECK(render->CommandOffset == 0);
   CHECK(render->AllocationCount == 2);
   CHECK(render->PatchLocationCount == 2);

   const BYTE *packet = static_cast<const BYTE *>(fixture->context.command_buffer);
   const VIOGPU_WDDM_RENDER_COMMAND *header = reinterpret_cast<const VIOGPU_WDDM_RENDER_COMMAND *>(packet);
   const uint32_t command_offset = sizeof(*header) + 2 * sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE);
   CHECK(render->CommandLength == command_offset + sizeof(test_msm_submit_two_bos));
   CHECK(header->AllocationReferenceCount == 2);
   CHECK(header->CommandStreamOffset == command_offset);
   CHECK(header->CommandStreamSize == sizeof(test_msm_submit_two_bos));

   const VIOGPU_WDDM_ALLOCATION_REFERENCE *references =
      reinterpret_cast<const VIOGPU_WDDM_ALLOCATION_REFERENCE *>(packet + sizeof(*header));
   CHECK(references[0].AllocationIndex == 0);
   CHECK(references[0].Flags == (VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE));
   CHECK(references[0].PatchOffset == 44);
   CHECK(references[1].AllocationIndex == 1);
   CHECK(references[1].Flags == VIOGPU_WDDM_REFERENCE_READ);
   CHECK(references[1].PatchOffset == 60);

   test_msm_submit_two_bos submit = {};
   memcpy(&submit, packet + command_offset, sizeof(submit));
   CHECK(submit.bos[0].flags == (TEST_MSM_SUBMIT_BO_READ | TEST_MSM_SUBMIT_BO_WRITE));
   CHECK(submit.bos[0].handle == 0);
   CHECK(submit.bos[0].presumed == 0);
   CHECK(submit.bos[1].flags == TEST_MSM_SUBMIT_BO_READ);
   CHECK(submit.bos[1].handle == 0);
   CHECK(submit.bos[1].presumed == 0);

   CHECK(fixture->context.allocation_list[0].hAllocation == kAllocationHandle);
   CHECK(fixture->context.allocation_list[0].WriteOperation == 1);
   CHECK(fixture->context.allocation_list[1].hAllocation == kAllocationHandle + 1);
   CHECK(fixture->context.allocation_list[1].WriteOperation == 0);
   CHECK(fixture->context.patch_location_list[0].AllocationIndex == 0);
   CHECK(fixture->context.patch_location_list[0].SlotId == 0);
   CHECK(fixture->context.patch_location_list[0].PatchOffset == command_offset + 44);
   CHECK(fixture->context.patch_location_list[1].AllocationIndex == 1);
   CHECK(fixture->context.patch_location_list[1].SlotId == 0);
   CHECK(fixture->context.patch_location_list[1].PatchOffset == command_offset + 60);
}

NTSTATUS APIENTRY
fake_render(D3DKMT_RENDER *render)
{
   test_fixture *fixture = current_fixture;
   CHECK(fixture != NULL);
   CHECK(render != NULL);
   if (fixture == NULL || render == NULL)
      return kStatusInvalidParameter;

   fixture->render_calls++;
   if (fixture->expected_allocation_count == 1)
      check_render_packet(render);
   else if (fixture->expected_allocation_count == 2)
      check_two_bo_render_packet(render);
   else
      CHECK(false);
   if (!fixture->render_leaves_replacements_untouched) {
      render->pNewCommandBuffer = fixture->next_command_buffer;
      render->NewCommandBufferSize = fixture->next_command_buffer_size;
      render->pNewAllocationList = fixture->next_allocation_list;
      render->NewAllocationListSize = fixture->next_allocation_list_size;
      render->pNewPatchLocationList = fixture->next_patch_list;
      render->NewPatchLocationListSize = fixture->next_patch_list_size;
      if (fixture->render_returns_null_replacements) {
         render->pNewCommandBuffer = NULL;
         render->NewCommandBufferSize = 0;
         render->pNewAllocationList = NULL;
         render->NewAllocationListSize = 0;
         render->pNewPatchLocationList = NULL;
         render->NewPatchLocationListSize = 0;
      }
      if (fixture->render_returns_partial_replacements) {
         render->pNewAllocationList = NULL;
         render->NewAllocationListSize = fixture->next_allocation_list_size;
      }
      if (fixture->render_returns_oversized_replacements) {
         render->NewCommandBufferSize = TU_WDDM_MAX_RENDER_COMMAND_SIZE + 1;
      }
   }
   return fixture->render_status;
}

NTSTATUS APIENTRY
fake_escape(const D3DKMT_ESCAPE *escape)
{
   test_fixture *fixture = current_fixture;
   CHECK(fixture != NULL);
   CHECK(escape != NULL);
   if (fixture == NULL || escape == NULL)
      return kStatusInvalidParameter;

   fixture->escape_calls++;
   CHECK(escape->hAdapter == kAdapterHandle);
   CHECK(escape->hDevice == kDeviceHandle);
   CHECK(escape->hContext == kContextHandle);
   CHECK(escape->Type == D3DKMT_ESCAPE_DRIVERPRIVATE);
   CHECK(escape->Flags.Value == 0);
   CHECK(escape->pPrivateDriverData != NULL);
   CHECK(escape->PrivateDriverDataSize == sizeof(VIOGPU_WDDM_FENCE_INFO));
   if (escape->pPrivateDriverData == NULL ||
       escape->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_FENCE_INFO))
      return kStatusInvalidParameter;

   VIOGPU_WDDM_FENCE_INFO *info =
      static_cast<VIOGPU_WDDM_FENCE_INFO *>(escape->pPrivateDriverData);
   CHECK(info->Header.Magic == VIOGPU_WDDM_ABI_MAGIC);
   CHECK(info->Header.Version == VIOGPU_WDDM_ABI_VERSION);
   CHECK(info->Header.Size == sizeof(*info));
   CHECK(info->Header.Reserved == 0);
   CHECK(info->Opcode == VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE);
   CHECK(info->Flags == VIOGPU_WDDM_ESCAPE_FLAGS_NONE);
   CHECK(info->ExpectedResetGeneration == kResetGeneration);
   CHECK(info->CompletedFence == 0);
   CHECK(info->ResetGeneration == 0);
   CHECK(info->ContextId == 0);
   CHECK(info->Reserved == 0);

   if (fixture->escape_status != kStatusSuccess)
      return fixture->escape_status;

   VIOGPU_WDDM_FENCE_INFO response = {};
   init_header(&response.Header, sizeof(response));
   response.Opcode = VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE;
   response.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
   response.ExpectedResetGeneration = kResetGeneration;
   response.CompletedFence = fixture->completed_fence;
   response.ResetGeneration = kResetGeneration;
   response.ContextId = fixture->returned_context_id;
   *info = response;
   return kStatusSuccess;
}

void
init_fixture(test_fixture *fixture)
{
   memset(fixture, 0, sizeof(*fixture));
   current_fixture = fixture;
   fixture->runtime.dispatch.CreateAllocation = fake_create_allocation;
   fixture->runtime.dispatch.DestroyAllocation = fake_destroy_allocation;
   fixture->runtime.dispatch.Lock = fake_lock;
   fixture->runtime.dispatch.Unlock = fake_unlock;
   fixture->runtime.dispatch.Render = fake_render;
   fixture->runtime.dispatch.Escape = fake_escape;
   fixture->render_status = kStatusSuccess;
   fixture->escape_status = kStatusSuccess;
   fixture->create_allocation_status = kStatusSuccess;
   fixture->destroy_allocation_status = kStatusSuccess;
   fixture->unlock_status = kStatusSuccess;
   fixture->next_allocation_handle = kAllocationHandle;
   fixture->expected_allocation_count = 1;
   fixture->next_command_buffer_size = sizeof(fixture->next_command_buffer);
   fixture->next_allocation_list_size = TU_WDDM_MAX_RENDER_ALLOCATIONS;
   fixture->next_patch_list_size = TU_WDDM_MAX_RENDER_ALLOCATIONS;
   fixture->returned_context_id = kContextId;
   fixture->expected_reference_flags = VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE;
   fixture->expected_submit_bo_flags = TEST_MSM_SUBMIT_BO_READ | TEST_MSM_SUBMIT_BO_WRITE;

   fixture->device.adapter.runtime = &fixture->runtime;
   fixture->device.adapter.handle = kAdapterHandle;
   fixture->device.adapter.private_info.ResetGeneration = kResetGeneration;
   fixture->device.handle = kDeviceHandle;
   fixture->context.device = &fixture->device;
   fixture->context.handle = kContextHandle;
   fixture->context.command_buffer = fixture->command_buffer;
   fixture->context.command_buffer_size = sizeof(fixture->command_buffer);
   fixture->context.allocation_list = fixture->allocation_list;
   fixture->context.allocation_list_size = TU_WDDM_MAX_RENDER_ALLOCATIONS;
   fixture->context.patch_location_list = fixture->patch_list;
   fixture->context.patch_location_list_size = TU_WDDM_MAX_RENDER_ALLOCATIONS;

   init_header(&fixture->context.info.Header, sizeof(fixture->context.info));
   fixture->context.info.Opcode = VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO;
   fixture->context.info.Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
   fixture->context.info.ExpectedResetGeneration = kResetGeneration;
   fixture->context.info.VaStart = kVaStart;
   fixture->context.info.VaSize = kVaSize;
   fixture->context.info.ResetGeneration = kResetGeneration;
   fixture->context.info.ContextId = kContextId;
   fixture->context.info.SubmitQueueId = kSubmitQueueId;
}

tu_wddm_allocation_desc
native_allocation_desc()
{
   tu_wddm_allocation_desc desc = {};
   desc.size = 4096;
   desc.alignment = 4096;
   desc.requested_iova = kVaStart;
   desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE | VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
   return desc;
}

test_msm_submit_one_bo
valid_submit()
{
   test_msm_submit_one_bo submit = {};
   submit.request.command = TEST_MSM_CCMD_GEM_SUBMIT;
   submit.request.length = sizeof(submit);
   submit.request.sequence = 1;
   submit.request.flags = TEST_MSM_PIPE_3D0 | TEST_MSM_SUBMIT_NO_IMPLICIT;
   submit.request.queue_id = kSubmitQueueId;
   submit.request.bo_count = 1;
   submit.request.command_count = 1;
   submit.request.fence = 1;
   submit.bo.flags = TEST_MSM_SUBMIT_BO_READ | TEST_MSM_SUBMIT_BO_WRITE;
   submit.command.type = TEST_MSM_SUBMIT_CMD_BUF;
   submit.command.size = 4;
   return submit;
}

tu_wddm_render_reference
valid_reference(tu_wddm_allocation *allocation)
{
   tu_wddm_render_reference reference = {};
   reference.allocation = allocation;
   reference.flags = VIOGPU_WDDM_REFERENCE_READ | VIOGPU_WDDM_REFERENCE_WRITE;
   reference.length = 4096;
   reference.patch_offset = 44;
   return reference;
}

bool
create_native_allocation(test_fixture *fixture, tu_wddm_allocation *allocation)
{
   tu_wddm_allocation_desc desc = native_allocation_desc();
   bool created = tu_wddm_allocation_create(&fixture->context, &desc, allocation);
   CHECK(created);
   CHECK(fixture->create_calls == 1);
   if (created) {
      CHECK(allocation->handle == kAllocationHandle);
      CHECK(allocation->private_info.RequestedIova == kVaStart);
      CHECK(allocation->private_info.ExpectedResetGeneration == kResetGeneration);
      CHECK(allocation->private_info.ContextId == kContextId);
   }
   return created;
}

void
test_allocation_and_lock()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   void *map = NULL;
   CHECK(tu_wddm_allocation_lock(&allocation, &map));
   CHECK(map == fixture.allocation_map);
   CHECK(allocation.locked);
   CHECK(fixture.lock_calls == 1);
   CHECK(tu_wddm_allocation_unlock(&allocation));
   CHECK(!allocation.locked);
   CHECK(allocation.map == NULL);
   CHECK(fixture.unlock_calls == 1);
}

void
test_gpu_read_only_allocation_maps_cpu_writable()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   tu_wddm_allocation_desc desc = native_allocation_desc();
   desc.flags |= VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY;
   CHECK(tu_wddm_allocation_create(&fixture.context, &desc, &allocation));
   CHECK((allocation.private_info.Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY) != 0);

   void *map = NULL;
   CHECK(tu_wddm_allocation_lock(&allocation, &map));
   CHECK(map == fixture.allocation_map);
   static_cast<BYTE *>(map)[0] = 0x5a;
   CHECK(fixture.allocation_map[0] == 0x5a);

   test_msm_submit_one_bo submit = valid_submit();
   submit.bo.flags = TEST_MSM_SUBMIT_BO_READ;
   tu_wddm_render_reference reference = valid_reference(&allocation);
   reference.flags = VIOGPU_WDDM_REFERENCE_READ;
   fixture.expected_reference_flags = VIOGPU_WDDM_REFERENCE_READ;
   fixture.expected_submit_bo_flags = TEST_MSM_SUBMIT_BO_READ;
   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));

   CHECK(tu_wddm_allocation_unlock(&allocation));
   CHECK(tu_wddm_allocation_destroy(&allocation));
}

void
test_allocation_range_rejected()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   tu_wddm_allocation_desc desc = native_allocation_desc();
   desc.requested_iova = kVaStart + kVaSize;
   CHECK(!tu_wddm_allocation_create(&fixture.context, &desc, &allocation));
   CHECK(fixture.create_calls == 0);

   desc = native_allocation_desc();
   desc.flags |= VIOGPU_WDDM_ALLOCATION_PRIMARY;
   desc.width = 1;
   desc.height = 1;
   desc.pitch = 4;
   desc.format = VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM;
   desc.refresh_rate_numerator = 60;
   desc.refresh_rate_denominator = 1;
   CHECK(!tu_wddm_allocation_create(&fixture.context, &desc, &allocation));
   CHECK(fixture.create_calls == 0);
}

void
test_failed_allocation_creation_compensates()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.create_allocation_status = kStatusInvalidParameter;

   tu_wddm_allocation allocation = {};
   tu_wddm_allocation_desc desc = native_allocation_desc();
   CHECK(!tu_wddm_allocation_create(&fixture.context, &desc, &allocation));
   CHECK(fixture.create_calls == 1);
   CHECK(fixture.destroy_allocation_calls == 1);
   CHECK(allocation.handle == 0);
   CHECK(allocation.context == NULL);
}

void
test_failed_allocation_rollback_retains_owner()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.create_allocation_status = kStatusInvalidParameter;
   fixture.destroy_allocation_status = kStatusInvalidParameter;

   tu_wddm_allocation allocation = {};
   tu_wddm_allocation_desc desc = native_allocation_desc();
   CHECK(!tu_wddm_allocation_create(&fixture.context, &desc, &allocation));
   CHECK(fixture.create_calls == 1);
   CHECK(fixture.destroy_allocation_calls == 1);
   CHECK(allocation.handle == kAllocationHandle);
   CHECK(allocation.context == &fixture.context);
   CHECK(allocation.private_info.RequestedIova == kVaStart);
   CHECK(allocation.vma_size == 4096);

   fixture.destroy_allocation_status = kStatusSuccess;
   CHECK(tu_wddm_allocation_destroy(&allocation));
   CHECK(fixture.destroy_allocation_calls == 2);
   CHECK(allocation.handle == 0);
   CHECK(allocation.context == NULL);
}

void
test_failed_allocation_teardown_retains_owner()
{
   test_fixture fixture;
   init_fixture(&fixture);

   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   fixture.destroy_allocation_status = kStatusInvalidParameter;
   CHECK(!tu_wddm_allocation_destroy(&allocation));
   CHECK(allocation.handle == kAllocationHandle);
   CHECK(allocation.context == &fixture.context);
   CHECK(allocation.private_info.RequestedIova == kVaStart);
   CHECK(allocation.vma_size == 4096);

   fixture.destroy_allocation_status = kStatusSuccess;
   CHECK(tu_wddm_allocation_destroy(&allocation));
   CHECK(allocation.handle == 0);
   CHECK(allocation.context == NULL);
}

void
test_failed_unlock_retains_owner()
{
   test_fixture fixture;
   init_fixture(&fixture);

   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   void *map = NULL;
   CHECK(tu_wddm_allocation_lock(&allocation, &map));
   CHECK(map == fixture.allocation_map);

   fixture.unlock_status = kStatusInvalidParameter;
   CHECK(!tu_wddm_allocation_unlock(&allocation));
   CHECK(allocation.handle == kAllocationHandle);
   CHECK(allocation.context == &fixture.context);
   CHECK(allocation.locked);
   CHECK(allocation.map == fixture.allocation_map);

   fixture.unlock_status = kStatusSuccess;
   CHECK(tu_wddm_allocation_unlock(&allocation));
   CHECK(!allocation.locked);
   CHECK(tu_wddm_allocation_destroy(&allocation));
}

void
test_null_lock_data_is_rolled_back()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.lock_returns_null_data = true;

   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   void *map = reinterpret_cast<void *>(UINTPTR_MAX);
   CHECK(!tu_wddm_allocation_lock(&allocation, &map));
   CHECK(map == NULL);
   CHECK(!allocation.locked);
   CHECK(allocation.map == NULL);
   CHECK(fixture.lock_calls == 1);
   CHECK(fixture.unlock_calls == 1);
}

void
test_null_lock_data_rollback_retains_owner()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.lock_returns_null_data = true;
   fixture.unlock_status = kStatusInvalidParameter;

   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   void *map = reinterpret_cast<void *>(UINTPTR_MAX);
   CHECK(!tu_wddm_allocation_lock(&allocation, &map));
   CHECK(map == NULL);
   CHECK(allocation.locked);
   CHECK(allocation.map == NULL);
   CHECK(fixture.lock_calls == 1);
   CHECK(fixture.unlock_calls == 1);

   fixture.unlock_status = kStatusSuccess;
   CHECK(tu_wddm_allocation_unlock(&allocation));
   CHECK(!allocation.locked);
   CHECK(fixture.unlock_calls == 2);
}

void
test_valid_render()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 1);
   CHECK(fixture.context.command_buffer == fixture.next_command_buffer);
   CHECK(fixture.context.command_buffer_size == fixture.next_command_buffer_size);
   CHECK(fixture.context.allocation_list == fixture.next_allocation_list);
   CHECK(fixture.context.allocation_list_size == fixture.next_allocation_list_size);
   CHECK(fixture.context.patch_location_list == fixture.next_patch_list);
   CHECK(fixture.context.patch_location_list_size == fixture.next_patch_list_size);
   CHECK(fixture.context.last_submitted_fence == submit.request.fence);
}

void
test_submission_retirement_snapshot()
{
   test_fixture fixture;
   init_fixture(&fixture);
   CHECK(fixture.context.last_submitted_fence == 0);
   CHECK(tu_wddm_context_wait_submissions(&fixture.context, 0));
   CHECK(fixture.escape_calls == 0);

   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   submit.request.sequence = 9;
   submit.request.fence = 9;
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit),
                                &reference, 1));
   CHECK(fixture.context.last_submitted_fence == 9);

   submit.request.sequence = 8;
   submit.request.fence = 8;
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit),
                                 &reference, 1));
   CHECK(fixture.render_calls == 1);
   CHECK(fixture.context.last_submitted_fence == 9);

   fixture.completed_fence = 8;
   CHECK(!tu_wddm_context_wait_submissions(&fixture.context, 0));
   fixture.completed_fence = 9;
   CHECK(tu_wddm_context_wait_submissions(&fixture.context, 0));
}

void
test_duplicate_fence_rejected_before_render()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit),
                                &reference, 1));
   CHECK(fixture.render_calls == 1);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit),
                                 &reference, 1));
   CHECK(fixture.render_calls == 1);
   CHECK(fixture.context.last_submitted_fence == submit.request.fence);
}

void
test_context_render_fence_wrap()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   tu_wddm_render_reference reference = valid_reference(&allocation);
   test_msm_submit_one_bo submit = valid_submit();
   submit.request.sequence = UINT32_MAX;
   submit.request.fence = UINT32_MAX;
   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit),
                                &reference, 1));
   CHECK(fixture.context.last_submitted_fence == UINT32_MAX);

   /* Fence 1 is the next serial value after UINT32_MAX.  This must remain
    * ordered without treating the wrap as a device reset. */
   submit.request.sequence = 1;
   submit.request.fence = 1;
   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit),
                                &reference, 1));
   CHECK(fixture.context.last_submitted_fence == 1);
}

void
test_valid_two_bo_render()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.expected_allocation_count = 2;

   tu_wddm_allocation allocations[2] = {};
   if (!create_native_allocation(&fixture, &allocations[0]))
      return;

   tu_wddm_allocation_desc desc = native_allocation_desc();
   desc.requested_iova += 4096;
   CHECK(tu_wddm_allocation_create(&fixture.context, &desc, &allocations[1]));
   CHECK(allocations[1].handle == kAllocationHandle + 1);
   CHECK(fixture.create_calls == 2);

   test_msm_submit_two_bos submit = {};
   submit.request = valid_submit().request;
   submit.request.length = sizeof(submit);
   submit.request.bo_count = 2;
   submit.bos[0].flags = TEST_MSM_SUBMIT_BO_READ | TEST_MSM_SUBMIT_BO_WRITE;
   submit.bos[1].flags = TEST_MSM_SUBMIT_BO_READ;
   submit.command = valid_submit().command;

   tu_wddm_render_reference references[2] = {
      valid_reference(&allocations[0]),
      valid_reference(&allocations[1]),
   };
   references[1].flags = VIOGPU_WDDM_REFERENCE_READ;
   references[1].patch_offset = 60;

   CHECK(tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), references, 2));
   CHECK(fixture.render_calls == 1);
}

void
test_malformed_render_rejected()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);

   submit.bo.handle = 1;
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 0);
   submit.bo.handle = 0;

   reference.patch_offset++;
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 0);
   reference.patch_offset--;

   fixture.context.info.ResetGeneration++;
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 0);
   fixture.context.info.ResetGeneration--;

   allocation.private_info.ExpectedResetGeneration++;
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 0);
   allocation.private_info.ExpectedResetGeneration--;

   test_msm_submit_two_bos duplicate_submit = {};
   duplicate_submit.request = submit.request;
   duplicate_submit.request.length = sizeof(duplicate_submit);
   duplicate_submit.request.bo_count = 2;
   duplicate_submit.bos[0] = submit.bo;
   duplicate_submit.bos[1] = submit.bo;
   duplicate_submit.command = submit.command;
   tu_wddm_render_reference duplicate_references[2] = { reference, reference };
   duplicate_references[1].patch_offset += sizeof(test_msm_submit_bo);
   CHECK(
      !tu_wddm_context_render(&fixture.context, &duplicate_submit, sizeof(duplicate_submit), duplicate_references, 2));
   CHECK(fixture.render_calls == 0);
}

void
test_queue_id_mismatch_rejected()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   submit.request.queue_id++;
   CHECK(submit.request.queue_id != fixture.context.info.SubmitQueueId);
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 0);
}

void
test_failed_render_updates_buffers()
{
   test_fixture fixture;
   init_fixture(&fixture);
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   fixture.render_status = kStatusInvalidParameter;
   fixture.next_command_buffer_size /= 2;
   fixture.next_allocation_list_size /= 2;
   fixture.next_patch_list_size /= 2;
   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 1);
   CHECK(fixture.context.command_buffer == fixture.next_command_buffer);
   CHECK(fixture.context.command_buffer_size == fixture.next_command_buffer_size);
   CHECK(fixture.context.allocation_list == fixture.next_allocation_list);
   CHECK(fixture.context.allocation_list_size == fixture.next_allocation_list_size);
   CHECK(fixture.context.patch_location_list == fixture.next_patch_list);
   CHECK(fixture.context.patch_location_list_size == fixture.next_patch_list_size);
   CHECK(fixture.context.last_submitted_fence == 0);
}

void
test_failed_render_with_untouched_replacements_is_retryable()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.render_status = kStatusInvalidParameter;
   fixture.render_leaves_replacements_untouched = true;
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.context.command_buffer == fixture.command_buffer);
   CHECK(fixture.context.allocation_list == fixture.allocation_list);
   CHECK(fixture.context.patch_location_list == fixture.patch_list);
   CHECK(fixture.context.last_submitted_fence == 0);
}

void
test_successful_render_with_null_replacements_fails_closed()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.render_returns_null_replacements = true;
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.render_calls == 1);
   CHECK(fixture.context.command_buffer == NULL);
   CHECK(fixture.context.command_buffer_size == 0);
   CHECK(fixture.context.allocation_list == NULL);
   CHECK(fixture.context.allocation_list_size == 0);
   CHECK(fixture.context.patch_location_list == NULL);
   CHECK(fixture.context.patch_location_list_size == 0);
   CHECK(fixture.context.last_submitted_fence == submit.request.fence);
}

void
test_successful_render_with_partial_replacements_fails_closed()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.render_returns_partial_replacements = true;
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.context.command_buffer == NULL);
   CHECK(fixture.context.allocation_list == NULL);
   CHECK(fixture.context.patch_location_list == NULL);
   CHECK(fixture.context.last_submitted_fence == submit.request.fence);
}

void
test_successful_render_with_oversized_replacements_fails_closed()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.render_returns_oversized_replacements = true;
   tu_wddm_allocation allocation = {};
   if (!create_native_allocation(&fixture, &allocation))
      return;

   test_msm_submit_one_bo submit = valid_submit();
   tu_wddm_render_reference reference = valid_reference(&allocation);
   CHECK(!tu_wddm_context_render(&fixture.context, &submit, sizeof(submit), &reference, 1));
   CHECK(fixture.context.command_buffer == NULL);
   CHECK(fixture.context.allocation_list == NULL);
   CHECK(fixture.context.patch_location_list == NULL);
   CHECK(fixture.context.last_submitted_fence == submit.request.fence);
}

void
test_completed_fence_query_and_wait()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.completed_fence = 7;

   uint32_t completed = 0;
   CHECK(tu_wddm_context_get_completed_fence(&fixture.context, &completed));
   CHECK(completed == 7);
   CHECK(fixture.escape_calls == 1);
   CHECK(tu_wddm_context_wait_fence(&fixture.context, 7, 0));
   CHECK(!tu_wddm_context_wait_fence(&fixture.context, 8, 0));

   /* Sequence one is after UINT32_MAX when the 32-bit queue fence wraps. */
   fixture.completed_fence = 1;
   CHECK(tu_wddm_context_wait_fence(&fixture.context, UINT32_MAX, 0));
}

void
test_completed_fence_identity_rejected()
{
   test_fixture fixture;
   init_fixture(&fixture);
   fixture.completed_fence = 1;
   fixture.returned_context_id++;

   uint32_t completed = UINT32_MAX;
   CHECK(!tu_wddm_context_get_completed_fence(&fixture.context, &completed));
   CHECK(completed == 0);

   fixture.returned_context_id = kContextId;
   fixture.escape_status = kStatusInvalidParameter;
   CHECK(!tu_wddm_context_get_completed_fence(&fixture.context, &completed));
   CHECK(completed == 0);
}

} /* namespace */

extern "C" bool
tu_wddm_dispatch_init(struct tu_wddm_dispatch *dispatch)
{
   (void) dispatch;
   return false;
}

extern "C" void
tu_wddm_dispatch_finish(struct tu_wddm_dispatch *dispatch)
{
   (void) dispatch;
}

int
main()
{
   test_priority_contract();
   test_heap_size_contract();
   test_allocation_and_lock();
   test_gpu_read_only_allocation_maps_cpu_writable();
   test_allocation_range_rejected();
   test_failed_allocation_creation_compensates();
   test_failed_allocation_rollback_retains_owner();
   test_failed_allocation_teardown_retains_owner();
   test_failed_unlock_retains_owner();
   test_null_lock_data_is_rolled_back();
   test_null_lock_data_rollback_retains_owner();
   test_valid_render();
   test_submission_retirement_snapshot();
   test_duplicate_fence_rejected_before_render();
   test_context_render_fence_wrap();
   test_valid_two_bo_render();
   test_malformed_render_rejected();
   test_queue_id_mismatch_rejected();
   test_failed_render_updates_buffers();
   test_failed_render_with_untouched_replacements_is_retryable();
   test_successful_render_with_null_replacements_fails_closed();
   test_successful_render_with_partial_replacements_fails_closed();
   test_successful_render_with_oversized_replacements_fails_closed();
   test_completed_fence_query_and_wait();
   test_completed_fence_identity_rejected();
   current_fixture = NULL;

   if (failures != 0)
      return 1;
   printf("tu WDDM render fixture passed\n");
   return 0;
}
