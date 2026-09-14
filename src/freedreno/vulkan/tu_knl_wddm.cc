/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Vulkan integration only.  KMT object/ABI plumbing is in ../wddm.
 */
#include "tu_knl_wddm.h"
#include "../wddm/freedreno_wddm_private.h"
#include "../wddm/freedreno_wddm_submit.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef TU_HAS_WDDM
#include <errno.h>

#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_dynarray.h"
#include "vk_sync_dummy.h"
#include "vk_sync_timeline.h"

#include "tu_cs.h"
#include "tu_device.h"
#include "tu_knl.h"
#include "tu_queue.h"
#include "tu_rmv.h"

bool
tu_wddm_probe_cleanup(struct tu_instance *instance)
{
   if (instance == NULL)
      return false;

   const bool cleaned = tu_wddm_probe_owner_cleanup(
      &instance->wddm_probe_device, &instance->wddm_probe_context);
   instance->wddm_probe_pending = !cleaned;
   return cleaned;
}

bool
tu_wddm_instance_prepare_destroy(struct tu_instance *instance)
{
   if (instance == NULL)
      return false;

   return !instance->wddm_runtime_initialized ||
          tu_wddm_probe_cleanup(instance);
}


/* The WDDM path deliberately starts with one context and one engine.  These
 * limits are also enforced by the private ABI/KMD, so rejecting an oversized
 * packet here keeps the UMD from constructing a request the KMD cannot own. */
enum {
   TU_WDDM_MAX_SUBMIT_REFERENCES = TU_WDDM_MAX_RENDER_ALLOCATIONS,
   TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE = 2048,
};

static_assert(sizeof(VIOGPU_WDDM_RENDER_COMMAND) +
                    TU_WDDM_MAX_SUBMIT_REFERENCES *
                       sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE) +
                    sizeof(tu_wddm_msm_submit_request) +
                    TU_WDDM_MAX_SUBMIT_REFERENCES *
                       sizeof(tu_wddm_msm_submit_bo) +
                    TU_WDDM_MAX_SUBMIT_COMMANDS *
                       sizeof(tu_wddm_msm_submit_command) <=
                 TU_WDDM_MAX_RENDER_COMMAND_SIZE,
              "maximum WDDM submit no longer fits the DMA buffer");
static_assert((TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE &
               (TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE - 1)) == 0,
              "submit reference index must be a power of two");
static_assert(TU_WDDM_MAX_SUBMIT_REFERENCES < UINT16_MAX,
              "submit reference index encoding overflow");

struct tu_wddm_sync {
   struct vk_sync base;
   /* vk_sync callbacks receive the device that owns the operation.  Keep the
    * owner explicitly instead of inferring it from the context pointer: a
    * sync object from another VkDevice must fail closed before it can read or
    * mutate this context's fence state. */
   struct tu_device *owner;
   struct tu_wddm_context *context;
   uint64_t reset_generation;
   alignas(8) uint64_t state;
};

enum : uint64_t {
   TU_WDDM_SYNC_SIGNALED = UINT64_C(1) << 63,
};

static uint64_t
tu_wddm_sync_state_read(struct tu_wddm_sync *sync)
{
   return static_cast<uint64_t>(p_atomic_cmpxchg(&sync->state, 0, 0));
}

static void
tu_wddm_sync_state_set(struct tu_wddm_sync *sync, uint64_t state)
{
   p_atomic_xchg(&sync->state, state);
}

struct tu_wddm_submit_entry {
   struct tu_bo *bo;
   uint32_t offset;
   uint32_t size;
};

struct tu_wddm_submit_reference {
   struct tu_bo *bo;
   uint32_t access;
};

/* One device-local staging allocation replaces two heap round trips per
 * Render.  KMT consumes/copies the staging data before Render returns. */
struct tu_wddm_submit_scratch {
   uint8_t packet[TU_WDDM_MAX_RENDER_COMMAND_SIZE];
   struct fd_wddm_submit_bo packet_bos[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   struct fd_wddm_submit_cmd packet_cmds[TU_WDDM_MAX_SUBMIT_COMMANDS];
   struct tu_wddm_render_reference references[TU_WDDM_MAX_RENDER_ALLOCATIONS];
};

struct tu_wddm_submit {
   struct util_dynarray entries;
   struct util_dynarray references;
   uint16_t reference_index[TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE];
   bool failed;
};

/* Hash a BO pointer for the submit-local open-addressing table. */
static uint32_t
tu_wddm_submit_reference_hash(const struct tu_bo *bo)
{
   uintptr_t key = reinterpret_cast<uintptr_t>(bo) >> 4;
   key ^= key >> 17;
   return static_cast<uint32_t>(key * 2654435761u);
}

/* Look up one BO and optionally return the first empty slot for insertion. */
static int
tu_wddm_submit_reference_lookup(const struct tu_wddm_submit *submit,
                                const struct tu_bo *bo,
                                uint32_t *empty_slot)
{
   const uint32_t mask = TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE - 1;
   uint32_t slot = tu_wddm_submit_reference_hash(bo) & mask;
   const struct tu_wddm_submit_reference *refs =
      static_cast<const struct tu_wddm_submit_reference *>(submit->references.data);

   for (uint32_t probe = 0; probe < TU_WDDM_SUBMIT_REFERENCE_INDEX_SIZE; probe++) {
      const uint16_t encoded_index = submit->reference_index[slot];
      if (encoded_index == 0) {
         if (empty_slot != NULL)
            *empty_slot = slot;
         return -1;
      }

      const uint32_t index = static_cast<uint32_t>(encoded_index) - 1;
      if (refs[index].bo == bo)
         return static_cast<int>(index);

      slot = (slot + 1) & mask;
   }

   if (empty_slot != NULL)
      *empty_slot = UINT32_MAX;
   return -1;
}

static inline struct tu_wddm_sync *
tu_wddm_sync_from_vk(struct vk_sync *sync)
{
   return container_of(sync, struct tu_wddm_sync, base);
}

static bool
tu_wddm_sync_is_current(const struct tu_wddm_sync *sync)
{
   return sync != NULL && sync->owner != NULL && sync->owner->wddm_initialized &&
          sync->context != NULL && sync->context == &sync->owner->wddm_context &&
          sync->context->device != NULL &&
          sync->context->handle != 0 && sync->reset_generation != 0 &&
          sync->reset_generation == sync->context->info.ResetGeneration &&
          sync->reset_generation ==
             sync->context->device->adapter.private_info.ResetGeneration;
}

static bool
tu_wddm_sync_belongs_to_device(const struct tu_wddm_sync *sync,
                               struct vk_device *base_device)
{
   if (base_device == NULL || !tu_wddm_sync_is_current(sync))
      return false;

   const struct tu_device *device = container_of(base_device, struct tu_device, vk);
   return sync->owner == device;
}

static VkResult
tu_wddm_sync_init(struct vk_device *_device, struct vk_sync *base,
                  uint64_t initial_value)
{
   struct tu_device *device = container_of(_device, struct tu_device, vk);
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);

