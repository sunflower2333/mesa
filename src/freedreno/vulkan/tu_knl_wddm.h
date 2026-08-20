/*
 * Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef TU_KNL_WDDM_H
#define TU_KNL_WDDM_H

#if !defined(_WIN32)
#error "tu_knl_wddm.h is a Windows-only interface"
#endif

#include <stdbool.h>
#include <stdint.h>

#include <dxgi1_2.h>

#include "tu_wddm_abi.h"
#include "tu_wddm_dispatch.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tu_wddm_runtime {
   struct tu_wddm_dispatch dispatch;
   HMODULE dxgi;
   IDXGIFactory1 *factory;
};

struct tu_wddm_adapter_info {
   LUID luid;
   uint32_t vendor_id;
   uint32_t device_id;
   uint32_t subsystem_id;
   uint32_t revision;
   uint64_t dedicated_video_memory;
   uint64_t dedicated_system_memory;
   uint64_t shared_system_memory;
   char description[128];
   VIOGPU_WDDM_ADAPTER_INFO private_info;
};

struct tu_wddm_adapter {
   struct tu_wddm_runtime *runtime;
   LUID luid;
   D3DKMT_HANDLE handle;
   VIOGPU_WDDM_ADAPTER_INFO private_info;
};

struct tu_wddm_device {
   struct tu_wddm_adapter adapter;
   D3DKMT_HANDLE handle;
   void *command_buffer;
   uint32_t command_buffer_size;
   D3DDDI_ALLOCATIONLIST *allocation_list;
   uint32_t allocation_list_size;
   D3DDDI_PATCHLOCATIONLIST *patch_location_list;
   uint32_t patch_location_list_size;
};

struct tu_wddm_context {
   struct tu_wddm_device *device;
   D3DKMT_HANDLE handle;
   void *command_buffer;
   uint32_t command_buffer_size;
   D3DDDI_ALLOCATIONLIST *allocation_list;
   uint32_t allocation_list_size;
   D3DDDI_PATCHLOCATIONLIST *patch_location_list;
   uint32_t patch_location_list_size;
   VIOGPU_WDDM_CONTEXT_INFO info;
   uint32_t last_submitted_fence;
};

struct tu_instance;

/* These limits mirror the compile-only KMD contract.  They are deliberately
 * smaller than the WDDM wire fields so arithmetic stays bounded before a
 * D3DKMT call is made. */
enum {
   TU_WDDM_MAX_RENDER_ALLOCATIONS = 64,
   TU_WDDM_MAX_RENDER_COMMAND_SIZE = 64 * 1024,
};

struct tu_wddm_allocation_desc {
   uint64_t size;
   uint64_t alignment;
   uint64_t requested_iova;
   uint32_t flags;
   uint32_t format;
   uint32_t width;
   uint32_t height;
   uint32_t pitch;
   uint32_t refresh_rate_numerator;
   uint32_t refresh_rate_denominator;
};

struct tu_wddm_allocation {
   struct tu_wddm_context *context;
   D3DKMT_HANDLE handle;
   VIOGPU_WDDM_ALLOCATION_INFO private_info;
   /* VidMm/KMD backs every native allocation to a page boundary.  Keep the
    * reservation size separately from the logical Vulkan allocation size so
    * adjacent requested IOVAs cannot overlap the rounded backing extent. */
   uint64_t vma_size;
   void *map;
   bool locked;
};

struct tu_wddm_render_reference {
   struct tu_wddm_allocation *allocation;
   uint32_t flags;
   uint64_t allocation_offset;
   uint64_t length;
   uint32_t patch_offset;
};

typedef bool (*tu_wddm_adapter_callback)(const struct tu_wddm_adapter_info *info,
                                         void *data);

bool tu_wddm_runtime_init(struct tu_wddm_runtime *runtime);
void tu_wddm_runtime_finish(struct tu_wddm_runtime *runtime);

/* Close handles retained by a failed physical-adapter probe.  The owner is
 * stored on tu_instance so a transient KMT close failure can be retried
 * before the runtime dispatch table is unloaded. */
bool tu_wddm_probe_cleanup(struct tu_instance *instance);

/* Enumerates only adapters that claim the exact DroidVM private endpoint.
 * The callback must return true to continue; false aborts enumeration and is
 * reported as failure, even when the current adapter can be closed cleanly. */
bool tu_wddm_runtime_foreach_adapter(struct tu_wddm_runtime *runtime,
                                      tu_wddm_adapter_callback callback,
                                      void *data);

bool tu_wddm_validate_adapter_info(const VIOGPU_WDDM_ADAPTER_INFO *info);
bool tu_wddm_validate_context_info(const VIOGPU_WDDM_CONTEXT_INFO *info,
                                  uint64_t expected_reset_generation);
bool tu_wddm_validate_fence_info(const VIOGPU_WDDM_FENCE_INFO *info,
                                 uint64_t expected_reset_generation,
                                 uint32_t expected_context_id);

bool tu_wddm_adapter_open(struct tu_wddm_runtime *runtime,
                          const struct tu_wddm_adapter_info *identity,
                          struct tu_wddm_adapter *adapter);
bool tu_wddm_adapter_close(struct tu_wddm_adapter *adapter);

bool tu_wddm_device_open(struct tu_wddm_runtime *runtime,
                         const struct tu_wddm_adapter_info *identity,
                         struct tu_wddm_device *device);
bool tu_wddm_device_close(struct tu_wddm_device *device);

bool tu_wddm_context_open(struct tu_wddm_device *device,
                          struct tu_wddm_context *context);
bool tu_wddm_context_get_info(struct tu_wddm_context *context);
bool tu_wddm_context_get_completed_fence(struct tu_wddm_context *context,
                                         uint32_t *completed_fence);
bool tu_wddm_context_wait_fence(struct tu_wddm_context *context,
                                uint32_t fence,
                                uint64_t timeout_ns);
bool tu_wddm_context_wait_submissions(struct tu_wddm_context *context,
                                      uint64_t timeout_ns);
bool tu_wddm_context_close(struct tu_wddm_context *context);

bool tu_wddm_allocation_create(struct tu_wddm_context *context,
                               const struct tu_wddm_allocation_desc *desc,
                               struct tu_wddm_allocation *allocation);
bool tu_wddm_allocation_destroy(struct tu_wddm_allocation *allocation);
bool tu_wddm_allocation_lock(struct tu_wddm_allocation *allocation,
                             bool read_only,
                             void **map);
bool tu_wddm_allocation_unlock(struct tu_wddm_allocation *allocation);

/* Build one bounded Native Context packet in the KMD-provided DMA buffers.
 * This only prepares the WDDM Render call; fence submission/retirement stays
 * with VidSch and the KMD. */
bool tu_wddm_context_render(struct tu_wddm_context *context,
                            const void *command_stream,
                            uint32_t command_stream_size,
                            const struct tu_wddm_render_reference *references,
                            uint32_t reference_count);

#ifdef __cplusplus
}
#endif

#endif /* TU_KNL_WDDM_H */
