// Production tu_cs initialization/failed allocation functions are inserted by
// tu_cs_failure_test.py. Only BO allocation and unrelated emission peers are fake.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#define unlikely(value) (value)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define TU_COND_EXEC_STACK_SIZE 4
using VkResult = int;
constexpr VkResult VK_SUCCESS = 0;
struct tu_device { unsigned property; };
struct tu_bo { uint64_t iova; uint32_t *map; uint32_t size; };
struct tu_pkt;
struct tu_crb;
// PRODUCTION_CS_STRUCTS
// PRODUCTION_SINK_SIZE
uint32_t tu_cs_fail_sink[TU_CS_FAIL_SINK_SIZE];

static uint32_t storage[256];
static tu_bo backing{0x100000, storage, sizeof(storage)};
static tu_bo *backing_array[]{&backing};
static tu_bo_array owned_bos(tu_bo **bos, uint32_t count) {
    tu_bo_array owned{};
    owned.bos = bos;
    owned.bo_count = owned.bo_capacity = count;
    owned.start = storage;
    return owned;
}
static VkResult allocation_result;
static unsigned reserve_calls;
static unsigned released_bos;
#define TU_RMV(...) ((void)0)
static void tu_bo_finish(tu_device *, tu_bo *) { ++released_bos; }
static uint32_t tu_sanitize_ib_size(uint32_t size) { return size < 0xfffff ? size : 0xfffff; }
static inline void tu_cs_fail(tu_cs *, VkResult);
static uint32_t tu_cs_get_space(const tu_cs *cs) {
    return cs->cur && cs->end ? static_cast<uint32_t>(cs->end - cs->cur) : 0;
}
static uint32_t tu_cs_get_offset(const tu_cs *cs) {
    return cs->start ? static_cast<uint32_t>(cs->start - storage) : 0;
}
static uint32_t tu_cs_get_size(const tu_cs *cs) {
    return static_cast<uint32_t>(cs->cur - cs->start);
}
static tu_bo *tu_cs_current_bo(const tu_cs *) { return &backing; }
static void tu_cs_sanity_check(const tu_cs *) {}
static void tu_cs_begin(tu_cs *) {}
static void tu_cs_end(tu_cs *) {}
static uint64_t tu_cs_get_cur_iova(const tu_cs *cs) {
    assert(cs->status == VK_SUCCESS);
    return backing.iova + tu_cs_get_offset(cs) * sizeof(uint32_t);
}
static VkResult tu_cs_reserve_space(tu_cs *cs, uint32_t size) {
    ++reserve_calls;
    if (cs->status != VK_SUCCESS) return cs->status;
    if (allocation_result != VK_SUCCESS && cs->mode != TU_CS_MODE_EXTERNAL) {
        tu_cs_fail(cs, allocation_result);
        return allocation_result;
    }
    if (!cs->start) {
        cs->start = cs->cur = storage;
        cs->end = storage + ARRAY_SIZE(storage);
    }
    assert(size <= tu_cs_get_space(cs));
    cs->reserved_end = cs->cur + size;
    return VK_SUCCESS;
}
static VkResult tu_cs_alloc(tu_cs *cs, uint32_t count, uint32_t size, tu_cs_memory *memory) {
    VkResult result = tu_cs_reserve_space(cs, count * size);
    if (result != VK_SUCCESS) return result;
    *memory = {cs->cur, backing.iova, cs->writeable};
    return VK_SUCCESS;
}
// PRODUCTION_FUNCTIONS