   sync->owner = device;
   sync->context = &device->wddm_context;
   sync->reset_generation = sync->context->info.ResetGeneration;
   tu_wddm_sync_state_set(sync,
                          initial_value != 0 ? TU_WDDM_SYNC_SIGNALED : 0);
   return sync->reset_generation != 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static void
tu_wddm_sync_finish(struct vk_device *device, struct vk_sync *base)
{
   (void)device;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   sync->owner = NULL;
   sync->context = NULL;
   sync->reset_generation = 0;
   tu_wddm_sync_state_set(sync, 0);
}

static VkResult
tu_wddm_sync_signal(struct vk_device *device, struct vk_sync *base,
                    uint64_t value)
{
   (void)value;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   if (!tu_wddm_sync_belongs_to_device(sync, device))
      return VK_ERROR_DEVICE_LOST;

   tu_wddm_sync_state_set(sync, TU_WDDM_SYNC_SIGNALED);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_sync_reset(struct vk_device *device, struct vk_sync *base)
{
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   if (!tu_wddm_sync_belongs_to_device(sync, device))
      return VK_ERROR_DEVICE_LOST;

   tu_wddm_sync_state_set(sync, 0);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_sync_move(struct vk_device *device, struct vk_sync *dst_base,
                  struct vk_sync *src_base)
{
   struct tu_wddm_sync *dst = tu_wddm_sync_from_vk(dst_base);
   struct tu_wddm_sync *src = tu_wddm_sync_from_vk(src_base);
   if (!tu_wddm_sync_belongs_to_device(src, device) ||
       !tu_wddm_sync_belongs_to_device(dst, device) ||
       src->owner != dst->owner)
      return VK_ERROR_DEVICE_LOST;

   const uint64_t state = p_atomic_xchg(&src->state, 0);
   dst->context = src->context;
   dst->reset_generation = src->reset_generation;
   tu_wddm_sync_state_set(dst, state);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_sync_wait(struct vk_device *_device, struct vk_sync *base,
                  uint64_t wait_value, enum vk_sync_wait_flags wait_flags,
                  uint64_t abs_timeout_ns)
{
   (void)wait_value;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);

   tu_wddm_fence_poll_wait poll_wait;
   for (;;) {
      if (!tu_wddm_sync_belongs_to_device(sync, _device))
         return vk_device_set_lost(_device,
                                   "WDDM sync wait observed a stale or foreign context");

      const uint64_t state = tu_wddm_sync_state_read(sync);
      const uint32_t fence = static_cast<uint32_t>(state);
      const bool signaled = (state & TU_WDDM_SYNC_SIGNALED) != 0;

      if (signaled || ((wait_flags & VK_SYNC_WAIT_PENDING) && fence != 0)) {
         if (!tu_wddm_device_execution_active(sync->context->device))
            return vk_device_set_lost(
               _device, "WDDM sync wait observed an inactive device");
         return VK_SUCCESS;
      }

      if (fence != 0) {
         uint32_t completed = 0;
         if (!tu_wddm_context_get_completed_fence(sync->context, &completed))
            return vk_device_set_lost(_device,
                                      "WDDM sync fence query failed");
         if (!tu_wddm_device_execution_active(sync->context->device))
            return vk_device_set_lost(
               _device, "WDDM sync wait observed an inactive device");
         if (completed == fence || tu_wddm_fence_after(completed, fence)) {
            if (static_cast<uint64_t>(p_atomic_cmpxchg(
                   &sync->state, state, TU_WDDM_SYNC_SIGNALED)) == state)
               return VK_SUCCESS;
            continue;
         }
      } else if (!tu_wddm_device_execution_active(sync->context->device))
         return vk_device_set_lost(
            _device, "WDDM sync wait observed an inactive device");

      const uint64_t now = static_cast<uint64_t>(os_time_get_nano());
      if (abs_timeout_ns != OS_TIMEOUT_INFINITE && now >= abs_timeout_ns)
         return VK_TIMEOUT;
      poll_wait.wait(abs_timeout_ns == OS_TIMEOUT_INFINITE
                        ? UINT64_MAX : abs_timeout_ns - now);
   }
}

static VkResult
tu_wddm_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                       const struct vk_sync_wait *waits,
                       enum vk_sync_wait_flags wait_flags,
                       uint64_t abs_timeout_ns)
{
   if (wait_count == 0)
      return VK_SUCCESS;

   if (wait_flags & VK_SYNC_WAIT_ANY) {
      tu_wddm_fence_poll_wait poll_wait;
      for (;;) {
         for (uint32_t i = 0; i < wait_count; i++) {
            if (waits[i].sync == NULL)
               continue;
            VkResult result = tu_wddm_sync_wait(device, waits[i].sync,
                                                waits[i].wait_value,
                                                static_cast<enum vk_sync_wait_flags>(
                                                   wait_flags & ~VK_SYNC_WAIT_ANY),
                                                0);
            if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
               return result;
         }
         const uint64_t now = static_cast<uint64_t>(os_time_get_nano());
         if (abs_timeout_ns != OS_TIMEOUT_INFINITE && now >= abs_timeout_ns)
            return VK_TIMEOUT;
         poll_wait.wait(abs_timeout_ns == OS_TIMEOUT_INFINITE
                           ? UINT64_MAX : abs_timeout_ns - now);
      }
   }

   for (uint32_t i = 0; i < wait_count; i++) {
      if (waits[i].sync == NULL)
         continue;
      VkResult result = tu_wddm_sync_wait(device, waits[i].sync,
                                          waits[i].wait_value, wait_flags,
                                          abs_timeout_ns);
      if (result != VK_SUCCESS)
         return result;
   }
   return VK_SUCCESS;
}

static const struct vk_sync_type tu_wddm_sync_type = {
   .size = sizeof(struct tu_wddm_sync),
   .features = (enum vk_sync_features)(VK_SYNC_FEATURE_BINARY |
                                       VK_SYNC_FEATURE_GPU_WAIT |
                                       VK_SYNC_FEATURE_GPU_MULTI_WAIT |
                                       VK_SYNC_FEATURE_CPU_WAIT |
                                       VK_SYNC_FEATURE_CPU_RESET |
                                       VK_SYNC_FEATURE_CPU_SIGNAL |
                                       VK_SYNC_FEATURE_WAIT_ANY |
                                       VK_SYNC_FEATURE_WAIT_PENDING),
   .init = tu_wddm_sync_init,
   .finish = tu_wddm_sync_finish,
   .signal = tu_wddm_sync_signal,
   .reset = tu_wddm_sync_reset,
   .move = tu_wddm_sync_move,
   .wait = tu_wddm_sync_wait,
   .wait_many = tu_wddm_sync_wait_many,
};

/* ------------------------------------------------------------------------- */
/* WDDM-backed Turnip kernel interface                                       */

static inline bool
tu_wddm_bo_valid(const struct tu_bo *bo)
{
   return bo != NULL && bo->gem_handle != 0 && bo->wddm_allocation != NULL &&
          bo->wddm_allocation->handle != 0 && bo->wddm_allocation->context != NULL;
}

static inline bool
tu_wddm_bo_valid_for_device(const struct tu_device *dev, const struct tu_bo *bo)
{
   return dev != NULL && tu_wddm_bo_valid(bo) &&
          !bo->wddm_allocation->retirement.pending &&
          bo->wddm_allocation->context->device == &dev->wddm_device;
}

static void
tu_wddm_remove_bo_locked(struct tu_device *dev, struct tu_bo *bo)
{
   for (uint32_t i = 0; i < dev->wddm_bo_count; i++) {
      if (dev->wddm_bos[i] != bo)
         continue;
      dev->wddm_bos[i] = dev->wddm_bos[--dev->wddm_bo_count];
      return;
   }
}

static uint32_t
tu_wddm_alloc_token_locked(struct tu_device *dev)
{
   /* Token zero is reserved.  Keep the search bounded; a process cannot have
    * anywhere near this many live allocations and a wrapped token must never
    * alias a live sparse-array slot. */
   for (uint32_t attempts = 0; attempts < (1u << 20); attempts++) {
      uint32_t token = dev->wddm_next_handle++;
      if (token == 0)
         continue;
      struct tu_bo *slot = tu_device_lookup_bo(dev, token);
      if (slot->refcnt == 0 && slot->wddm_allocation == NULL)
         return token;
   }
   return 0;
}

static bool
tu_wddm_add_bo_locked(struct tu_device *dev, struct tu_bo *bo)
{
   if (dev->wddm_bo_count >= TU_WDDM_MAX_RENDER_ALLOCATIONS)
      return false;

   if (dev->wddm_bo_count == dev->wddm_bo_capacity) {
      uint32_t capacity = dev->wddm_bo_capacity ? dev->wddm_bo_capacity * 2 : 64;
      struct tu_bo **bos = (struct tu_bo **)vk_realloc(
         &dev->vk.alloc, dev->wddm_bos, capacity * sizeof(*bos), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (bos == NULL)
         return false;
      dev->wddm_bos = bos;
      dev->wddm_bo_capacity = capacity;
   }
   dev->wddm_bos[dev->wddm_bo_count++] = bo;
   return true;
}

/* Reap a bounded number of slots under wddm_mutex, never under bo_mutex.
 * One fence query serves the whole pass; busy objects remain quarantined.
 * A rotating cursor prevents a busy owner from starving later ready objects. */
static bool
tu_wddm_reap_retired_bos_locked(struct tu_device *dev, uint32_t budget)
{
   if (dev->wddm_retired_count == 0)
      return true;

   uint32_t completed = 0;
   bool queried = false;
   mtx_lock(&dev->bo_mutex);
   const uint32_t limit = MIN2(budget, dev->wddm_bo_count);
   mtx_unlock(&dev->bo_mutex);
   for (uint32_t scanned = 0; scanned < limit && dev->wddm_retired_count; scanned++) {
      mtx_lock(&dev->bo_mutex);
      if (dev->wddm_bo_count == 0) {
         mtx_unlock(&dev->bo_mutex);
         return false;
      }
      dev->wddm_reap_cursor %= dev->wddm_bo_count;
      struct tu_bo *bo = dev->wddm_bos[dev->wddm_reap_cursor++];
      struct tu_wddm_allocation *allocation = bo->wddm_allocation;
      mtx_unlock(&dev->bo_mutex);
      if (allocation == NULL || !allocation->retirement.pending)
         continue;

      if (allocation->retirement.reset_generation != dev->wddm_context.info.ResetGeneration ||
          allocation->retirement.reset_generation !=
             dev->wddm_device.adapter.private_info.ResetGeneration) {
         dev->wddm_lifetime_stats.epoch_failures++;
         vk_device_set_lost(&dev->vk, "WDDM retired BO belongs to an old reset epoch");
         return false;
      }
      if (!allocation->retirement.fence_ready && allocation->retirement.fence != 0) {
         if (!queried) {
            dev->wddm_lifetime_stats.fence_queries++;
            if (!tu_wddm_context_get_completed_fence(&dev->wddm_context, &completed) ||
                !tu_wddm_device_execution_active(&dev->wddm_device)) {
               dev->wddm_lifetime_stats.query_failures++;
               vk_device_set_lost(&dev->vk, "WDDM deferred retirement query failed");
               return false;
            }
            queried = true;
         }
         if (!tu_wddm_retirement_observe(&allocation->retirement, completed)) {
            dev->wddm_lifetime_stats.fence_pending++;
            continue;
         }
      }

      if (allocation->locked && !tu_wddm_allocation_unlock(allocation)) {
         dev->wddm_lifetime_stats.unlock_failures++;
         vk_device_set_lost(&dev->vk, "failed to unlock a retired WDDM allocation");
         return false;
      }
      bo->map = NULL;
      const uint64_t iova = allocation->private_info.RequestedIova;
      const uint64_t size = allocation->vma_size;
      const NTSTATUS status = tu_wddm_allocation_try_destroy(allocation);
      if (status == TU_WDDM_STATUS_DEVICE_BUSY ||
          status == TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY) {
         dev->wddm_lifetime_stats.destroy_busy++;
         continue;
      }
      if (status != TU_WDDM_STATUS_SUCCESS) {
         dev->wddm_lifetime_stats.destroy_failures++;
         vk_device_set_lost(&dev->vk, "failed to destroy a retired WDDM allocation");
         return false;
      }

      /* Success acknowledges transfer to VidMm.  KMD still retains its own
       * native IOVA range until host detach; a subsequent create may return
       * DEVICE_BUSY and must use the existing guarded create retry path. */
      tu_bo_release_heap_accounting(dev, bo);
      tu_debug_bos_del(dev, bo);
      tu_dump_bo_del(dev, bo);
      mtx_lock(&dev->bo_mutex);
      tu_wddm_remove_bo_locked(dev, bo);
      memset(bo, 0, sizeof(*bo));
      dev->wddm_reap_cursor--;
      mtx_unlock(&dev->bo_mutex);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, size);
      mtx_unlock(&dev->vma_mutex);
      vk_free(&dev->vk.alloc, allocation);
      dev->wddm_retired_count--;
      dev->wddm_lifetime_stats.reaped++;
   }
   return true;
}

/* Report aggregate reasons once at successful device teardown, not per BO or
 * per submit.  Pending/busy values count observations, not elapsed GPU time. */
static void
tu_wddm_report_lifetime_stats(struct tu_device *dev)
{
   const struct tu_wddm_lifetime_stats *s = &dev->wddm_lifetime_stats;
   tu_wddm_diag("lifetime queued=%llu reaped=%llu pending=%u peak=%llu\n"
                "  fence_queries=%llu fence_pending=%llu destroy_busy=%llu\n"
                "  unlock_failures=%llu destroy_failures=%llu query_failures=%llu epoch_failures=%llu\n"
                "  scratch_allocations=%llu scratch_reuses=%llu",
                (unsigned long long)s->queued, (unsigned long long)s->reaped,
                dev->wddm_retired_count, (unsigned long long)s->pending_peak,
                (unsigned long long)s->fence_queries, (unsigned long long)s->fence_pending,
                (unsigned long long)s->destroy_busy, (unsigned long long)s->unlock_failures,
                (unsigned long long)s->destroy_failures, (unsigned long long)s->query_failures,
                (unsigned long long)s->epoch_failures, (unsigned long long)s->scratch_allocations,
                (unsigned long long)s->scratch_reuses);
}

static VkResult
tu_wddm_device_init(struct tu_device *dev)
{
   struct tu_physical_device *physical = dev->physical_device;
   struct tu_instance *instance = physical->instance;

   dev->fd = -1;
   dev->wddm_next_handle = 1;
   dev->wddm_next_fence = 1;
   dev->wddm_pending_submission_upper_bound = 0;
   dev->wddm_teardown_failed = false;
   dev->wddm_bos = NULL;
   dev->wddm_bo_count = 0;
   dev->wddm_bo_capacity = 0;
   dev->wddm_retired_count = 0;
   dev->wddm_reap_cursor = 0;
   dev->wddm_submit_scratch = NULL;
   memset(&dev->wddm_lifetime_stats, 0, sizeof(dev->wddm_lifetime_stats));
   /* Keep the new lifetime policy opt-in until target-device A/B and reset
    * testing is complete.  This is read once, never in the submit hot path. */
   dev->wddm_deferred_bo_destroy =
      debug_get_bool_option("TU_WDDM_DEFERRED_BO_DESTROY", false);

   if (!instance->wddm_runtime_initialized ||
       !tu_wddm_device_open(&instance->wddm_runtime, &physical->wddm_adapter,
                            &dev->wddm_device)) {
      if (dev->wddm_device.handle != 0 || dev->wddm_device.adapter.handle != 0) {
         /* There is no context owner on this path, so a failed close can be
          * retried by the device-finish hook without destroying a live
          * context. */
         tu_wddm_device_close(&dev->wddm_device);
      }
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                               "failed to open WDDM adapter device");
   }

   if (!tu_wddm_context_open(&dev->wddm_device, &dev->wddm_context)) {
      if (dev->wddm_context.handle != 0 &&
          !tu_wddm_context_close(&dev->wddm_context))
         return vk_startup_errorf(instance, VK_ERROR_DEVICE_LOST,
                                  "failed to close WDDM native context");
      /* The device may only be closed after the context owner is gone. */
      if ((dev->wddm_device.handle != 0 ||
           dev->wddm_device.adapter.handle != 0) &&
          !tu_wddm_device_close(&dev->wddm_device))
         return vk_startup_errorf(instance, VK_ERROR_DEVICE_LOST,
                                  "failed to close WDDM adapter device");
      return vk_startup_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                               "failed to open WDDM native context");
   }

