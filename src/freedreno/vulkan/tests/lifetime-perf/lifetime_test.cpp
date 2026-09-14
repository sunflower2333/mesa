/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Fake OS boundary around extracted production ownership and staging code.
 * These are correctness/operation-count tests, not GPU benchmarks.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>
#include "tu_wddm_lifetime.h"
#include "../wddm/freedreno_wddm_submit.h"

using NTSTATUS = int32_t;
using D3DKMT_HANDLE = uint32_t;
using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_DEVICE_LOST = -4;
constexpr VkResult VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_INITIALIZATION_FAILED = -3;
constexpr NTSTATUS TU_WDDM_STATUS_SUCCESS = 0;
constexpr NTSTATUS TU_WDDM_STATUS_DEVICE_BUSY = static_cast<NTSTATUS>(0x80000011U);
constexpr NTSTATUS TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY = static_cast<NTSTATUS>(0xc01e0102U);
constexpr uint64_t TU_WDDM_DESTROY_WAIT_TIMEOUT_NS = 250000000;
constexpr uint32_t TU_WDDM_MAX_SUBMIT_REFERENCES = 1024;
constexpr uint32_t TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE = 2048;
constexpr int VK_SYSTEM_ALLOCATION_SCOPE_DEVICE = 0;
constexpr uint32_t TU_SUBMIT_BO_ACCESS_READ = 1, TU_SUBMIT_BO_ACCESS_WRITE = 2;
constexpr uint32_t VIOGPU_WDDM_REFERENCE_READ = 1, VIOGPU_WDDM_REFERENCE_WRITE = 2;
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

static unsigned checks = 0;
// Fail with a stable name so negative controls must detect the intended bug.
void check(bool good, const char *name)
{
   checks++;
   if (!good) {
      std::fprintf(stderr, "FAIL %s\n", name);
      std::exit(1);
   }
}
struct TestMutex { bool held = false; };
// These single-thread boundary checks reject recursive and lock-held OS calls.
void mtx_lock(TestMutex *m) { check(!m->held, "nonrecursive_lock"); m->held = true; }
void mtx_unlock(TestMutex *m) { check(m->held, "balanced_unlock"); m->held = false; }
int p_atomic_read(int *p) { return *p; }
bool p_atomic_dec_zero(int *p) { return --*p == 0; }
void p_atomic_set(int *p, int n) { *p = n; }
struct tu_device;
struct Allocator { tu_device *owner; };
struct VkDevice { Allocator alloc; bool lost = false; };
struct VIOGPU_WDDM_ALLOCATION_INFO { uint64_t RequestedIova, Size; };
struct D3DKMT_DESTROYALLOCATION2 {
   D3DKMT_HANDLE hDevice;
   D3DKMT_HANDLE *phAllocationList;
   uint32_t AllocationCount;
   struct { uint32_t AssumeNotInUse; } Flags;
};
struct tu_wddm_runtime {
   struct { NTSTATUS (*DestroyAllocation2)(D3DKMT_DESTROYALLOCATION2 *); } dispatch;
};
struct tu_wddm_device {
   struct {
      tu_wddm_runtime *runtime;
      uint32_t handle;
      struct { uint64_t ResetGeneration; } private_info;
   } adapter;
   uint32_t handle;
};
struct tu_wddm_context {
   tu_wddm_device *device;
   uint32_t handle;
   struct { uint64_t ResetGeneration; } info;
   uint32_t last_submitted_fence;
   uint32_t last_destroy_status;
   uint32_t destroy_attempt_count;
};
struct tu_bo;
struct util_dynarray { void *data = nullptr; size_t size = 0; };
#define util_dynarray_num_elements(p, t) static_cast<uint32_t>((p)->size / sizeof(t))
#define util_dynarray_element(p, t, i) (static_cast<t *>((p)->data) + (i))
// INSERT_RECORDS
static_assert(TU_WDDM_FENCE_HALF_RANGE == (UINT64_C(1) << 31), "production fence serial-number bound");

