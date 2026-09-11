/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 *
 * Exact-revision private ABI shared with the DroidVM viogpu WDDM miniport.
 *
 * This is deliberately a small, WDK-independent header.  The Windows KMD
 * copy lives in viogpu/shared/viogpu_wddm_abi.h.  Keep the two copies layout
 * identical; do not add compatibility fields or pointer-sized members here.
 */

#ifndef TU_WDDM_ABI_H
#define TU_WDDM_ABI_H

#include <stddef.h>
#include <stdint.h>

#define VIOGPU_WDDM_ABI_MAGIC                0x504D5644U
#define VIOGPU_WDDM_ABI_VERSION              0U

#define VIOGPU_WDDM_CAPABILITIES_NONE        0ULL

#define VIOGPU_WDDM_ALLOCATION_PRIMARY       0x00000001U
#define VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE   0x00000002U
#define VIOGPU_WDDM_ALLOCATION_NATIVE        0x00000004U
#define VIOGPU_WDDM_ALLOCATION_GPU_READ_ONLY 0x00000008U

#define VIOGPU_WDDM_CONTEXT_FLAGS_NONE       0U
#define VIOGPU_WDDM_ESCAPE_FLAGS_NONE        0U
#define VIOGPU_WDDM_RENDER_FLAGS_NONE        0U

#define VIOGPU_WDDM_REFERENCE_READ           0x00000001U
#define VIOGPU_WDDM_REFERENCE_WRITE          0x00000002U

typedef uint32_t VIOGPU_WDDM_UINT32;
typedef uint64_t VIOGPU_WDDM_UINT64;

typedef enum VIOGPU_WDDM_FORMAT {
   VIOGPU_WDDM_FORMAT_NONE = 0,
   VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM = 1,
   VIOGPU_WDDM_FORMAT_B8G8R8X8_UNORM = 2,
   VIOGPU_WDDM_FORMAT_R8G8B8A8_UNORM = 3,
   VIOGPU_WDDM_FORMAT_R10G10B10A2_UNORM = 4,
   VIOGPU_WDDM_FORMAT_B10G10R10A2_UNORM = 5,
} VIOGPU_WDDM_FORMAT;

typedef enum VIOGPU_WDDM_RENDER_OPCODE {
   VIOGPU_WDDM_RENDER_NATIVE_SUBMIT = 1,
   VIOGPU_WDDM_RENDER_ALLOCATION_COPY = 2,
} VIOGPU_WDDM_RENDER_OPCODE;

typedef enum VIOGPU_WDDM_ESCAPE_OPCODE {
   VIOGPU_WDDM_ESCAPE_GET_CONTEXT_INFO = 1,
   VIOGPU_WDDM_ESCAPE_GET_COMPLETED_FENCE = 2,
   VIOGPU_WDDM_ESCAPE_PRESENT_BLIT = 3,
} VIOGPU_WDDM_ESCAPE_OPCODE;

#pragma pack(push, 4)

typedef struct VIOGPU_WDDM_ABI_HEADER {
   VIOGPU_WDDM_UINT32 Magic;
   VIOGPU_WDDM_UINT32 Version;
   VIOGPU_WDDM_UINT32 Size;
   VIOGPU_WDDM_UINT32 Reserved;
} VIOGPU_WDDM_ABI_HEADER;

