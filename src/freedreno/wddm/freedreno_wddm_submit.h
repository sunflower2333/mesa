/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * API-neutral native-context packet encoder.  It submits no GPU work.
 */
#ifndef FREEDRENO_WDDM_SUBMIT_H
#define FREEDRENO_WDDM_SUBMIT_H
#include "freedreno_wddm_packet.h"
#ifdef __cplusplus
extern "C" {
#endif

struct fd_wddm_submit_bo {
   uint64_t size;
   uint32_t flags;
};

struct fd_wddm_submit_cmd {
   uint32_t bo_index;
   uint32_t offset;
   uint32_t size;
};

/* Return the native packet size, or zero for invalid/beyond-KMD counts. */
uint32_t fd_wddm_submit_packet_size(uint32_t bo_count, uint32_t cmd_count);

/* Return the presumed-IOVA patch offset, or UINT32_MAX for an invalid index. */
uint32_t fd_wddm_submit_bo_patch_offset(uint32_t bo_index);

/* Build a complete native packet with zero host handles/IOVAs for KMD patching.
 * On rejection, output_size is zero and packet bytes remain untouched.
 * Inputs/output/output_size must be valid, non-overlapping caller-owned ranges.
 * No Vulkan object, DRM fd, allocation, lock, fence wait or GPU call is made.
 */
bool fd_wddm_build_submit_packet(
   uint32_t queue_id,
   uint32_t fence,
   const struct fd_wddm_submit_bo *bos,
   uint32_t bo_count,
   const struct fd_wddm_submit_cmd *cmds,
   uint32_t cmd_count,
   void *packet,
   uint32_t capacity,
   uint32_t *output_size);

#ifdef __cplusplus
}
#endif
#endif /* FREEDRENO_WDDM_SUBMIT_H */
