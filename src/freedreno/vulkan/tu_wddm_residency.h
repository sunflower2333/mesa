/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * WDDM 2.0 residency policy shared by the DroidVM Windows user-mode drivers.
 *
 * This header is deliberately WDK-independent: it holds only the status
 * classification and the bounded MakeResident/trim loop.  The Turnip WDDM
 * transport drives it through D3DKMTMakeResident (NTSTATUS), the D3D10/11 UMD
 * through the runtime pfnMakeResidentCb (HRESULT).  Keeping one loop keeps the
 * two drivers' budget behaviour identical and lets host tests execute it.
 *
 * Contract (d3dumddi pfnMakeResidentCb, d3dukmdt D3DDDI_MAKERESIDENT,
 * display/residency-overview.md, display/process-residency-budgets.md):
 *  - Residency in WDDM v2 is controlled exclusively by the device residency
 *    requirement list; an allocation-list reference to a non-resident
 *    allocation makes VidSch reject the submission and put the device in error.
 *  - MakeResident and Evict are reference counted per allocation.
 *  - success (S_OK / STATUS_SUCCESS): resident and usable immediately.
 *  - pending (E_PENDING / STATUS_PENDING): on the list, but the caller must wait
 *    for PagingFenceValue on the paging queue's monitored fence before it
 *    submits a command buffer referencing the allocation.  A fence value of
 *    zero means the operation already completed.
 *  - over budget (E_OUTOFMEMORY / STATUS_NO_MEMORY): atomic failure, no
 *    residency count changed, NumBytesToTrim is the amount to trim before a
 *    retry.  The driver retries in a loop, trimming between attempts, and
 *    makes its final attempt with CantTrimFurther once nothing more can be
 *    trimmed.  If that final attempt fails the request fails.
 */

#ifndef TU_WDDM_RESIDENCY_H
#define TU_WDDM_RESIDENCY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum tu_wddm_residency_outcome {
   /* A residency reference was taken and the allocation is usable now. */
   TU_WDDM_RESIDENCY_RESIDENT = 0,
   /* A residency reference was taken; wait for the paging fence first. */
   TU_WDDM_RESIDENCY_PENDING = 1,
   /* No reference was taken: the process is over its residency budget. */
   TU_WDDM_RESIDENCY_OVER_BUDGET = 2,
   /* No reference was taken for any other reason. */
   TU_WDDM_RESIDENCY_FAILED = 3,
};

/* Every MakeResident call for one request, including the final attempt made
 * with CantTrimFurther.  The loop can never issue more calls than this. */
#define TU_WDDM_RESIDENCY_MAX_ATTEMPTS 8U

/* D3DKMT_DRIVERVERSION (d3dkmthk QAI_DRIVERVERSION) KMT_DRIVERVERSION_WDDM_2_0.
 * Residency calls are made only for adapters that report at least this. */
#define TU_WDDM_DRIVER_VERSION_WDDM_2_0 2000U

/* One MakeResident attempt.  cant_trim_further selects the
 * D3DDDI_MAKERESIDENT_FLAGS.CantTrimFurther bit.  paging_fence and
 * bytes_to_trim are the D3DDDI_MAKERESIDENT out fields. */
typedef enum tu_wddm_residency_outcome
(*tu_wddm_make_resident_attempt_fn)(void *data, bool cant_trim_further,
                                    uint64_t *paging_fence,
                                    uint64_t *bytes_to_trim);

/* Releases residency this driver can recreate on demand.  Returns the number
 * of bytes actually released; zero means nothing more can be trimmed. */
typedef uint64_t (*tu_wddm_trim_fn)(void *data, uint64_t bytes_to_trim);