typedef struct VIOGPU_WDDM_ADAPTER_INFO {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT64 Capabilities;
   VIOGPU_WDDM_UINT64 ResetGeneration;
   VIOGPU_WDDM_UINT32 MsmMajorVersion;
   VIOGPU_WDDM_UINT32 MsmMinorVersion;
   VIOGPU_WDDM_UINT32 MsmPatchVersion;
   VIOGPU_WDDM_UINT32 GpuId;
   VIOGPU_WDDM_UINT64 ChipId;
   VIOGPU_WDDM_UINT32 GmemSize;
   VIOGPU_WDDM_UINT32 PriorityCount;
   VIOGPU_WDDM_UINT64 GmemBase;
   VIOGPU_WDDM_UINT32 HighestBankBit;
   VIOGPU_WDDM_UINT32 HasCachedCoherentMemory;
   VIOGPU_WDDM_UINT64 UbwcSwizzle;
   VIOGPU_WDDM_UINT64 MacrotileMode;
   VIOGPU_WDDM_UINT64 UcheTrapBase;
   VIOGPU_WDDM_UINT32 HasRayTracing;
   VIOGPU_WDDM_UINT32 MaxFrequency;
   VIOGPU_WDDM_UINT64 Reserved[2];
} VIOGPU_WDDM_ADAPTER_INFO;

typedef struct VIOGPU_WDDM_ALLOCATION_INFO {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT64 Size;
   VIOGPU_WDDM_UINT64 Alignment;
   VIOGPU_WDDM_UINT64 RequestedIova;
   VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT32 Format;
   VIOGPU_WDDM_UINT32 Width;
   VIOGPU_WDDM_UINT32 Height;
   VIOGPU_WDDM_UINT32 Pitch;
   VIOGPU_WDDM_UINT32 RefreshRateNumerator;
   VIOGPU_WDDM_UINT32 RefreshRateDenominator;
   VIOGPU_WDDM_UINT32 ContextId;
} VIOGPU_WDDM_ALLOCATION_INFO;

typedef struct VIOGPU_WDDM_CONTEXT_CREATE {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT32 Reserved;
} VIOGPU_WDDM_CONTEXT_CREATE;

typedef struct VIOGPU_WDDM_CONTEXT_INFO {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT32 Opcode;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
   VIOGPU_WDDM_UINT64 VaStart;
   VIOGPU_WDDM_UINT64 VaSize;
   VIOGPU_WDDM_UINT64 ResetGeneration;
   VIOGPU_WDDM_UINT32 ContextId;
   VIOGPU_WDDM_UINT32 SubmitQueueId;
} VIOGPU_WDDM_CONTEXT_INFO;

/* Context-scoped completion snapshot.  CompletedFence is the UMD submit
 * sequence endpoint, not the opaque VidSch fence id. */
typedef struct VIOGPU_WDDM_FENCE_INFO {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT32 Opcode;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
   VIOGPU_WDDM_UINT64 CompletedFence;
   VIOGPU_WDDM_UINT64 ResetGeneration;
   VIOGPU_WDDM_UINT32 ContextId;
   VIOGPU_WDDM_UINT32 Reserved;
} VIOGPU_WDDM_FENCE_INFO;

typedef struct VIOGPU_WDDM_RENDER_COMMAND {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT32 Opcode;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT64 ExpectedResetGeneration;
   VIOGPU_WDDM_UINT32 AllocationReferencesOffset;
   VIOGPU_WDDM_UINT32 AllocationReferenceCount;
   VIOGPU_WDDM_UINT32 CommandStreamOffset;
   VIOGPU_WDDM_UINT32 CommandStreamSize;
   VIOGPU_WDDM_UINT32 Reserved[4];
} VIOGPU_WDDM_RENDER_COMMAND;

/* Scheduled BGRA allocation copy. The runtime allocation list supplies source
 * at index 0 and destination at index 1; no user pointers or GPU addresses are
 * accepted. VidMm pages both allocations in before VidSch executes the copy. */
typedef struct VIOGPU_WDDM_ALLOCATION_COPY {
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT32 Opcode;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT32 Width;
   VIOGPU_WDDM_UINT32 Height;
   VIOGPU_WDDM_UINT32 Reserved[8];
} VIOGPU_WDDM_ALLOCATION_COPY;

/* Frame publication. The user-mode driver renders on the host, so the guest
 * pages behind its back buffer stay zero and nothing the display path copies
 * from them can be visible. This carries the finished frame itself: the
 * geometry here, the pixels immediately after this structure. It names no
 * resource -- the receiver publishes to the surface it already scans out. */
