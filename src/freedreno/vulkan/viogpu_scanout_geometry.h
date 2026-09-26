#pragma once

/* Native scanout geometry v1. Keep this file byte-identical in the KMD and
 * Mesa; it describes presentation, never the AHB storage/lease ABI. */
#include <stddef.h>

typedef unsigned int VIOGPU_GEOMETRY_U32;
typedef unsigned long long VIOGPU_GEOMETRY_U64;
static_assert(sizeof(VIOGPU_GEOMETRY_U32) == 4, "geometry u32");
static_assert(sizeof(VIOGPU_GEOMETRY_U64) == 8, "geometry u64");

#define VIOGPU_SCANOUT_GEOMETRY_VERSION 1U
#define VIOGPU_SCANOUT_GEOMETRY_SIZE 64U
#define VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT 8192U

#pragma pack(push, 4)
struct VIOGPU_SCANOUT_GEOMETRY
{
    VIOGPU_GEOMETRY_U32 Version;
    VIOGPU_GEOMETRY_U32 Size;
    VIOGPU_GEOMETRY_U32 StorageWidth;
    VIOGPU_GEOMETRY_U32 StorageHeight;
    VIOGPU_GEOMETRY_U32 LogicalWidth;
    VIOGPU_GEOMETRY_U32 LogicalHeight;
    VIOGPU_GEOMETRY_U32 ContentRotationCw;
    VIOGPU_GEOMETRY_U32 Reserved;
    VIOGPU_GEOMETRY_U64 HostResetGeneration;
    VIOGPU_GEOMETRY_U64 ModeGeneration;
    VIOGPU_GEOMETRY_U64 ReservedTail[2];
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_SCANOUT_GEOMETRY) == VIOGPU_SCANOUT_GEOMETRY_SIZE, "geometry payload");
static_assert(offsetof(VIOGPU_SCANOUT_GEOMETRY, HostResetGeneration) == 32, "host reset offset");
static_assert(offsetof(VIOGPU_SCANOUT_GEOMETRY, ModeGeneration) == 40, "mode offset");

/* Only a QUERY may carry mode zero, and then it must describe identity.
 * HostResetGeneration is obtained from the host, not the KMD reset counter. */
static inline bool VioGpuScanoutGeometryValid(const VIOGPU_SCANOUT_GEOMETRY *g, bool configured = true)
{
    if (g == 0 || g->Version != VIOGPU_SCANOUT_GEOMETRY_VERSION ||
        g->Size != VIOGPU_SCANOUT_GEOMETRY_SIZE || g->Reserved != 0 ||
        g->ReservedTail[0] != 0 || g->ReservedTail[1] != 0 ||
        g->HostResetGeneration == 0 || g->ContentRotationCw > 3 ||
        g->StorageWidth == 0 || g->StorageWidth > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        g->StorageHeight == 0 || g->StorageHeight > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        g->LogicalWidth == 0 || g->LogicalWidth > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        g->LogicalHeight == 0 || g->LogicalHeight > VIOGPU_SCANOUT_GEOMETRY_MAX_EXTENT ||
        (configured ? g->ModeGeneration == 0 : (g->ModeGeneration != 0 || g->ContentRotationCw != 0)))
        return false;
    return (g->ContentRotationCw & 1)
        ? g->StorageWidth == g->LogicalHeight && g->StorageHeight == g->LogicalWidth
        : g->StorageWidth == g->LogicalWidth && g->StorageHeight == g->LogicalHeight;
}

static inline bool VioGpuScanoutGeometryEqual(const VIOGPU_SCANOUT_GEOMETRY *a,
                                             const VIOGPU_SCANOUT_GEOMETRY *b)
{
    return VioGpuScanoutGeometryValid(a) && VioGpuScanoutGeometryValid(b) &&
        a->StorageWidth == b->StorageWidth && a->StorageHeight == b->StorageHeight &&
        a->LogicalWidth == b->LogicalWidth && a->LogicalHeight == b->LogicalHeight &&
        a->ContentRotationCw == b->ContentRotationCw &&
        a->HostResetGeneration == b->HostResetGeneration && a->ModeGeneration == b->ModeGeneration;
}

