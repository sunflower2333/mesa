#pragma once

#include "viogpu_wddm_scanout.h"

/* Explicitly diagnostic v1. Mechanisms describe implemented code paths, never
 * observed DWM pixels, orientation or final-pass rendering. Production opcode12
 * retains its independent readiness contract and remains zero. */
#define VIOGPU_WDDM_ESCAPE_SCANOUT_DIAGNOSTIC 14U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_VERSION 1U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_RESERVE 1U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_RESERVATION 1U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_CONFIGURE_COMMIT 2U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_DEFERRED_BIND 4U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_VIDPN 8U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_PRIMARY_CONTRACT 16U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_BOOTSTRAP 32U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_MECHANISMS_ALL 63U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_PROFILE_AVAILABLE 1U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_CANDIDATE 2U
#define VIOGPU_WDDM_SCANOUT_DIAGNOSTIC_COMMITTED 4U

#pragma pack(push, 4)
struct VIOGPU_WDDM_SCANOUT_DIAGNOSTIC
{
    VIOGPU_WDDM_ABI_HEADER Header;
    VIOGPU_WDDM_UINT32 Opcode;
    VIOGPU_WDDM_UINT32 Version;
    VIOGPU_WDDM_UINT32 RequestFlags;
    VIOGPU_WDDM_UINT32 Mechanisms;
    VIOGPU_WDDM_UINT32 StateFlags;
    VIOGPU_WDDM_UINT32 ModeWidth;
    VIOGPU_WDDM_UINT32 ModeHeight;
    VIOGPU_WDDM_UINT32 ModeRotation;
    VIOGPU_WDDM_UINT64 LocalResetGeneration;
    VIOGPU_WDDM_UINT64 Reserved;
    VIOGPU_SCANOUT_GEOMETRY Geometry;
    VIOGPU_SCANOUT_PROFILE Profile;
};
#pragma pack(pop)
static_assert(sizeof(VIOGPU_WDDM_SCANOUT_DIAGNOSTIC) == 192, "diagnostic state ABI");
static_assert(offsetof(VIOGPU_WDDM_SCANOUT_DIAGNOSTIC, Geometry) == 64, "diagnostic geometry offset");
