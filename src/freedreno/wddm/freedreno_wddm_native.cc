/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Gallium's native KMT ownership, not a Vulkan or DRM emulation layer.
 */
#include "freedreno_wddm_native_private.h"
#include "freedreno_wddm_private.h"
#include "freedreno_wddm_submit.h"
#include "freedreno_wddm_timeline.h"
#include <stdlib.h>
#include <string.h>

struct fd_wddm_native_bo {
   fd_wddm_native_device *dev;
   fd_wddm_native_bo *next;
   tu_wddm_allocation allocation;
   uint64_t address, size, retire_serial;
   bool released;
};
struct fd_wddm_native_scratch {
   fd_wddm_native_bo *owners[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   tu_wddm_render_reference refs[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   fd_wddm_submit_bo bos[TU_WDDM_MAX_RENDER_ALLOCATIONS];
   fd_wddm_submit_cmd cmds[TU_WDDM_MAX_SUBMIT_COMMANDS];
   uint8_t packet[TU_WDDM_MAX_RENDER_COMMAND_SIZE];
};
struct fd_wddm_native_device {
   SRWLOCK mutex;
   tu_wddm_runtime owned_runtime;
   tu_wddm_runtime *runtime;
   tu_wddm_device device;
   tu_wddm_context context;
   fd_wddm_native_info info;
   fd_wddm_native_bo *bos; /* Address-sorted; includes failed/busy retry owners. */
   fd_wddm_native_scratch *scratch;
   fd_wddm_native_device *quarantine_next;
   uint64_t submitted, completed;
   uint32_t owner_count;
   bool owns_runtime, ready, closing, lost;
};
static SRWLOCK quarantine_mutex = SRWLOCK_INIT;
static fd_wddm_native_device *quarantine_head;

/* Serialize both KMT replacement buffers and the address/retirement graph. */
class native_lock {
 public:
   explicit native_lock(SRWLOCK *m) : mutex(m) { AcquireSRWLockExclusive(mutex); }
   ~native_lock() { ReleaseSRWLockExclusive(mutex); }
   native_lock(const native_lock &) = delete;
   native_lock &operator=(const native_lock &) = delete;
 private:
   SRWLOCK *mutex;
};

/* Fail closed on reset, execution failure, or impossible fence observations. */
static bool
observe_completion(fd_wddm_native_device *dev)
{
   uint32_t wire = 0;
   if (dev->lost || !dev->ready ||
       !tu_wddm_context_get_completed_fence(&dev->context, &wire) ||
       !tu_wddm_device_execution_active(&dev->device) ||
       !fd_wddm_timeline_observe(dev->submitted, &dev->completed, wire)) {
      dev->lost = true;
      return false;
   }
   return true;
}

/* Only successful KMT destruction releases an address, including on device loss.
 * close may ask VidMm to arbitrate without fence evidence, but never sets
 * AssumeNotInUse. Ordinary reclamation first requires observed completion.
 */
static void
collect_released(fd_wddm_native_device *dev, bool closing)
{
   fd_wddm_native_bo **link = &dev->bos;
   while (*link) {
      fd_wddm_native_bo *bo = *link;
      if (!bo->released ||
          (!closing && (dev->lost || bo->retire_serial > dev->completed))) {
         link = &bo->next;
         continue;
      }
      if (bo->allocation.locked && !tu_wddm_allocation_unlock(&bo->allocation)) {
         link = &bo->next;
         continue;
      }
      if (bo->allocation.handle &&
          tu_wddm_allocation_try_destroy(&bo->allocation) != TU_WDDM_STATUS_SUCCESS) {
         link = &bo->next;
         continue;
      }
      *link = bo->next;
      dev->owner_count--;
      free(bo);
   }
}

/* Find the first fit without allocating allocator metadata during rollback. */
static fd_wddm_native_bo **
find_address(fd_wddm_native_device *dev, uint64_t size, uint64_t *address)
{
   uint64_t cursor = dev->context.info.VaStart;
   const uint64_t end = cursor + dev->context.info.VaSize;
   fd_wddm_native_bo **link = &dev->bos;
   while (*link) {
      fd_wddm_native_bo *bo = *link;
      if (size <= bo->address - cursor)
         break;
      cursor = bo->address + bo->size;
      link = &bo->next;
   }
   if (cursor > end || size > end - cursor)
      return NULL;
   *address = cursor;
   return link;
}

/* Enumeration must continue so the common layer closes every probe handle. */
static bool
select_adapter(const tu_wddm_adapter_info *info, void *data)
{
   fd_wddm_luid *luid = static_cast<fd_wddm_luid *>(data);
   if (!luid->low && !luid->high) {
      luid->low = info->luid.LowPart;
      luid->high = info->luid.HighPart;
   }
   return true;
}

/* Open one shared address space/queue: allocations cannot cross KMT contexts. */
static fd_wddm_native_device *
open_device(tu_wddm_runtime *borrowed, const fd_wddm_luid *requested)
{
   fd_wddm_native_device *dev = static_cast<fd_wddm_native_device *>(calloc(1, sizeof(*dev)));
   if (!dev)
      return NULL;
   InitializeSRWLock(&dev->mutex);
   dev->runtime = borrowed ? borrowed : &dev->owned_runtime;
   dev->owns_runtime = borrowed == NULL;
   fd_wddm_luid selected = requested ? *requested : fd_wddm_luid{};
   bool runtime_ready = borrowed || tu_wddm_runtime_init(dev->runtime);
   if (runtime_ready &&
       (requested || tu_wddm_runtime_foreach_adapter(dev->runtime, select_adapter, &selected)) &&
       (selected.low || selected.high)) {
      tu_wddm_adapter_info identity = {};
      identity.luid.LowPart = selected.low;
      identity.luid.HighPart = selected.high;
      if (tu_wddm_device_open(dev->runtime, &identity, &dev->device) &&
          tu_wddm_context_open_with_hint(&dev->device, &dev->context,
                                        D3DKMT_CLIENTHINT_OPENGL)) {
         const auto &a = dev->device.adapter.private_info;
         dev->info.luid = selected;
         dev->info.gpu_id = a.GpuId;
         dev->info.chip_id = a.ChipId;
         dev->info.gmem_size = a.GmemSize;
         dev->info.gmem_base = a.GmemBase;
         dev->info.max_frequency = a.MaxFrequency;
         dev->info.va_size = dev->context.info.VaSize;
         dev->info.uche_trap_base = a.UcheTrapBase;
         dev->info.reset_generation = dev->context.info.ResetGeneration;
         dev->ready = true;
         if (observe_completion(dev))
            return dev;
      }
   }
   if (!fd_wddm_native_close(&dev))
      fd_wddm_native_quarantine(&dev);
   return NULL;
}

/* The normal entry owns its DLL dispatch until all KMT owners are gone. */
fd_wddm_native_device *
fd_wddm_native_open(const fd_wddm_luid *luid)
{
   fd_wddm_native_reap_quarantine();
   return open_device(NULL, luid);
}

/* Explicit borrowed dispatch for deterministic tests of the real implementation. */
fd_wddm_native_device *
fd_wddm_native_open_with_runtime(tu_wddm_runtime *runtime, const fd_wddm_luid *luid)
{
   return runtime ? open_device(runtime, luid) : NULL;
}

/* Copy a snapshot instead of exposing mutable transport handles to Gallium. */
bool
fd_wddm_native_get_info(fd_wddm_native_device *dev, fd_wddm_native_info *info)
{
   if (!dev || !info)
      return false;
   native_lock lock(&dev->mutex);
   if (!dev->ready || dev->closing || dev->lost)
      return false;
   *info = dev->info;
   return true;
}

/* Close is retryable and must not race with client operations or BO release. */
bool
fd_wddm_native_close(fd_wddm_native_device **owner)
{
   if (!owner || !*owner)
      return true;
   fd_wddm_native_device *dev = *owner;
   {
      native_lock lock(&dev->mutex);
      for (auto *bo = dev->bos; bo; bo = bo->next)
         if (!bo->released)
            return false;
      dev->closing = true;
      collect_released(dev, true);
      if (dev->bos || !tu_wddm_probe_owner_cleanup(&dev->device, &dev->context))
         return false;
      if (dev->owns_runtime)
         tu_wddm_runtime_finish(dev->runtime);
      free(dev->scratch);
   }
   free(dev);
   *owner = NULL;
   return true;
}

/* Transfer to a process-local retry list rather than dropping a live graph. */
void
fd_wddm_native_quarantine(fd_wddm_native_device **owner)
{
   if (!owner || !*owner)
      return;
   native_lock lock(&quarantine_mutex);
   (*owner)->quarantine_next = quarantine_head;
   quarantine_head = *owner;
   *owner = NULL;
}

/* Frontends must use the result as an unload guard if a close failed. */
bool
fd_wddm_native_reap_quarantine(void)
{
   native_lock lock(&quarantine_mutex);
   fd_wddm_native_device **link = &quarantine_head;
   while (*link) {
      auto *dev = *link;
      auto *next = dev->quarantine_next;
      if (fd_wddm_native_close(&dev))
         *link = next;
      else
         link = &dev->quarantine_next;
   }
   return quarantine_head == NULL;
}

/* Keep the sorted VA node even when a failing CreateAllocation returned a handle. */
fd_wddm_native_bo *
fd_wddm_native_bo_create(fd_wddm_native_device *dev, uint32_t size, uint32_t flags)
{
   if (!dev || !size || size > UINT32_MAX - 4095 ||
       (flags & ~(FD_WDDM_NATIVE_CPU_VISIBLE | FD_WDDM_NATIVE_GPU_READONLY)))
      return NULL;
   native_lock lock(&dev->mutex);
   if (dev->closing || !observe_completion(dev))
      return NULL;
   collect_released(dev, false);
   if (dev->owner_count >= TU_WDDM_MAX_RENDER_ALLOCATIONS ||
       dev->owner_count >= dev->context.allocation_list_size ||
       dev->owner_count >= dev->context.patch_location_list_size)
      return NULL;
   const uint64_t aligned = (static_cast<uint64_t>(size) + 4095) & ~UINT64_C(4095);
   uint64_t address = 0;
   fd_wddm_native_bo **link = find_address(dev, aligned, &address);
   if (!link)
      return NULL;
   auto *bo = static_cast<fd_wddm_native_bo *>(calloc(1, sizeof(fd_wddm_native_bo)));
   if (!bo)
      return NULL;
   bo->dev = dev;
   bo->address = address;
   bo->size = aligned;
   tu_wddm_allocation_desc desc = {};
   desc.size = aligned;
   desc.alignment = 4096;
   desc.requested_iova = address;
   desc.flags = VIOGPU_WDDM_ALLOCATION_NATIVE;
   if (flags & FD_WDDM_NATIVE_CPU_VISIBLE)
      desc.flags |= VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
   if (flags & FD_WDDM_NATIVE_GPU_READONLY)
      desc.flags |= VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY;
   const bool created = tu_wddm_allocation_create(&dev->context, &desc, &bo->allocation);
   if (created || bo->allocation.handle) {
      bo->next = *link;
      *link = bo;
      dev->owner_count++;
      bo->released = !created;
   } else {
      free(bo);
      bo = NULL;
   }
   return created ? bo : NULL;
}

/* Immutable while the caller retains ownership. */
uint64_t
fd_wddm_native_bo_iova(const fd_wddm_native_bo *bo)
{
   return bo ? bo->address : 0;
}

/* Handles identify private native BOs; this is not DMA-BUF/NT-handle export. */
uint32_t
fd_wddm_native_bo_handle(const fd_wddm_native_bo *bo)
{
   return bo ? bo->allocation.handle : 0;
}

/* Keep a successful map stable until release; unlock failure retains ownership. */
void *
fd_wddm_native_bo_map(fd_wddm_native_bo *bo)
{
   if (!bo)
      return NULL;
   native_lock lock(&bo->dev->mutex);
   if (bo->released || bo->dev->lost || bo->dev->closing)
      return NULL;
   if (!bo->allocation.locked) {
      void *map = NULL;
      if (!tu_wddm_allocation_lock(&bo->allocation, &map))
         return NULL;
   }
   return bo->allocation.map;
}

/* Client release never frees an in-use VA: the device becomes the retry owner. */
void
fd_wddm_native_bo_release(fd_wddm_native_bo **owner)
{
   if (!owner || !*owner)
      return;
   auto *bo = *owner;
   auto *dev = bo->dev;
   native_lock lock(&dev->mutex);
   bo->released = true;
   bo->retire_serial = dev->submitted;
   *owner = NULL;
   if (observe_completion(dev))
      collect_released(dev, false);
}

/* All-live residency makes the device submission snapshot conservative for any BO. */
int
fd_wddm_native_bo_wait(fd_wddm_native_bo *bo, uint64_t timeout_ns)
{
   if (!bo)
      return FD_WDDM_NATIVE_INVALID;
   auto *dev = bo->dev;
   uint64_t serial;
   {
      native_lock lock(&dev->mutex);
      if (bo->released || dev->closing)
         return FD_WDDM_NATIVE_INVALID;
      if (!observe_completion(dev))
         return FD_WDDM_NATIVE_LOST;
      serial = dev->submitted;
      if (serial == 0)
         return FD_WDDM_NATIVE_OK;
   }
   return fd_wddm_native_wait(dev, serial, timeout_ns);
}

/* Build a coherent snapshot and submit it while the KMT replacement buffers are locked. */
int
fd_wddm_native_submit(
   fd_wddm_native_device *dev,
   const fd_wddm_native_command *commands,
   uint32_t command_count,
   uint64_t *serial)
{
   if (serial)
      *serial = 0;
   if (!dev || !commands || !serial || !command_count ||
       command_count > TU_WDDM_MAX_SUBMIT_COMMANDS)
      return FD_WDDM_NATIVE_INVALID;
   native_lock lock(&dev->mutex);
   if (dev->closing || !observe_completion(dev))
      return FD_WDDM_NATIVE_LOST;
   collect_released(dev, false);
   if (dev->submitted - dev->completed >= TU_WDDM_MAX_PENDING_SUBMISSIONS)
      return FD_WDDM_NATIVE_PENDING;
   const uint64_t next = fd_wddm_timeline_next(dev->submitted);
   if (!next) {
      dev->lost = true;
      return FD_WDDM_NATIVE_LOST;
   }
   if (!dev->scratch) {
      dev->scratch = static_cast<fd_wddm_native_scratch *>(calloc(1, sizeof(*dev->scratch)));
      if (!dev->scratch)
         return FD_WDDM_NATIVE_NOMEM;
   }
   auto *s = dev->scratch;
   uint32_t count = 0;
   for (auto *bo = dev->bos; bo; bo = bo->next) {
      if (bo->released)
         continue;
      const bool readonly = bo->allocation.private_info.Flags & VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY;
      s->owners[count] = bo;
      s->bos[count].size = bo->size;
      s->bos[count].flags = TU_WDDM_MSM_SUBMIT_BO_READ |
         (readonly ? 0 : TU_WDDM_MSM_SUBMIT_BO_WRITE);
      s->refs[count].allocation = &bo->allocation;
      s->refs[count].flags = VIOGPU_WDDM_REFERENCE_READ |
         (readonly ? 0 : VIOGPU_WDDM_REFERENCE_WRITE);
      s->refs[count].allocation_offset = 0;
      s->refs[count].length = bo->size;
      s->refs[count].patch_offset = fd_wddm_submit_bo_patch_offset(count);
      count++;
   }
   for (uint32_t i = 0; i < command_count; i++) {
      uint32_t index = 0;
      while (index < count && s->owners[index] != commands[i].bo)
         index++;
      if (index == count)
         return FD_WDDM_NATIVE_INVALID; /* Includes cross-device and released BOs. */
      s->cmds[i].bo_index = index;
      s->cmds[i].offset = commands[i].offset;
      s->cmds[i].size = commands[i].size;
   }
   uint32_t packet_size = 0;
   if (!fd_wddm_build_submit_packet(dev->context.info.SubmitQueueId,
                                    static_cast<uint32_t>(next), s->bos, count,
                                    s->cmds, command_count, s->packet,
                                    sizeof(s->packet), &packet_size))
      return FD_WDDM_NATIVE_INVALID;
   MemoryBarrier();
   const bool rendered = tu_wddm_context_render(&dev->context, s->packet, packet_size,
                                                s->refs, count);
   if (dev->context.last_submitted_fence == static_cast<uint32_t>(next)) {
      dev->submitted = next;
      *serial = next;
   }
   if (!rendered) {
      dev->lost = true;
      return FD_WDDM_NATIVE_LOST;
   }
   return FD_WDDM_NATIVE_OK;
}

/* One poll for zero timeout; bounded waits never turn timeout into completion. */
int
fd_wddm_native_wait(fd_wddm_native_device *dev, uint64_t serial, uint64_t timeout_ns)
{
   if (!dev || !serial)
      return FD_WDDM_NATIVE_INVALID;
   const ULONGLONG start = GetTickCount64();
   const uint64_t timeout_ms = timeout_ns == UINT64_MAX ? UINT64_MAX :
      timeout_ns / 1000000 + (timeout_ns % 1000000 != 0);
   for (;;) {
      {
         native_lock lock(&dev->mutex);
         if (dev->closing || serial > dev->submitted || !(uint32_t)serial)
            return FD_WDDM_NATIVE_INVALID;
         if (!observe_completion(dev))
            return FD_WDDM_NATIVE_LOST;
         if (serial <= dev->completed)
            return FD_WDDM_NATIVE_OK;
      }
      if (timeout_ms != UINT64_MAX && GetTickCount64() - start >= timeout_ms)
         return FD_WDDM_NATIVE_PENDING;
      Sleep(1);
   }
}

/* Upper layers may report failure, but may not fabricate fence completion. */
void
fd_wddm_native_set_lost(fd_wddm_native_device *dev)
{
   if (!dev)
      return;
   native_lock lock(&dev->mutex);
   dev->lost = true;
}
