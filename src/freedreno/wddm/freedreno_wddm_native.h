/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Native Gallium resource/queue ownership. No graphics API or Windows headers.
 */
#ifndef FREEDRENO_WDDM_NATIVE_H
#define FREEDRENO_WDDM_NATIVE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct fd_wddm_native_device;
struct fd_wddm_native_bo;
struct fd_wddm_luid { uint32_t low; int32_t high; };
struct fd_wddm_native_info {
   struct fd_wddm_luid luid;
   uint32_t gpu_id, gmem_size, max_frequency;
   uint64_t chip_id, gmem_base, va_size, uche_trap_base, reset_generation;
};

enum fd_wddm_native_result {
   FD_WDDM_NATIVE_OK = 0,
   FD_WDDM_NATIVE_INVALID = -1,
   FD_WDDM_NATIVE_NOMEM = -2,
   FD_WDDM_NATIVE_PENDING = -3,
   FD_WDDM_NATIVE_LOST = -4,
};
enum fd_wddm_native_bo_flags {
   FD_WDDM_NATIVE_CPU_VISIBLE = 1,
   FD_WDDM_NATIVE_GPU_READONLY = 2,
};
struct fd_wddm_native_command {
   struct fd_wddm_native_bo *bo;
   uint32_t offset, size;
};

/* Select an exact LUID, or the first validated DroidVM adapter when NULL. */
struct fd_wddm_native_device *fd_wddm_native_open(const struct fd_wddm_luid *luid);
/* Copy immutable adapter/context properties; no Linux DRM version is invented. */
bool fd_wddm_native_get_info(
   struct fd_wddm_native_device *dev,
   struct fd_wddm_native_info *info);
/* Refuse active BOs; on KMT failure retain *dev and its full ownership graph. */
bool fd_wddm_native_close(struct fd_wddm_native_device **dev);
/* Transfer a failed close to an explicit retry owner, never discard handles. */
void fd_wddm_native_quarantine(struct fd_wddm_native_device **dev);
/* Retry quarantined graphs. False prohibits unloading the native runtime. */
bool fd_wddm_native_reap_quarantine(void);

/* Reserve a page-aligned GPU VA before KMT creation, retaining it on rollback failure. */
struct fd_wddm_native_bo *fd_wddm_native_bo_create(
   struct fd_wddm_native_device *dev,
   uint32_t size,
   uint32_t flags);
/* Return immutable BO properties while the caller holds its BO reference. */
uint64_t fd_wddm_native_bo_iova(const struct fd_wddm_native_bo *bo);
uint32_t fd_wddm_native_bo_handle(const struct fd_wddm_native_bo *bo);
/* Map CPU-visible memory; GPU READONLY does not make CPU mappings read-only. */
void *fd_wddm_native_bo_map(struct fd_wddm_native_bo *bo);
/* Release client ownership; native device retains pending/busy handles and VA. */
void fd_wddm_native_bo_release(struct fd_wddm_native_bo **bo);
/* Wait for all work submitted before the CPU access request. */
int fd_wddm_native_bo_wait(struct fd_wddm_native_bo *bo, uint64_t timeout_ns);

/* Submit real native IBs with all live BOs resident; never allocate per-submit.
 * Caller holds all command/resource BO references throughout this call and
 * publishes CPU writes before entry. A successful KMT transfer with malformed
 * replacement buffers returns LOST but still reports the accepted serial.
 */
int fd_wddm_native_submit(
   struct fd_wddm_native_device *dev,
   const struct fd_wddm_native_command *commands,
   uint32_t command_count,
   uint64_t *serial);
/* Poll (timeout=0) or wait a submitted 64-bit serial in this device's timeline. */
int fd_wddm_native_wait(
   struct fd_wddm_native_device *dev,
   uint64_t serial,
   uint64_t timeout_ns);
/* Mark an unrecoverable upper-layer submission error, not a completed fence. */
void fd_wddm_native_set_lost(struct fd_wddm_native_device *dev);

#ifdef __cplusplus
}
#endif
#endif
