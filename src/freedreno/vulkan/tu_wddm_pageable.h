/* SPDX-License-Identifier: MIT */
#ifndef TU_WDDM_PAGEABLE_H
#define TU_WDDM_PAGEABLE_H
#include "mesa_wddm_pageable.h"
struct tu_device;
struct tu_bo;
struct tu_pageable_record {
   struct tu_pageable_record *next;
   struct tu_bo *bo;
   uint64_t object;
   uint32_t type, flags;
};
void tu_wddm_pageable_register(struct tu_device *device, struct tu_pageable_record *record,
      uint32_t type, uint64_t object, struct tu_bo *bo, bool no_backing);
void tu_wddm_pageable_unregister(struct tu_device *device, struct tu_pageable_record *record);
int32_t MWD_CALL tu_wddm_pageable_acquire(void *device, void *owner, uint64_t generation,
      uint32_t type, uint64_t object, struct mwd_pageable_backing *out);
#endif
