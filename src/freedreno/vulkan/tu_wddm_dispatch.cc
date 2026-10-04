/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#include "tu_wddm_dispatch.h"

#include <stdio.h>
#include <string.h>

static FARPROC
tu_wddm_get_proc(HMODULE module, const char *name)
{
   return module == NULL ? NULL : GetProcAddress(module, name);
}

/* Opt-in (TU_WDDM_TIMING=1) per-call timing of the kernel thunks, to find
 * where a guest spends the gap between a fence retiring on the host and the
 * next submission.  Once a second it prints, per thunk, the calls made and the
 * time spent in them since the previous line.  The wrappers are installed over
 * the dispatch table entries, so no call site changes. */
enum {
   TU_WDDM_T_RENDER,
   TU_WDDM_T_ESCAPE,
   TU_WDDM_T_LOCK,
   TU_WDDM_T_UNLOCK,
   TU_WDDM_T_CREATE_ALLOC,
   TU_WDDM_T_DESTROY_ALLOC,
   TU_WDDM_T_MAKE_RESIDENT,
   TU_WDDM_T_EVICT,
   TU_WDDM_T_WAIT_CPU,
   TU_WDDM_T_CREATE_CONTEXT,
   TU_WDDM_T_DESTROY_CONTEXT,
   TU_WDDM_T_COUNT,
};

static const char *const tu_wddm_timing_names[TU_WDDM_T_COUNT] = {
   "Render", "Escape", "Lock", "Unlock", "CreateAlloc", "DestroyAlloc",
   "MakeResident", "Evict", "WaitCpu", "CreateContext", "DestroyContext",
};

static volatile LONG64 tu_wddm_timing_calls[TU_WDDM_T_COUNT];
static volatile LONG64 tu_wddm_timing_ticks[TU_WDDM_T_COUNT];
static volatile LONG64 tu_wddm_timing_max[TU_WDDM_T_COUNT];
static volatile LONG64 tu_wddm_timing_next_report;
static LONG64 tu_wddm_timing_freq;

static void
tu_wddm_timing_report(LONG64 now)
{
   static LONG64 prev_calls[TU_WDDM_T_COUNT];
   static LONG64 prev_ticks[TU_WDDM_T_COUNT];
   char line[768];
   int len = snprintf(line, sizeof(line), "TU_WDDM_TIME:");
   for (int i = 0; i < TU_WDDM_T_COUNT && len > 0 && len < (int)sizeof(line); ++i) {
      const LONG64 calls = tu_wddm_timing_calls[i];
      const LONG64 ticks = tu_wddm_timing_ticks[i];
      const LONG64 max_ticks = InterlockedExchange64(&tu_wddm_timing_max[i], 0);
      const LONG64 dc = calls - prev_calls[i];
      const LONG64 dt = ticks - prev_ticks[i];
      prev_calls[i] = calls;
      prev_ticks[i] = ticks;
      if (dc == 0)
         continue;
      len += snprintf(line + len, sizeof(line) - (size_t)len,
                      " %s n=%lld us=%lld max=%lld;", tu_wddm_timing_names[i],
                      (long long)dc, (long long)(dt * 1000000 / tu_wddm_timing_freq),
                      (long long)(max_ticks * 1000000 / tu_wddm_timing_freq));
   }
   (void)now;
   fprintf(stderr, "%s\n", line);
   fflush(stderr);
}

static void
tu_wddm_timing_account(int slot, LONG64 ticks)
{
   InterlockedIncrement64(&tu_wddm_timing_calls[slot]);
   InterlockedExchangeAdd64(&tu_wddm_timing_ticks[slot], ticks);
   for (;;) {
      const LONG64 seen = tu_wddm_timing_max[slot];
      if (ticks <= seen ||
          InterlockedCompareExchange64(&tu_wddm_timing_max[slot], ticks, seen) == seen)
         break;
   }
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   const LONG64 due = tu_wddm_timing_next_report;
   if (now.QuadPart >= due &&
       InterlockedCompareExchange64(&tu_wddm_timing_next_report,
                                    now.QuadPart + tu_wddm_timing_freq, due) == due)
      tu_wddm_timing_report(now.QuadPart);
}

template <int Slot, typename F>
struct tu_wddm_timed;

template <int Slot, typename R, typename... A>
struct tu_wddm_timed<Slot, R(APIENTRY *)(A...)> {
   static inline R(APIENTRY *real)(A...) = nullptr;

