/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include <cstdio>
#include <vector>

enum VkResult { VK_SUCCESS, VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY };
enum { SR_NONE, SR_IN_CHAIN, SR_AFTER_PRE_CHAIN };
constexpr unsigned VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT = 4;
struct tu_knl { uint32_t max_submit_entries = 256; };
struct tu_instance { tu_knl *knl; };
struct vk_device { bool lost = false; };
struct tu_device {
   vk_device vk;
   tu_instance *instance;
   bool trace_context = false;
   void *perfcntrs_pass_cs_entries = nullptr;
};
struct vk_queue {};
struct tu_queue { vk_queue vk; tu_device *device; };
#define list_entry(pointer, type, member) reinterpret_cast<type *>(pointer)
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
struct tu_cmd_buffer {
   struct { uint32_t entry_count = 1; } cs;
   uint32_t usage_flags = 0;
   struct { int suspend_resume = SR_NONE; } state;
   uint32_t id = 0;
};
struct vk_queue_submit {
   uint32_t command_buffer_count = 0;
   tu_cmd_buffer **command_buffers = nullptr;
   uint32_t wait_count = 0;
   void *waits = nullptr;
   uint32_t signal_count = 0;
   void *signals = nullptr;
   bool has_bind = false;
};
static bool vk_queue_submit_has_bind(vk_queue_submit *s) { return s->has_bind; }
static bool u_trace_should_process(bool *value) { return *value; }
static VkResult vk_device_set_lost(vk_device *dev, const char *) {
   dev->lost = true;
   return VK_ERROR_DEVICE_LOST;
}
static unsigned checks, failures;
static void check(bool ok, const char *why) {
   checks++;
   if (!ok) { failures++; std::fprintf(stderr, "FAIL: %s\n", why); }
}
static unsigned calls, fail_call;
static uint32_t transferred, signal_fence;
static std::vector<uint32_t> ids, sizes;
static VkResult queue_submit_single(vk_queue *base, vk_queue_submit *s) {
   tu_device *dev = reinterpret_cast<tu_queue *>(base)->device;
   calls++;
   check(signal_fence == 0, "no earlier batch published application signals");
   check((s->wait_count != 0) == (calls == 1), "waits occur only in the first batch");
   check((s->waits != nullptr) == (calls == 1), "wait pointer follows wait count");
   check((s->signals != nullptr) == (s->signal_count != 0), "signal pointer follows count");
   if (fail_call == calls) return VK_ERROR_OUT_OF_HOST_MEMORY;
   uint64_t entries = 1;
   for (uint32_t i = 0; i < s->command_buffer_count; i++) {
      const tu_cmd_buffer *cmd = s->command_buffers[i];
      entries += (uint64_t)cmd->cs.entry_count +
                 !!(cmd->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) +
                 !!dev->perfcntrs_pass_cs_entries;
      ids.push_back(cmd->id);
   }
   sizes.push_back(static_cast<uint32_t>(entries));
   // Model accepted KMT tokens. GPU retirement is separate from transfer.
   transferred++;
   if (transferred == 0) transferred = 1;
   if (s->signal_count) signal_fence = transferred;
   return VK_SUCCESS;
}

// PRODUCTION_FUNCTIONS

struct fixture {
   tu_knl knl;
   tu_instance instance{&knl};
   tu_device device{{}, &instance};
   tu_queue queue{{}, &device};
   std::vector<tu_cmd_buffer> commands;
   std::vector<tu_cmd_buffer *> pointers;
   vk_queue_submit submit;
   explicit fixture(uint32_t count) : commands(count), pointers(count) {
      calls = fail_call = transferred = signal_fence = 0;
      ids.clear(); sizes.clear();
      for (uint32_t i = 0; i < count; i++) {
         commands[i].id = i;
         pointers[i] = &commands[i];
      }
      submit = {count, pointers.data(), 1, this, 1, this, false};
   }
   VkResult run() { return queue_submit(&queue.vk, &submit); }
   void verify() {
      check(ids.size() == commands.size(), "every command buffer transfers exactly once");
      bool ordered = true;
      for (uint32_t i = 0; i < ids.size(); i++) ordered &= ids[i] == i;
      check(ordered, "command-buffer order is preserved");
      bool bounded = true;
      for (auto size : sizes) bounded &= size <= knl.max_submit_entries;
      check(bounded, "each physical batch includes room for generated entries");
      check(signal_fence == transferred, "application completion uses only the final transferred fence");
   }
};
int main() {
   for (uint32_t count : {0u, 1u, 255u, 256u, 299u, 1000u, 2600u}) {
      fixture f(count);
      check(f.run() == VK_SUCCESS, "empty, boundary and GB7-sized submits succeed");
      f.verify();
      check(count <= 255 || calls > 1, "large submit actually uses multiple packets");
   }
   {
      fixture f(1000);
      f.device.perfcntrs_pass_cs_entries = &f;
      for (auto &cmd : f.commands) {
         cmd.cs.entry_count = cmd.id % 5;
         cmd.usage_flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
      }
      check(f.run() == VK_SUCCESS, "mixed empty/multi-entry CS plus generated streams fit");
      f.verify();
   }
   for (auto initial : {UINT32_C(0x7ffffffe), UINT32_C(0xfffffffe)}) {
      fixture f(1000);
      transferred = initial;
      check(f.run() == VK_SUCCESS, "batching preserves opaque signed/wrapped tokens");
      f.verify();
      check(signal_fence != 0 && signal_fence != initial, "final token never becomes reserved zero");
   }
   for (unsigned fail_at : {1u, 2u, 4u}) {
      fixture f(1000);
      fail_call = fail_at;
      check(f.run() == (fail_at == 1 ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_DEVICE_LOST),
            "partial transfer failure marks the device lost");
      check(calls == fail_at && signal_fence == 0, "failed submit never signals or continues");
      check(f.device.vk.lost == (fail_at != 1), "pre-transfer OOM does not lose the device");
   }
   {
      fixture f(1000);
      f.commands.back().cs.entry_count = UINT32_MAX;
      check(f.run() == VK_ERROR_OUT_OF_HOST_MEMORY && calls == 0,
            "oversized last CB is preflighted without integer wrap or partial transfer");
   }
   for (unsigned mode = 0; mode < 4; mode++) {
      fixture f(1000);
      if (mode == 0) f.commands[500].state.suspend_resume = SR_IN_CHAIN;
      if (mode == 1) f.device.trace_context = true;
      if (mode == 2) f.submit.has_bind = true;
      if (mode == 3) f.knl.max_submit_entries = 0;
      check(f.run() == VK_SUCCESS && calls == 1,
            "dynamic chains, trace copies, sparse binds and unlimited backends stay unsplit");
   }
   std::printf("queue batch production regression: %u/%u PASS\n", checks - failures, checks);
   return failures ? 1 : 0;
}