struct tu_bo {
   uint32_t gem_handle;
   uint64_t size, iova;
   void *map;
   const char *name;
   int refcnt;
   bool gpu_read_only, dump, heap_accounted;
   void *base;
   tu_wddm_allocation *wddm_allocation;
};
struct Heap { std::set<uint64_t> addresses; };
struct tu_device {
   VkDevice vk{};
   tu_wddm_runtime runtime{};
   tu_wddm_device wddm_device{};
   tu_wddm_context wddm_context{};
   TestMutex wddm_mutex, bo_mutex, vma_mutex;
   tu_bo **wddm_bos = nullptr;
   uint32_t wddm_bo_count = 0, wddm_bo_capacity = 1024;
   uint32_t wddm_retired_count = 0, wddm_reap_cursor = 0;
   bool wddm_deferred_bo_destroy = true, wddm_initialized = true, wddm_teardown_failed = false;
   tu_wddm_submit_scratch *wddm_submit_scratch = nullptr;
   tu_wddm_lifetime_stats wddm_lifetime_stats{};
   Heap vma;
   uint32_t completed = 0, queries = 0, waits = 0, destroy_calls = 0, unlocks = 0;
   uint32_t releases = 0, allocations = 0, scratch_frees = 0, reports = 0;
   bool query_ok = true, execution_ok = true, wait_ok = true, unlock_ok = true;
   bool context_close_ok = true, device_close_ok = true, alloc_ok = true, render_ok = true;
   std::map<uint32_t, NTSTATUS> statuses;
   std::set<uint32_t> destroyed;
   std::vector<std::unique_ptr<tu_bo>> slots;
   std::vector<tu_bo *> residency;
   std::vector<uint8_t> packet;
   std::vector<tu_wddm_render_reference> refs;
   uint32_t identity;
   tu_device();
   ~tu_device();
};
struct tu_queue { tu_device *device; uint32_t msm_queue_id = 7; };
static std::map<uint32_t, tu_device *> devices;
static uint32_t next_device = 1;

