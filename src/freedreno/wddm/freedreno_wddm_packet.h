/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Native MSM wire layout shared by Windows Gallium bring-up and Turnip.
 * No Windows SDK, Vulkan, libdrm or Gallium headers are required.
 */
#ifndef FREEDRENO_WDDM_PACKET_H
#define FREEDRENO_WDDM_PACKET_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#define FD_WDDM_STATIC_ASSERT static_assert
extern "C" {
#else
#define FD_WDDM_STATIC_ASSERT _Static_assert
#endif

/* This flag does not fit a signed C enum. */
#define TU_WDDM_MSM_SUBMIT_NO_IMPLICIT UINT32_C(0x80000000)

enum {
   TU_WDDM_MAX_RENDER_ALLOCATIONS = 1024,
   TU_WDDM_MAX_RENDER_COMMAND_SIZE = 64 * 1024,
   TU_WDDM_MSM_CCMD_GEM_SUBMIT = 7,
   TU_WDDM_MSM_PIPE_3D0 = 0x10,
   TU_WDDM_MSM_SUBMIT_BO_READ = 0x0001,
   TU_WDDM_MSM_SUBMIT_BO_WRITE = 0x0002,
   TU_WDDM_MSM_SUBMIT_BO_DUMP = 0x0004,
   TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT = 0x0008,
   TU_WDDM_MSM_SUBMIT_CMD_BUF = 0x0001,
   TU_WDDM_MSM_SUBMIT_CMD_IB_TARGET_BUF = 0x0002,
   /* Keep native-context command packets within the KMD's bounded ring. */
   TU_WDDM_MAX_SUBMIT_COMMANDS = 256,
   /* Must match VioGpuWddmContextFenceTrackerCapacity in the dedicated KMD. */
   TU_WDDM_MAX_PENDING_SUBMISSIONS = 4096,
};

#pragma pack(push, 1)
struct tu_wddm_msm_submit_request {
   uint32_t command;
   uint32_t length;
   uint32_t sequence;
   uint32_t response_offset;
   uint32_t flags;
   uint32_t queue_id;
   uint32_t bo_count;
   uint32_t command_count;
   uint32_t fence;
};

struct tu_wddm_msm_submit_bo {
   uint32_t flags;
   uint32_t handle;
   uint64_t presumed;
};

struct tu_wddm_msm_submit_command {
   uint32_t type;
   uint32_t submit_index;
   uint32_t submit_offset;
   uint32_t size;
   uint32_t padding;
   uint32_t relocation_count;
   uint64_t iova;
};
#pragma pack(pop)

FD_WDDM_STATIC_ASSERT(sizeof(struct tu_wddm_msm_submit_request) == 36, "MSM submit request layout changed");
FD_WDDM_STATIC_ASSERT(offsetof(struct tu_wddm_msm_submit_request, flags) == 16, "MSM submit flags offset changed");
FD_WDDM_STATIC_ASSERT(offsetof(struct tu_wddm_msm_submit_request, fence) == 32, "MSM submit fence offset changed");
FD_WDDM_STATIC_ASSERT(sizeof(struct tu_wddm_msm_submit_bo) == 16, "MSM submit BO layout changed");
FD_WDDM_STATIC_ASSERT(offsetof(struct tu_wddm_msm_submit_bo, presumed) == 8, "MSM submit presumed offset changed");
FD_WDDM_STATIC_ASSERT(sizeof(struct tu_wddm_msm_submit_command) == 32, "MSM submit command layout changed");
FD_WDDM_STATIC_ASSERT(offsetof(struct tu_wddm_msm_submit_command, iova) == 24, "MSM submit command IOVA offset changed");


#undef FD_WDDM_STATIC_ASSERT
#ifdef __cplusplus
}
#endif
#endif /* FREEDRENO_WDDM_PACKET_H */
