/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#include "freedreno_wddm_submit.h"
#include "tu_wddm_abi.h"
#include <string.h>

/* Include the enclosing Render header/reference table in the 64 KiB proof. */
uint32_t
fd_wddm_submit_packet_size(uint32_t bo_count, uint32_t cmd_count)
{
   if (bo_count == 0 || bo_count > TU_WDDM_MAX_RENDER_ALLOCATIONS ||
       cmd_count == 0 || cmd_count > TU_WDDM_MAX_SUBMIT_COMMANDS)
      return 0;

   const uint64_t size = sizeof(struct tu_wddm_msm_submit_request) +
      (uint64_t)bo_count * sizeof(struct tu_wddm_msm_submit_bo) +
      (uint64_t)cmd_count * sizeof(struct tu_wddm_msm_submit_command);
   const uint64_t total = size + sizeof(VIOGPU_WDDM_RENDER_COMMAND) +
      (uint64_t)bo_count * sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE);
   return total <= TU_WDDM_MAX_RENDER_COMMAND_SIZE ? (uint32_t)size : 0;
}

/* Match the exact presumed field patched by the Native Context KMD. */
uint32_t
fd_wddm_submit_bo_patch_offset(uint32_t bo_index)
{
   if (bo_index >= TU_WDDM_MAX_RENDER_ALLOCATIONS)
      return UINT32_MAX;
   return (uint32_t)(sizeof(struct tu_wddm_msm_submit_request) +
      (size_t)bo_index * sizeof(struct tu_wddm_msm_submit_bo) +
      offsetof(struct tu_wddm_msm_submit_bo, presumed));
}

/* Validate the entire logical input before writing any packet byte. */
bool
fd_wddm_build_submit_packet(
   uint32_t queue_id,
   uint32_t fence,
   const struct fd_wddm_submit_bo *bos,
   uint32_t bo_count,
   const struct fd_wddm_submit_cmd *cmds,
   uint32_t cmd_count,
   void *packet,
   uint32_t capacity,
   uint32_t *output_size)
{
   if (output_size == NULL)
      return false;
   *output_size = 0;
   const uint32_t size = fd_wddm_submit_packet_size(bo_count, cmd_count);
   if (queue_id == 0 || fence == 0 || bos == NULL || cmds == NULL ||
       packet == NULL || size == 0 || capacity < size)
      return false;

   const uint32_t access = TU_WDDM_MSM_SUBMIT_BO_READ | TU_WDDM_MSM_SUBMIT_BO_WRITE;
   const uint32_t allowed = access | TU_WDDM_MSM_SUBMIT_BO_DUMP |
                           TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;
   for (uint32_t i = 0; i < bo_count; i++) {
      if (bos[i].size == 0 || (bos[i].flags & access) == 0 ||
          (bos[i].flags & ~allowed) != 0)
         return false;
   }
   for (uint32_t i = 0; i < cmd_count; i++) {
      const struct fd_wddm_submit_cmd *cmd = &cmds[i];
      if (cmd->bo_index >= bo_count || cmd->size == 0 ||
          ((cmd->offset | cmd->size) & 3) != 0 ||
          cmd->offset > bos[cmd->bo_index].size ||
          cmd->size > bos[cmd->bo_index].size - cmd->offset)
         return false;
   }

   struct tu_wddm_msm_submit_request request = {0};
   request.command = TU_WDDM_MSM_CCMD_GEM_SUBMIT;
   request.length = size;
   request.sequence = fence;
   request.flags = TU_WDDM_MSM_PIPE_3D0 | TU_WDDM_MSM_SUBMIT_NO_IMPLICIT;
   request.queue_id = queue_id;
   request.bo_count = bo_count;
   request.command_count = cmd_count;
   request.fence = fence;
   uint8_t *out = packet;
   memcpy(out, &request, sizeof(request));
   uint32_t cursor = (uint32_t)sizeof(request);
   for (uint32_t i = 0; i < bo_count; i++) {
      struct tu_wddm_msm_submit_bo bo = {0};
      bo.flags = bos[i].flags | TU_WDDM_MSM_SUBMIT_BO_NO_IMPLICIT;
      memcpy(out + cursor, &bo, sizeof(bo));
      cursor += (uint32_t)sizeof(bo);
   }
   for (uint32_t i = 0; i < cmd_count; i++) {
      struct tu_wddm_msm_submit_command cmd = {0};
      cmd.type = TU_WDDM_MSM_SUBMIT_CMD_BUF;
      cmd.submit_index = cmds[i].bo_index;
      cmd.submit_offset = cmds[i].offset;
      cmd.size = cmds[i].size;
      memcpy(out + cursor, &cmd, sizeof(cmd));
      cursor += (uint32_t)sizeof(cmd);
   }
   *output_size = size;
   return true;
}
