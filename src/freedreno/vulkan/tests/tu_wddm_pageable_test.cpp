/* SPDX-License-Identifier: MIT */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include "tu_wddm_pageable.h"

using mtx_t = std::mutex;
static void mtx_lock(mtx_t *lock) { lock->lock(); }
static void mtx_unlock(mtx_t *lock) { lock->unlock(); }
struct tu_wddm_device { void *runtime_owner; };
struct tu_wddm_context { tu_wddm_device *device; struct { uint64_t ResetGeneration; } info; };
struct tu_wddm_allocation {
    void *runtime_token;
    tu_wddm_context *context;
    struct { uint64_t ExpectedResetGeneration, RequestedIova; } private_info;
    uint64_t vma_size;
};
struct tu_bo { unsigned references; tu_wddm_allocation *wddm_allocation; };
struct tu_device {
    void *wddm_runtime_owner;
    mwd_callbacks wddm_callbacks;
    tu_wddm_context wddm_context;
    mtx_t wddm_mutex;
    tu_pageable_record *wddm_pageables = nullptr;
};
using VkDevice = tu_device *;
#define VK_FROM_HANDLE(type, name, handle) auto *name = static_cast<type *>(handle)
static std::function<void()> on_retain, on_status, on_finish;
static unsigned token_references = 1, status_calls;
static bool mismatch_reply;
static int32_t retain_result, status_result;
static void *expected_owner;
static int token;
static tu_bo *observed_bo;
static void fail(const char *message) { std::fprintf(stderr, "FAIL pageable %s\n", message); std::exit(1); }
static int32_t MWD_CALL retain(void *owner, void *value, mwd_allocation *out) {
    if (owner != expected_owner || value != &token) fail("callback identity mismatch");
    if (!observed_bo || observed_bo->references < 2) fail("BO ownership expired inside runtime callback");
    if (retain_result < 0) return retain_result;
    ++token_references;
    *out = {&token, 0x10010000, 65536, mismatch_reply ? 8u : 7u, 42, 6};
    if (on_retain) on_retain();
    return 0;
}
static int32_t MWD_CALL release_token(void *owner, void *value) {
    if (owner != expected_owner || value != &token || !token_references) fail("release ownership mismatch");
    --token_references; return 0;
}
static int32_t MWD_CALL status(void *owner) {
    ++status_calls;
    if (owner != expected_owner) return (int32_t)0x80070057u;
    if (on_status) on_status();
    return status_result;
}
static tu_bo *tu_bo_get_ref(tu_bo *bo) { ++bo->references; return bo; }
static void tu_bo_finish(tu_device *, tu_bo *bo) {
    if (!bo->references) fail("BO released twice");
    --bo->references;
    if (on_finish) on_finish();
}
// PRODUCTION_FUNCTIONS

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main() {
    int owner, foreign_owner;
    expected_owner = &owner;
    tu_wddm_device kernel{&owner};
    tu_wddm_context context{&kernel, {7}};
    tu_wddm_allocation allocation{&token, &context, {7, 0x10010000}, 65536};
    tu_bo bo{1, &allocation}; observed_bo = &bo;
    tu_device device{}; device.wddm_runtime_owner = &owner; device.wddm_context = context;
    auto &cb = device.wddm_callbacks;
    cb.magic = MWD_RUNTIME_MAGIC; cb.version = MWD_RUNTIME_ABI_VERSION; cb.size = sizeof(cb);
    cb.context = [](void *, mwd_context_info *) -> int32_t { return 0; };
    cb.allocate = [](void *, uint64_t, uint64_t, uint64_t, uint32_t, mwd_allocation *) -> int32_t { return 0; };
    cb.retain = retain; cb.release = release_token;
    cb.map = [](void *, void *, void **, uint32_t *) -> int32_t { return 0; };
    cb.unmap = [](void *, void *) -> int32_t { return 0; };
    cb.submit = [](void *, uint32_t, const void *, uint32_t, const mwd_reference *, uint32_t) -> int32_t { return 0; };
    cb.completed = [](void *, uint32_t *) -> int32_t { return 0; };
    cb.status = status;
    cb.queue_retain = cb.queue_release = [](void *, void *) -> int32_t { return 0; };
    cb.submit_queue = [](void *, void *, uint32_t, const void *, uint32_t, const mwd_reference *, uint32_t) -> int32_t { return 0; };
    tu_pageable_record memory{}, descriptor{}, query{}, cpu{}, lazy{};
    tu_wddm_pageable_register(&device, &memory, MWD_PAGEABLE_MEMORY, 101, &bo, false);
    tu_wddm_pageable_register(&device, &descriptor, MWD_PAGEABLE_DESCRIPTOR_POOL, 102, &bo, false);
    tu_wddm_pageable_register(&device, &query, MWD_PAGEABLE_QUERY_POOL, 103, &bo, false);
    tu_wddm_pageable_register(&device, &cpu, MWD_PAGEABLE_DESCRIPTOR_POOL, 104, nullptr, true);
    tu_wddm_pageable_register(&device, &lazy, MWD_PAGEABLE_MEMORY, 105, nullptr, false);
    mwd_pageable_backing result{};
    for (uint32_t type : {1u, 2u, 3u}) {
        CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, type, 100 + type, &result) == 0);
        if (token_references != 2) fail("output did not transfer retained allocation ownership");
        CHECK(bo.references == 1 && !result.flags && result.allocation.token == &token &&
            result.allocation.handle == 42 && result.allocation.generation == 7);
        CHECK(release_token(&owner, result.allocation.token) == 0);
    }
    std::memset(&result, 0xa5, sizeof(result));
    const auto untouched = result;
    const unsigned old_status = status_calls;
    CHECK(tu_wddm_pageable_acquire(&device, &foreign_owner, 7, 1, 101, &result) < 0);
    if (status_calls != old_status) fail("foreign owner reached runtime callback");
    CHECK(!std::memcmp(&result, &untouched, sizeof(result)) && token_references == 1);
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 8, 1, 101, &result) < 0);
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 2, 101, &result) < 0);
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 1, 1, &result) < 0);
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 1, 105, &result) < 0);
    CHECK(!std::memcmp(&result, &untouched, sizeof(result)));
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 2, 104, &result) == 0);
    CHECK(result.flags == MWD_PAGEABLE_NO_BACKING && !result.allocation.token && token_references == 1);
    mismatch_reply = true; result = untouched;
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 1, 101, &result) < 0);
    CHECK(!std::memcmp(&result, &untouched, sizeof(result)) && token_references == 1 && bo.references == 1);
    mismatch_reply = false; retain_result = (int32_t)0x8007000eu;
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 1, 101, &result) == retain_result);
    CHECK(token_references == 1 && bo.references == 1);
    retain_result = 0;

    // Reentrant deletion removes only the object; the borrowed BO and exact
    // allocation stay live through return. No provider lock spans callbacks.
    on_retain = [&] { tu_wddm_pageable_unregister(&device, &memory); tu_bo_finish(&device, &bo); };
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 1, 101, &result) == 0);
    CHECK(bo.references == 0 && token_references == 2);
    CHECK(release_token(&owner, result.allocation.token) == 0);
    CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 1, 101, &result) < 0);
    on_retain = {}; bo.references = 1;
    // Loss after token acquisition, and separately during BO-pin release, must
    // roll back token ownership and preserve caller output.
    for (unsigned mode : {0u, 1u}) {
        auto loss = [&] { status_result = (int32_t)0x887a0005u; };
        if (mode) on_finish = loss; else on_retain = loss;
        result = untouched;
        CHECK(tu_wddm_pageable_acquire(&device, &owner, 7, 2, 102, &result) == (int32_t)0x887a0005u);
        CHECK(token_references == 1 && bo.references == 1 && !std::memcmp(&result, &untouched, sizeof(result)));
        status_result = 0; on_retain = {}; on_finish = {};
    }
    tu_wddm_pageable_unregister(&device, &descriptor);
    tu_wddm_pageable_unregister(&device, &query);
    tu_wddm_pageable_unregister(&device, &cpu);
    tu_wddm_pageable_unregister(&device, &lazy);
    CHECK(!device.wddm_pageables);
    std::puts("PASS production pageable registry ownership, retained backing, stale/foreign rejection, reset and callback cleanup");
    return 0;
}
