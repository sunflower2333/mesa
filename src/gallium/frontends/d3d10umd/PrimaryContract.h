#pragma once

#include "viogpu_scanout_geometry.h"

/* Values are checked against the DDI enums at the call site. Keep the policy
 * independent of WDK headers so malformed descriptions run in host tests. */
struct DroidvmPrimaryDescription
{
   unsigned flags, source_id;
   unsigned mode_width, mode_height, mode_rotation, mode_format;
   unsigned refresh_numerator, refresh_denominator;
   unsigned width, height, depth, format, mips, array_size, samples, quality;
   bool texture_2d, runtime_present;
};

/* The KMD query must attest every readiness bit plus the committed tuple and
 * exact DDI mode representation. Current KMD keeps those bits clear; neither
 * host preference nor a rotated desktop can synthesize this contract. */
struct DroidvmPrimaryProfile
{
   VIOGPU_SCANOUT_GEOMETRY geometry;
   VIOGPU_SCANOUT_READINESS readiness;
   unsigned mode_width, mode_height, mode_rotation;
   bool diagnostic = false;
   unsigned diagnostic_mechanisms = 0;
};

static inline bool
DroidvmPrimaryCanScanOut(const DroidvmPrimaryDescription &p,
                        const DroidvmPrimaryProfile *profile = 0)
{
   if (!p.runtime_present || !p.texture_2d || p.source_id != 0 || (p.flags & ~3U) != 0 ||
       p.width == 0 || p.height == 0 || p.depth != 1 || p.mips != 1 || p.array_size != 1 ||
       p.samples != 1 || p.quality != 0 || p.mode_format != p.format ||
       p.refresh_numerator == 0 || p.refresh_denominator == 0)
      return false;

   if (profile == 0)
      return p.mode_rotation == 1 && p.mode_width == p.width && p.mode_height == p.height;

   if ((profile->diagnostic ? profile->diagnostic_mechanisms != 63U
                            : !VioGpuScanoutReady(profile->readiness)) ||
       !VioGpuScanoutStorageMatches(&profile->geometry, p.width, p.height) ||
       p.mode_width != profile->mode_width || p.mode_height != profile->mode_height ||
       p.mode_rotation != profile->mode_rotation || p.mode_rotation < 1 || p.mode_rotation > 4)
      return false;

   /* NONPREROTATED is the producer's identity-resource contract: the producer
    * handles the viewport/projection. Without it a rotated primary requires
    * UMD render/lock/blt transformations, which this direct path cannot do.
    * Physical mip size alone does not establish the content orientation. */
   return (profile->geometry.ContentRotationCw == 0 && p.mode_rotation == 1) ||
          (p.mode_rotation != 1 && (p.flags & 2U) != 0);
}