   static R APIENTRY call(A... args)
   {
      LARGE_INTEGER t0, t1;
      QueryPerformanceCounter(&t0);
      R result = real(args...);
      QueryPerformanceCounter(&t1);
      tu_wddm_timing_account(Slot, t1.QuadPart - t0.QuadPart);
      return result;
   }
};

#define TU_WDDM_TIMED(slot, member)                                        \
   do {                                                                    \
      if (dispatch->member != NULL) {                                      \
         using Timed = tu_wddm_timed<slot, decltype(dispatch->member)>;    \
         Timed::real = dispatch->member;                                   \
         dispatch->member = &Timed::call;                                  \
      }                                                                    \
   } while (0)

static void
tu_wddm_install_timing(struct tu_wddm_dispatch *dispatch)
{
   char enabled[4] = {};
   const DWORD len = GetEnvironmentVariableA("TU_WDDM_TIMING", enabled, sizeof(enabled));
   if (len != 1 || enabled[0] != '1')
      return;
   /* One installation per process: a second device would wrap the wrappers. */
   static volatile LONG installed;
   if (InterlockedCompareExchange(&installed, 1, 0) != 0)
      return;

   LARGE_INTEGER freq, now;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&now);
   tu_wddm_timing_freq = freq.QuadPart;
   tu_wddm_timing_next_report = now.QuadPart + freq.QuadPart;

   TU_WDDM_TIMED(TU_WDDM_T_RENDER, Render);
   TU_WDDM_TIMED(TU_WDDM_T_ESCAPE, Escape);
   TU_WDDM_TIMED(TU_WDDM_T_LOCK, Lock);
   TU_WDDM_TIMED(TU_WDDM_T_UNLOCK, Unlock);
   TU_WDDM_TIMED(TU_WDDM_T_CREATE_ALLOC, CreateAllocation);
   TU_WDDM_TIMED(TU_WDDM_T_DESTROY_ALLOC, DestroyAllocation2);
   TU_WDDM_TIMED(TU_WDDM_T_MAKE_RESIDENT, MakeResident);
   TU_WDDM_TIMED(TU_WDDM_T_EVICT, Evict);
   TU_WDDM_TIMED(TU_WDDM_T_WAIT_CPU, WaitForSynchronizationObjectFromCpu);
   TU_WDDM_TIMED(TU_WDDM_T_CREATE_CONTEXT, CreateContext);
   TU_WDDM_TIMED(TU_WDDM_T_DESTROY_CONTEXT, DestroyContext);
}
#undef TU_WDDM_TIMED

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

   TU_WDDM_LOAD(EnumAdapters2);
   TU_WDDM_LOAD(OpenAdapterFromLuid);
   TU_WDDM_LOAD(CloseAdapter);
   TU_WDDM_LOAD(QueryAdapterInfo);
   TU_WDDM_LOAD(CreateDevice);
   TU_WDDM_LOAD(DestroyDevice);
   TU_WDDM_LOAD(CreateContext);
   TU_WDDM_LOAD(DestroyContext);
   TU_WDDM_LOAD(CreateAllocation);
   TU_WDDM_LOAD(DestroyAllocation2);
   TU_WDDM_LOAD(Lock);
   TU_WDDM_LOAD(Unlock);
   TU_WDDM_LOAD(Escape);
   TU_WDDM_LOAD(Render);
   TU_WDDM_LOAD(GetDeviceState);

#define TU_WDDM_LOAD_OPTIONAL(name) \
   dispatch->name = reinterpret_cast<decltype(dispatch->name)>( \
      tu_wddm_get_proc(dispatch->gdi32, "D3DKMT" #name))

   /* Residency is required only by WDDM 2.0 adapters; tu_wddm_device_open
    * checks these entries once it knows the adapter's driver model. */
   TU_WDDM_LOAD_OPTIONAL(CreatePagingQueue);
   TU_WDDM_LOAD_OPTIONAL(DestroyPagingQueue);
   TU_WDDM_LOAD_OPTIONAL(MakeResident);
   TU_WDDM_LOAD_OPTIONAL(Evict);
   TU_WDDM_LOAD_OPTIONAL(WaitForSynchronizationObjectFromCpu);
   TU_WDDM_LOAD_OPTIONAL(QueryResourceInfoFromNtHandle);
   TU_WDDM_LOAD_OPTIONAL(OpenResourceFromNtHandle);

#undef TU_WDDM_LOAD_OPTIONAL
#undef TU_WDDM_LOAD
   tu_wddm_install_timing(dispatch);
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
