/* SPDX-License-Identifier: MIT */
#ifndef MESA_WDDM_PAGEABLE_INTEROP_H
#define MESA_WDDM_PAGEABLE_INTEROP_H
#include "mesa_wddm_runtime.h"

/* Optional private provider contract, independently versioned. The runtime-v2
 * callback structure and KMD wire protocol are unchanged. */
#define MWD_PAGEABLE_VERSION 1u
#define MWD_PAGEABLE_MAGIC 0x3150574du
#define MWD_STYPE_PAGEABLE_SUPPORT 0x7fff2334
#define MWD_PAGEABLE_MEMORY 1u
#define MWD_PAGEABLE_DESCRIPTOR_POOL 2u
#define MWD_PAGEABLE_QUERY_POOL 3u
#define MWD_PAGEABLE_NO_BACKING 1u
struct mwd_pageable_backing {
   uint32_t flags, reserved;
   struct mwd_allocation allocation;
};
/* Caller holds the Vulkan object/device alive. Each successful nonempty reply
 * transfers one runtime token reference; release through runtime-v2 release.
 * Error leaves output unchanged and retains nothing. Opaque handles are looked
 * up by type in the actual device registry before dereferencing. */
typedef int32_t (MWD_CALL *mwd_pageable_acquire_fn)(void *device, void *owner,
      uint64_t generation, uint32_t type, uint64_t object,
      struct mwd_pageable_backing *out);
struct mwd_pageable_support {
   int32_t sType;
   void *pNext;
   uint32_t magic, version, size, flags;
   mwd_pageable_acquire_fn acquire;
};
#endif
