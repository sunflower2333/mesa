#pragma once

#include "viogpu_scanout_geometry.h"

/* Host preferred profile v1. Mirror byte-for-byte in Mesa. These flags attest
 * host endpoints/mechanisms only, never producer or WDDM readiness. */
#define VIOGPU_SCANOUT_PROFILE_VERSION 1U
#define VIOGPU_SCANOUT_PROFILE_SIZE 64U
#define VIOGPU_SCANOUT_PROFILE_ATTACHED 1U
#define VIOGPU_SCANOUT_PROFILE_MAIN_HINT 2U
#define VIOGPU_SCANOUT_PROFILE_NATIVE_ATOMIC 4U
#define VIOGPU_SCANOUT_PROFILE_USABLE 7U

#pragma pack(push, 4)
struct VIOGPU_SCANOUT_PROFILE
{
    VIOGPU_GEOMETRY_U32 Version;
    VIOGPU_GEOMETRY_U32 Size;
    VIOGPU_GEOMETRY_U32 Flags;
    VIOGPU_GEOMETRY_U32 ContentRotationCw;
    VIOGPU_GEOMETRY_U32 StorageWidth;
    VIOGPU_GEOMETRY_U32 StorageHeight;
    VIOGPU_GEOMETRY_U32 LogicalWidth;
    VIOGPU_GEOMETRY_U32 LogicalHeight;
    VIOGPU_GEOMETRY_U64 EndpointGeneration;
    VIOGPU_GEOMETRY_U64 ProfileGeneration;
    VIOGPU_GEOMETRY_U64 ReservedTail[2];
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_SCANOUT_PROFILE) == VIOGPU_SCANOUT_PROFILE_SIZE, "profile payload");
static_assert(offsetof(VIOGPU_SCANOUT_PROFILE, EndpointGeneration) == 32, "profile endpoint offset");
static_assert(offsetof(VIOGPU_SCANOUT_PROFILE, ProfileGeneration) == 40, "profile generation offset");

static inline bool VioGpuScanoutProfileValid(const VIOGPU_SCANOUT_PROFILE *p)
{
    if (p == 0 || p->Version != VIOGPU_SCANOUT_PROFILE_VERSION || p->Size != VIOGPU_SCANOUT_PROFILE_SIZE ||
        p->ReservedTail[0] != 0 || p->ReservedTail[1] != 0)
        return false;
    /* Unavailable has no guessed orientation/extent; epochs may survive revocation. */
    if (p->Flags == 0)
        return p->ContentRotationCw == 0 && p->StorageWidth == 0 && p->StorageHeight == 0 &&
               p->LogicalWidth == 0 && p->LogicalHeight == 0;
    if (p->Flags != VIOGPU_SCANOUT_PROFILE_USABLE || p->EndpointGeneration == 0 || p->ProfileGeneration == 0 ||
        p->ContentRotationCw > 3 ||
        p->StorageWidth == 0 || p->StorageWidth > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        p->StorageHeight == 0 || p->StorageHeight > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        p->LogicalWidth == 0 || p->LogicalWidth > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        p->LogicalHeight == 0 || p->LogicalHeight > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT)
        return false;
    return (p->ContentRotationCw & 1)
        ? p->StorageWidth == p->LogicalHeight && p->StorageHeight == p->LogicalWidth
        : p->StorageWidth == p->LogicalWidth && p->StorageHeight == p->LogicalHeight;
}

static inline bool VioGpuScanoutProfileMatches(const VIOGPU_SCANOUT_PROFILE *profile,
                                               const VIOGPU_SCANOUT_GEOMETRY *geometry)
{
    return VioGpuScanoutProfileValid(profile) && profile->Flags == VIOGPU_SCANOUT_PROFILE_USABLE &&
        VioGpuScanoutGeometryValid(geometry) &&
        profile->StorageWidth == geometry->StorageWidth && profile->StorageHeight == geometry->StorageHeight &&
        profile->LogicalWidth == geometry->LogicalWidth && profile->LogicalHeight == geometry->LogicalHeight &&
        profile->ContentRotationCw == geometry->ContentRotationCw;
}

static inline bool VioGpuScanoutProfileEqual(const VIOGPU_SCANOUT_PROFILE *a,
                                            const VIOGPU_SCANOUT_PROFILE *b)
{
    return VioGpuScanoutProfileValid(a) && VioGpuScanoutProfileValid(b) &&
        a->Flags == VIOGPU_SCANOUT_PROFILE_USABLE && b->Flags == a->Flags &&
        a->ContentRotationCw == b->ContentRotationCw &&
        a->StorageWidth == b->StorageWidth && a->StorageHeight == b->StorageHeight &&
        a->LogicalWidth == b->LogicalWidth && a->LogicalHeight == b->LogicalHeight &&
        a->EndpointGeneration == b->EndpointGeneration && a->ProfileGeneration == b->ProfileGeneration;
}

static inline bool VioGpuScanoutBindingProfileMatches(const VIOGPU_SCANOUT_BINDING *binding,
                                                      const VIOGPU_SCANOUT_GEOMETRY *active,
                                                      const VIOGPU_SCANOUT_PROFILE *profile)
{
    return binding != 0 && VioGpuScanoutGeometryEqual(&binding->Geometry, active) &&
        VioGpuScanoutProfileMatches(profile, active) &&
        binding->EndpointGeneration == profile->EndpointGeneration &&
        binding->ProfileGeneration == profile->ProfileGeneration;
}
