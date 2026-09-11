#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#define TU_HAS_WDDM 1
#define TU_COND_EXEC_STACK_SIZE 4
#define VK_TRACE_MODE_RMV 1
#define unlikely(x) (x)
#define MAX2(a,b) ((a) > (b) ? (a) : (b))
#define MIN2(a,b) ((a) < (b) ? (a) : (b))
#define COND(c,v) ((c) ? (v) : 0)
#define TU_RMV(...) ((void)0)
using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1;
enum tu_bo_alloc_flags { TU_BO_ALLOC_GPU_READ_ONLY = 1, TU_BO_ALLOC_ALLOW_DUMP = 2 };
struct tu_device;
struct tu_bo { uint64_t iova; uint32_t *map; uint32_t size; unsigned refs; bool readonly; };
// PRODUCTION_SUB_STRUCTS
struct tu_knl { const char *name; };
struct instance { struct { unsigned trace_mode; } vk; const tu_knl *knl; };
struct physical { struct instance *instance; };
struct tu_device { physical *physical_device; struct instance *instance; tu_suballocator pipeline_suballoc; std::mutex pipeline_mutex; };
[[maybe_unused]] static void mtx_lock(std::mutex *mutex) { mutex->lock(); }
[[maybe_unused]] static void mtx_unlock(std::mutex *mutex) { mutex->unlock(); }
static uint32_t align(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }
static unsigned live, allocations;
static bool fail_map, fail_alloc;
static uint64_t next_iova = 0x100000;
static tu_bo *tu_bo_get_ref(tu_bo *bo) { ++bo->refs; return bo; }
static void tu_bo_finish(tu_device *, tu_bo *bo) {
    assert(bo && bo->refs);
    if (!--bo->refs) { --live; std::free(bo->map); delete bo; }
}
static VkResult tu_bo_init_new(tu_device *, void *, tu_bo **out, uint32_t size,
                               tu_bo_alloc_flags flags, const char *) {
    *out = nullptr;
    if (fail_alloc || live >= 1024) return -2;
    auto *map = static_cast<uint32_t *>(std::calloc(size, 1));
    assert(map);
    *out = new tu_bo{next_iova, map, size, 1, (flags & TU_BO_ALLOC_GPU_READ_ONLY) != 0};
    next_iova += align(size, 4096);
    ++live; ++allocations;
    return VK_SUCCESS;
}
static VkResult tu_bo_map(tu_device *, tu_bo *, void *) { return fail_map ? -1 : 0; }
struct tu_pkt;
struct tu_crb;
// PRODUCTION_CS_STRUCTS
static bool tu_cs_is_empty(const tu_cs *cs) { return cs->cur == cs->start; }
static uint32_t tu_cs_get_size(const tu_cs *cs) { return static_cast<uint32_t>(cs->cur - cs->start); }
// PRODUCTION_FUNCTIONS

