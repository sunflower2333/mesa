/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef TU_WDDM_ACCESS_H
#define TU_WDDM_ACCESS_H

#include <stdint.h>
#include "util/u_dynarray.h"

struct tu_bo;
struct tu_descriptor_set;

/* CPU provenance accompanies image descriptors; descriptor bytes alone do
 * not establish the owner of an arbitrary GPU address. */
struct tu_wddm_descriptor_image {
   uint32_t offset;
   struct tu_bo *bo;
};

struct tu_wddm_image_use {
   struct tu_bo *bo;
   uint32_t access;
};

/* A constant array index has count 1. A dynamic index covers the binding,
 * including variable-count descriptors that are actually allocated. */
struct tu_wddm_shader_image_use {
   uint32_t set;
   uint32_t offset;
   uint32_t count;
   uint32_t stride;
   uint32_t access;
};

struct tu_wddm_descriptor_use {
   struct tu_descriptor_set *set;
   struct tu_wddm_shader_image_use range;
};

static inline bool
tu_wddm_set_descriptor_image(struct util_dynarray *images, uint32_t offset,
                             struct tu_bo *bo)
{
   util_dynarray_foreach(images, struct tu_wddm_descriptor_image, image) {
      if (image->offset == offset) {
         image->bo = bo;
         return true;
      }
   }
   if (!bo)
      return true;
   struct tu_wddm_descriptor_image *image = (struct tu_wddm_descriptor_image *) util_dynarray_grow(
      images, struct tu_wddm_descriptor_image, 1);
   if (!image)
      return false;
   *image = { offset, bo };
   return true;
}

static inline bool
tu_wddm_record_image_use(struct util_dynarray *uses, struct tu_bo *bo,
                          uint32_t access)
{
   if (!bo || !access)
      return true;
   util_dynarray_foreach(uses, struct tu_wddm_image_use, use) {
      if (use->bo == bo) {
         use->access |= access;
         return true;
      }
   }
   struct tu_wddm_image_use *use =
      (struct tu_wddm_image_use *) util_dynarray_grow(uses, struct tu_wddm_image_use, 1);
   if (!use)
      return false;
   *use = { bo, access };
   return true;
}

static inline bool
tu_wddm_image_in_range(uint32_t offset,
                        const struct tu_wddm_shader_image_use *range)
{
   return range->stride && offset >= range->offset &&
          (offset - range->offset) % range->stride == 0 &&
          (offset - range->offset) / range->stride < range->count;
}

#endif