   dev->va_start = dev->wddm_context.info.VaStart;
   dev->va_size = dev->wddm_context.info.VaSize;
   dev->wddm_initialized = true;
   return VK_SUCCESS;
}

static void
tu_wddm_device_finish(struct tu_device *dev)
{
   if (!dev->wddm_initialized && dev->wddm_context.handle == 0 &&
       dev->wddm_device.handle == 0 && dev->wddm_device.adapter.handle == 0)
      return;

   tu_wddm_diag("device_finish begin initialized=%u bos=%u context=%u device=%u adapter=%u",
                static_cast<unsigned>(dev->wddm_initialized), dev->wddm_bo_count,
                static_cast<unsigned>(dev->wddm_context.handle),
                static_cast<unsigned>(dev->wddm_device.handle),
                static_cast<unsigned>(dev->wddm_device.adapter.handle));

   /* This is the final Vulkan-device hook.  A failed KMT operation cannot be
    * retried after tu_DestroyDevice releases the outer device, so preserve the
    * complete owner graph on every failure path.  The retained KMT objects are
    * intentionally process-lifetime owned; leaking them is safer than freeing
    * UMD bookkeeping/VMA state while a live allocation or parent handle still
    * exists. */
   const bool submissions_retired =
      tu_wddm_context_wait_submissions(
         &dev->wddm_context,
         dev->wddm_deferred_bo_destroy ? TU_WDDM_DESTROY_WAIT_TIMEOUT_NS : UINT64_MAX);
   if (!submissions_retired) {
      tu_wddm_diag("device_finish failed waiting submissions fence=%u",
                   dev->wddm_context.last_submitted_fence);
      dev->wddm_teardown_failed = true;
      vk_device_set_lost(&dev->vk, "failed to retire WDDM queue work");
      return;
   }

   while (dev->wddm_bo_count != 0) {
      struct tu_bo *bo = dev->wddm_bos[dev->wddm_bo_count - 1];
      struct tu_wddm_allocation *allocation = bo->wddm_allocation;
      if (allocation == NULL) {
         /* A zero sparse-array slot is never an owned allocation.  Drop only
          * the bookkeeping entry; a non-zero handle is retained below on all
          * failure paths. */
         dev->wddm_bo_count--;
         continue;
      }
      const bool was_retired = allocation->retirement.pending;
      const uint64_t allocation_iova = allocation->private_info.RequestedIova;
      const uint64_t allocation_size = allocation->vma_size != 0
                                          ? allocation->vma_size
                                          : (allocation->private_info.Size + UINT64_C(4095)) &
                                               ~UINT64_C(4095);

      if (allocation->locked && !tu_wddm_allocation_unlock(allocation)) {
         tu_wddm_diag("device_finish failed unlocking allocation=%u",
                      static_cast<unsigned>(allocation->handle));
         dev->wddm_teardown_failed = true;
         vk_device_set_lost(&dev->vk,
                            "failed to unlock WDDM allocation during teardown");
         return;
      }
      bo->map = NULL;
      if (allocation->handle != 0 &&
          !tu_wddm_allocation_destroy(allocation)) {
         tu_wddm_diag("device_finish failed destroying allocation=%u status=0x%08x",
                      static_cast<unsigned>(allocation->handle),
                      allocation->last_destroy_status);
         dev->wddm_teardown_failed = true;
         vk_device_set_lost(&dev->vk,
                            "failed to destroy WDDM allocation during teardown");
         return;
      }

      tu_bo_release_heap_accounting(dev, bo);
      tu_debug_bos_del(dev, bo);
      tu_dump_bo_del(dev, bo);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, allocation_iova, allocation_size);
      mtx_unlock(&dev->vma_mutex);
      vk_free(&dev->vk.alloc, allocation);
      memset(bo, 0, sizeof(*bo));
      dev->wddm_bo_count--;
      if (was_retired) {
         dev->wddm_retired_count--;
         dev->wddm_lifetime_stats.reaped++;
      }
   }

   vk_free(&dev->vk.alloc, dev->wddm_bos);
   dev->wddm_bos = NULL;
   dev->wddm_bo_capacity = 0;

   bool context_closed = dev->wddm_context.handle == 0;
   if (!context_closed) {
      context_closed = tu_wddm_context_close(&dev->wddm_context);
      if (!context_closed) {
         dev->wddm_teardown_failed = true;
         tu_wddm_diag("device_finish failed closing context status=0x%08x attempts=%u handle=%u",
                      dev->wddm_context.last_destroy_status,
                      dev->wddm_context.destroy_attempt_count,
                      static_cast<unsigned>(dev->wddm_context.handle));
         mesa_loge("failed to close WDDM context: status=0x%08x attempts=%u handle=%u",
                   dev->wddm_context.last_destroy_status,
                   dev->wddm_context.destroy_attempt_count,
                   dev->wddm_context.handle);
         vk_device_set_lost(&dev->vk, "failed to close WDDM context");
         return;
      }
   }
   if (context_closed &&
       (dev->wddm_device.handle != 0 ||
        dev->wddm_device.adapter.handle != 0)) {
      if (!tu_wddm_device_close(&dev->wddm_device)) {
         dev->wddm_teardown_failed = true;
         tu_wddm_diag("device_finish failed closing device=%u adapter=%u",
                      static_cast<unsigned>(dev->wddm_device.handle),
                      static_cast<unsigned>(dev->wddm_device.adapter.handle));
         vk_device_set_lost(&dev->vk, "failed to close WDDM device");
         return;
      }
   }

   tu_wddm_report_lifetime_stats(dev);
   vk_free(&dev->vk.alloc, dev->wddm_submit_scratch);
   dev->wddm_submit_scratch = NULL;
   memset(&dev->wddm_context, 0, sizeof(dev->wddm_context));
   memset(&dev->wddm_device, 0, sizeof(dev->wddm_device));
   dev->wddm_initialized = false;
   dev->wddm_teardown_failed = false;
   tu_wddm_diag("device_finish complete");
}