// The fake KMT accepts only a one-handle destroy with AssumeNotInUse clear.
NTSTATUS fake_destroy(D3DKMT_DESTROYALLOCATION2 *request)
{
   auto d = devices.at(request->hDevice);
   check(!d->bo_mutex.held && !d->vma_mutex.held, "no_allocator_lock_across_KMT");
   check(request->AllocationCount == 1 && request->Flags.AssumeNotInUse == 0, "VidMm_owns_in_use_check");
   d->destroy_calls++;
   uint32_t handle = *request->phAllocationList;
   NTSTATUS status = d->statuses[handle];
   if (status == 0)
      check(d->destroyed.insert(handle).second, "destroy_exactly_once");
   return status;
}
// Construct one independent adapter/device/context owner graph.
tu_device::tu_device()
{
   identity = next_device++;
   vk.alloc.owner = this;
   runtime.dispatch.DestroyAllocation2 = fake_destroy;
   wddm_device.adapter.runtime = &runtime;
   wddm_device.adapter.handle = identity;
   wddm_device.adapter.private_info.ResetGeneration = 9;
   wddm_device.handle = identity;
   wddm_context.device = &wddm_device;
   wddm_context.handle = identity;
   wddm_context.info.ResetGeneration = 9;
   wddm_bos = static_cast<tu_bo **>(std::calloc(1024, sizeof(tu_bo *)));
   devices[identity] = this;
}
// Simulate process reclamation for intentionally retained failure-path owners.
tu_device::~tu_device()
{
   for (auto &bo : slots) {
      if (bo->wddm_allocation) {
         std::free(bo->wddm_allocation->metadata);
         std::free(bo->wddm_allocation);
      }
   }
   std::free(wddm_bos);
   std::free(wddm_submit_scratch);
   devices.erase(identity);
}
void *vk_alloc(Allocator *a, size_t size, size_t, int)
{
   if (!a->owner->alloc_ok) return nullptr;
   a->owner->allocations++;
   return std::malloc(size);
}
void vk_free(Allocator *a, void *p)
{
   if (p && p == a->owner->wddm_submit_scratch) a->owner->scratch_frees++;
   std::free(p);
}
VkResult vk_device_set_lost(VkDevice *d, const char *) { d->lost = true; return VK_ERROR_DEVICE_LOST; }
void tu_wddm_diag(const char *, ...) {}
void mesa_loge(const char *, ...) {}
bool tu_wddm_context_get_completed_fence(tu_wddm_context *c, uint32_t *out)
{
   auto d = devices.at(c->device->handle); d->queries++; *out = d->completed; return d->query_ok;
}
bool tu_wddm_device_execution_active(tu_wddm_device *d) { return devices.at(d->handle)->execution_ok; }
bool tu_wddm_context_wait_submissions(tu_wddm_context *c, uint64_t timeout)
{
   auto d = devices.at(c->device->handle); d->waits++;
   check(d->wddm_deferred_bo_destroy ? timeout == TU_WDDM_DESTROY_WAIT_TIMEOUT_NS
                                    : timeout == UINT64_MAX, "wait_budget_matches_selected_policy");
   return d->wait_ok;
}
bool tu_wddm_allocation_unlock(tu_wddm_allocation *a)
{
   auto d = devices.at(a->context->device->handle); d->unlocks++;
   if (!d->unlock_ok) return false;
   a->locked = false; a->map = nullptr; return true;
}
bool tu_wddm_allocation_destroy(tu_wddm_allocation *a);
void tu_bo_release_heap_accounting(tu_device *d, tu_bo *bo)
{
   check(d->destroyed.count(bo->gem_handle) == 1, "accounting_after_destroy_ack");
   if (bo->heap_accounted) { d->releases++; bo->heap_accounted = false; }
}
void tu_debug_bos_del(tu_device *, tu_bo *) {}
void tu_dump_bo_del(tu_device *, tu_bo *b) { b->dump = false; }
void util_vma_heap_free(Heap *heap, uint64_t address, uint64_t size)
{
   check(size == 8192, "rounded_VMA_extent");
   check(heap->addresses.erase(address) == 1, "VMA_released_once");
}
bool tu_wddm_context_close(tu_wddm_context *c)
{
   auto d = devices.at(c->device->handle);
   check(d->vma.addresses.empty(), "parent_after_allocations");
   if (!d->context_close_ok) return false;
   c->handle = 0; return true;
}
bool tu_wddm_device_close(tu_wddm_device *d)
{
   auto owner = devices.at(d->handle);
   check(owner->wddm_context.handle == 0, "device_after_context");
   if (!owner->device_close_ok) return false;
   d->handle = d->adapter.handle = 0; return true;
}
bool tu_wddm_submit_add_reference(tu_device *d, tu_wddm_submit *, tu_bo *bo, uint32_t)
{
   d->residency.push_back(bo); return true;
}
int tu_wddm_submit_reference_index(const tu_wddm_submit *s, const tu_bo *bo)
{
   const auto refs = static_cast<tu_wddm_submit_reference *>(s->references.data);
   for (uint32_t i = 0; i < util_dynarray_num_elements(&s->references, tu_wddm_submit_reference); i++)
      if (refs[i].bo == bo) return static_cast<int>(i);
   return -1;
}
bool tu_wddm_context_render(tu_wddm_context *c, const void *packet, uint32_t size,
                            const tu_wddm_render_reference *refs, uint32_t count)
{
   auto d = devices.at(c->device->handle);
   check(d->wddm_mutex.held, "scratch_serialized_with_Render");
   const auto bytes = static_cast<const uint8_t *>(packet);
   d->packet.assign(bytes, bytes + size); d->refs.assign(refs, refs + count);
   return d->render_ok;
}
// INSERT_PRODUCTION

