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
};

typedef bool (*tu_wddm_adapter_callback)(const struct tu_wddm_adapter_info *info,
                                         void *data);

bool tu_wddm_runtime_init(struct tu_wddm_runtime *runtime);
void tu_wddm_runtime_finish(struct tu_wddm_runtime *runtime);

/* Enumerates only adapters that claim the exact DroidVM private endpoint. */
bool tu_wddm_runtime_foreach_adapter(struct tu_wddm_runtime *runtime,
                                      tu_wddm_adapter_callback callback,
                                      void *data);

bool tu_wddm_validate_adapter_info(const VIOGPU_WDDM_ADAPTER_INFO *info);
bool tu_wddm_validate_context_info(const VIOGPU_WDDM_CONTEXT_INFO *info,
                                  uint64_t expected_reset_generation);

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
bool tu_wddm_context_close(struct tu_wddm_context *context);

#ifdef __cplusplus
}
#endif

#endif /* TU_KNL_WDDM_H */
