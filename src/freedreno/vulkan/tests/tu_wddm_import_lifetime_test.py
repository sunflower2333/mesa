#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute production imported BO sharing and retirement with injected peers."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here.parent / 'tu_knl_wddm.cc').read_text()


def definition(name):
    match = re.search(r'(?:static\s+)?(?:inline\s+)?(?:bool|void|uint32_t|VkResult)\s+' +
                      name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    depth = 0
    for index in range(source.index('{', match.start()), len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if not depth:
            return source[match.start():index + 1]
    raise AssertionError(name)


fixture = r'''
#include "vulkan/vulkan_core.h"
#include "tu_wddm_abi.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <set>
constexpr unsigned TU_WDDM_MAX_RENDER_ALLOCATIONS=16;
constexpr uint64_t os_page_size=4096;
struct vk_device { int alloc; bool lost; };
struct tu_wddm_device {};
struct tu_wddm_context {
 tu_wddm_device *device; VIOGPU_WDDM_CONTEXT_INFO info; unsigned last_submitted_fence;
};
struct tu_wddm_allocation {
 tu_wddm_context *context; uint32_t handle; bool imported, aliased, locked;
 uint64_t share_key, vma_size, imported_size; VIOGPU_WDDM_ALLOCATION_INFO private_info;
 struct { uint64_t reset_generation; unsigned fence; bool pending, fence_ready; } retirement;
};
struct tu_bo {
 uint32_t gem_handle; uint64_t size, iova; const char *name; int refcnt;
 tu_wddm_allocation *wddm_allocation; void *base, *map;
};
struct tu_device {
 vk_device vk; int bo_mutex, vma_mutex, wddm_mutex;
 std::set<uint64_t> vma; uint32_t wddm_next_handle=1;
 tu_bo slots[32]{}; tu_bo **wddm_bos=nullptr;
 uint32_t wddm_bo_count=0, wddm_bo_capacity=0, wddm_retired_count=0;
 bool wddm_deferred_bo_destroy=false;
 struct { unsigned queued, pending_peak; } wddm_lifetime_stats{};
 tu_wddm_device wddm_device; tu_wddm_context wddm_context{};
};
unsigned importCalls, releaseCalls, waitCalls, freeCalls;
bool importOk=true, releaseOk=true, waitOk=true;
bool aliasAnswer=false;
uint64_t nextIova=4096;
void mtx_lock(int *m) { assert(*m==0); *m=1; }
void mtx_unlock(int *m) { assert(*m==1); *m=0; }
int p_atomic_read(int *p) { return *p; }
void p_atomic_inc(int *p) { ++*p; }
bool p_atomic_dec_zero(int *p) { return --*p==0; }
void p_atomic_set(int *p,int n) { *p=n; }
void *vk_zalloc(int*,size_t n,unsigned,VkSystemAllocationScope) { return calloc(1,n); }
void *vk_realloc(int*,void *p,size_t n,unsigned,VkSystemAllocationScope) { return realloc(p,n); }
void vk_free(int*,void *p) { ++freeCalls; free(p); }
VkResult vk_error(tu_device*,VkResult r) { return r; }
bool vk_device_is_lost(vk_device *v) { return v->lost; }
void vk_device_set_lost(vk_device *v,const char*) { v->lost=true; }
void mesa_loge(const char*,...) {}
void mesa_logi(const char*,...) {}
uint64_t util_vma_heap_alloc(std::set<uint64_t> *v,uint64_t size,uint64_t) {
 uint64_t iova=nextIova; nextIova+=size; assert(v->insert(iova).second); return iova;
}
void util_vma_heap_free(std::set<uint64_t> *v,uint64_t iova,uint64_t) { assert(v->erase(iova)==1); }
tu_bo *tu_device_lookup_bo(tu_device *d,uint32_t token) { return &d->slots[token%32]; }
const char *tu_debug_bos_add(tu_device*,uint64_t,const char *name) { return name; }
void tu_debug_bos_del(tu_device*,tu_bo*) {}
void tu_dump_bo_del(tu_device*,tu_bo*) {}
void tu_bo_release_heap_accounting(tu_device*,tu_bo*) {}
bool tu_wddm_context_wait_submissions(tu_wddm_context*,uint64_t) { ++waitCalls; return waitOk; }
bool tu_wddm_allocation_unlock(tu_wddm_allocation*) { assert(false); return false; }
bool tu_wddm_allocation_destroy(tu_wddm_allocation*) { assert(false); return false; }
bool tu_wddm_reap_retired_bos_locked(tu_device*,unsigned) { assert(false); return false; }
#define MAX2(a,b) ((a)>(b) ? (a) : (b))
bool tu_wddm_context_native_share(tu_wddm_context*,uint32_t opcode,VIOGPU_WDDM_NATIVE_SHARE *s) {
 if (opcode==VIOGPU_WDDM_ESCAPE_IMPORT_NATIVE) {
  ++importCalls;
  if (aliasAnswer) s->Iova=0x100000;
  return importOk;
 }
 assert(opcode==VIOGPU_WDDM_ESCAPE_RELEASE_NATIVE && waitCalls!=0);
 ++releaseCalls; return releaseOk;
}
// FUNCTIONS
int main() {
 tu_device d{}; d.wddm_context.device=&d.wddm_device; d.wddm_context.info.ResetGeneration=1;
 tu_bo *first=nullptr, *second=nullptr;
 assert(tu_wddm_bo_init_shared(&d,&first,8192,7)==VK_SUCCESS);
 assert(tu_wddm_bo_init_shared(&d,&second,8192,7)==VK_SUCCESS);
 assert(first==second && first->refcnt==2 && importCalls==1 && d.vma.size()==1);
 tu_bo *bad=nullptr;
 assert(tu_wddm_bo_init_shared(&d,&bad,4096,7)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 assert(!bad && first->refcnt==2 && importCalls==1);
 tu_wddm_bo_finish(&d,first);
 assert(second->refcnt==1 && waitCalls==0 && releaseCalls==0);
 releaseOk=false;
 tu_wddm_bo_finish(&d,second);
 assert(d.vk.lost && second->refcnt==1 && d.vma.size()==1 && d.wddm_bo_count==1 && freeCalls==0);
 assert(releaseCalls==1 && waitCalls==1);
 releaseOk=true;
 tu_wddm_bo_finish(&d,second);
 assert(d.vma.empty() && d.wddm_bo_count==0 && freeCalls==1);
 d.vk.lost=false;
 importOk=false;
 assert(tu_wddm_bo_init_shared(&d,&bad,8192,8)==VK_ERROR_INVALID_EXTERNAL_HANDLE);
 assert(!bad && d.vma.empty() && d.wddm_bo_count==0);
 importOk=true; waitOk=false;
 assert(tu_wddm_bo_init_shared(&d,&first,8192,8)==VK_SUCCESS);
 unsigned releases=releaseCalls, frees=freeCalls;
 tu_wddm_bo_finish(&d,first);
 assert(first->refcnt==1 && d.vma.size()==1 && d.wddm_bo_count==1);
 assert(releaseCalls==releases && freeCalls==frees);
 waitOk=true;
 tu_wddm_bo_finish(&d,first);
 assert(d.vma.empty() && d.wddm_bo_count==0);
 d.vk.lost=false; aliasAnswer=true;
 assert(tu_wddm_bo_init_shared(&d,&first,8192,9)==VK_SUCCESS);
 assert(first->wddm_allocation->aliased && d.vma.empty());
 releases=releaseCalls;
 tu_wddm_bo_finish(&d,first);
 assert(releaseCalls==releases && d.wddm_bo_count==0 && d.vma.empty());
 free(d.wddm_bos);
 puts("PASS production imported BO lifetime: reuse, size mismatch, failed import/fence/release, alias ownership");
}
'''
names = ('tu_wddm_allocation_release_import', 'tu_wddm_bo_valid',
         'tu_wddm_bo_valid_for_device', 'tu_wddm_remove_bo_locked',
         'tu_wddm_alloc_token_locked', 'tu_wddm_add_bo_locked',
         'tu_wddm_bo_init_shared_locked', 'tu_wddm_bo_init_shared', 'tu_wddm_bo_finish')
fixture = fixture.replace('// FUNCTIONS', '\n\n'.join(definition(n) for n in names))
with tempfile.TemporaryDirectory(prefix='turnip-import-lifetime-') as temporary:
    output = Path(temporary)
    (output / 'test.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-missing-field-initializers', '-I', str(here.parent),
                    '-I', str(here.parents[3] / 'include'), str(output / 'test.cpp'),
                    '-o', str(output / 'test')], check=True)
    subprocess.run([str(output / 'test')], check=True)
