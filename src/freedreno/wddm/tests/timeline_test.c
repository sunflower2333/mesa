/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#include "freedreno_wddm_timeline.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
/* Preserve tests in NDEBUG builds. */
static void check(bool v) { checks++; if (!v) abort(); }
/* Exercise wrap, reserved zero, regressions, future and ambiguous completions. */
int main(void)
{
   uint64_t completed = 0;
   check(fd_wddm_timeline_next(0) == 1);
   check(fd_wddm_timeline_next(UINT32_MAX) == UINT64_C(0x100000001));
   check(fd_wddm_timeline_next(UINT64_MAX) == 0);
   check(fd_wddm_timeline_observe(1, &completed, 0) && completed == 0);
   check(!fd_wddm_timeline_observe(1, &completed, 2) && completed == 0);
   check(fd_wddm_timeline_observe(1, &completed, 1) && completed == 1);
   check(!fd_wddm_timeline_observe(2, &completed, 0) && completed == 1);
   check(!fd_wddm_timeline_observe(0, &completed, 1));
   check(!fd_wddm_timeline_observe(1, NULL, 1));
   completed = 1;
   check(!fd_wddm_timeline_observe(UINT64_C(0x80000001), &completed, 2));
   for (uint64_t i = UINT64_C(0xffffff00); i < UINT64_C(0x100000100); i++) {
      if (!(uint32_t)i) continue;
      completed = i;
      uint64_t next = fd_wddm_timeline_next(i);
      check(fd_wddm_timeline_observe(next, &completed, (uint32_t)next));
      check(completed == next);
      check(!fd_wddm_timeline_observe(next, &completed, (uint32_t)(i - 1)));
      check(completed == next);
   }
   printf("PASS native timeline: %u checks; no GPU\n", checks);
   return 0;
}
