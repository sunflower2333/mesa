/* SPDX-License-Identifier: MIT */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

static constexpr uint32_t TU_WDDM_MAX_RENDER_ALLOCATIONS = 1024;
struct tu_wddm_allocation { uint32_t handle; };
struct tu_wddm_render_reference {
   tu_wddm_allocation *allocation;
   uint32_t patch_offset;
};

// PRODUCTION_FUNCTIONS

/* Independent pairwise oracle, using widened endpoints for overflow safety. */
static bool oracle(const tu_wddm_render_reference *refs, uint32_t count)
{
   if (!refs || !count || count > TU_WDDM_MAX_RENDER_ALLOCATIONS)
      return false;
   for (uint32_t i = 0; i < count; i++) {
      if (!refs[i].allocation || !refs[i].allocation->handle)
         return false;
      for (uint32_t j = 0; j < i; j++) {
         if (refs[i].allocation->handle == refs[j].allocation->handle)
            return false;
         const uint64_t a = refs[i].patch_offset, b = refs[j].patch_offset;
         if (a < b + 8 && b < a + 8)
            return false;
      }
   }
   return true;
}

int main()
{
   std::mt19937 random(0x7dd02026);
   tu_wddm_allocation allocations[TU_WDDM_MAX_RENDER_ALLOCATIONS + 1] = {};
   tu_wddm_render_reference refs[TU_WDDM_MAX_RENDER_ALLOCATIONS + 1] = {};
   uint32_t checks = 0;
   auto verify = [&](uint32_t count) {
      tu_wddm_render_reference before[TU_WDDM_MAX_RENDER_ALLOCATIONS + 1];
      memcpy(before, refs, sizeof(refs));
      const bool expected = oracle(refs, count);
      const bool actual = tu_wddm_render_references_unique(refs, count);
      checks++;
      if (expected != actual || memcmp(before, refs, sizeof(refs))) {
         fprintf(stderr, "reference check %u count=%u expected=%d actual=%d\n",
                 checks, count, expected, actual);
         return false;
      }
      return true;
   };
   if (tu_wddm_render_references_unique(NULL, 1) || !verify(0) || !verify(1025))
      return 1;
   for (uint32_t trial = 0; trial < 256; trial++) {
      const uint32_t count = trial < 2 ? trial + 1 : trial < 4 ? 1024 : 2 + random() % 1023;
      for (uint32_t i = 0; i < count; i++) {
         allocations[i].handle = 0x80000000u + i * 1024;
         refs[i] = {&allocations[i], i * 16};
      }
      std::shuffle(refs, refs + count, random);
      if (!verify(count)) return 1;
      if (count < 2) continue;
      const auto saved = refs[count - 1];
      // Distinct objects with the same KMT handle must still be rejected.
      allocations[1024].handle = refs[0].allocation->handle;
      refs[count - 1].allocation = &allocations[1024];
      if (!verify(count)) return 1;
      refs[count - 1] = saved;
      const uint32_t first_offset = refs[0].patch_offset;
      for (uint32_t gap : {0u, 4u, 7u, 8u, 12u}) {
         refs[count - 1].patch_offset = first_offset + gap;
         if (!verify(count)) return 1;
      }
      refs[count - 1] = saved;
      refs[count - 1].allocation = NULL;
      if (!verify(count)) return 1;
      refs[count - 1] = saved;
      // High valid endpoints must not wrap while checking disjoint ranges.
      refs[0].patch_offset = UINT32_MAX - 16;
      refs[count - 1].patch_offset = UINT32_MAX - 8;
      if (!verify(count)) return 1;
      refs[count - 1].patch_offset = UINT32_MAX - 12;
      if (!verify(count)) return 1;
   }
   printf("WDDM production reference validation: %u differential/order checks PASS\n", checks);
   return 0;
}
