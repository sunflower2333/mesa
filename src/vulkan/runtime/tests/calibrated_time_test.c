/* SPDX-License-Identifier: MIT */
#include "../vk_calibrated_time.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
   assert(vk_time_ticks_to_ns_ceil(1, 10000000) == 100);
   assert(vk_time_ticks_to_ns_ceil(1, 19200000) == 53);
   assert(vk_time_ticks_to_ns_ceil(19200000, 19200000) == 1000000000);
   assert(vk_time_ticks_to_ns_ceil(UINT64_MAX, UINT64_MAX) == 1000000000);
   assert(vk_time_ticks_to_ns_ceil(UINT64_MAX - 1, UINT64_MAX) == 1000000000);
   assert(vk_time_ticks_to_ns_ceil(UINT64_MAX, 1) == UINT64_MAX);
   assert(vk_time_ticks_to_ns_ceil(0, 0) == UINT64_MAX);
   assert(vk_time_ticks_to_ns_ceil(0, 10000000) == 0);
   assert(vk_time_calibrated_deviation(100, 110, 10000000, 53) == 1153);
   assert(vk_time_calibrated_deviation(100, 100, 19200000, 53) == 106);
   assert(vk_time_calibrated_deviation(100, 99, 19200000, 53) == UINT64_MAX);
   assert(vk_time_calibrated_deviation(0, UINT64_MAX, 19200000, 53) == UINT64_MAX);
   assert(vk_time_calibrated_deviation(1, 2, 1, UINT64_MAX) == UINT64_MAX);
#if defined(__SIZEOF_INT128__)
   /* Independent exact 128-bit arithmetic oracle across arbitrary 64-bit
    * ticks/frequencies, including overflow in the naive ticks*1e9 product. */
   uint64_t rng = 0x83019200000;
   for (unsigned i = 0; i < 200000; i++) {
      rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
      uint64_t ticks = rng;
      rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
      uint64_t hz = rng | 1;
      __uint128_t exact = ((__uint128_t)ticks * 1000000000 + hz - 1) / hz;
      uint64_t expected = exact > UINT64_MAX ? UINT64_MAX : (uint64_t)exact;
      assert(vk_time_ticks_to_ns_ceil(ticks, hz) == expected);
   }
#endif
   puts("PASS calibrated interval: raw QPC ticks, ns ceiling, reversed time, saturation and arithmetic oracle");
   return 0;
}
