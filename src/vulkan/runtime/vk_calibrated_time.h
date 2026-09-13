/* SPDX-License-Identifier: MIT */
#ifndef VK_CALIBRATED_TIME_H
#define VK_CALIBRATED_TIME_H

#include <stdint.h>

/* Portable to MSVC x86/ARM64: ceil(ticks * 1e9 / frequency), with saturation.
 * Convert the fractional second by binary long division, avoiding a 128-bit
 * product and preserving the upper bound even at extreme clock frequencies.
 */
static inline uint64_t
vk_time_ticks_to_ns_ceil(uint64_t ticks, uint64_t frequency)
{
   if (!frequency)
      return UINT64_MAX;
   const uint64_t seconds = ticks / frequency;
   if (seconds > UINT64_MAX / 1000000000)
      return UINT64_MAX;
   const uint64_t fraction = ticks % frequency;
   uint64_t remainder = 0, nanos = 0;
   for (int bit = 29; bit >= 0; bit--) {
      nanos *= 2;
      if (remainder >= frequency - remainder) {
         remainder -= frequency - remainder;
         nanos++;
      } else {
         remainder += remainder;
      }
      if ((UINT64_C(1000000000) >> bit) & 1) {
         if (remainder >= frequency - fraction) {
            remainder -= frequency - fraction;
            nanos++;
         } else {
            remainder += fraction;
         }
      }
   }
   nanos += remainder != 0;
   uint64_t whole = seconds * 1000000000;
   return whole > UINT64_MAX - nanos ? UINT64_MAX : whole + nanos;
}

static inline uint64_t
vk_time_calibrated_deviation(uint64_t begin, uint64_t end,
                             uint64_t frequency, uint64_t max_clock_period_ns)
{
   if (end < begin || end - begin == UINT64_MAX)
      return UINT64_MAX;
   /* One interval-clock tick covers its sampling quantization. */
   uint64_t interval = vk_time_ticks_to_ns_ceil(end - begin + 1, frequency);
   return interval > UINT64_MAX - max_clock_period_ns
             ? UINT64_MAX : interval + max_clock_period_ns;
}
#endif
