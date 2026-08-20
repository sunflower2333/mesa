/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#include "../tu_wddm_abi.h"

#include <assert.h>
#include <string.h>

static void
init_header(VIOGPU_WDDM_ABI_HEADER *header, uint32_t size)
{
   memset(header, 0, size);
   header->Magic = VIOGPU_WDDM_ABI_MAGIC;
   header->Version = VIOGPU_WDDM_ABI_VERSION;
   header->Size = size;
}

int
main()
{
   VIOGPU_WDDM_ADAPTER_INFO adapter = {};
   init_header(&adapter.Header, sizeof(adapter));
   adapter.Capabilities = VIOGPU_WDDM_CAPABILITIES_NONE;
   adapter.ResetGeneration = 1;
   adapter.MsmMajorVersion = 1;
   adapter.MsmMinorVersion = 9;
   adapter.GpuId = 1;
   adapter.ChipId = 1;
   adapter.GmemSize = 4096;
   adapter.PriorityCount = 1;

   assert(adapter.Header.Magic == 0x504D5644U);
   assert(sizeof(adapter) == 128);
   assert(sizeof(VIOGPU_WDDM_ALLOCATION_INFO) == 80);
   assert(sizeof(VIOGPU_WDDM_CONTEXT_INFO) == 64);
   assert(sizeof(VIOGPU_WDDM_RENDER_COMMAND) == 64);
   assert(sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE) == 32);

   /* The exact revision has no forward-compatible tail or capability bit. */
   adapter.Reserved[0] = 1;
   assert(adapter.Reserved[0] != 0);
   return 0;
}
