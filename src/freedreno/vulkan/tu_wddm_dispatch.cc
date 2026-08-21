/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#include "tu_wddm_dispatch.h"

#include <string.h>

static FARPROC
tu_wddm_get_proc(HMODULE module, const char *name)
{
   return module == NULL ? NULL : GetProcAddress(module, name);
}

bool
tu_wddm_dispatch_init(struct tu_wddm_dispatch *dispatch)
{
   if (dispatch == NULL)
      return false;

   memset(dispatch, 0, sizeof(*dispatch));

   /* Restrict the search to the system copy.  Loading an app-local gdi32
    * would make the KMD/UMD ABI provenance impossible to audit. */
   dispatch->gdi32 = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (dispatch->gdi32 == NULL)
      return false;

#define TU_WDDM_LOAD(name) \
   dispatch->name = reinterpret_cast<decltype(dispatch->name)>( \
      tu_wddm_get_proc(dispatch->gdi32, "D3DKMT" #name)); \
   if (dispatch->name == NULL) \
      goto fail

   TU_WDDM_LOAD(OpenAdapterFromLuid);
   TU_WDDM_LOAD(CloseAdapter);
   TU_WDDM_LOAD(QueryAdapterInfo);
   TU_WDDM_LOAD(CreateDevice);
   TU_WDDM_LOAD(DestroyDevice);
   TU_WDDM_LOAD(CreateContext);
   TU_WDDM_LOAD(DestroyContext);
   TU_WDDM_LOAD(CreateAllocation);
   TU_WDDM_LOAD(DestroyAllocation);
   TU_WDDM_LOAD(Lock);
   TU_WDDM_LOAD(Unlock);
   TU_WDDM_LOAD(Escape);
   TU_WDDM_LOAD(Render);
   TU_WDDM_LOAD(GetDeviceState);

#undef TU_WDDM_LOAD
   return true;

fail:
#undef TU_WDDM_LOAD
   tu_wddm_dispatch_finish(dispatch);
   return false;
}

void
tu_wddm_dispatch_finish(struct tu_wddm_dispatch *dispatch)
{
   if (dispatch == NULL)
      return;

   if (dispatch->gdi32 != NULL)
      FreeLibrary(dispatch->gdi32);

   memset(dispatch, 0, sizeof(*dispatch));
}
