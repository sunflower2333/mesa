/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

enum VkResult { VK_SUCCESS, VK_TIMEOUT, VK_ERROR_DEVICE_LOST };
enum vk_sync_wait_flags { VK_SYNC_WAIT_COMPLETE = 0, VK_SYNC_WAIT_PENDING = 1 };
constexpr uint64_t OS_TIMEOUT_INFINITE = UINT64_MAX;
constexpr uint64_t TU_WDDM_SYNC_SIGNALED = UINT64_C(1) << 63;
struct vk_device { bool active = true; };
struct tu_wddm_context {
   vk_device *device;
   uint32_t last_submitted_fence = 0;
   uint32_t completed = 0;
};
struct tu_device {
   vk_device vk;
   tu_wddm_context wddm_context{&vk};
   uint32_t wddm_next_fence = 1;
   uint32_t wddm_pending_submission_upper_bound = 0;
};
struct tu_queue { tu_device *device; int fence = 0; };
struct vk_sync_type {};
static const vk_sync_type tu_wddm_sync_type{}, dummy_type{};
struct vk_sync { const vk_sync_type *type = &tu_wddm_sync_type; };
struct tu_wddm_sync : vk_sync {
   tu_wddm_context *context;
   uint64_t state = 0;
   bool current = true;
   explicit tu_wddm_sync(tu_wddm_context *ctx) : context(ctx) {}
};
struct vk_sync_signal { vk_sync *sync; };
struct util_dynarray { uint32_t count; };
struct tu_wddm_submit { util_dynarray entries; };
#define util_dynarray_num_elements(array, type) ((array)->count)
static bool vk_sync_type_is_dummy(const vk_sync_type *type) { return type == &dummy_type; }
static tu_wddm_sync *tu_wddm_sync_from_vk(vk_sync *sync) { return static_cast<tu_wddm_sync *>(sync); }
static bool tu_wddm_sync_is_current(const tu_wddm_sync *sync) { return sync->current; }
static void tu_wddm_sync_state_set(tu_wddm_sync *sync, uint64_t state) { sync->state = state; }
static uint64_t tu_wddm_sync_state_read(const tu_wddm_sync *sync) { return sync->state; }
static uint64_t p_atomic_cmpxchg(uint64_t *value, uint64_t old, uint64_t desired) {
   uint64_t observed = *value;
   if (observed == old) *value = desired;
   return observed;
}
static bool tu_wddm_sync_belongs_to_device(tu_wddm_sync *sync, vk_device *device) {
   return sync->current && sync->context->device == device;
}
static bool tu_wddm_context_get_completed_fence(tu_wddm_context *context, uint32_t *completed) {
   *completed = context->completed;
   return true;
}
static bool tu_wddm_device_execution_active(vk_device *device) { return device->active; }
static bool tu_wddm_fence_after(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
static VkResult vk_device_set_lost(vk_device *, const char *) { return VK_ERROR_DEVICE_LOST; }
static int64_t os_time_get_nano() { return 1; }
struct tu_wddm_fence_poll_wait {
   void wait(uint64_t) { std::abort(); } // Zero-deadline probes must never block.
};
static bool tu_wddm_submit_add_live_bos(tu_device *, tu_wddm_submit *) { return true; }
static VkResult tu_wddm_wait_submission_slot(tu_device *) { return VK_SUCCESS; }
static unsigned render_calls;
static VkResult tu_wddm_submit_render(tu_queue *queue, tu_wddm_submit *, uint32_t fence) {
   render_calls++;
   queue->device->wddm_context.last_submitted_fence = fence;
   return VK_SUCCESS; // KMT accepts work; GPU completion remains controlled separately.
}

// PRODUCTION_FUNCTIONS

static unsigned checks, failures;
static void check(bool ok, const char *why) {
   checks++;
   if (!ok) { failures++; std::fprintf(stderr, "FAIL: %s\n", why); }
}
static VkResult poll(tu_device &device, tu_wddm_sync &sync) {
   return tu_wddm_sync_wait(&device.vk, &sync, 0, VK_SYNC_WAIT_COMPLETE, 0);
}
int main() {
   tu_wddm_submit empty{{0}}, work{{1}};
   {
      tu_device device;
      tu_queue queue{&device};
      tu_wddm_sync idle(&device.wddm_context);
      vk_sync_signal signal{&idle};
      check(tu_wddm_queue_submit_locked(&queue, &empty, &signal, 1) == VK_SUCCESS,
            "fresh queue accepts empty submit");
      check(poll(device, idle) == VK_SUCCESS, "fresh queue is idle");
      check(render_calls == 0 && device.wddm_next_fence == 1,
            "fresh empty submit does not invent GPU work");
   }
   for (uint32_t token : {1u, 121u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu}) {
      tu_device device;
      device.wddm_next_fence = token;
      device.wddm_context.completed = token - 1;
      tu_queue queue{&device}, second_queue{&device};
      check(tu_wddm_queue_submit_locked(&queue, &work, nullptr, 0) == VK_SUCCESS,
            "work without an explicit fence transfers to KMT");
      const unsigned calls = render_calls;
      tu_wddm_sync idle(&device.wddm_context), chained(&device.wddm_context);
      vk_sync_signal signals[] = {{&idle}, {&chained}};
      check(tu_wddm_queue_submit_locked(&queue, &empty, signals, 2) == VK_SUCCESS,
            "empty fence-only submit accepted after work");
      check(poll(device, idle) == VK_TIMEOUT,
            "QueueWaitIdle-style empty submit cannot complete before earlier work");
      check(poll(device, chained) == VK_TIMEOUT, "all empty-submit signals retain dependency");
      tu_wddm_sync later(&device.wddm_context);
      vk_sync_signal later_signal{&later};
      check(tu_wddm_queue_submit_locked(&second_queue, &empty, &later_signal, 1) == VK_SUCCESS,
            "shared-context queue accepts empty submit");
      check(poll(device, later) == VK_TIMEOUT, "chained empty submit cannot erase pending work");
      check(render_calls == calls && device.wddm_pending_submission_upper_bound == 1,
            "empty submits consume no new token or submission slot");
      device.wddm_context.completed = token;
      check(poll(device, idle) == VK_SUCCESS && poll(device, chained) == VK_SUCCESS &&
            poll(device, later) == VK_SUCCESS, "GPU retirement releases all inherited waits");
      check(device.wddm_next_fence == (token == UINT32_MAX ? 1 : token + 1),
            "empty submit preserves wrap and zero reservation");
      idle.state = 0;
      check(tu_wddm_queue_submit_locked(&queue, &empty, signals, 1) == VK_SUCCESS &&
            poll(device, idle) == VK_SUCCESS, "already retired dependency completes");
      device.vk.active = false;
      check(poll(device, idle) == VK_ERROR_DEVICE_LOST, "reset rejects cached completion");
   }
   std::printf("empty-submit production regression: %u/%u PASS\n", checks - failures, checks);
   return failures ? 1 : 0;
}
