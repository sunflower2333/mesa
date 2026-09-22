/* Copyright 2026 DroidVM contributors
 * SPDX-License-Identifier: MIT
 */
#include "vulkan/vulkan_core.h"
#include "tu_wddm_abi.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>
#include <mutex>
#include <thread>
using mtx_t = std::mutex;
#define TU_HAS_WDDM 1
#define TU_SUBMIT_BO_ACCESS_READ 1u
#define TU_SUBMIT_BO_ACCESS_WRITE 2u
#define MAX_SETS 8
#define MESA_SHADER_STAGES 6
#define MESA_SHADER_COMPUTE 5
#define VK_FROM_HANDLE(type, name, handle) type *name = (type *)(uintptr_t)(handle)
struct util_dynarray { void *data = nullptr; size_t size = 0; };
static bool fail_grow;
static void *grow(util_dynarray *a, size_t bytes) {
   if (fail_grow) return nullptr;
   void *data = realloc(a->data, a->size + bytes);
   if (!data) return nullptr;
   void *tail = (uint8_t *)data + a->size;
   a->data = data; a->size += bytes;
   return tail;
}
#define util_dynarray_grow(a, type, n) grow(a, sizeof(type) * (n))
#define util_dynarray_num_elements(a, type) ((a)->size / sizeof(type))
#define util_dynarray_foreach(a, type, name) \
   for (type *name = (type *)(a)->data; (uint8_t *)name < (uint8_t *)(a)->data + (a)->size; name++)
// ACCESS_HEADER
struct tu_wddm_context { VIOGPU_WDDM_CONTEXT_INFO info; };
struct tu_wddm_allocation {
   bool imported;
   uint64_t share_key, imported_size;
   VIOGPU_WDDM_ALLOCATION_INFO private_info;
};
struct tu_bo { tu_wddm_allocation *wddm_allocation; uint64_t iova; bool gpu_read_only; };
struct tu_device_memory { tu_bo *bo; };
struct tu_image { struct { uint32_t create_flags; } vk; tu_device_memory *mem; };
struct tu_image_view { tu_image *image; };
struct tu_descriptor_set_binding_layout {
   VkDescriptorType type; uint32_t offset, array_size, size;
};
struct tu_descriptor_set_layout { uint32_t flags; };
struct tu_descriptor_set {
   tu_descriptor_set_layout *layout;
   uint32_t *mapped_ptr;
   util_dynarray wddm_images;
   bool wddm_images_failed = false;
};
struct tu_shader { util_dynarray wddm_image_uses; bool wddm_image_uses_failed = false; };
struct tu_descriptor_state { tu_descriptor_set *sets[MAX_SETS]{}; };
struct tu_subpass_attachment { uint32_t attachment; };
struct tu_subpass {
   uint32_t color_count, input_count, resolve_count, unresolve_count;
   tu_subpass_attachment *color_attachments, *input_attachments;
   tu_subpass_attachment *resolve_attachments, *unresolve_attachments;
   tu_subpass_attachment depth_stencil_attachment;
   uint32_t fsr_attachment;
};
struct tu_render_pass {
   uint32_t subpass_count; tu_subpass *subpasses;
   tu_subpass_attachment fragment_density_map;
};
struct tu_device {
   tu_wddm_context wddm_context{}; uint32_t wddm_bo_count; tu_bo **wddm_bos; mtx_t bo_mutex;
};
struct tu_cmd_buffer {
   struct { VkResult record_result = VK_SUCCESS; } vk;
   tu_device *device;
   util_dynarray wddm_image_uses, wddm_descriptor_uses;
   struct { tu_shader *shaders[MESA_SHADER_STAGES]{};
            const tu_image_view **attachments; tu_render_pass *pass; } state;
   tu_descriptor_state descriptors[2];
};
static auto tu_get_descriptors_state(tu_cmd_buffer *cmd, VkPipelineBindPoint point) {
   return &cmd->descriptors[point == VK_PIPELINE_BIND_POINT_COMPUTE];
}
template<class T> void vk_command_buffer_set_error(T *vk, VkResult result) { vk->record_result = result; }
static bool vk_descriptor_type_is_dynamic(VkDescriptorType type) {
   return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}
