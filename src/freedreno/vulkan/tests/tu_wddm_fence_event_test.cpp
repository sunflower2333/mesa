#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>
#include "tu_wddm_abi.h"
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
using NTSTATUS = int32_t;
constexpr bool FALSE = false;
constexpr DWORD WAIT_TIMEOUT = 258, D3DKMT_ESCAPE_DRIVERPRIVATE = 0;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define NT_SUCCESS(x) ((x) >= 0)
struct Event { bool signaled = false, open = true; };
using HANDLE = Event *;
struct D3DKMT_ESCAPE {
   unsigned hAdapter = 0, hDevice = 0, Type = 0, hContext = 0;
   void *pPrivateDriverData = nullptr;
   unsigned PrivateDriverDataSize = 0;
};
static void check(bool v) { if (!v) { std::cout << "FAIL" << std::endl; std::_Exit(1); } }
static unsigned arms, cancels, closes, polls, waits, queries;
static uint64_t clock_ms, next_cookie;
static DWORD last_timeout;
static bool supported = true, inline_complete, cancel_fail, create_fail, query_ok = true;
static bool publish_on_wait, loss_on_wait, signal_on_wait;
static uint32_t completed;
static std::vector<Event *> events;
static std::map<uint64_t, Event *> pending;
static NTSTATUS dispatch(D3DKMT_ESCAPE *call) {
   auto *p = static_cast<VIOGPU_WDDM_FENCE_EVENT *>(call->pPrivateDriverData);
   check(call->hContext && call->hAdapter == 12 && call->hDevice == 13);
   check(call->PrivateDriverDataSize == 72 && p->Header.Size == 72 &&
         p->Header.Magic == VIOGPU_WDDM_ABI_MAGIC && p->Header.Version == 0 && !p->Flags);
   if (p->Opcode == VIOGPU_WDDM_ESCAPE_ARM_FENCE_EVENT) {
      ++arms;
      if (!supported) return -1;
      check(p->ExpectedResetGeneration == 17 && p->Fence && p->EventHandle && !p->Cookie);
      auto *event = reinterpret_cast<Event *>(uintptr_t(p->EventHandle));
      if (inline_complete) { event->signaled = true; p->Cookie = 0; }
      else { p->Cookie = ++next_cookie; pending[p->Cookie] = event; }
   } else {
      check(p->Opcode == VIOGPU_WDDM_ESCAPE_CANCEL_FENCE_EVENT && p->Cookie);
      check(!p->ExpectedResetGeneration && !p->Fence && !p->EventHandle);
      ++cancels;
      if (cancel_fail) return -1;
      pending.erase(p->Cookie);
   }
   return 0;
}
struct tu_wddm_runtime { struct { NTSTATUS (*Escape)(D3DKMT_ESCAPE *) = ::dispatch; } dispatch; };
struct tu_wddm_device {
   struct { unsigned handle = 12; tu_wddm_runtime *runtime; } adapter;
   unsigned handle = 13;
   bool shared = false, active = true;
};
struct tu_wddm_context {
   tu_wddm_device *device;
   unsigned handle = 14;
   struct { uint64_t ResetGeneration = 17; } info;
};
static tu_wddm_device *waiting_device;
static HANDLE CreateEventW(void *, bool, bool, void *) {
   if (create_fail) return nullptr;
   auto *event = new Event;
   events.push_back(event);
   return event;
}
static bool ResetEvent(HANDLE event) { check(event->open); event->signaled = false; return true; }
static bool CloseHandle(HANDLE event) { check(event->open); event->open = false; ++closes; return true; }
static DWORD WaitForSingleObject(HANDLE event, DWORD ms) {
   check(event->open && ms && ms <= 32);
   ++waits; last_timeout = ms;
   if (publish_on_wait) completed = 9;
   if (loss_on_wait) waiting_device->active = false;
   if (signal_on_wait || publish_on_wait) event->signaled = true;
   if (event->signaled) { event->signaled = false; return 0; }
   clock_ms += ms;
   return WAIT_TIMEOUT;
}
static bool tu_wddm_shared_context(tu_wddm_context *c) { return c->device->shared; }
static void tu_wddm_init_header(VIOGPU_WDDM_ABI_HEADER *h, unsigned size) {
   *h = {VIOGPU_WDDM_ABI_MAGIC, 0, size, 0};
}
struct tu_wddm_fence_poll_wait { void wait(uint64_t) { ++polls; ++clock_ms; } };
static bool tu_wddm_fence_was_submitted(tu_wddm_context *, uint32_t fence) { return fence <= 9; }
static ULONGLONG GetTickCount64() { return clock_ms; }
static bool tu_wddm_context_get_completed_fence(tu_wddm_context *, uint32_t *out) {
   ++queries; *out = completed; return query_ok;
}
static bool tu_wddm_device_execution_active(tu_wddm_device *d) { return d->active; }
static bool tu_wddm_fence_after(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
// PRODUCTION
static void reset() {
   for (auto *event : events) { check(!event->open); delete event; }
   events.clear();
   pending.clear();
   arms = cancels = closes = polls = waits = queries = 0;
   clock_ms = next_cookie = 0;
   last_timeout = 0;
   supported = query_ok = true;
   inline_complete = cancel_fail = create_fail = publish_on_wait = loss_on_wait = signal_on_wait = false;
   completed = 0;
}
int main() {
   tu_wddm_runtime runtime;
   tu_wddm_device device{{12, &runtime}};
   tu_wddm_context context{&device, 14, {17}}, second{&device, 15, {17}};
   waiting_device = &device;
   {
      tu_wddm_fence_event_wait wait;
      inline_complete = true;
      wait.add(&context, 9); wait.wait();
      check(clock_ms == 0 && waits == 1 && polls == 0 && cancels == 0);
   }
   reset();
   {
      tu_wddm_fence_event_wait wait;
      wait.add(&context, 9);
      inline_complete = true;
      wait.add(&second, 9); // second context satisfies WAIT_ANY
      wait.wait();
      check(clock_ms == 0 && waits == 1 && cancels == 1 && pending.empty());
      inline_complete = false;
      wait.add(&context, 9); wait.wait(1);
      check(last_timeout == 1 && clock_ms == 1 && pending.empty());
   }
   reset();
   {
      tu_wddm_fence_event_wait wait;
      supported = false;
      wait.add(&context, 9); wait.wait(1000000);
      wait.add(&context, 9); wait.wait(1000000);
      check(arms == 1 && polls == 2 && !waits);
   }
   reset();
   {
      tu_wddm_fence_event_wait wait;
      cancel_fail = true;
      wait.add(&context, 9); wait.wait();
      check(closes == 1 && pending.size() == 1);
      wait.add(&context, 9); wait.wait();
      check(arms == 1 && polls == 1); // never reuse a potentially late-signaled event
   }
   reset();
   {
      tu_wddm_fence_event_wait wait;
      wait.add(&context, 9);
   }
   check(cancels == 1 && pending.empty());
   reset();
   {
      tu_wddm_fence_event_wait wait;
      device.shared = true;
      wait.add(&context, 9); wait.wait();
      device.shared = false;
      check(!arms && polls == 1);
   }
   reset();
   check(!tu_wddm_context_wait_fence(&context, 9, 0) && !arms && !waits && !polls);
   completed = 9;
   check(tu_wddm_context_wait_fence(&context, 9, 0) && !arms);
   reset();
   publish_on_wait = true;
   check(tu_wddm_context_wait_fence(&context, 9, UINT64_MAX));
   check(queries == 2 && arms == 1 && waits == 1 && pending.empty());
   reset();
   // A wake plus completed fence is still failure if the device reset.
   publish_on_wait = loss_on_wait = true;
   check(!tu_wddm_context_wait_fence(&context, 9, UINT64_MAX));
   device.active = true;
   reset();
   check(!tu_wddm_context_wait_fence(&context, 9, 33000000));
   check(clock_ms == 33 && waits == 2 && queries == 3 && pending.empty());
   reset();
   create_fail = true;
   check(!tu_wddm_context_wait_fence(&context, 9, 1000000));
   check(polls == 1 && !arms);
   reset();
   std::cout << "PASS: event wake, multiwait, deadline, fallback, cancellation and authoritative rechecks\n";
}
