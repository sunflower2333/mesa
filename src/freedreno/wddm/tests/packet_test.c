/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Byte-level tests of the production native packet encoder; no mock encoder.
 */
#include "freedreno_wddm_submit.h"
#include "tu_wddm_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

/* Keep test verdicts active in release builds and emit stable failure markers. */
static void
check(bool condition, const char *name)
{
   checks++;
   if (!condition) {
      fprintf(stderr, "FAIL: %s\n", name);
      exit(1);
   }
}

/* Decode independently of the packed C structs, including unaligned bytes. */
static uint32_t
read32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
          (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Check every untouched byte, not only one sentinel. */
static bool
filled(const uint8_t *p, size_t size, uint8_t expected)
{
   for (size_t i = 0; i < size; i++)
      if (p[i] != expected)
         return false;
   return true;
}

/* Require a byte-exact one-BO/one-command wire packet at an unaligned address. */
static void
test_golden(void)
{
   static const uint8_t expected[84] = {
      0x07,0,0,0, 0x54,0,0,0, 0x09,0,0,0, 0,0,0,0,
      0x10,0,0,0x80, 0x11,0,0,0, 1,0,0,0, 1,0,0,0, 9,0,0,0,
      0x09,0,0,0, 0,0,0,0, 0,0,0,0,0,0,0,0,
      1,0,0,0, 0,0,0,0, 0x10,0,0,0, 0x20,0,0,0,
      0,0,0,0, 0,0,0,0, 0,0,0,0,0,0,0,0,
   };
   const struct fd_wddm_submit_bo bo = {4096, TU_WDDM_MSM_SUBMIT_BO_READ};
   const struct fd_wddm_submit_cmd cmd = {0, 16, 32};
   uint8_t packet[86];
   memset(packet, 0xa5, sizeof(packet));
   uint32_t size = 99;
   check(fd_wddm_build_submit_packet(17, 9, &bo, 1, &cmd, 1,
                                    packet + 1, 84, &size), "golden-build");
   check(size == 84, "golden-size");
   check(memcmp(packet + 1, expected, sizeof(expected)) == 0, "golden-byte-exact");
   check(packet[0] == 0xa5 && packet[85] == 0xa5, "golden-guards");
   check(fd_wddm_submit_bo_patch_offset(0) == 44, "first-patch-offset");
   check(fd_wddm_submit_bo_patch_offset(1) == 60, "second-patch-offset");
   check(fd_wddm_submit_bo_patch_offset(1024) == UINT32_MAX, "invalid-patch-index");
   check(fd_wddm_submit_bo_patch_offset(UINT32_MAX) == UINT32_MAX, "overflow-patch-index");
}

/* Reject every invalid variant before writing to caller-owned packet bytes. */
static void
test_rejections(void)
{
   struct fd_wddm_submit_bo bo = {4096, TU_WDDM_MSM_SUBMIT_BO_READ};
   struct fd_wddm_submit_cmd cmd = {0, 16, 32};
   uint8_t packet[128];
   uint32_t size;
   #define REJECT(name, expression) do { \
      memset(packet, 0xa5, sizeof(packet)); size = 99; \
      check(!(expression), name); \
      check(size == 0, "rejection-output-size"); \
      check(filled(packet, sizeof(packet), 0xa5), "rejection-atomic-bytes"); \
   } while (0)
   #define BUILD(q, f, bp, bc, cp, cc, out, cap) \
      fd_wddm_build_submit_packet(q, f, bp, bc, cp, cc, out, cap, &size)
   REJECT("reject-short-capacity", BUILD(17, 9, &bo, 1, &cmd, 1, packet, 83));
   REJECT("reject-zero-queue", BUILD(0, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   REJECT("reject-zero-fence", BUILD(17, 0, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   REJECT("reject-null-bos", BUILD(17, 9, NULL, 1, &cmd, 1, packet, sizeof(packet)));
   REJECT("reject-null-cmds", BUILD(17, 9, &bo, 1, NULL, 1, packet, sizeof(packet)));
   REJECT("reject-null-packet", BUILD(17, 9, &bo, 1, &cmd, 1, NULL, sizeof(packet)));
   REJECT("reject-zero-bos", BUILD(17, 9, &bo, 0, &cmd, 1, packet, sizeof(packet)));
   REJECT("reject-zero-cmds", BUILD(17, 9, &bo, 1, &cmd, 0, packet, sizeof(packet)));
   REJECT("reject-large-bos", BUILD(17, 9, &bo, 1025, &cmd, 1, packet, sizeof(packet)));
   REJECT("reject-large-cmds", BUILD(17, 9, &bo, 1, &cmd, 257, packet, sizeof(packet)));
   REJECT("reject-overflow-bos", BUILD(17, 9, &bo, UINT32_MAX, &cmd, 1, packet, sizeof(packet)));
   REJECT("reject-overflow-cmds", BUILD(17, 9, &bo, 1, &cmd, UINT32_MAX, packet, sizeof(packet)));
   bo.size = 0;
   REJECT("reject-zero-bo-size", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   bo.size = 4096; bo.flags = 0;
   REJECT("reject-no-access", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   bo.flags = TU_WDDM_MSM_SUBMIT_BO_READ | 0x100;
   REJECT("reject-unknown-flags", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   bo.flags = TU_WDDM_MSM_SUBMIT_BO_READ; cmd.bo_index = 1;
   REJECT("reject-cmd-index", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   cmd.bo_index = 0; cmd.size = 0;
   REJECT("reject-zero-cmd-size", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   cmd.size = 3;
   REJECT("reject-unaligned-size", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   cmd.size = 32; cmd.offset = 3;
   REJECT("reject-unaligned-offset", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   cmd.offset = 4100;
   REJECT("reject-offset-past-end", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   cmd.offset = 4096;
   REJECT("reject-offset-at-end", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   cmd.offset = 4080;
   REJECT("reject-range-overrun", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   bo.size = UINT32_MAX; cmd.offset = UINT32_MAX - 3; cmd.size = 8;
   REJECT("reject-range-wrap", BUILD(17, 9, &bo, 1, &cmd, 1, packet, sizeof(packet)));
   check(!fd_wddm_build_submit_packet(17, 9, &bo, 1, &cmd, 1,
                                     packet, sizeof(packet), NULL), "reject-null-size");
   check(filled(packet, sizeof(packet), 0xa5), "null-size-atomic-bytes");
   #undef BUILD
   #undef REJECT
}

/* Exercise max legal counts and ensure command references remain byte-accurate. */
static void
test_maximum(void)
{
   struct fd_wddm_submit_bo bos[1024];
   struct fd_wddm_submit_cmd cmds[256];
   uint8_t packet[65538];
   for (unsigned i = 0; i < 1024; i++) {
      bos[i].size = 4096;
      bos[i].flags = TU_WDDM_MSM_SUBMIT_BO_READ | TU_WDDM_MSM_SUBMIT_BO_WRITE;
   }
   for (unsigned i = 0; i < 256; i++) {
      cmds[i].bo_index = 1023 - i;
      cmds[i].offset = 0;
      cmds[i].size = 4096;
   }
   const uint32_t expected = 36 + 1024 * 16 + 256 * 32;
   check(fd_wddm_submit_packet_size(1024, 256) == expected, "maximum-size");
   check((uint64_t)expected + sizeof(VIOGPU_WDDM_RENDER_COMMAND) +
         1024 * sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE) <= 65536, "maximum-envelope");
   memset(packet, 0xa5, sizeof(packet));
   uint32_t size = 0;
   check(fd_wddm_build_submit_packet(17, UINT32_MAX, bos, 1024, cmds, 256,
                                    packet + 1, expected, &size), "maximum-build");
   check(size == expected, "maximum-written-size");
   for (unsigned i = 0; i < 1024; i++) {
      const uint8_t *b = packet + 1 + 36 + i * 16;
      check(read32(b) == 11, "maximum-bo-flags");
      check(filled(b + 4, 12, 0), "maximum-zero-host-identity");
   }
   for (unsigned i = 0; i < 256; i++) {
      const uint8_t *c = packet + 1 + 36 + 1024 * 16 + i * 32;
      check(read32(c) == 1 && read32(c + 4) == 1023 - i, "maximum-command-index");
      check(read32(c + 8) == 0 && read32(c + 12) == 4096, "maximum-command-range");
      check(filled(c + 16, 16, 0), "maximum-zero-reloc-iova");
   }
   check(packet[0] == 0xa5 && packet[expected + 1] == 0xa5, "maximum-guards");
}

/* Use a fixed seed; this is packet-layout coverage, not a performance benchmark. */
static uint32_t
next_random(uint32_t *state)
{
   *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
   return *state;
}

/* Cover varying BO/command counts, access flags, sizes and nonzero fence values. */
static void
test_randomized(void)
{
   struct fd_wddm_submit_bo bos[32];
   struct fd_wddm_submit_cmd cmds[16];
   uint8_t packet[4096];
   uint32_t rng = 0x12345678;
   for (unsigned trial = 0; trial < 500; trial++) {
      uint32_t nb = 1 + (next_random(&rng) >> 16) % 32;
      uint32_t nc = 1 + (next_random(&rng) >> 16) % 16;
      for (uint32_t i = 0; i < nb; i++) {
         bos[i].size = 1024 + 4 * ((next_random(&rng) >> 8) % 1024);
         bos[i].flags = 1 + ((next_random(&rng) >> 16) % 3);
         if (next_random(&rng) & 0x80000000U)
            bos[i].flags |= TU_WDDM_MSM_SUBMIT_BO_DUMP;
      }
      for (uint32_t i = 0; i < nc; i++) {
         cmds[i].bo_index = (next_random(&rng) >> 16) % nb;
         cmds[i].offset = 4 * ((next_random(&rng) >> 16) % 128);
         cmds[i].size = 4 * (1 + ((next_random(&rng) >> 16) % 128));
      }
      uint32_t fence = next_random(&rng) | 1;
      uint32_t size = 0;
      check(fd_wddm_build_submit_packet(17, fence, bos, nb, cmds, nc,
                                       packet, sizeof(packet), &size), "random-build");
      check(size == 36 + nb * 16 + nc * 32 && read32(packet + 4) == size,
            "random-size");
      check(read32(packet + 8) == fence && read32(packet + 32) == fence,
            "random-fence-sequence");
      for (uint32_t i = 0; i < nb; i++) {
         const uint8_t *b = packet + 36 + i * 16;
         check(read32(b) == (bos[i].flags | 8), "random-bo-flags");
         check(filled(b + 4, 12, 0), "random-zero-host-identity");
      }
      for (uint32_t i = 0; i < nc; i++) {
         const uint8_t *c = packet + 36 + nb * 16 + i * 32;
         check(read32(c + 4) == cmds[i].bo_index &&
               read32(c + 8) == cmds[i].offset && read32(c + 12) == cmds[i].size,
               "random-command-range");
         check(filled(c + 16, 16, 0), "random-zero-reloc-iova");
      }
   }
}

/* Keep native C-header/ABI and byte-level verdicts independent of assert/NDEBUG. */
int
main(void)
{
   test_golden();
   test_rejections();
   test_maximum();
   test_randomized();
   printf("PASS native packet: %u checks, four groups, 500 randomized packets; no GPU\n", checks);
   return 0;
}
