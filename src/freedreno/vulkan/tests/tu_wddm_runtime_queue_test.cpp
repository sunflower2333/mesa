/* SPDX-License-Identifier: MIT */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <thread>
#include "mesa_wddm_runtime.h"

using VkResult = int;
constexpr int VK_SUCCESS = 0, VK_ERROR_DEVICE_LOST = -4, VK_ERROR_INITIALIZATION_FAILED = -3;
struct VkBaseInStructure { int32_t sType; const VkBaseInStructure *pNext; };
struct vk_device { bool lost = false; };
struct list_head { bool empty = true; };
static bool list_is_empty(const list_head *list) { return list->empty; }
using mtx_t = std::mutex;
using cnd_t = std::condition_variable;
constexpr int thrd_error = 1;
static void mtx_lock(mtx_t *m) { m->lock(); }
static void mtx_unlock(mtx_t *m) { m->unlock(); }
static int cnd_wait(cnd_t *c, mtx_t *m) {
   std::unique_lock<std::mutex> lock(*m, std::adopt_lock);
   c->wait(lock); lock.release(); return 0;
}
static int cnd_timedwait(cnd_t *c, mtx_t *m, const timespec *time) {
   const auto deadline = std::chrono::system_clock::from_time_t(time->tv_sec) + std::chrono::nanoseconds(time->tv_nsec);
   std::unique_lock<std::mutex> lock(*m, std::adopt_lock);
   c->wait_until(lock, deadline); lock.release(); return 0;
}
struct vk_queue {
   struct { vk_device *device; } base{};
   struct { mtx_t mutex; cnd_t pop; list_head submits; } submit;
   void (*driver_destroy_submit_data)(vk_queue *, void *) = nullptr;
   bool driver_submit_sync = true;
};
struct vk_queue_submit { void *driver_data = nullptr; };
struct tu_device {
   vk_device vk;
   struct { void *runtime_owner; mwd_callbacks callbacks; } wddm_device{};
};
struct tu_queue { vk_queue vk; tu_device *device; };
// vk is the first member in this production fixture, as checked below.
#define list_entry(ptr, type, member) reinterpret_cast<type *>(ptr)
static bool vk_device_is_lost(vk_device *device) { return device->lost; }
static int vk_queue_set_lost(vk_queue *queue, const char *) { queue->base.device->lost = true; return VK_ERROR_DEVICE_LOST; }
static void vk_device_set_lost(vk_device *device, const char *) { device->lost = true; }
// PRODUCTION_FUNCTIONS

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
struct owner {
   void *token = this;
   unsigned refs = 0, releases = 0;
   bool fail = false;
   mwd_submit_info *mutate = nullptr;
};
static int32_t MWD_CALL retain(void *opaque, void *token) {
   auto *o = static_cast<owner *>(opaque);
   if (o->fail || token != o->token) return -1;
   ++o->refs;
   if (o->mutate) o->mutate->queue = nullptr;
   return 0;
}
static int32_t MWD_CALL release(void *opaque, void *token) {
   auto *o = static_cast<owner *>(opaque);
   CHECK(token == o->token && o->refs);
   if (!o->refs) return -1;
   --o->refs; ++o->releases; return 0;
}
int main() {
   owner o;
   tu_device device{};
   device.wddm_device.runtime_owner = &o;
   device.wddm_device.callbacks.queue_retain = retain;
   device.wddm_device.callbacks.queue_release = release;
   tu_queue queue{};
   queue.device = &device;
   queue.vk.base.device = &device.vk;
   queue.vk.driver_destroy_submit_data = tu_wddm_runtime_submit_data_destroy;
   vk_queue_submit submit;
   mwd_submit_info info{MWD_STYPE_SUBMIT, nullptr, &o, o.token};
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, nullptr, &submit.driver_data) == VK_SUCCESS);
   CHECK(!submit.driver_data && !o.refs);
   info.owner = &device;
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, &info, &submit.driver_data) == VK_ERROR_DEVICE_LOST);
   CHECK(!o.refs && !submit.driver_data);
   info.owner = &o; info.pNext = &info;
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, &info, &submit.driver_data) == VK_ERROR_INITIALIZATION_FAILED);
   VkBaseInStructure cycle{1, nullptr}; cycle.pNext = &cycle;
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, &cycle, &submit.driver_data) == VK_ERROR_INITIALIZATION_FAILED);
   info.pNext = nullptr; o.fail = true;
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, &info, &submit.driver_data) == VK_ERROR_DEVICE_LOST);
   CHECK(!o.refs && !submit.driver_data);
   o.fail = false; o.mutate = &info;
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, &info, &submit.driver_data) == VK_SUCCESS);
   CHECK(o.refs == 1 && submit.driver_data == o.token && !info.queue);
   vk_queue_submit_finish_driver_data(&queue.vk, &submit);
   vk_queue_submit_finish_driver_data(&queue.vk, &submit);
   CHECK(!o.refs && o.releases == 1 && !submit.driver_data);
   o.mutate = nullptr; info.queue = o.token;
   CHECK(tu_wddm_runtime_submit_data_create(&queue.vk, &info, &submit.driver_data) == VK_SUCCESS);
   queue.vk.submit.submits.empty = false;
   std::atomic<bool> enqueued{false};
   std::thread worker([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      // Worker finishes actual kernel enqueue and releases metadata first.
      vk_queue_submit_finish_driver_data(&queue.vk, &submit);
      std::lock_guard<std::mutex> lock(queue.vk.submit.mutex);
      enqueued = true; queue.vk.submit.submits.empty = true;
      queue.vk.submit.pop.notify_one();
   });
   CHECK(vk_queue_drain(&queue.vk) == VK_SUCCESS);
   CHECK(enqueued.load());
   worker.join();
   CHECK(!o.refs && o.releases == 2);
   queue.vk.submit.submits.empty = false; device.vk.lost = true;
   CHECK(vk_queue_drain(&queue.vk) == VK_ERROR_DEVICE_LOST);
   device.vk.lost = false;
   std::thread failed_worker([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
      std::lock_guard<std::mutex> lock(queue.vk.submit.mutex);
      device.vk.lost = true; // No pop signal: exercise the real failure exit.
   });
   CHECK(vk_queue_drain(&queue.vk) == VK_ERROR_DEVICE_LOST);
   failed_worker.join();
   std::printf("runtime queue ownership/drain: %s\n", failures ? "FAIL" : "PASS");
   return failures != 0;
}