static int
tu_wddm_device_get_gpu_timestamp(struct tu_device *dev, uint64_t *ts)
{
   (void)dev;
   if (ts != NULL)
      *ts = 0;
   return -ENOSYS;
}

static int
tu_wddm_device_get_suspend_count(struct tu_device *dev, uint64_t *suspend_count)
{
   if (suspend_count != NULL)
      *suspend_count = 0;
   if (dev == NULL || suspend_count == NULL || !dev->wddm_initialized ||
       dev->wddm_context.handle == 0)
      return -EINVAL;

   /* The private endpoint has no separate suspend counter.  Refreshing the
    * context snapshot gives Perfetto the same epoch semantics as the DRM
    * MSM SUSPENDS parameter: a reset/resume invalidates the old GPU clock,
    * while ordinary submissions leave the value unchanged. */
   if (!tu_wddm_context_get_info(&dev->wddm_context))
      return -ENODEV;
   *suspend_count = dev->wddm_context.info.ResetGeneration;
   return 0;
}

static VkResult
tu_wddm_device_check_status(struct tu_device *dev)
{
   uint32_t completed = 0;
   if (!dev->wddm_initialized ||
       !tu_wddm_context_get_completed_fence(&dev->wddm_context, &completed) ||
       !tu_wddm_device_execution_active(&dev->wddm_device))
      return vk_device_set_lost(&dev->vk,
                                "WDDM context or device execution query failed");
   return VK_SUCCESS;
}

static int
tu_wddm_submitqueue_new(struct tu_device *dev, struct tu_queue *queue)
{
   if (queue->type != TU_QUEUE_GFX ||
       dev->physical_device->submitqueue_priority_count != 1 ||
       !tu_wddm_submitqueue_priority_is_supported(queue->priority) ||
       dev->wddm_context.info.SubmitQueueId == 0)
      return -EINVAL;
   queue->msm_queue_id = dev->wddm_context.info.SubmitQueueId;
   return 0;
}

static void
tu_wddm_submitqueue_close(struct tu_device *dev, struct tu_queue *queue)
{
   (void)dev;
   queue->msm_queue_id = 0;
}

