/* SPDX-License-Identifier: MIT */
#ifndef MESA_WDDM_RUNTIME_INTEROP_H
#define MESA_WDDM_RUNTIME_INTEROP_H
#include <stddef.h>
#include <stdint.h>

/* Private, matched UMD/ICD in-process protocol. Not a public Vulkan extension
 * or KMD ABI. No runtime opaque HANDLE is represented as a KMT handle here.
 * These private pNext tags are intentionally outside registered Vulkan ranges. */
#define MWD_RUNTIME_ABI_VERSION 1u
#define MWD_RUNTIME_MAGIC 0x3152574du
#define MWD_STYPE_SUPPORT 0x7fff2330
#define MWD_STYPE_DEVICE  0x7fff2331
#define MWD_STYPE_IMPORT  0x7fff2332
#ifdef _WIN32
#define MWD_CALL __stdcall
#else
#define MWD_CALL
#endif

struct mwd_context_info {
   uint8_t luid[8];
   uint64_t generation, va_start, va_size;
   uint32_t context_id, queue_id;
};
struct mwd_allocation {
   void *token;
   uint64_t address, size, generation;
   uint32_t handle, flags;
};
struct mwd_reference {
   void *token;
   uint64_t offset, length;
   uint32_t flags, patch_offset;
};
struct mwd_callbacks {
   uint32_t magic, version, size, reserved;
   int32_t (MWD_CALL *context)(void *owner, struct mwd_context_info *info);
   int32_t (MWD_CALL *allocate)(void *owner, uint64_t size, uint64_t alignment,
                               uint64_t requested_address, uint32_t flags,
                               struct mwd_allocation *allocation);
   int32_t (MWD_CALL *retain)(void *owner, void *token, struct mwd_allocation *allocation);
   int32_t (MWD_CALL *release)(void *owner, void *token);
   int32_t (MWD_CALL *map)(void *owner, void *token, void **mapping, uint32_t *handle);
   int32_t (MWD_CALL *unmap)(void *owner, void *token);
   int32_t (MWD_CALL *submit)(void *owner, const void *stream, uint32_t stream_size,
                             const struct mwd_reference *references, uint32_t count);
   int32_t (MWD_CALL *completed)(void *owner, uint32_t *fence);
   int32_t (MWD_CALL *status)(void *owner);
};
/* VkPhysicalDeviceProperties2 query. A zero reply means unsupported. */
struct mwd_support {
   int32_t sType;
   void *pNext;
   uint32_t magic, version, size, flags;
};
/* VkDeviceCreateInfo input. ICD copies callbacks before returning; owner is
 * borrowed until VkDevice destruction, including failure cleanup. */
struct mwd_device_create_info {
   int32_t sType;
   const void *pNext;
   const struct mwd_callbacks *callbacks;
   void *owner;
};
/* VkMemoryAllocateInfo input. A successful import retains the exact existing
 * allocation; failed imports transfer no ownership. */
struct mwd_import_memory_info {
   int32_t sType;
   const void *pNext;
   void *owner;
   void *token;
};

static inline int mwd_callbacks_valid(const struct mwd_callbacks *cb)
{
   return cb && cb->magic == MWD_RUNTIME_MAGIC &&
      cb->version == MWD_RUNTIME_ABI_VERSION && cb->size == sizeof(*cb) &&
      !cb->reserved && cb->context && cb->allocate && cb->retain &&
      cb->release && cb->map && cb->unmap && cb->submit &&
      cb->completed && cb->status;
}
#endif

