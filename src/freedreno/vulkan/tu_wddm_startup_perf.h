/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef TU_WDDM_STARTUP_PERF_H
#define TU_WDDM_STARTUP_PERF_H

#include <stdint.h>

/* Optional CPU-side startup attribution. Nested wall/CPU intervals overlap;
 * calling-thread CPU time excludes compiler workers and is never GPU time.
 * Keep Win32 headers inside the transport, out of common compiler sources. */
class tu_wddm_startup_scope {
public:
#ifdef TU_HAS_WDDM
   tu_wddm_startup_scope(const char *phase, uint32_t count = 1,
                         bool sample = false);
   ~tu_wddm_startup_scope();
#else
   tu_wddm_startup_scope(const char *, uint32_t = 1, bool = false) {}
   ~tu_wddm_startup_scope() {}
#endif
   tu_wddm_startup_scope(const tu_wddm_startup_scope &) = delete;
   tu_wddm_startup_scope &operator=(const tu_wddm_startup_scope &) = delete;

private:
#ifdef TU_HAS_WDDM
   const char *phase;
   uint32_t count;
   bool sample, enabled = false, cpu_valid = false;
   uint64_t start = 0, frequency = 0, cpu_start = 0;
#endif
};

#endif
