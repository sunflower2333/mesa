/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Compile-only probe for the Windows SDK thunk and private ABI boundary.
 * This intentionally does not open an adapter or submit work.
 */

#include "../tu_knl_wddm.h"

static_assert(sizeof(VIOGPU_WDDM_ADAPTER_INFO) == 128, "private ABI drift");
static_assert(sizeof(VIOGPU_WDDM_CONTEXT_INFO) == 60, "private ABI drift");

int
main()
{
   tu_wddm_runtime runtime = {};
   tu_wddm_dispatch dispatch = {};
   (void)runtime;
   (void)dispatch;
   return 0;
}