static inline enum tu_wddm_residency_outcome
tu_wddm_residency_classify_ntstatus(uint32_t status)
{
   switch (status) {
   case UINT32_C(0x00000000): /* STATUS_SUCCESS */
      return TU_WDDM_RESIDENCY_RESIDENT;
   case UINT32_C(0x00000103): /* STATUS_PENDING: success code, still pending */
      return TU_WDDM_RESIDENCY_PENDING;
   case UINT32_C(0xc0000017): /* STATUS_NO_MEMORY (process residency budget) */
   case UINT32_C(0xc01e0100): /* STATUS_GRAPHICS_NO_VIDEO_MEMORY */
      return TU_WDDM_RESIDENCY_OVER_BUDGET;
   default:
      /* Any other status, including other success codes, is not the
       * documented contract.  Fail closed: the caller releases the handle. */
      return TU_WDDM_RESIDENCY_FAILED;
   }
}

static inline enum tu_wddm_residency_outcome
tu_wddm_residency_classify_hresult(uint32_t hr)
{
   switch (hr) {
   case UINT32_C(0x00000000): /* S_OK */
      return TU_WDDM_RESIDENCY_RESIDENT;
   case UINT32_C(0x8000000a): /* E_PENDING: FAILED() is true, yet it succeeded */
      return TU_WDDM_RESIDENCY_PENDING;
   case UINT32_C(0x8007000e): /* E_OUTOFMEMORY */
   case UINT32_C(0x8876017c): /* D3DERR_OUTOFVIDEOMEMORY */
      return TU_WDDM_RESIDENCY_OVER_BUDGET;
   default:
      return TU_WDDM_RESIDENCY_FAILED;
   }
}

/* Make one request resident with the bounded trim-and-retry loop.
 *
 * Returns RESIDENT or PENDING only when exactly one attempt took the residency
 * reference; *paging_fence is then the fence to wait for (zero for RESIDENT).
 * On OVER_BUDGET or FAILED no reference is held.  *attempts receives the number
 * of MakeResident calls issued, never more than TU_WDDM_RESIDENCY_MAX_ATTEMPTS.
 */
static inline enum tu_wddm_residency_outcome
tu_wddm_make_resident_bounded(tu_wddm_make_resident_attempt_fn attempt_fn,
                              tu_wddm_trim_fn trim_fn, void *data,
                              uint64_t *paging_fence, uint32_t *attempts)
{
   if (paging_fence != NULL)
      *paging_fence = 0;
   if (attempts != NULL)
      *attempts = 0;
   if (attempt_fn == NULL || paging_fence == NULL || attempts == NULL)
      return TU_WDDM_RESIDENCY_FAILED;

   bool cant_trim_further = false;
   for (uint32_t attempt = 0; attempt < TU_WDDM_RESIDENCY_MAX_ATTEMPTS; attempt++) {
      uint64_t fence = 0;
      uint64_t bytes_to_trim = 0;
      *attempts = attempt + 1;
      const enum tu_wddm_residency_outcome outcome =
         attempt_fn(data, cant_trim_further, &fence, &bytes_to_trim);
      switch (outcome) {
      case TU_WDDM_RESIDENCY_RESIDENT:
         return TU_WDDM_RESIDENCY_RESIDENT;
      case TU_WDDM_RESIDENCY_PENDING:
         /* A zero paging fence names an operation that already finished. */
         *paging_fence = fence;
         return fence != 0 ? TU_WDDM_RESIDENCY_PENDING : TU_WDDM_RESIDENCY_RESIDENT;
      case TU_WDDM_RESIDENCY_OVER_BUDGET:
         break;
      default:
         return TU_WDDM_RESIDENCY_FAILED;
      }

      /* The attempt that already carried CantTrimFurther was the final one. */
      if (cant_trim_further)
         return TU_WDDM_RESIDENCY_OVER_BUDGET;

      const uint64_t trimmed = trim_fn != NULL ? trim_fn(data, bytes_to_trim) : 0;
      /* Keep trimming only while it releases something, and always reserve the
       * last permitted call for the CantTrimFurther attempt. */
      if (trimmed == 0 || attempt + 2 >= TU_WDDM_RESIDENCY_MAX_ATTEMPTS)
         cant_trim_further = true;
   }

   return TU_WDDM_RESIDENCY_OVER_BUDGET;
}

#ifdef __cplusplus
}
#endif

#endif /* TU_WDDM_RESIDENCY_H */
