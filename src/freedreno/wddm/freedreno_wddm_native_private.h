/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Explicit dispatch injection for transport tests, not a fake GPU backend.
 */
#ifndef FREEDRENO_WDDM_NATIVE_PRIVATE_H
#define FREEDRENO_WDDM_NATIVE_PRIVATE_H
#include "freedreno_wddm.h"
#include "freedreno_wddm_native.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Borrow a live runtime; caller must retain it until close/reap succeeds. */
struct fd_wddm_native_device *fd_wddm_native_open_with_runtime(
   struct tu_wddm_runtime *runtime,
   const struct fd_wddm_luid *luid);
#ifdef __cplusplus
}
#endif
#endif
