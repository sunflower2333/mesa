/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef TU_WDDM_LIFETIME_H
#define TU_WDDM_LIFETIME_H

#include <stdbool.h>
#include <stdint.h>

/* UMD-only ownership.  None of these fields is part of a KMT wire packet. */
struct tu_wddm_retirement {
   uint64_t reset_generation;
   uint32_t fence;
   bool pending;
   bool fence_ready;
};

struct tu_wddm_lifetime_stats {
   uint64_t queued;
   uint64_t reaped;
   uint64_t pending_peak;
   uint64_t fence_queries;
   uint64_t fence_pending;
   uint64_t destroy_busy;
   uint64_t unlock_failures;
   uint64_t destroy_failures;
   uint64_t query_failures;
   uint64_t epoch_failures;
   uint64_t scratch_allocations;
   uint64_t scratch_reuses;
};

/* Latch completion before uint32 wrap can make an old, busy owner look new.
 * Zero is the never-submitted sentinel, not evidence for a nonzero fence.
 * Completion only permits a KMT destroy attempt; it never releases an IOVA. */
static inline bool
tu_wddm_retirement_observe(struct tu_wddm_retirement *retirement,
                            uint32_t completed)
{
   if (retirement->fence == 0 ||
       (completed != 0 &&
        (completed == retirement->fence ||
         (uint32_t)(completed - retirement->fence) < UINT32_C(0x80000000))))
      retirement->fence_ready = true;
   return retirement->fence_ready;
}

#endif /* TU_WDDM_LIFETIME_H */