static VkResult
tu_wddm_bo_init(struct tu_device *dev, struct vk_object_base *base,
                struct tu_bo **out_bo, uint64_t size, uint64_t client_iova,
                VkMemoryPropertyFlags mem_property,
                enum tu_bo_alloc_flags flags,
                struct tu_sparse_vma *lazy_vma, const char *name)
{
   if (out_bo != NULL)
      *out_bo = NULL;
   if (out_bo == NULL || lazy_vma != NULL || size == 0 ||
       (flags & (TU_BO_ALLOC_DMABUF | TU_BO_ALLOC_SHAREABLE |
                 TU_BO_ALLOC_IMPLICIT_SYNC)) != 0)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);

   if ((mem_property & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0)
      return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);

   if (size > UINT64_MAX - UINT64_C(4095))
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   const uint64_t vma_size = (size + UINT64_C(4095)) & ~UINT64_C(4095);
   if (vma_size == 0)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   mtx_lock(&dev->wddm_mutex);
   const bool reaped = tu_wddm_reap_retired_bos_locked(dev, TU_WDDM_MAX_RENDER_ALLOCATIONS);
   mtx_unlock(&dev->wddm_mutex);
   if (!reaped)
      return vk_error(dev, VK_ERROR_DEVICE_LOST);

   uint64_t iova = 0;
   mtx_lock(&dev->vma_mutex);
   if (client_iova != 0) {
      if (!util_vma_heap_alloc_addr(&dev->vma, client_iova, vma_size)) {
         mtx_unlock(&dev->vma_mutex);
         return vk_error(dev, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS);
      }
      iova = client_iova;
   } else {
      iova = util_vma_heap_alloc(&dev->vma, vma_size, os_page_size);
   }
   mtx_unlock(&dev->vma_mutex);
   if (iova == 0)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   struct tu_wddm_allocation *allocation = (struct tu_wddm_allocation *)vk_zalloc(
      &dev->vk.alloc, sizeof(*allocation), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (allocation == NULL) {
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, vma_size);
      mtx_unlock(&dev->vma_mutex);
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   struct tu_wddm_allocation_desc desc = {
      .size = size,
      .alignment = os_page_size,
      .requested_iova = iova,
      .flags = VIOGPU_WDDM_ALLOCATION_NATIVE |
               VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE |
               ((flags & TU_BO_ALLOC_GPU_READ_ONLY) != 0
                   ? VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY : 0),
   };

   /* Reserve the sparse-array slot before asking the KMD to create the
    * allocation.  The WDDM owner stays unpublished until CreateAllocation
    * returns, so a concurrent residency snapshot can skip this placeholder. */
   mtx_lock(&dev->bo_mutex);
   const bool capacity_available =
      dev->wddm_bo_count < TU_WDDM_MAX_RENDER_ALLOCATIONS;
   uint32_t token = capacity_available ? tu_wddm_alloc_token_locked(dev) : 0;
   struct tu_bo *bo = token ? tu_device_lookup_bo(dev, token) : NULL;
   bool added = bo != NULL && tu_wddm_add_bo_locked(dev, bo);
   if (!added) {
      mtx_unlock(&dev->bo_mutex);
      vk_free(&dev->vk.alloc, allocation);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, vma_size);
      mtx_unlock(&dev->vma_mutex);
      return vk_error(dev, capacity_available
                              ? VK_ERROR_OUT_OF_HOST_MEMORY
                              : VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   *bo = (struct tu_bo) {
      .gem_handle = token,
      .size = size,
      .iova = iova,
      /* Account the debug name only after CreateAllocation succeeds. */
      .name = NULL,
      .refcnt = 1,
      .gpu_read_only = (flags & TU_BO_ALLOC_GPU_READ_ONLY) != 0,
      .base = base,
      .wddm_allocation = NULL,
   };
   mtx_unlock(&dev->bo_mutex);

   const bool allocation_created =
      tu_wddm_allocation_create(&dev->wddm_context, &desc, allocation);

   mtx_lock(&dev->bo_mutex);
   if (allocation_created || allocation->handle != 0) {
      bo->wddm_allocation = allocation;
   } else {
      tu_wddm_remove_bo_locked(dev, bo);
      memset(bo, 0, sizeof(*bo));
   }
   mtx_unlock(&dev->bo_mutex);

   if (!allocation_created) {
      if (allocation->handle != 0) {
         /* CreateAllocation returned a handle and its compensating destroy
          * failed.  Keep the sparse-array slot, VMA, and final BO reference as
          * the retry owner instead of orphaning a live KMT allocation. */
         return vk_device_set_lost(
            &dev->vk, "failed to roll back partial WDDM allocation creation");
      }
      vk_free(&dev->vk.alloc, allocation);
      mtx_lock(&dev->vma_mutex);
      util_vma_heap_free(&dev->vma, iova, vma_size);
      mtx_unlock(&dev->vma_mutex);
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   bo->name = tu_debug_bos_add(dev, size, name);
   tu_dump_bo_init(dev, bo);
   *out_bo = bo;
   return VK_SUCCESS;
}

static VkResult
tu_wddm_bo_init_dmabuf(struct tu_device *dev, struct tu_bo **out_bo,
                       uint64_t size, enum tu_bo_alloc_flags flags, int prime_fd)
{
   (void)size;
   (void)flags;
   (void)prime_fd;
   if (out_bo != NULL)
      *out_bo = NULL;
   return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
}

static int
tu_wddm_bo_export_dmabuf(struct tu_device *dev, struct tu_bo *bo)
{
   (void)dev;
   (void)bo;
   errno = ENOSYS;
   return -1;
}

static VkResult
tu_wddm_bo_map(struct tu_device *dev, struct tu_bo *bo, void *placed_addr)
{
   if (placed_addr != NULL || !tu_wddm_bo_valid_for_device(dev, bo))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   void *map = NULL;
   if (!tu_wddm_allocation_lock(bo->wddm_allocation, &map))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   bo->map = map;
   TU_RMV(bo_map, dev, bo);
   return VK_SUCCESS;
}

static VkResult
tu_wddm_bo_unmap(struct tu_device *dev, struct tu_bo *bo, bool reserve)
{
   if (!tu_wddm_bo_valid_for_device(dev, bo))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);

   /* The KMT Unlock drops only the CPU mapping.  The WDDM allocation and its
    * requested GPU IOVA stay owned until bo_finish, so the address reservation
    * required by VK_MEMORY_UNMAP_RESERVE_BIT_EXT is already preserved. */
   (void)reserve;
   if (!tu_wddm_allocation_unlock(bo->wddm_allocation))
      return vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
   bo->map = NULL;
   return VK_SUCCESS;
}

static void
tu_wddm_bo_allow_dump(struct tu_device *dev, struct tu_bo *bo)
{
   if (tu_wddm_bo_valid_for_device(dev, bo))
      bo->dump = true;
}

static void
tu_wddm_bo_finish(struct tu_device *dev, struct tu_bo *bo)
{
   if (!tu_wddm_bo_valid_for_device(dev, bo) || p_atomic_read(&bo->refcnt) <= 0 ||
       !p_atomic_dec_zero(&bo->refcnt))
      return;

   /* The callback has no error return.  Keep one final reference whenever a
    * KMD operation fails so device teardown (or a later retry) still owns the
    * allocation and its VMA. */
   const auto restore_owner = [bo]() {
      p_atomic_set(&bo->refcnt, 1);
   };

   /* Generic queue preparation already owns submit_mutex and can release a
    * replaced internal BO.  Use the WDDM-specific lock to serialize this
    * destruction with the live-BO residency snapshot and Render call without
    * recursively acquiring submit_mutex. */
   mtx_lock(&dev->wddm_mutex);
   if (dev->wddm_deferred_bo_destroy) {
      restore_owner();
      bo->wddm_allocation->retirement = {
         .reset_generation = dev->wddm_context.info.ResetGeneration,
         .fence = dev->wddm_context.last_submitted_fence,
         .pending = true,
         .fence_ready = dev->wddm_context.last_submitted_fence == 0,
      };
      /* VkDeviceMemory may disappear now.  Keep only the driver's ownership
       * graph, never a pointer to its freed Vulkan object or dump membership. */
      bo->base = NULL;
      tu_dump_bo_del(dev, bo);
      dev->wddm_retired_count++;
      dev->wddm_lifetime_stats.queued++;
      dev->wddm_lifetime_stats.pending_peak =
         MAX2(dev->wddm_lifetime_stats.pending_peak, dev->wddm_retired_count);
      tu_wddm_reap_retired_bos_locked(dev, 16);
      mtx_unlock(&dev->wddm_mutex);
      return;
   }
   if (!tu_wddm_context_wait_submissions(&dev->wddm_context, UINT64_MAX)) {
      vk_device_set_lost(&dev->vk, "failed to retire WDDM queue work");
      restore_owner();
      mtx_unlock(&dev->wddm_mutex);
      return;
   }

   if (bo->wddm_allocation->locked) {
      if (!tu_wddm_allocation_unlock(bo->wddm_allocation)) {
         vk_device_set_lost(&dev->vk, "failed to unlock WDDM allocation");
         restore_owner();
         mtx_unlock(&dev->wddm_mutex);
         return;
      }
      bo->map = NULL;
   }

   struct tu_wddm_allocation *allocation = bo->wddm_allocation;
   const uint64_t allocation_iova = allocation->private_info.RequestedIova;
   const uint64_t allocation_size = allocation->vma_size != 0
                                       ? allocation->vma_size
                                       : (allocation->private_info.Size + UINT64_C(4095)) &
                                            ~UINT64_C(4095);
   if (!tu_wddm_allocation_destroy(allocation)) {
      vk_device_set_lost(&dev->vk, "failed to destroy WDDM allocation");
      restore_owner();
      mtx_unlock(&dev->wddm_mutex);
      return;
   }

   tu_bo_release_heap_accounting(dev, bo);
   tu_debug_bos_del(dev, bo);
   tu_dump_bo_del(dev, bo);

   mtx_lock(&dev->bo_mutex);
   tu_wddm_remove_bo_locked(dev, bo);
   memset(bo, 0, sizeof(*bo));
   mtx_unlock(&dev->bo_mutex);

   mtx_lock(&dev->vma_mutex);
   util_vma_heap_free(&dev->vma, allocation_iova, allocation_size);
   mtx_unlock(&dev->vma_mutex);
   vk_free(&dev->vk.alloc, allocation);
   mtx_unlock(&dev->wddm_mutex);
}

static void
tu_wddm_bo_set_metadata(struct tu_device *dev, struct tu_bo *bo,
                        void *metadata, uint32_t metadata_size)
{
   if (!tu_wddm_bo_valid_for_device(dev, bo) ||
       metadata == NULL || metadata_size == 0 ||
       metadata_size > TU_WDDM_MAX_BO_METADATA_SIZE)
      return;

   void *copy = malloc(metadata_size);
   if (copy == NULL)
      return;

   memcpy(copy, metadata, metadata_size);
   free(bo->wddm_allocation->metadata);
   bo->wddm_allocation->metadata = copy;
   bo->wddm_allocation->metadata_size = metadata_size;
}

static int
tu_wddm_bo_get_metadata(struct tu_device *dev, struct tu_bo *bo,
                        void *metadata, uint32_t metadata_size)
{
   if (!tu_wddm_bo_valid_for_device(dev, bo) ||
       metadata == NULL || metadata_size == 0)
      return -EINVAL;

   const struct tu_wddm_allocation *allocation = bo->wddm_allocation;
   if (allocation->metadata == NULL || allocation->metadata_size == 0)
      return -ENOENT;
   if (metadata_size < allocation->metadata_size)
      return -ENOSPC;

   memcpy(metadata, allocation->metadata, allocation->metadata_size);
   return 0;
}

static bool
tu_wddm_submit_add_reference(struct tu_device *device,
                             struct tu_wddm_submit *submit,
                             struct tu_bo *bo, uint32_t access)
{
   if (device == NULL || submit == NULL)
      return false;

   if (!tu_wddm_bo_valid_for_device(device, bo) || access == 0 ||
       (access & ~(TU_SUBMIT_BO_ACCESS_READ | TU_SUBMIT_BO_ACCESS_WRITE)) != 0 ||
       ((access & TU_SUBMIT_BO_ACCESS_WRITE) != 0 && bo->gpu_read_only)) {
      submit->failed = true;
      return false;
   }

   uint32_t index_slot = UINT32_MAX;
   const int existing_index =
      tu_wddm_submit_reference_lookup(submit, bo, &index_slot);
   if (existing_index >= 0) {
      struct tu_wddm_submit_reference *refs =
         static_cast<struct tu_wddm_submit_reference *>(submit->references.data);
      refs[existing_index].access |= access;
      return true;
   }
   if (index_slot == UINT32_MAX) {
      submit->failed = true;
      return false;
   }

   const uint32_t reference_count = util_dynarray_num_elements(
      &submit->references, struct tu_wddm_submit_reference);
   if (reference_count >= TU_WDDM_MAX_SUBMIT_REFERENCES) {
      submit->failed = true;
      return false;
   }

   struct tu_wddm_submit_reference *ref = (struct tu_wddm_submit_reference *)
      util_dynarray_grow(&submit->references, struct tu_wddm_submit_reference, 1);
   if (ref == NULL) {
      submit->failed = true;
      return false;
   }
   ref->bo = bo;
   ref->access = access;
   submit->reference_index[index_slot] = static_cast<uint16_t>(reference_count + 1);
   return true;
}

static void *
tu_wddm_submit_create(struct tu_device *device)
{
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)vk_zalloc(
      &device->vk.alloc, sizeof(*submit), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (submit == NULL)
      return NULL;
   util_dynarray_init(&submit->entries, NULL);
   util_dynarray_init(&submit->references, NULL);
   return submit;
}

static void
tu_wddm_submit_finish(struct tu_device *device, void *_submit)
{
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit == NULL)
      return;
   util_dynarray_fini(&submit->entries);
   util_dynarray_fini(&submit->references);
   vk_free(&device->vk.alloc, submit);
}

