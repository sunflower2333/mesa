/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Turnip-specific ownership around the common Freedreno WDDM transport.
 */
#ifndef TU_KNL_WDDM_H
#define TU_KNL_WDDM_H
#include "../wddm/freedreno_wddm.h"
#ifdef __cplusplus
extern "C" {
#endif
struct tu_instance;
/* Retry a physical-device probe's retained KMT ownership graph. */
bool tu_wddm_probe_cleanup(struct tu_instance *instance);
/* Do not unload the runtime while a probe still owns KMT handles. */
bool tu_wddm_instance_prepare_destroy(struct tu_instance *instance);
#ifdef __cplusplus
}
#endif
#endif /* TU_KNL_WDDM_H */
