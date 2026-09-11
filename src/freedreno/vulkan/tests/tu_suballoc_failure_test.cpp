#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1;
enum tu_bo_alloc_flags { TU_BO_ALLOC_NO_FLAGS };
struct tu_device {};
struct tu_bo { uint64_t iova; uint32_t size; unsigned refs; };
// PRODUCTION_STRUCTS
static unsigned live, releases, allocations;
static bool fail_map, fail_alloc;
static uint32_t align(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
static tu_bo *tu_bo_get_ref(tu_bo *bo) { ++bo->refs; return bo; }
static void tu_bo_finish(tu_device *, tu_bo *bo) {
    assert(bo && bo->refs);
    ++releases;
    if (!--bo->refs) { --live; delete bo; }
}
static VkResult tu_bo_init_new(tu_device *, void *, tu_bo **out, uint32_t size,
                               tu_bo_alloc_flags, const char *) {
    *out = nullptr;
    if (fail_alloc) return -2;
    *out = new tu_bo{0x100000, size, 1};
    ++live; ++allocations;
    return VK_SUCCESS;
}
static VkResult tu_bo_map(tu_device *, tu_bo *, void *) { return fail_map ? -5 : VK_SUCCESS; }
// PRODUCTION_FUNCTIONS
static unsigned checks, failures;
static void check(bool pass, const char *name) {
    ++checks;
    if (!pass) { ++failures; std::printf("FAIL: %s\n", name); }
}
int main() {
    tu_device device{};
    for (bool replace : {false, true}) {
        tu_suballocator allocator{};
        tu_bo_suballocator_init(&allocator, &device, 4096, TU_BO_ALLOC_NO_FLAGS, "test");
        tu_suballoc_bo first{}, failed{}, retry{};
        fail_map = fail_alloc = false;
        if (replace) {
            check(tu_suballoc_bo_alloc(&first, &allocator, 4096, 64) == VK_SUCCESS,
                  "initial allocation fills first BO");
        }
        fail_map = true;
        const unsigned before = releases;
        check(tu_suballoc_bo_alloc(&failed, &allocator, 128, 64) == VK_ERROR_OUT_OF_HOST_MEMORY,
              "mapping failure reported");
        check(!allocator.bo, "suballocator no longer owns released BO");
        check(!failed.bo, "failed request does not publish BO");
        check(releases == before + (replace ? 2u : 1u), "owned references dropped once");
        if (allocator.bo) {
            // The old-source negative control has already failed. Do not
            // dereference its stale pointer while checking other scenarios.
            allocator.bo = nullptr;
        } else {
            fail_map = false;
            const unsigned old_allocations = allocations;
            check(tu_suballoc_bo_alloc(&retry, &allocator, 128, 64) == VK_SUCCESS,
                  "retry succeeds with a fresh BO");
            check(allocations == old_allocations + 1 && retry.iova == retry.bo->iova,
                  "retry starts at fresh mapped storage");
            tu_bo_finish(&device, retry.bo);
        }
        if (replace) {
            check(first.bo->refs == 1 && first.bo->size == 4096,
                  "earlier live suballocation survives replacement failure");
            tu_bo_finish(&device, first.bo);
        }
        tu_bo_suballocator_finish(&allocator);
        check(live == 0, "cleanup has no live or double-freed BOs");
    }
    tu_suballocator allocator{};
    tu_bo_suballocator_init(&allocator, &device, 4096, TU_BO_ALLOC_NO_FLAGS, "test");
    tu_suballoc_bo failed{};
    fail_alloc = true;
    check(tu_suballoc_bo_alloc(&failed, &allocator, 128, 64) == -2 && !allocator.bo,
          "BO allocation failure remains safe");
    tu_bo_suballocator_finish(&allocator);
    check(live == 0, "failed initial BO needs no cleanup");
    std::printf("Suballocator failure paths: %u/%u PASS\n", checks - failures, checks);
    return failures ? 1 : 0;
}