static void
tu_wddm_submit_add_entries(struct tu_device *device, void *_submit,
                           struct tu_cs_entry *entries, unsigned num_entries)
{
   (void)device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit == NULL || entries == NULL || num_entries == 0 ||
       util_dynarray_num_elements(&submit->entries, struct tu_wddm_submit_entry) >
          TU_WDDM_MAX_SUBMIT_COMMANDS ||
       num_entries > TU_WDDM_MAX_SUBMIT_COMMANDS -
          util_dynarray_num_elements(&submit->entries, struct tu_wddm_submit_entry)) {
      if (submit != NULL)
         submit->failed = true;
      return;
   }

   for (unsigned i = 0; i < num_entries; i++) {
      const struct tu_cs_entry *entry = &entries[i];
      if (!tu_wddm_bo_valid_for_device(device, entry->bo) || entry->size == 0 ||
          (entry->size & 3) != 0 || entry->offset > entry->bo->size ||
          entry->size > entry->bo->size - entry->offset ||
          !tu_wddm_submit_add_reference(device, submit, (struct tu_bo *)entry->bo,
                                        TU_SUBMIT_BO_ACCESS_READ)) {
         submit->failed = true;
         return;
      }
   }

   struct tu_wddm_submit_entry *out = (struct tu_wddm_submit_entry *)
      util_dynarray_grow(&submit->entries, struct tu_wddm_submit_entry, num_entries);
   if (out == NULL) {
      submit->failed = true;
      return;
   }
   for (unsigned i = 0; i < num_entries; i++) {
      out[i] = (struct tu_wddm_submit_entry) {
         .bo = (struct tu_bo *)entries[i].bo,
         .offset = entries[i].offset,
         .size = entries[i].size,
      };
   }
}

static void
tu_wddm_submit_add_bos(struct tu_device *device, void *_submit,
                       struct tu_bo **bos, unsigned num_bos,
                       uint32_t access_flags)
{
   (void)device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit == NULL || bos == NULL || num_bos == 0) {
      if (submit != NULL && num_bos != 0)
         submit->failed = true;
      return;
   }
   for (unsigned i = 0; i < num_bos; i++)
      tu_wddm_submit_add_reference(device, submit, bos[i], access_flags);
}

static bool
tu_wddm_submit_add_live_bos(struct tu_device *device,
                            struct tu_wddm_submit *submit)
{
   /* Command packets contain raw IOVAs for descriptors, images, buffers, and
    * internal resources.  Like the legacy drm/msm submit path, conservatively
    * make every live BO resident because the packet itself cannot enumerate
    * those transitive dependencies. */
   mtx_lock(&device->bo_mutex);
   for (uint32_t i = 0; i < device->wddm_bo_count && !submit->failed; i++) {
      struct tu_bo *bo = device->wddm_bos[i];
      if (!tu_wddm_bo_valid_for_device(device, bo))
         continue;
      const uint32_t access = bo->gpu_read_only
                                 ? TU_SUBMIT_BO_ACCESS_READ
                                 : TU_SUBMIT_BO_ACCESS_READ |
                                      TU_SUBMIT_BO_ACCESS_WRITE;
      tu_wddm_submit_add_reference(device, submit, bo, access);
   }
   mtx_unlock(&device->bo_mutex);
   return !submit->failed;
}

static void
tu_wddm_submit_add_bind(struct tu_device *device, void *_submit,
                        struct tu_sparse_vma *vma, uint64_t vma_offset,
                        struct tu_bo *bo, uint64_t bo_offset, uint64_t size)
{
   (void)device;
   (void)vma;
   (void)vma_offset;
   (void)bo;
   (void)bo_offset;
   (void)size;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *)_submit;
   if (submit != NULL)
      submit->failed = true;
}

static int
tu_wddm_submit_reference_index(const struct tu_wddm_submit *submit,
                               const struct tu_bo *bo)
{
   return tu_wddm_submit_reference_lookup(submit, bo, NULL);
}

