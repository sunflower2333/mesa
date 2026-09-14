/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef FREEDRENO_WDDM_TIMELINE_H
#define FREEDRENO_WDDM_TIMELINE_H
#include <stdbool.h>
#include <stdint.h>

/* Extend only a forward, already-submitted completion into a 64-bit timeline.
 * Zero is the initial sentinel, not a wrap completion. Never accept future,
 * regressed, or half-range-ambiguous values as retirement evidence.
 */
static inline bool
fd_wddm_timeline_observe(uint64_t submitted, uint64_t *completed, uint32_t wire)
{
   if (!completed || *completed > submitted ||
       submitted - *completed >= UINT32_C(0x80000000))
      return false;
   if (wire == 0)
      return *completed == 0;
   uint32_t delta = wire - (uint32_t)*completed;
   if (delta >= UINT32_C(0x80000000) || delta > submitted - *completed)
      return false;
   *completed += delta;
   return true;
}

/* Skip the reserved wire value zero without truncating the CPU-side serial. */
static inline uint64_t
fd_wddm_timeline_next(uint64_t submitted)
{
   if (submitted >= UINT64_MAX - 1)
      return 0;
   uint64_t next = submitted + 1;
   return (uint32_t)next ? next : next + 1;
}
#endif
