/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 * Internal helpers shared by the KMT transport and its API consumers.
 */
#ifndef FREEDRENO_WDDM_PRIVATE_H
#define FREEDRENO_WDDM_PRIVATE_H
#include "freedreno_wddm.h"

/* The SDK headers do not consistently export STATUS_SUCCESS to user-mode
 * translation units; its NTSTATUS value is defined by the WDK contract. */
static constexpr NTSTATUS TU_WDDM_STATUS_SUCCESS = static_cast<NTSTATUS>(0);
/* VidMm may defer the KMD DestroyAllocation callback after returning success.
 * A fixed native IOVA remains reserved until that callback detaches its range,
 * so retry only the transient busy result while preserving real collisions. */
static constexpr NTSTATUS TU_WDDM_STATUS_DEVICE_BUSY = static_cast<NTSTATUS>(0x80000011L);
static constexpr uint32_t TU_WDDM_CREATE_BUSY_RETRIES = 1000;
/* Context destroy can also race asynchronous VidSch submission retirement. */
static constexpr NTSTATUS TU_WDDM_STATUS_GRAPHICS_ALLOCATION_BUSY =
   static_cast<NTSTATUS>(0xc01e0102L);
static constexpr uint32_t TU_WDDM_DESTROY_BUSY_RETRIES = 1000;
/* Retiring this context's submissions before a destroy is an optimisation, not a
 * correctness requirement - AssumeNotInUse is never claimed, so VidMm decides.
 * It must therefore be bounded: a fence that never retires, which is exactly
 * what happens after a fault or an adapter reset, must not hang the caller
 * forever.  The busy-retry loop below absorbs the rest. */
static constexpr uint64_t TU_WDDM_DESTROY_WAIT_TIMEOUT_NS = UINT64_C(250000000);

/* Sleep(1) may defer every fence poll by a full ~15.6 ms scheduler tick.
 * Use a per-wait one-shot timer without changing the process/global timer
 * resolution. Create it only after observing pending work and checking the
 * deadline, so completed fences and nonblocking queries need no handle.
 * Waking up is only a reason to query again, never evidence of completion. */
class tu_wddm_fence_poll_wait {
public:
   tu_wddm_fence_poll_wait() = default;
   tu_wddm_fence_poll_wait(const tu_wddm_fence_poll_wait &) = delete;
   tu_wddm_fence_poll_wait &operator=(const tu_wddm_fence_poll_wait &) = delete;

   ~tu_wddm_fence_poll_wait()
   {
      if (timer != NULL)
         CloseHandle(timer);
   }

   void wait(uint64_t remaining_ns = UINT64_MAX)
   {
      if (remaining_ns == 0)
         return;
      if (!initialized) {
         initialized = true;
         timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
      }
      if (timer != NULL) {
         const uint64_t interval_ns = remaining_ns < UINT64_C(1000000)
                                        ? remaining_ns : UINT64_C(1000000);
         LARGE_INTEGER due;
         /* Relative time is negative, in 100ns units; round up, never to 0. */
         due.QuadPart = -static_cast<LONGLONG>((interval_ns + 99) / 100);
         /* WAIT_OBJECT_0 is zero, but its SDK macro references STATUS_WAIT_0,
          * hidden by this transport's WIN32_NO_STATUS header boundary. */
         if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
             WaitForSingleObject(timer, INFINITE) == 0)
            return;
         CloseHandle(timer);
         timer = NULL;
      }
      /* Older Windows or resource pressure retain the existing sleep path. */
      Sleep(1);
   }

private:
   HANDLE timer = NULL;
   bool initialized = false;
};

template <typename T>
static constexpr uint32_t
tu_wddm_sizeof()
{
   static_assert(sizeof(T) <= UINT32_MAX, "WDDM size field overflow");
   return static_cast<uint32_t>(sizeof(T));
}

static constexpr uint64_t TU_WDDM_FENCE_HALF_RANGE = UINT64_C(1) << 31;

static inline bool
tu_wddm_fence_after(uint32_t a, uint32_t b)
{
   return a != b && static_cast<int32_t>(a - b) > 0;
}

static constexpr uint64_t
tu_wddm_fence_distance(uint32_t submitted, uint32_t completed)
{
   return submitted > completed
             ? static_cast<uint64_t>(submitted - completed)
             : static_cast<uint64_t>(UINT32_MAX - completed) + submitted;
}

static_assert(tu_wddm_fence_distance(4096, 0) == 4096,
              "WDDM initial fence distance changed");
static_assert(tu_wddm_fence_distance(1, UINT32_MAX) == 1,
              "WDDM wrapped fence distance changed");
static_assert(tu_wddm_fence_distance(UINT32_C(0x80000000), 0) == TU_WDDM_FENCE_HALF_RANGE,
              "WDDM half-range fence distance changed");

/* One KMT attempt, no sleeping or retry scheduling.  An error retains ownership. */
NTSTATUS tu_wddm_allocation_try_destroy(struct tu_wddm_allocation *allocation);
#endif /* FREEDRENO_WDDM_PRIVATE_H */