static int failures, checks;
static void check(bool pass, const char *name) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL: %s\n", name); }
}
static tu_cs parent(tu_device *device, bool writeable) {
    tu_cs cs{};
    cs.device = device;
    cs.name = "failure-regression";
    cs.mode = TU_CS_MODE_SUB_STREAM;
    cs.writeable = writeable;
    return cs;
}
int main() {
    tu_device device{0x1234};
    const VkResult errors[]{-1, -2, -4}; // host OOM, device OOM, device lost
    for (VkResult error : errors) {
        for (bool writeable : {false, true}) {
            for (int route = 0; route < 3; ++route) {
                tu_cs cs = parent(&device, writeable), child{};
                // Existing parent storage exercises the alignment branch too.
                if (route == 1) {
                    cs.start = cs.cur = storage + 1;
                    cs.end = storage + ARRAY_SIZE(storage);
                    cs.read_only = owned_bos(backing_array, 1);
                }
                allocation_result = error;
                VkResult result;
                if (route == 2) {
                    const auto state = tu_cs_draw_state(&cs, &child, 16);
                    check(state.iova == 0 && state.size == 0, "failed draw has no GPU state");
                    result = child.status;
                } else {
                    result = tu_cs_begin_sub_stream_aligned(&cs, 2, 8, &child);
                }
                check(result == error && cs.status == error && child.status == error,
                      "allocation error remains sticky");
                check(child.device == &device && child.mode == TU_CS_MODE_EXTERNAL &&
                      child.writeable == writeable, "failed emitter retains device context");
                check(child.device && child.device->property == 0x1234, "device property read");
                // Record a complete maximum-sized packet into the sink.
                std::memset(child.cur, 0xab, sizeof(tu_cs_fail_sink));
                child.cur += ARRAY_SIZE(tu_cs_fail_sink);
                const auto entry = tu_cs_end_sub_stream(&cs, &child);
                check(!entry.bo && !entry.size, "failed stream never published as GPU IB");

                // Retry on an already-failed parent without reading BO offsets.
                reserve_calls = 0;
                result = tu_cs_begin_sub_stream_aligned(&cs, 2, 8, &child);
                check(result == error && child.device == &device && reserve_calls == 0,
                      "failed parent exits before cursor arithmetic or allocation");
            }
        }
    }
    allocation_result = VK_SUCCESS;
    for (bool writeable : {false, true}) {
        tu_cs cs = parent(&device, writeable), child{};
        cs.start = cs.cur = storage + 1;
        cs.end = storage + ARRAY_SIZE(storage);
        cs.read_only = owned_bos(backing_array, 1);
        const auto result = tu_cs_begin_sub_stream_aligned(&cs, 2, 8, &child);
        check(result == VK_SUCCESS && child.device == &device && child.start == storage + 8 &&
              child.reserved_end == storage + 24 && child.writeable == writeable,
              "successful aligned stream unchanged");
        child.cur += 16;
        const auto entry = tu_cs_end_sub_stream(&cs, &child);
        check(entry.bo == &backing && entry.size == 64 && entry.offset == 32,
              "successful stream preserves real GPU storage");
    }
    for (VkResult error : errors) {
        for (int route = 0; route < 3; ++route) {
            tu_cs cs = parent(&device, false);
            tu_bo *owned[]{&backing, &backing};
            if (route != 0) {
                cs.start = storage;
                cs.cur = storage + 4;
                cs.end = storage + ARRAY_SIZE(storage);
                if (route == 1)
                    cs.read_only = owned_bos(owned, 2);
                else
                    cs.refcount_bo = &backing;
            }
            tu_cs_fail(&cs, error);
            released_bos = 0;
            tu_cs_reset(&cs);
            check(cs.status == VK_SUCCESS && cs.cur == cs.start && cs.reserved_end == cs.start,
                  "reset discards failed sink even before first BO");
            check(cs.cur != tu_cs_fail_sink && cs.reserved_end == cs.start,
                  "reset never exposes failure sink as successful storage");
            check(released_bos == (route == 1 ? 1u : 0u) && cs.entry_count == 0,
                  "reset retires only old BOs and entries");
            check(route == 0 ? !cs.start && !cs.end : cs.start == storage &&
                  cs.end == storage + ARRAY_SIZE(storage), "reset retains real backing bounds");
            // Avoid invalid pointer arithmetic in the old-source negative control.
            if (cs.cur != cs.start || cs.reserved_end != cs.start) continue;
            tu_cs child{};
            allocation_result = VK_SUCCESS;
            check(tu_cs_begin_sub_stream_aligned(&cs, 2, 8, &child) == VK_SUCCESS &&
                  child.start == storage && child.device == &device,
                  "fresh recording after failure reset uses GPU backing");
        }
    }
    std::printf("CS failure paths: %d/%d PASS\n", checks - failures, checks);
    return failures ? 1 : 0;
}