// Only the synchronous boundary is faked; the single-attempt destroy is production code.
bool tu_wddm_allocation_destroy(tu_wddm_allocation *a)
{
   return tu_wddm_allocation_try_destroy(a) == TU_WDDM_STATUS_SUCCESS;
}
// Reserve a sparse slot, rounded VMA, mapping, metadata and accounting owner.
tu_bo *make_bo(tu_device &d, uint32_t handle)
{
   check(d.wddm_bo_count < 1024, "fixture_capacity");
   auto bo = std::make_unique<tu_bo>();
   bo->refcnt = 1; bo->heap_accounted = true;
   bo->gem_handle = handle; bo->size = 4097; bo->iova = handle * UINT64_C(8192);
   bo->base = &d;
   bo->wddm_allocation = static_cast<tu_wddm_allocation *>(std::calloc(1, sizeof(tu_wddm_allocation)));
   auto a = bo->wddm_allocation;
   a->context = &d.wddm_context; a->handle = handle;
   a->private_info.RequestedIova = bo->iova; a->private_info.Size = bo->size; a->vma_size = 8192;
   a->metadata = std::malloc(8); a->metadata_size = 8;
   d.vma.addresses.insert(bo->iova);
   auto pointer = bo.get(); d.slots.push_back(std::move(bo));
   d.wddm_bos[d.wddm_bo_count++] = pointer;
   return pointer;
}
// Invoke the actual reaper with the production lock contract.
bool reap(tu_device &d, uint32_t budget = 1024)
{
   mtx_lock(&d.wddm_mutex);
   bool result = tu_wddm_reap_retired_bos_locked(&d, budget);
   mtx_unlock(&d.wddm_mutex);
   return result;
}
// Pending and busy owners retain every resource until an exact success reply.
void lifetime_cases()
{
   {
      tu_device d; d.wddm_context.last_submitted_fence = 17; d.completed = 16;
      auto bo = make_bo(d, 1); auto a = bo->wddm_allocation;
      tu_wddm_bo_finish(&d, bo);
      check(d.waits == 0 && d.destroy_calls == 0 && d.wddm_retired_count == 1 &&
            a->handle == 1 && bo->refcnt == 1 && d.vma.addresses.count(8192) && bo->heap_accounted,
            "pending_fence_retains_owner");
      check(bo->base == nullptr, "retired_owner_has_no_dead_Vulkan_pointer");
      tu_wddm_submit s{}; mtx_lock(&d.wddm_mutex);
      check(tu_wddm_submit_add_live_bos(&d, &s) && d.residency.empty(), "retired_excluded_from_residency");
      mtx_unlock(&d.wddm_mutex);
      tu_wddm_bo_finish(&d, bo);
      check(d.wddm_retired_count == 1 && bo->refcnt == 1, "duplicate_retirement_is_ignored");
      d.completed = 17; d.statuses[1] = TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY;
      check(reap(d) && a->handle == 1 && a->metadata && bo->heap_accounted && !d.vk.lost &&
            d.wddm_retired_count == 1 && d.vma.addresses.count(8192), "busy_retains_owner");
      check(a->retirement.fence_ready && d.destroy_calls == 1 && d.waits == 0, "busy_is_one_attempt");
      d.statuses[1] = TU_WDDM_STATUS_DEVICE_BUSY;
      check(reap(d) && a->handle == 1, "device_busy_is_retryable");
      const auto queries = d.queries;
      d.statuses[1] = 0; d.completed = UINT32_C(0x80000030);
      check(reap(d) && d.queries == queries && d.wddm_retired_count == 0 &&
            d.vma.addresses.empty() && d.releases == 1 && bo->wddm_allocation == nullptr,
            "latched_completion_survives_wrap");
      check(d.wddm_lifetime_stats.queued == 1 && d.wddm_lifetime_stats.reaped == 1 &&
            d.wddm_lifetime_stats.destroy_busy == 2, "aggregate_reason_accounting");
   }
   {
      tu_device d; auto bo = make_bo(d, 1);
      tu_wddm_bo_finish(&d, bo);
      check(d.destroy_calls == 1 && d.waits == 0 && d.queries == 0, "never_submitted_is_immediately_eligible");
   }
   for (auto status : {NTSTATUS(1), NTSTATUS(-1)}) {
      tu_device d; d.statuses[1] = status; auto bo = make_bo(d, 1);
      tu_wddm_bo_finish(&d, bo);
      check(d.vk.lost && bo->wddm_allocation->handle == 1 && bo->wddm_allocation->metadata &&
            d.releases == 0 && d.vma.addresses.size() == 1, "nonzero_status_retains_graph");
   }
   for (unsigned fault = 0; fault < 4; fault++) {
      tu_device d; d.wddm_context.last_submitted_fence = 3; d.completed = 2;
      auto bo = make_bo(d, 1); tu_wddm_bo_finish(&d, bo);
      d.completed = 3;
      if (fault == 0) d.query_ok = false;
      if (fault == 1) d.execution_ok = false;
      if (fault == 2) d.wddm_context.info.ResetGeneration++;
      if (fault == 3) d.wddm_device.adapter.private_info.ResetGeneration++;
      check(!reap(d) && d.vk.lost && d.destroy_calls == 0 && d.releases == 0 &&
            bo->wddm_allocation->handle == 1, "query_or_epoch_failure_retains_graph");
   }
   {
      tu_device d; d.unlock_ok = false; auto bo = make_bo(d, 1);
      bo->wddm_allocation->locked = true; bo->map = &d;
      tu_wddm_bo_finish(&d, bo);
      check(d.vk.lost && d.destroy_calls == 0 && bo->wddm_allocation->locked && bo->map == &d,
            "failed_unlock_retains_mapping");
      d.unlock_ok = true;
      check(reap(d) && d.releases == 1, "unlock_retry_can_release_owner");
   }
   {
      tu_device d; d.wddm_context.last_submitted_fence = UINT32_MAX; d.completed = 0;
      auto bo = make_bo(d, 1); tu_wddm_bo_finish(&d, bo);
      check(d.destroy_calls == 0, "zero_is_not_wrapped_completion");
      d.completed = 1;
      check(reap(d) && d.releases == 1, "uint32_wrap_retires_correct_fence");
   }
   {
      tu_device d; d.wddm_context.last_submitted_fence = 99; d.completed = 98;
      for (uint32_t i = 1; i <= 40; i++) tu_wddm_bo_finish(&d, make_bo(d, i));
      d.completed = 99;
      for (uint32_t i = 1; i <= 40; i++) d.statuses[i] = TU_WDDM_STATUS_DEVICE_BUSY;
      auto before = d.destroy_calls, queries = d.queries;
      check(reap(d, 7) && d.destroy_calls - before == 7 && d.queries - queries == 1, "bounded_single_query_batch");
      d.statuses[40] = 0;
      for (unsigned i = 0; i < 8; i++) check(reap(d, 7), "rotating_reap_pass");
      check(d.destroyed.count(40) == 1 && d.wddm_retired_count == 39, "busy_head_does_not_starve_ready_tail");
      d.statuses.clear();
      check(reap(d) && d.wddm_retired_count == 0 && d.releases == 40, "all_swapped_slots_are_reaped");
   }
   {
      tu_device d; d.wddm_deferred_bo_destroy = false;
      tu_wddm_bo_finish(&d, make_bo(d, 1));
      check(d.waits == 1 && d.destroy_calls == 1 && d.wddm_retired_count == 0, "legacy_path_remains_available");
   }
}
// Compile the actual final hook and verify parent handles and scratch survive failures.
void teardown_cases()
{
   for (unsigned failure = 0; failure < 5; failure++) {
      tu_device d; d.wddm_context.last_submitted_fence = 2; d.completed = 1;
      auto bo = make_bo(d, 1); tu_wddm_bo_finish(&d, bo);
      d.wddm_submit_scratch = static_cast<tu_wddm_submit_scratch *>(vk_alloc(&d.vk.alloc, sizeof(tu_wddm_submit_scratch), 8, 0));
      if (failure == 0) d.wait_ok = false;
      if (failure == 1) { d.unlock_ok = false; bo->wddm_allocation->locked = true; }
      if (failure == 2) d.statuses[1] = -1;
      if (failure == 3) d.context_close_ok = false;
      if (failure == 4) d.device_close_ok = false;
      tu_wddm_device_finish(&d);
      check(d.wddm_teardown_failed && d.wddm_submit_scratch && d.scratch_frees == 0,
            "failed_teardown_retains_outer_graph");
      d.wait_ok = d.unlock_ok = d.context_close_ok = d.device_close_ok = true;
      d.statuses.clear();
      tu_wddm_device_finish(&d);
      check(!d.wddm_initialized && !d.wddm_teardown_failed && d.wddm_retired_count == 0 &&
            d.wddm_submit_scratch == nullptr && d.scratch_frees == 1, "successful_teardown_releases_scratch_once");
   }
}
// Alternate packet sizes, allocation failures and Render failures with one shared staging allocation.
void scratch_cases()
{
   tu_device d; auto first = make_bo(d, 1); auto second = make_bo(d, 2);
   tu_queue q{&d}; tu_wddm_submit s{};
   tu_wddm_submit_entry entries[2]{{first, 0, 64}, {second, 64, 128}};
   tu_wddm_submit_reference references[2]{{first, TU_SUBMIT_BO_ACCESS_READ},
                                         {second, TU_SUBMIT_BO_ACCESS_READ | TU_SUBMIT_BO_ACCESS_WRITE}};
   s.entries.data = entries; s.references.data = references;
   mtx_lock(&d.wddm_mutex);
   check(tu_wddm_submit_render(&q, &s, 1) == VK_ERROR_INITIALIZATION_FAILED && d.allocations == 0, "invalid_packet_does_not_allocate");
   s.entries.size = sizeof(entries); s.references.size = sizeof(references); d.alloc_ok = false;
   check(tu_wddm_submit_render(&q, &s, 1) == VK_ERROR_OUT_OF_HOST_MEMORY && !d.wddm_submit_scratch,
         "scratch_allocation_failure_is_retryable");
   d.alloc_ok = true;
   for (uint32_t fence = 1; fence <= 10000; fence++) {
      const uint32_t count = fence % 2 + 1;
      s.entries.size = count * sizeof(entries[0]); s.references.size = count * sizeof(references[0]);
      d.render_ok = fence % 7 != 0;
      const auto expected = d.render_ok ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
      check(tu_wddm_submit_render(&q, &s, fence) == expected, "Render_result_preserved");
      check(d.allocations == 1 && d.scratch_frees == 0, "scratch_reused");
      tu_wddm_msm_submit_request request;
      std::memcpy(&request, d.packet.data(), sizeof(request));
      check(request.fence == fence && request.bo_count == count && request.command_count == count &&
            request.response_offset == 0 && request.length == d.packet.size() && d.refs.size() == count,
            "used_prefix_has_fresh_packet_and_references");
      for (uint32_t i = 0; i < count; i++) {
         tu_wddm_msm_submit_command command;
         const auto offset = sizeof(request) + count * sizeof(tu_wddm_msm_submit_bo) + i * sizeof(command);
         std::memcpy(&command, d.packet.data() + offset, sizeof(command));
         check(command.submit_index == i && command.padding == 0 && command.relocation_count == 0 && command.iova == 0,
               "reserved_command_fields_stay_zero");
      }
   }
   check(d.wddm_lifetime_stats.scratch_allocations == 1 && d.wddm_lifetime_stats.scratch_reuses == 9999,
         "scratch_operation_counts");
   mtx_unlock(&d.wddm_mutex);
   tu_device other; auto third = make_bo(other, 1); tu_queue oq{&other};
   entries[0].bo = references[0].bo = third;
   s.entries.size = sizeof(entries[0]); s.references.size = sizeof(references[0]);
   mtx_lock(&other.wddm_mutex);
   check(tu_wddm_submit_render(&oq, &s, 1) == VK_SUCCESS &&
         other.wddm_submit_scratch != d.wddm_submit_scratch, "scratch_is_not_shared_between_devices");
   mtx_unlock(&other.wddm_mutex);
   tu_wddm_device_finish(&d); tu_wddm_device_finish(&other);
}
// Exercise randomized busy/ready retirement order against exact owner counts.
void randomized_cases()
{
   for (uint32_t seed = 1; seed <= 64; seed++) {
      std::mt19937 rng(seed); tu_device d;
      d.wddm_context.last_submitted_fence = 10; d.completed = 9;
      for (uint32_t i = 1; i <= 128; i++) tu_wddm_bo_finish(&d, make_bo(d, i));
      d.completed = 10;
      for (uint32_t i = 1; i <= 128; i++)
         if (rng() % 2) d.statuses[i] = TU_WDDM_STATUS_DEVICE_BUSY;
      for (uint32_t pass = 0; pass < 32; pass++) {
         d.statuses[static_cast<uint32_t>(rng() % 128) + 1] = 0;
         check(reap(d, static_cast<uint32_t>(rng() % 16) + 1), "randomized_reap");
         check(d.wddm_retired_count + d.releases == 128 && d.vma.addresses.size() == d.wddm_retired_count,
               "randomized_exact_owner_accounting");
      }
      d.statuses.clear();
      check(reap(d) && d.wddm_retired_count == 0 && d.releases == 128, "randomized_final_drain");
   }
}
int main()
{
   lifetime_cases(); teardown_cases(); scratch_cases(); randomized_cases();
   std::printf("PASS WDDM production lifetime and scratch: %u checks\n"
               "  pending/busy/epoch/unlock/teardown paths; 64 randomized schedules\n"
               "  10000 Render calls: one staging allocation, 9999 reuses\n", checks);
   return 0;
}