static VkResult
tu_wddm_submit_render(struct tu_queue *queue, struct tu_wddm_submit *submit,
                      uint32_t fence)
{
   struct tu_device *device = queue->device;
   const uint32_t entry_count = util_dynarray_num_elements(
      &submit->entries, struct tu_wddm_submit_entry);
   const uint32_t reference_count = util_dynarray_num_elements(
      &submit->references, struct tu_wddm_submit_reference);
   if (entry_count == 0 || reference_count == 0 ||
       entry_count > TU_WDDM_MAX_SUBMIT_COMMANDS ||
       reference_count > TU_WDDM_MAX_SUBMIT_REFERENCES)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (device->wddm_submit_scratch == NULL) {
      device->wddm_submit_scratch = (struct tu_wddm_submit_scratch *)vk_alloc(
         &device->vk.alloc, sizeof(*device->wddm_submit_scratch), 8,
         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (device->wddm_submit_scratch == NULL)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      device->wddm_lifetime_stats.scratch_allocations++;
   } else {
      device->wddm_lifetime_stats.scratch_reuses++;
   }
   uint8_t *packet = device->wddm_submit_scratch->packet;
   struct fd_wddm_submit_bo *packet_bos = device->wddm_submit_scratch->packet_bos;
   struct fd_wddm_submit_cmd *packet_cmds = device->wddm_submit_scratch->packet_cmds;

   struct tu_wddm_render_reference *render_refs = device->wddm_submit_scratch->references;
   memset(render_refs, 0, (size_t)reference_count * sizeof(*render_refs));
   for (uint32_t i = 0; i < reference_count; i++) {
      struct tu_wddm_submit_reference *ref = util_dynarray_element(
         &submit->references, struct tu_wddm_submit_reference, i);
      uint32_t flags = 0;
      if (ref->access & TU_SUBMIT_BO_ACCESS_READ)
         flags |= TU_WDDM_MSM_SUBMIT_BO_READ;
      if (ref->access & TU_SUBMIT_BO_ACCESS_WRITE)
         flags |= TU_WDDM_MSM_SUBMIT_BO_WRITE;
      if (ref->bo->dump)
         flags |= TU_WDDM_MSM_SUBMIT_BO_DUMP;
      flags |= TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;

      packet_bos[i] = {
         .size = ref->bo->size,
         .flags = flags,
      };
      render_refs[i] = {
         .allocation = ref->bo->wddm_allocation,
         .flags = ((ref->access & TU_SUBMIT_BO_ACCESS_READ) ?
                      VIOGPU_WDDM_REFERENCE_READ : 0) |
                  ((ref->access & TU_SUBMIT_BO_ACCESS_WRITE) ?
                      VIOGPU_WDDM_REFERENCE_WRITE : 0),
         .allocation_offset = 0,
         .length = ref->bo->size,
         .patch_offset = fd_wddm_submit_bo_patch_offset(i),
      };
   }

   for (uint32_t i = 0; i < entry_count; i++) {
      struct tu_wddm_submit_entry *entry = util_dynarray_element(
         &submit->entries, struct tu_wddm_submit_entry, i);
      int ref_index = tu_wddm_submit_reference_index(submit, entry->bo);
      if (ref_index < 0)
         return VK_ERROR_DEVICE_LOST;
      packet_cmds[i] = {
         .bo_index = (uint32_t)ref_index,
         .offset = entry->offset,
         .size = entry->size,
      };
   }

   uint32_t packet_size;
   if (!fd_wddm_build_submit_packet(queue->msm_queue_id, fence,
                                    packet_bos, reference_count,
                                    packet_cmds, entry_count, packet,
                                    TU_WDDM_MAX_RENDER_COMMAND_SIZE,
                                    &packet_size))
      return VK_ERROR_INITIALIZATION_FAILED;

   bool rendered = tu_wddm_context_render(&device->wddm_context, packet,
                                          packet_size, render_refs,
                                          reference_count);
   return rendered ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static bool
tu_wddm_sync_set_submit_fence(struct vk_sync *base,
                              struct tu_wddm_context *context,
                              uint32_t fence, bool signal_now)
{
   if (base == NULL || vk_sync_type_is_dummy(base->type) ||
       base->type != &tu_wddm_sync_type)
      return vk_sync_type_is_dummy(base ? base->type : NULL);
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   if (!tu_wddm_sync_is_current(sync) || sync->context != context)
      return false;
   tu_wddm_sync_state_set(sync,
                          signal_now ? TU_WDDM_SYNC_SIGNALED : fence);
   return true;
}

static bool
tu_wddm_pending_fence_count(uint32_t submitted, uint32_t completed,
                            uint32_t *pending)
{
   if (pending == NULL)
      return false;
   *pending = 0;

   if (submitted == 0)
      return completed == 0;
   if (submitted == completed)
      return true;
   if (completed != 0 && !tu_wddm_fence_after(submitted, completed))
      return false;

   const uint64_t distance = tu_wddm_fence_distance(submitted, completed);
   if (distance >= TU_WDDM_FENCE_HALF_RANGE)
      return false;
   *pending = static_cast<uint32_t>(distance);
   return true;
}

static VkResult
tu_wddm_wait_submission_slot(struct tu_device *device)
{
   tu_wddm_fence_poll_wait poll_wait;
   while (device->wddm_pending_submission_upper_bound >=
          TU_WDDM_MAX_PENDING_SUBMISSIONS) {
      uint32_t completed = 0;
      if (!tu_wddm_context_get_completed_fence(&device->wddm_context,
                                               &completed))
         return vk_device_set_lost(
            &device->vk, "WDDM submission-slot fence query failed");
      if (!tu_wddm_device_execution_active(&device->wddm_device))
         return vk_device_set_lost(
            &device->vk, "WDDM submission-slot wait observed an inactive device");

      uint32_t pending = 0;
      if (!tu_wddm_pending_fence_count(
             device->wddm_context.last_submitted_fence, completed, &pending) ||
          pending > device->wddm_pending_submission_upper_bound)
         return VK_ERROR_DEVICE_LOST;

      device->wddm_pending_submission_upper_bound = pending;
      if (pending >= TU_WDDM_MAX_PENDING_SUBMISSIONS)
         poll_wait.wait();
   }

   return VK_SUCCESS;
}

static bool
tu_wddm_sync_signal_valid(struct vk_sync *base,
                          struct tu_wddm_context *context)
{
   if (base == NULL || vk_sync_type_is_dummy(base->type))
      return base != NULL && vk_sync_type_is_dummy(base->type);
   if (base->type != &tu_wddm_sync_type)
      return false;
   struct tu_wddm_sync *sync = tu_wddm_sync_from_vk(base);
   return tu_wddm_sync_is_current(sync) && sync->context == context;
}

static VkResult
tu_wddm_queue_submit_locked(struct tu_queue *queue,
                            struct tu_wddm_submit *submit,
                            struct vk_sync_signal *signals,
                            uint32_t signal_count)
{
   struct tu_device *device = queue->device;
   const uint32_t entry_count = util_dynarray_num_elements(
      &submit->entries, struct tu_wddm_submit_entry);
   uint32_t fence = 0;
   if (!tu_wddm_reap_retired_bos_locked(device, 16))
      return VK_ERROR_DEVICE_LOST;
   if (entry_count != 0) {
      if (!tu_wddm_submit_add_live_bos(device, submit))
         return vk_device_set_lost(
            &device->vk, "WDDM submit exceeds the allocation-list capacity");

      /* tu_queue stores the last fence in a signed field for compatibility
       * with the other Turnip backends, but WDDM treats that field as an
       * opaque 32-bit token.  Keep zero reserved and use serial arithmetic
       * across the complete UINT32 range; the KMD's bounded ring keeps the
       * half-range ordering rule unambiguous. */
      if (device->wddm_next_fence == 0)
         return VK_ERROR_DEVICE_LOST;
      VkResult slot_result = tu_wddm_wait_submission_slot(device);
      if (slot_result != VK_SUCCESS)
         return slot_result;
      fence = device->wddm_next_fence;
      VkResult result = tu_wddm_submit_render(queue, submit, fence);

      /* A successful D3DKMTRender transfers this fence to VidSch even if its
       * replacement metadata is malformed or signal publication races reset.
       * Consume the token immediately so no later submit can reuse it. */
      if (device->wddm_context.last_submitted_fence == fence) {
         queue->fence = (int)fence;
         device->wddm_next_fence = fence + 1;
         if (device->wddm_next_fence == 0)
            device->wddm_next_fence = 1;
         device->wddm_pending_submission_upper_bound++;
      }
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < signal_count; i++) {
      if (!tu_wddm_sync_set_submit_fence(signals[i].sync,
                                         &device->wddm_context, fence,
                                         entry_count == 0))
         return VK_ERROR_DEVICE_LOST;
   }

   return VK_SUCCESS;
}

static VkResult
tu_wddm_queue_submit(struct tu_queue *queue,
                     void *_submit,
                     struct vk_sync_wait *waits,
                     uint32_t wait_count,
                     struct vk_sync_signal *signals,
                     uint32_t signal_count,
                     struct tu_u_trace_submission_data *u_trace_submission_data)
{
   (void) u_trace_submission_data;
   struct tu_device *device = queue->device;
   struct tu_wddm_submit *submit = (struct tu_wddm_submit *) _submit;
   if (submit == NULL || submit->failed || !device->wddm_initialized)
      return VK_ERROR_DEVICE_LOST;

   for (uint32_t i = 0; i < signal_count; i++) {
      if (!tu_wddm_sync_signal_valid(signals[i].sync, &device->wddm_context))
         return VK_ERROR_DEVICE_LOST;
   }

   /* The pre-v1 private ABI has no scheduler wait list.  Resolve waits through
    * the same context-scoped completion endpoint before issuing Render. */
   for (uint32_t i = 0; i < wait_count; i++) {
      if (waits[i].sync == NULL || vk_sync_type_is_dummy(waits[i].sync->type))
         continue;
      VkResult result =
         vk_sync_wait(&device->vk, waits[i].sync, waits[i].wait_value, VK_SYNC_WAIT_COMPLETE, OS_TIMEOUT_INFINITE);
      if (result != VK_SUCCESS)
         return result == VK_TIMEOUT ? VK_TIMEOUT : VK_ERROR_DEVICE_LOST;
   }

   /* queue_submit already owns submit_mutex.  The nested WDDM lock protects
    * the all-live-BO snapshot and Render transfer from concurrent BO teardown
    * without making generic BO release recursively acquire submit_mutex. */
   mtx_lock(&device->wddm_mutex);
   VkResult result = tu_wddm_queue_submit_locked(queue, submit, signals, signal_count);
   mtx_unlock(&device->wddm_mutex);
   return result;
}

static VkResult
tu_wddm_queue_wait_fence(struct tu_queue *queue, uint32_t fence,
                         uint64_t timeout_ns)
{
   if (fence == 0)
      return VK_SUCCESS;
   if (!queue->device->wddm_initialized)
      return VK_ERROR_DEVICE_LOST;
   if (!tu_wddm_fence_was_submitted(&queue->device->wddm_context, fence))
      return vk_device_set_lost(
         &queue->device->vk, "WDDM queue wait targeted an unsubmitted fence");

   const uint64_t start = (uint64_t)os_time_get_nano();
   tu_wddm_fence_poll_wait poll_wait;
   for (;;) {
      uint32_t completed = 0;
      if (!tu_wddm_context_get_completed_fence(&queue->device->wddm_context,
                                               &completed))
         return vk_device_set_lost(
            &queue->device->vk, "WDDM queue fence query failed");
      if (!tu_wddm_device_execution_active(&queue->device->wddm_device))
         return vk_device_set_lost(
            &queue->device->vk,
            "WDDM queue wait observed an inactive device");
      if (completed == fence || tu_wddm_fence_after(completed, fence))
         return VK_SUCCESS;
      uint64_t elapsed = (uint64_t)os_time_get_nano() - start;
      if (timeout_ns != UINT64_MAX && elapsed >= timeout_ns)
         return VK_TIMEOUT;
      poll_wait.wait(timeout_ns == UINT64_MAX ? UINT64_MAX : timeout_ns - elapsed);
   }
}

static VkResult
tu_wddm_sparse_vma_init(struct tu_device *dev, struct vk_object_base *base,
                        struct tu_sparse_vma *out_vma, uint64_t *out_iova,
                        enum tu_sparse_vma_flags flags, uint64_t size,
                        uint64_t client_iova)
{
   (void)base;
   (void)out_vma;
   (void)out_iova;
   (void)flags;
   (void)size;
   (void)client_iova;
   return vk_error(dev, VK_ERROR_FEATURE_NOT_PRESENT);
}

static void
tu_wddm_sparse_vma_finish(struct tu_device *dev, struct tu_sparse_vma *vma)
{
   (void)dev;
   (void)vma;
}

static const struct tu_knl wddm_knl_funcs = {
   .name = "wddm",
   .device_init = tu_wddm_device_init,
   .device_finish = tu_wddm_device_finish,
   .device_get_gpu_timestamp = tu_wddm_device_get_gpu_timestamp,
   .device_get_suspend_count = tu_wddm_device_get_suspend_count,
   .device_check_status = tu_wddm_device_check_status,
   .submitqueue_new = tu_wddm_submitqueue_new,
   .submitqueue_close = tu_wddm_submitqueue_close,
   .bo_init = tu_wddm_bo_init,
   .bo_init_dmabuf = tu_wddm_bo_init_dmabuf,
   .bo_export_dmabuf = tu_wddm_bo_export_dmabuf,
   .bo_map = tu_wddm_bo_map,
   .bo_unmap = tu_wddm_bo_unmap,
   .bo_allow_dump = tu_wddm_bo_allow_dump,
   .bo_finish = tu_wddm_bo_finish,
   .bo_set_metadata = tu_wddm_bo_set_metadata,
   .bo_get_metadata = tu_wddm_bo_get_metadata,
   .submit_create = tu_wddm_submit_create,
   .submit_finish = tu_wddm_submit_finish,
   .submit_add_entries = tu_wddm_submit_add_entries,
   .submit_add_bos = tu_wddm_submit_add_bos,
   .submit_add_bind = tu_wddm_submit_add_bind,
   .queue_submit = tu_wddm_queue_submit,
   .queue_wait_fence = tu_wddm_queue_wait_fence,
   .sparse_vma_init = tu_wddm_sparse_vma_init,
   .sparse_vma_finish = tu_wddm_sparse_vma_finish,
};

struct tu_wddm_probe_state {
   struct tu_instance *instance;
   VkResult result;
   bool found;
   uint32_t added_devices;
};

static void
tu_wddm_destroy_added_devices(struct tu_instance *instance, uint32_t count)
{
   if (instance == NULL || instance->vk.physical_devices.destroy == NULL)
      return;

   while (count != 0 && !list_is_empty(&instance->vk.physical_devices.list)) {
      struct vk_physical_device *device = list_last_entry(
         &instance->vk.physical_devices.list, struct vk_physical_device, link);
      list_del(&device->link);
      instance->vk.physical_devices.destroy(device);
      count--;
   }
}

static bool
tu_wddm_probe_adapter(const struct tu_wddm_adapter_info *identity, void *data)
{
   struct tu_wddm_probe_state *state = (struct tu_wddm_probe_state *)data;
   struct tu_wddm_device *probe_device = &state->instance->wddm_probe_device;
   struct tu_wddm_context *probe_context = &state->instance->wddm_probe_context;

   if (state->instance->wddm_probe_pending &&
       !tu_wddm_probe_cleanup(state->instance)) {
      state->result = VK_ERROR_DEVICE_LOST;
      return false;
   }

   memset(probe_device, 0, sizeof(*probe_device));
   memset(probe_context, 0, sizeof(*probe_context));
   if (!tu_wddm_device_open(&state->instance->wddm_runtime, identity,
                            probe_device)) {
      if (!tu_wddm_probe_cleanup(state->instance)) {
         state->result = VK_ERROR_DEVICE_LOST;
         return false;
      }
      return true;
   }
   bool opened_context = tu_wddm_context_open(probe_device, probe_context);
   uint64_t va_start = 0;
   uint64_t va_size = 0;
   if (opened_context) {
      va_start = probe_context->info.VaStart;
      va_size = probe_context->info.VaSize;
   }
   if (!tu_wddm_probe_cleanup(state->instance)) {
      state->result = VK_ERROR_DEVICE_LOST;
      return false;
   }
   if (!opened_context) {
      state->result = VK_ERROR_DEVICE_LOST;
      return true;
   }
   struct tu_physical_device *device = (struct tu_physical_device *)vk_zalloc(
      &state->instance->vk.alloc, sizeof(*device), 8,
      VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (device == NULL) {
      state->result = VK_ERROR_OUT_OF_HOST_MEMORY;
      return false;
   }

   device->instance = state->instance;
   device->local_fd = -1;
   device->master_fd = -1;
   device->kgsl_dma_fd = -1;
   device->wddm_adapter = *identity;
   device->msm_major_version = (int)identity->private_info.MsmMajorVersion;
   device->msm_minor_version = (int)identity->private_info.MsmMinorVersion;
   device->dev_id.gpu_id = identity->private_info.GpuId;
   device->dev_id.chip_id = identity->private_info.ChipId;
   device->gmem_size = debug_get_num_option("TU_GMEM",
                                             identity->private_info.GmemSize);
   device->gmem_base = identity->private_info.GmemBase;
   device->va_start = va_start;
   device->va_size = va_size;
   device->has_set_iova = true;
   device->has_cached_coherent_memory =
      identity->private_info.HasCachedCoherentMemory != 0;
   device->has_cached_non_coherent_memory = false;
   device->has_raytracing = identity->private_info.HasRayTracing != 0;
   device->has_preemption = false;
   device->has_vm_bind = false;
   device->has_sparse = false;
   device->has_sparse_prr = false;
   device->has_lazy_bos = false;
   device->is_perf_cntr_selectable = false;
   /* The WDDM context exposes one host submitqueue, fixed at priority zero. */
   device->submitqueue_priority_count = 1;
   device->uche_trap_base = identity->private_info.UcheTrapBase;
   device->ubwc_config.highest_bank_bit = identity->private_info.HighestBankBit;
   device->ubwc_config.bank_swizzle_levels =
      identity->private_info.UbwcSwizzle ?
         (uint32_t)identity->private_info.UbwcSwizzle : ~0u;
   device->ubwc_config.macrotile_mode = identity->private_info.MacrotileMode ?
      (enum fdl_macrotile_mode)identity->private_info.MacrotileMode :
      FDL_MACROTILE_INVALID;
   device->timeline_type = vk_sync_timeline_get_type(&tu_wddm_sync_type);
   device->sync_types[0] = &tu_wddm_sync_type;
   device->sync_types[1] = &device->timeline_type.sync;
   device->sync_types[2] = NULL;
   /* WDDM allocations are pageable guest RAM.  The context VA window is an
    * address-space limit, while the system-memory estimate supplies the
    * process budget; DXGI DedicatedVideoMemory is diagnostic only. */
   device->heap.size = tu_get_system_heap_size(device);
   device->heap.used = 0;
   device->heap.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   state->instance->knl = &wddm_knl_funcs;
   state->result = tu_physical_device_init(device, state->instance);
   if (state->result != VK_SUCCESS) {
      vk_free(&state->instance->vk.alloc, device);
      return true;
   }

   list_addtail(&device->vk.link, &state->instance->vk.physical_devices.list);
   state->found = true;
   state->added_devices++;
   return true;
}

VkResult
tu_knl_wddm_load(struct tu_instance *instance)
{
   if (instance == NULL)
      return VK_ERROR_INITIALIZATION_FAILED;

   if (!instance->wddm_runtime_initialized) {
      if (!tu_wddm_runtime_init(&instance->wddm_runtime))
         return VK_ERROR_INCOMPATIBLE_DRIVER;
      instance->wddm_runtime_initialized = true;
   }

   struct tu_wddm_probe_state state = {
      .instance = instance,
      .result = VK_ERROR_INCOMPATIBLE_DRIVER,
      .found = false,
   };
   const bool enumeration_ok = tu_wddm_runtime_foreach_adapter(
      &instance->wddm_runtime, tu_wddm_probe_adapter, &state);
   if (!tu_wddm_probe_cleanup(instance)) {
      /* Keep the runtime initialized so tu_DestroyInstance() can retry the
       * close while the dispatch table is still valid. */
      return VK_ERROR_DEVICE_LOST;
   }
   if (!enumeration_ok) {
      /* The callback may have published earlier adapters before a later
       * adapter, DXGI call, or KMT close failed.  Remove exactly those
       * devices before returning an error so a subsequent enumeration cannot
       * duplicate them. */
      tu_wddm_destroy_added_devices(instance, state.added_devices);
      if (state.result == VK_SUCCESS || state.result == VK_ERROR_INCOMPATIBLE_DRIVER)
         state.result = VK_ERROR_INITIALIZATION_FAILED;
   }
   if (enumeration_ok && state.found)
      return VK_SUCCESS;
   if (state.result == VK_ERROR_OUT_OF_HOST_MEMORY)
      return state.result;

   tu_wddm_runtime_finish(&instance->wddm_runtime);
   instance->wddm_runtime_initialized = false;
   return state.result == VK_SUCCESS ? VK_ERROR_INCOMPATIBLE_DRIVER : state.result;
}


#endif /* TU_HAS_WDDM */
