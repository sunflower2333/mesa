/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Compile-only probe for the Windows SDK thunk and private ABI boundary.
 * This intentionally does not open an adapter or submit work.
 */

#include "../tu_knl_wddm.h"

static_assert(sizeof(VIOGPU_WDDM_ADAPTER_INFO) == 128, "private ABI drift");
static_assert(sizeof(VIOGPU_WDDM_CONTEXT_INFO) == 64, "private ABI drift");
static_assert(sizeof(VIOGPU_WDDM_FENCE_INFO) == 56, "private ABI drift");
static_assert(TU_WDDM_MAX_RENDER_ALLOCATIONS == 128, "KMD allocation limit drift");
static_assert(TU_WDDM_MAX_RENDER_COMMAND_SIZE == 64 * 1024, "KMD command limit drift");

int
main()
{
   tu_wddm_runtime runtime = {};
   tu_wddm_dispatch dispatch = {};
   tu_wddm_allocation_desc allocation_desc = {};
   tu_wddm_allocation allocation = {};
   tu_wddm_render_reference render_reference = {};
   (void)runtime;
   (void)dispatch;
   (void)allocation_desc;
   (void)allocation;
   (void)render_reference;
   return 0;
}