typedef struct VIOGPU_WDDM_PRESENT_BLIT
{
   VIOGPU_WDDM_ABI_HEADER Header;
   VIOGPU_WDDM_UINT32 Opcode;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT32 Width;
   VIOGPU_WDDM_UINT32 Height;
   VIOGPU_WDDM_UINT32 SourcePitch;
   VIOGPU_WDDM_UINT32 Format;
   VIOGPU_WDDM_UINT32 PayloadSize;
   VIOGPU_WDDM_UINT32 ReadbackUsec;
} VIOGPU_WDDM_PRESENT_BLIT;

typedef struct VIOGPU_WDDM_ALLOCATION_REFERENCE {
   VIOGPU_WDDM_UINT32 AllocationIndex;
   VIOGPU_WDDM_UINT32 Flags;
   VIOGPU_WDDM_UINT64 AllocationOffset;
   VIOGPU_WDDM_UINT64 Length;
   VIOGPU_WDDM_UINT32 PatchOffset;
   VIOGPU_WDDM_UINT32 Reserved;
} VIOGPU_WDDM_ALLOCATION_REFERENCE;

#pragma pack(pop)

#ifdef __cplusplus
static_assert(sizeof(VIOGPU_WDDM_UINT32) == 4, "WDDM ABI uint32 width changed");
static_assert(sizeof(VIOGPU_WDDM_UINT64) == 8, "WDDM ABI uint64 width changed");
static_assert(sizeof(VIOGPU_WDDM_ABI_HEADER) == 16, "WDDM ABI header layout changed");
static_assert(sizeof(VIOGPU_WDDM_ADAPTER_INFO) == 128, "WDDM adapter ABI layout changed");
static_assert(sizeof(VIOGPU_WDDM_ALLOCATION_INFO) == 80, "WDDM allocation ABI layout changed");
static_assert(sizeof(VIOGPU_WDDM_CONTEXT_CREATE) == 32, "WDDM context-create ABI layout changed");
static_assert(sizeof(VIOGPU_WDDM_CONTEXT_INFO) == 64, "WDDM context-info ABI layout changed");
static_assert(sizeof(VIOGPU_WDDM_FENCE_INFO) == 56, "WDDM fence-info ABI layout changed");
static_assert(sizeof(VIOGPU_WDDM_RENDER_COMMAND) == 64, "WDDM render ABI layout changed");
static_assert(sizeof(VIOGPU_WDDM_ALLOCATION_REFERENCE) == 32, "WDDM reference ABI layout changed");
static_assert(offsetof(VIOGPU_WDDM_CONTEXT_INFO, VaStart) == 32, "WDDM context VA offset changed");
static_assert(offsetof(VIOGPU_WDDM_CONTEXT_INFO, VaSize) == 40, "WDDM context VA size offset changed");
static_assert(offsetof(VIOGPU_WDDM_CONTEXT_INFO, ResetGeneration) == 48,
              "WDDM context generation offset changed");
static_assert(offsetof(VIOGPU_WDDM_CONTEXT_INFO, ContextId) == 56,
              "WDDM context id offset changed");
static_assert(offsetof(VIOGPU_WDDM_CONTEXT_INFO, SubmitQueueId) == 60, "WDDM submit queue id offset changed");
static_assert(offsetof(VIOGPU_WDDM_FENCE_INFO, CompletedFence) == 32,
              "WDDM completed fence offset changed");
static_assert(offsetof(VIOGPU_WDDM_FENCE_INFO, ResetGeneration) == 40,
              "WDDM fence generation offset changed");
static_assert(offsetof(VIOGPU_WDDM_FENCE_INFO, ContextId) == 48,
              "WDDM fence context offset changed");
#endif

#endif /* TU_WDDM_ABI_H */