/* The host token must have been queried in the current transport epoch.
 * A mode rollback is a newer tuple; even identity cannot clear the profile. */
static inline bool VioGpuScanoutGeometryMayConfigure(const VIOGPU_SCANOUT_GEOMETRY *current,
                                                     const VIOGPU_SCANOUT_GEOMETRY *next,
                                                     VIOGPU_GEOMETRY_U64 queriedHostReset)
{
    if (!VioGpuScanoutGeometryValid(next) || next->HostResetGeneration != queriedHostReset)
        return false;
    if (current == 0)
        return true;
    if (!VioGpuScanoutGeometryValid(current) || next->HostResetGeneration < current->HostResetGeneration)
        return false;
    if (next->HostResetGeneration != current->HostResetGeneration)
        return true;
    return next->ModeGeneration > current->ModeGeneration || VioGpuScanoutGeometryEqual(current, next);
}

/* Windows VidPN values 1..4 are counterclockwise. Reject UNINITIALIZED,
 * NOTSPECIFIED/UNPINNED and path-independent OFFSET rotations explicitly.
 * Do not pass DXGI or Android bitmasks to this function. */
static inline bool VioGpuVidPnRotationToContentCw(unsigned int rotation, unsigned int *cw)
{
    if (cw == 0 || rotation < 1 || rotation > 4)
        return false;
    *cw = (5U - rotation) & 3U;
    return true;
}

/* Admission prerequisites are separate from feature negotiation. A configured
 * host tuple alone promises neither producer orientation nor WDDM updates. */
struct VIOGPU_SCANOUT_READINESS
{
    bool HostGeometry;
    bool ProducerFinalRender;
    bool RotationAwarePrimary;
    bool ModeCommitAndUpdate;
    bool ResourceBinding;
    bool AllPresentPaths;
};

static inline bool VioGpuScanoutReady(const VIOGPU_SCANOUT_READINESS &r)
{
    return r.HostGeometry && r.ProducerFinalRender && r.RotationAwarePrimary &&
           r.ModeCommitAndUpdate && r.ResourceBinding && r.AllPresentPaths;
}

/* Record this with the allocation, never derive it from the current mode at
 * present/retry/refresh time. A different mode needs a new allocation. */
struct VIOGPU_SCANOUT_BINDING
{
    VIOGPU_GEOMETRY_U32 ResourceId;
    VIOGPU_GEOMETRY_U64 ShareKey;
    VIOGPU_GEOMETRY_U64 LocalResetGeneration;
    VIOGPU_GEOMETRY_U64 EndpointGeneration;
    VIOGPU_GEOMETRY_U64 ProfileGeneration;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
};

static inline bool VioGpuScanoutBindingMatches(const VIOGPU_SCANOUT_BINDING *binding,
                                              const VIOGPU_SCANOUT_GEOMETRY *active,
                                              unsigned int resourceId,
                                              VIOGPU_GEOMETRY_U64 shareKey,
                                              VIOGPU_GEOMETRY_U64 localReset,
                                              unsigned int width, unsigned int height)
{
    return binding != 0 && resourceId != 0 && shareKey != 0 && localReset != 0 &&
        binding->ResourceId == resourceId && binding->ShareKey == shareKey &&
        binding->LocalResetGeneration == localReset &&
        VioGpuScanoutGeometryEqual(&binding->Geometry, active) &&
        width == binding->Geometry.StorageWidth && height == binding->Geometry.StorageHeight;
}

/* Source/mip storage is checked as storage, independently of the logical
 * desktop. No inference from equal pixel counts or an orientation bit. */
static inline bool VioGpuScanoutStorageMatches(const VIOGPU_SCANOUT_GEOMETRY *geometry,
                                              unsigned int width, unsigned int height)
{
    return VioGpuScanoutGeometryValid(geometry) &&
           width == geometry->StorageWidth && height == geometry->StorageHeight;
}
