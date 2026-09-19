/* SPDX-License-Identifier: MIT */
#include "tu_device.h"
#include "tu_knl.h"
#include "tu_wddm_pageable.h"

void
tu_wddm_pageable_register(struct tu_device *device, struct tu_pageable_record *record,
                         uint32_t type, uint64_t object, struct tu_bo *bo, bool no_backing)
{
   if (!device->wddm_runtime_owner)
      return;
   mtx_lock(&device->wddm_mutex);
   record->object = object; record->type = type; record->bo = bo;
   record->flags = no_backing ? MWD_PAGEABLE_NO_BACKING : 0;
   record->next = device->wddm_pageables;
   device->wddm_pageables = record;
   mtx_unlock(&device->wddm_mutex);
}

void
tu_wddm_pageable_unregister(struct tu_device *device, struct tu_pageable_record *record)
{
   if (!record->type)
      return;
   mtx_lock(&device->wddm_mutex);
   auto **link = &device->wddm_pageables;
   while (*link && *link != record)
      link = &(*link)->next;
   if (*link)
      *link = record->next;
   memset(record, 0, sizeof(*record));
   mtx_unlock(&device->wddm_mutex);
}

int32_t MWD_CALL
tu_wddm_pageable_acquire(void *device_handle, void *owner, uint64_t generation,
                        uint32_t type, uint64_t object, struct mwd_pageable_backing *out)
{
   /* HRESULT bits match the runtime callback protocol, not VkResult. */
   const int32_t invalid = (int32_t)0x80070057u;
   const int32_t unsupported = (int32_t)0x887a0004u;
   const int32_t removed = (int32_t)0x887a0005u;
   if (!device_handle || !out || !owner || !generation || !object ||
       type < MWD_PAGEABLE_MEMORY || type > MWD_PAGEABLE_QUERY_POOL)
      return invalid;
   VK_FROM_HANDLE(tu_device, device, (VkDevice)device_handle);
   if (owner != device->wddm_runtime_owner || !mwd_callbacks_valid(&device->wddm_callbacks))
      return invalid;
   const auto callbacks = device->wddm_callbacks;
   if (generation != device->wddm_context.info.ResetGeneration)
      return removed;
   int32_t hr = callbacks.status(owner);
   if (hr < 0)
      return hr;

   struct tu_bo *bo = NULL;
   struct mwd_pageable_backing result = {};
   void *token = NULL;
   uint64_t address = 0, size = 0;
   mtx_lock(&device->wddm_mutex);
   const struct tu_pageable_record *record = device->wddm_pageables;
   while (record && (record->object != object || record->type != type))
      record = record->next;
   if (!record) {
      hr = invalid;
   } else if (!record->bo) {
      /* Null/lazy memory is not proof of zero backing. Only an explicitly
       * registered host-only descriptor pool may return this result. */
      if (record->flags == MWD_PAGEABLE_NO_BACKING)
         result.flags = MWD_PAGEABLE_NO_BACKING;
      else
         hr = unsupported;
   } else {
      auto *allocation = record->bo->wddm_allocation;
      if (!allocation || !allocation->runtime_token || !allocation->context ||
          !allocation->context->device || allocation->context->device->runtime_owner != owner) {
         hr = unsupported;
      } else if (allocation->private_info.ExpectedResetGeneration != generation) {
         hr = removed;
      } else {
         bo = tu_bo_get_ref(record->bo);
         token = allocation->runtime_token;
         address = allocation->private_info.RequestedIova;
         size = allocation->vma_size;
      }
   }
   mtx_unlock(&device->wddm_mutex);
   if (hr < 0)
      return hr;

   bool retained = false;
   if (bo) {
      hr = callbacks.retain(owner, token, &result.allocation);
      retained = hr >= 0;
      if (retained && (result.allocation.token != token || !result.allocation.handle ||
                      result.allocation.generation != generation || result.allocation.address != address ||
                      result.allocation.size != size || !(result.allocation.flags & 4u) ||
                      (result.allocation.flags & ~14u)))
         hr = removed;
   }
   if (hr >= 0)
      hr = callbacks.status(owner);
   /* Drop the BO outside the registry lock. Destruction can synchronously enter
    * runtime unmap/release and even retire the device's callback owner. */
   if (bo)
      tu_bo_finish(device, bo);
   if (hr >= 0)
      hr = callbacks.status(owner);
   if (hr < 0) {
      if (retained)
         callbacks.release(owner, token);
      return hr;
   }
   *out = result;
   return 0;
}
