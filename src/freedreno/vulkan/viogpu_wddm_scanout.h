#pragma once

/* Include the endpoint's existing private ABI before this header. Keep this
 * file identical in KMD and Mesa; no legacy reserved fields are repurposed. */
#include "viogpu_scanout_profile.h"

#define VIOGPU_WDDM_ESCAPE_QUERY_SCANOUT_PROFILE 12U
#define VIOGPU_WDDM_ESCAPE_ALLOCATE_PROFILE_SURFACE 13U
#define VIOGPU_WDDM_SCANOUT_READY_ALL 63U
#define VIOGPU_WDDM_PROFILE_SURFACE_CANDIDATE 1U

#pragma pack(push, 4)
struct VIOGPU_WDDM_SCANOUT_STATE
{
    VIOGPU_WDDM_ABI_HEADER Header;
    VIOGPU_WDDM_UINT32 Opcode;
    VIOGPU_WDDM_UINT32 ReadyFlags;
    VIOGPU_WDDM_UINT64 LocalResetGeneration;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
    VIOGPU_SCANOUT_PROFILE Profile;
    VIOGPU_WDDM_UINT32 ModeWidth;
    VIOGPU_WDDM_UINT32 ModeHeight;
    VIOGPU_WDDM_UINT32 ModeRotation;
    VIOGPU_WDDM_UINT32 Reserved;
};

struct VIOGPU_WDDM_NATIVE_PROFILE_SURFACE
{
    VIOGPU_WDDM_ABI_HEADER Header;
    VIOGPU_WDDM_UINT32 Opcode;
    VIOGPU_WDDM_UINT32 Flags;
    VIOGPU_WDDM_UINT64 ExpectedLocalResetGeneration;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
    VIOGPU_SCANOUT_PROFILE Profile;
    VIOGPU_WDDM_NATIVE_SURFACE Surface;
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_WDDM_SCANOUT_STATE) == 176, "scanout state ABI");
static_assert(sizeof(VIOGPU_WDDM_NATIVE_PROFILE_SURFACE) == 288, "profile surface ABI");
static_assert(offsetof(VIOGPU_WDDM_NATIVE_PROFILE_SURFACE, Surface) == 160, "profile surface offset");
