/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Link the real backend from an API-neutral Windows executable. No GPU calls.
 */
#include "freedreno_wddm.h"
#include "freedreno_wddm_submit.h"
#include <cstdio>

/* Take addresses through volatile objects so release builds retain the symbols. */
int main()
{
   auto volatile render = &tu_wddm_context_render;
   auto volatile device_open = &tu_wddm_device_open;
   auto volatile allocate = &tu_wddm_allocation_create;
   auto volatile wait = &tu_wddm_context_wait_fence;
   auto volatile dispatch = &tu_wddm_runtime_init;
   if (!render || !device_open || !allocate || !wait || !dispatch ||
       fd_wddm_submit_packet_size(1, 1) != 84)
      return 1;
   std::puts("PASS: shared native KMT backend linked without Vulkan/Zink/DRM libraries");
   std::puts("No adapter opened, no GPU submission, no OpenGL context created.");
   return 0;
}