static std::vector<tu_wddm_image_use> submitted;
static void tu_submit_add_bos(tu_device *, void *, tu_bo **bos, unsigned count, uint32_t access) {
   assert(count == 1); submitted.push_back({*bos, access});
}
enum { nir_deref_type_var, nir_deref_type_array };
struct nir_src { bool constant; uint32_t value; };
struct nir_deref_instr { int deref_type; struct { nir_src index; } arr; };
static bool nir_src_is_const(nir_src src) { return src.constant; }
static uint32_t nir_src_as_uint(nir_src src) { return src.value; }
enum { TU_WDDM_MAX_SUBMIT_REFERENCES = 1024 };
struct tu_wddm_submit_reference { tu_bo *bo; uint32_t access; };
struct tu_wddm_submit {
   util_dynarray references, imports;
   uint16_t reference_index[2048]{};
   bool failed = false;
};
static bool tu_wddm_bo_valid_for_device(tu_device *, tu_bo *bo) { return bo && bo->wddm_allocation; }
static void tu_wddm_diag(const char *, ...) {}
static int tu_wddm_submit_reference_lookup(tu_wddm_submit *s, tu_bo *bo, uint32_t *slot) {
   unsigned index = 0;
   util_dynarray_foreach(&s->references, tu_wddm_submit_reference, ref) {
      if (ref->bo == bo) return index;
      ++index;
   }
   *slot = index; return -1;
}
static void mtx_lock(mtx_t *mutex) { mutex->lock(); }
static void mtx_unlock(mtx_t *mutex) { mutex->unlock(); }
// PRODUCTION_FUNCTIONS
static unsigned checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)
static uint32_t access_to(tu_bo *bo) {
   uint32_t access = 0;
   for (auto use : submitted) if (use.bo == bo) access |= use.access;
   return access;
}
static void clear(util_dynarray &array) { free(array.data); array = {}; }
static void reset(tu_cmd_buffer &cmd) {
   clear(cmd.wddm_image_uses); clear(cmd.wddm_descriptor_uses);
   cmd.vk.record_result = VK_SUCCESS; submitted.clear();
}
int main() {
   tu_device device{}; device.wddm_context.info.ResetGeneration = 7;
   tu_wddm_allocation allocations[4]{};
   tu_bo bos[4]{}; tu_device_memory memories[4]{}; tu_image images[4]{};
   tu_image_view views[4]{};
   for (unsigned i = 0; i < 4; i++) {
      allocations[i].imported = i != 3;
      allocations[i].share_key = 100 + i;
      allocations[i].imported_size = 8192;
      allocations[i].private_info.ExpectedResetGeneration = 7;
      bos[i] = {&allocations[i], 0x10000ull + i * 0x10000, false};
      memories[i] = {&bos[i]}; images[i] = {{0}, &memories[i]}; views[i] = {&images[i]};
   }
   // A is back, B is front, C is the following back, D is ordinary owned memory.
   tu_cmd_buffer cmd{}; cmd.device = &device;
   tu_descriptor_set_layout normal{0}, push{VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR};
   uint32_t bytes[64]{};
   tu_descriptor_set set{&normal, bytes};
   VkDescriptorImageInfo a{VK_NULL_HANDLE, (VkImageView)(uintptr_t)&views[0], VK_IMAGE_LAYOUT_GENERAL};
   VkDescriptorImageInfo b{VK_NULL_HANDLE, (VkImageView)(uintptr_t)&views[1], VK_IMAGE_LAYOUT_GENERAL};
   VkDescriptorImageInfo c{VK_NULL_HANDLE, (VkImageView)(uintptr_t)&views[2], VK_IMAGE_LAYOUT_GENERAL};
   tu_descriptor_image_write(&device, &set, bytes, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &a);
   tu_descriptor_image_write(&device, &set, bytes + 16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &b);
   tu_shader shader{};
   nir_deref_instr index0{nir_deref_type_array, {{true, 0}}};
   tu_descriptor_set_binding_layout binding{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0, 2, 64};
   tu_shader_record_image_use(&shader, &index0, 0, &binding, TU_SUBMIT_BO_ACCESS_WRITE);
   cmd.state.shaders[4] = &shader; cmd.descriptors[0].sets[0] = &set;
   tu_cmd_use_descriptors(&cmd, false);
   // Legal update after binding/recording redirects the pending draw to C.
   tu_descriptor_image_write(&device, &set, bytes, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &c);
   CHECK(tu_cmd_submit_image_uses(&cmd, nullptr) == VK_SUCCESS);
   CHECK(!access_to(&bos[0]) && !access_to(&bos[1]));
   CHECK(access_to(&bos[2]) == TU_SUBMIT_BO_ACCESS_WRITE);
   reset(cmd);
   // Push snapshots preserve both draws even though backing metadata changes.
   set.layout = &push;
   tu_descriptor_image_write(&device, &set, bytes, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &a);
   tu_cmd_use_descriptors(&cmd, false);
   tu_descriptor_image_write(&device, &set, bytes, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &c);
   tu_cmd_use_descriptors(&cmd, false);
   CHECK(tu_cmd_submit_image_uses(&cmd, nullptr) == VK_SUCCESS);
   CHECK(access_to(&bos[0]) == 2 && access_to(&bos[2]) == 2 && !access_to(&bos[1]));
   CHECK(cmd.wddm_descriptor_uses.size == 0);
   reset(cmd);
   // Read-only shader sampling of a storage descriptor remains READ.
   clear(shader.wddm_image_uses);
   tu_shader_record_image_use(&shader, &index0, 0, &binding, 1);
   cmd.state.shaders[4] = nullptr; cmd.state.shaders[MESA_SHADER_COMPUTE] = &shader;
   cmd.descriptors[1].sets[0] = &set;
   tu_cmd_use_descriptors(&cmd, true);
   CHECK(tu_cmd_submit_image_uses(&cmd, nullptr) == VK_SUCCESS && access_to(&bos[2]) == 1);
   reset(cmd);
   // Dynamic array selection covers all possible elements; constant OOB does not.
   clear(shader.wddm_image_uses);
   nir_deref_instr dynamic{nir_deref_type_array, {{false, 0}}};
   tu_shader_record_image_use(&shader, &dynamic, 0, &binding, 1);
   tu_cmd_use_descriptors(&cmd, true);
   CHECK(tu_cmd_submit_image_uses(&cmd, nullptr) == VK_SUCCESS);
   CHECK(access_to(&bos[1]) == 1 && access_to(&bos[2]) == 1);
   nir_deref_instr oob{nir_deref_type_array, {{true, 4}}};
   auto size = shader.wddm_image_uses.size;
   tu_shader_record_image_use(&shader, &oob, 0, &binding, 2);
   CHECK(shader.wddm_image_uses.size == size);
   // Descriptor copies retain provenance and mutable non-image replacement clears it.
   uint32_t copy_bytes[64]{}; tu_descriptor_set copied{&normal, copy_bytes};
   tu_descriptor_image_copy(&device, &copied, copy_bytes, &set, bytes, VK_DESCRIPTOR_TYPE_MUTABLE_EXT);
   CHECK(((tu_wddm_descriptor_image *)copied.wddm_images.data)[0].bo == &bos[2]);
   tu_descriptor_image_write(&device, &copied, copy_bytes, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr);
   CHECK(!((tu_wddm_descriptor_image *)copied.wddm_images.data)[0].bo);
   reset(cmd);
   // Attachments: back target WRITE, front input READ, resolve destination WRITE.
   const tu_image_view *attachments[]{&views[0], &views[1], &views[2]};
   tu_subpass_attachment color{0}, input{1}, resolve{2};
   tu_subpass subpass{1, 1, 1, 0, &color, &input, &resolve, nullptr,
                      {VK_ATTACHMENT_UNUSED}, VK_ATTACHMENT_UNUSED};
   tu_render_pass pass{1, &subpass, {VK_ATTACHMENT_UNUSED}};
   cmd.state.attachments = attachments; cmd.state.pass = &pass;
   tu_cmd_use_renderpass(&cmd);
   tu_cmd_use_image(&cmd, &images[3], 2); // Owned BOs do not enter import tracking.
   CHECK(tu_cmd_submit_image_uses(&cmd, nullptr) == VK_SUCCESS);
   CHECK(access_to(&bos[0]) == 3 && access_to(&bos[1]) == 1 && access_to(&bos[2]) == 2);
   CHECK(!access_to(&bos[3]));
   reset(cmd);
   // Allocation failure must poison recording, never silently drop an access.
   fail_grow = true;
   tu_cmd_use_image(&cmd, &images[0], 2);
   CHECK(cmd.vk.record_result == VK_ERROR_OUT_OF_HOST_MEMORY);
   tu_descriptor_image_write(&device, &copied, copy_bytes + 16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &a);
   CHECK(copied.wddm_images_failed);
   clear(shader.wddm_image_uses);
   tu_shader_record_image_use(&shader, &index0, 0, &binding, 2);
   CHECK(shader.wddm_image_uses_failed);
   fail_grow = false;
   // Legal concurrent updates of distinct update-after-bind elements cannot
   // race vector growth or lose either element's CPU provenance.
   clear(copied.wddm_images); copied.wddm_images_failed = false;
   std::thread first([&] {
      for (unsigned i = 0; i < 2000; i++)
         tu_descriptor_image_write(&device, &copied, copy_bytes, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &a);
   });
   std::thread second([&] {
      for (unsigned i = 0; i < 2000; i++)
         tu_descriptor_image_write(&device, &copied, copy_bytes + 16, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &b);
   });
   first.join(); second.join();
   CHECK(!copied.wddm_images_failed && util_dynarray_num_elements(&copied.wddm_images, tu_wddm_descriptor_image) == 2);
   // The owned residency sweep must not manufacture front/back writer claims.
   tu_wddm_submit submit{};
   tu_bo *live[]{&bos[0], &bos[1], &bos[2], &bos[3]};
   device.wddm_bos = live; device.wddm_bo_count = 4;
   CHECK(tu_wddm_submit_add_live_bos(&device, &submit));
   CHECK(submit.imports.size == 0 && util_dynarray_num_elements(&submit.references, tu_wddm_submit_reference) == 1);
   CHECK(tu_wddm_submit_add_reference(&device, &submit, &bos[0], 2));
   CHECK(tu_wddm_submit_add_reference(&device, &submit, &bos[0], 1));
   auto *wire = (VIOGPU_WDDM_IMPORTED_REFERENCE *)submit.imports.data;
   CHECK(util_dynarray_num_elements(&submit.imports, VIOGPU_WDDM_IMPORTED_REFERENCE) == 1);
   CHECK(wire->Access == 3 && wire->ShareKey == 100 && wire->Size == 8192 && wire->Iova == bos[0].iova);
   CHECK(tu_wddm_import_references_valid(&device.wddm_context, wire, 1));
   auto good = *wire;
   for (unsigned mutation = 0; mutation < 8; mutation++) {
      auto bad = good;
      switch (mutation) {
      case 0: bad.ShareKey = 0; break;
      case 1: bad.Iova = 0; break;
      case 2: bad.Size = 0; break;
      case 3: bad.ResetGeneration++; break;
      case 4: bad.Access = 0; break;
      case 5: bad.Access = 4; break;
      case 6: bad.Reserved = 1; break;
      case 7: bad.Iova = UINT64_MAX; break;
      }
      CHECK(!tu_wddm_import_references_valid(&device.wddm_context, &bad, 1));
   }
   VIOGPU_WDDM_IMPORTED_REFERENCE duplicate[]{good, good};
   CHECK(!tu_wddm_import_references_valid(&device.wddm_context, duplicate, 2));
   duplicate[1].ShareKey++;
   CHECK(!tu_wddm_import_references_valid(&device.wddm_context, duplicate, 2));
   CHECK(!tu_wddm_import_references_valid(&device.wddm_context, nullptr, 1));
   CHECK(!tu_wddm_import_references_valid(&device.wddm_context, wire, 257));
   CHECK(tu_wddm_import_references_valid(&device.wddm_context, nullptr, 0));
   clear(submit.imports); clear(submit.references);
   allocations[0].private_info.ExpectedResetGeneration++;
   CHECK(!tu_wddm_submit_add_reference(&device, &submit, &bos[0], 2) && submit.failed);
   reset(cmd); clear(shader.wddm_image_uses); clear(set.wddm_images); clear(copied.wddm_images);
   printf("PASS production image access: %u checks (late updates, push snapshots, exact shader ranges, copies, renderpass, OOM, imports, no live writer sweep)\n", checks);
}