static unsigned checks, failures;
static void check(bool pass, const char *name) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL: %s\n", name); }
}
static void init_pool(tu_device *device) {
    tu_bo_suballocator_init(&device->pipeline_suballoc, device, 128 * 1024,
                            TU_BO_ALLOC_GPU_READ_ONLY, "pool");
}
int main() {
    const tu_knl wddm{"wddm"}, other{"drm"};
    struct instance instance{{0}, &wddm};
    physical physical{&instance};
    tu_device device{};
    device.instance = &instance;
    device.physical_device = &physical;
    init_pool(&device);
    std::vector<tu_cs> streams(1280);
    unsigned created = 0;
    for (auto &cs : streams) {
        tu_cs_init(&cs, &device, TU_CS_MODE_GROW, 2048, "small-cs");
        if (tu_cs_add_bo(&cs, 2048) != VK_SUCCESS) break;
        std::memset(cs.start, static_cast<int>((created % 250) + 1), 8192);
        ++created;
    }
    check(created == streams.size(), "1280 small streams fit below 1024 KMT BO owners");
    check(live <= 81, "8KiB streams pack into 128KiB BOs");
    std::printf("Small streams: %u created, %u KMT BOs\n", created, live);
    bool ranges_ok = true;
    for (unsigned i = 0; i < created; i++) {
        auto &cs = streams[i];
        auto *begin = cs.start;
        auto *end = cs.end;
        ranges_ok &= end - begin == 2048;
        ranges_ok &= tu_cs_get_cur_iova(&cs) == cs.read_only.bos[0]->iova + tu_cs_get_offset(&cs) * 4;
        ranges_ok &= reinterpret_cast<uintptr_t>(begin) % 4 == 0;
        // Advancing and switching must preserve the slice end, not expose
        // storage belonging to the next stream in the same BO.
        cs.cur += 8;
        if (tu_cs_reserve_entry(&cs) != VK_SUCCESS) return 2;
        tu_cs_set_writeable(&cs, true);
        ranges_ok &= cs.end == nullptr;
        tu_cs_set_writeable(&cs, false);
        ranges_ok &= cs.cur == begin + 8 && cs.end == end;
        tu_cs_reset(&cs);
        ranges_ok &= cs.start == begin && cs.end == end;
        const auto expected = static_cast<unsigned char>((i % 250) + 1);
        for (unsigned j = 0; j < 8192; ++j)
            ranges_ok &= reinterpret_cast<unsigned char *>(begin)[j] == expected;
    }
    check(ranges_ok, "independent contents and IOVAs survive reset and writable switching");
    for (auto &cs : streams) tu_cs_finish(&cs);
    tu_bo_suballocator_finish(&device.pipeline_suballoc);
    check(live == 0, "all pool and CS references released once");

    init_pool(&device);
    tu_cs cs{};
    tu_cs_init(&cs, &device, TU_CS_MODE_SUB_STREAM, 2048, "grow");
    check(tu_cs_add_bo(&cs, 2048) == VK_SUCCESS, "first pooled slice");
    tu_bo *first = cs.read_only.bos[0];
    auto *first_map = cs.start;
    first_map[0] = 0xaabbccdd;
    check(tu_cs_add_bo(&cs, 4096) == VK_SUCCESS, "second pooled slice");
    auto *second_map = cs.start;
    check(second_map != first_map && first_map[0] == 0xaabbccdd, "growth never aliases the earlier live slice");
    check(tu_cs_add_bo(&cs, 8192) == VK_SUCCESS, "large stream uses standalone BO");
    check(cs.read_only.bos[2] != first && cs.end - cs.start == 8192, "large fallback owns complete range");
    auto *large = cs.start;
    tu_cs_reset(&cs);
    check(cs.start == large && cs.end - cs.start == 8192 && cs.read_only.bo_count == 1,
          "reset after mixed pooled and standalone growth keeps last range");
    tu_cs_set_writeable(&cs, true);
    check(tu_cs_add_bo(&cs, 2048) == VK_SUCCESS && !cs.read_write.bos[0]->readonly,
          "GPU-writeable streams stay separate from read-only pool");
    tu_cs_set_writeable(&cs, false);
    tu_cs_finish(&cs);
    tu_bo_suballocator_finish(&device.pipeline_suballoc);
    check(live == 0, "mixed stream teardown releases every owner");

    for (int failure = 0; failure < 2; failure++) {
        init_pool(&device);
        tu_cs_init(&cs, &device, TU_CS_MODE_SUB_STREAM, 2048, "failure");
        fail_alloc = failure == 0; fail_map = failure == 1;
        check(tu_cs_add_bo(&cs, 2048) != VK_SUCCESS && cs.read_only.bo_count == 0,
              "failed pool allocation publishes no slice");
        fail_alloc = fail_map = false;
        check(tu_cs_add_bo(&cs, 2048) == VK_SUCCESS, "pool allocation retries safely");
        tu_cs_finish(&cs);
        tu_bo_suballocator_finish(&device.pipeline_suballoc);
        check(live == 0, "failed map and allocation cleanup has no leaked owner");
    }
    for (int route = 0; route < 3; route++) {
        init_pool(&device);
        instance.knl = route != 0 ? &wddm : &other;
        instance.vk.trace_mode = route == 1 ? VK_TRACE_MODE_RMV : 0;
        if (route == 2) device.pipeline_suballoc.dev = nullptr;
        tu_cs_init(&cs, &device, TU_CS_MODE_SUB_STREAM, 2048, "fallback");
        const unsigned before = allocations;
        check(tu_cs_add_bo(&cs, 2048) == VK_SUCCESS && allocations == before + 1 &&
              cs.read_only.bos[0]->size == 8192 && cs.start == cs.read_only.bos[0]->map,
              "non-WDDM, RMV and pre-pool initialization keep standalone allocation");
        tu_cs_finish(&cs);
        check(live == 0, "standalone fallback releases allocation");
    }
    std::printf("CS pool paths: %u/%u PASS\n", checks - failures, checks);
    return failures ? 1 : 0;
}
