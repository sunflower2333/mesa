#!/usr/bin/env python3
import ctypes
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
abi = (root / "freedreno/vulkan/tu_wddm_abi.h").read_text()
resource = (root / "gallium/frontends/d3d10umd/Resource.cpp").read_text()


class Header(ctypes.Structure):
    _pack_ = 4
    _fields_ = [("magic", ctypes.c_uint32), ("version", ctypes.c_uint32),
                ("size", ctypes.c_uint32), ("reserved", ctypes.c_uint32)]


class Surface(ctypes.Structure):
    _pack_ = 4
    _fields_ = [("header", Header), ("opcode", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("expected_reset", ctypes.c_uint64),
                ("share_key", ctypes.c_uint64), ("size", ctypes.c_uint64),
                ("reset", ctypes.c_uint64), ("modifier", ctypes.c_uint64),
                ("plane_offset", ctypes.c_uint64), ("resource_id", ctypes.c_uint32),
                ("context_id", ctypes.c_uint32), ("width", ctypes.c_uint32),
                ("height", ctypes.c_uint32), ("fourcc", ctypes.c_uint32),
                ("stride", ctypes.c_uint32), ("plane_count", ctypes.c_uint32),
                ("layout_flags", ctypes.c_uint32), ("reserved", ctypes.c_uint64 * 3)]


assert ctypes.sizeof(Surface) == 128
for text in ("VIOGPU_WDDM_ESCAPE_ALLOCATE_NATIVE_SURFACE = 8",
             "VIOGPU_WDDM_ESCAPE_FREE_NATIVE_SURFACE = 9",
             "static_assert(sizeof(VIOGPU_WDDM_NATIVE_SURFACE) == 128"):
    assert text in abi

for text in ("VIOGPU_NATIVE_HOST_SURFACE", "CreateNativeHostSurfaceTexture(",
             "resource_from_handle(pipe->screen", "surface->ShareKey <= UINT32_MAX",
             "surface->Modifier == DROIDVM_DRM_FORMAT_MOD_LINEAR",
             "surface->PlaneOffset == 0", "surface->PlaneCount == 1",
             "surface->ResourceId != 0 && surface->ContextId == 0",
             "surface->LayoutFlags == DROIDVM_NATIVE_SURFACE_LINEAR_ALIAS",
             "surface->Size >= (UINT64)surface->Stride * surface->Height"):
    assert text in resource

create = resource[resource.index("void APIENTRY\nCreateResource"):resource.index("SIZE_T APIENTRY\nCalcPrivateOpenedResourceSize")]
assert create.index("if (nativeHostSurface)") < create.index("CreateSharedTextureCache")
assert "native host surface allocation/import failed" in create
assert "SharedAllocationFlags(hostSurface != NULL, pResource->scanout_primary)" in create
assert "(privateData.Flags & VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE) != 0" in create

destroy = resource[resource.index("void APIENTRY\nDestroyResource"):resource.index("void APIENTRY\nResourceMap")]
assert destroy.index("pipe_resource_reference(&pResource->resource, NULL);") < destroy.index(
    "ReleaseNativeHostSurface(pipe->screen, pResource);")


def extract(name):
    start = resource.index('\n' + name + '(')
    start = resource.rfind('\n', 0, start - 1) + 1
    brace = resource.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (resource[end] == '{') - (resource[end] == '}')
        end += 1
    return resource[start:end]


# Execute the real allocation/import/rollback functions with an injected KMD
# response, including padded host allocations and malformed layouts.
fixture = r'''
#include "tu_wddm_abi.h"
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdio>
using UINT64=uint64_t;
using ULONG_PTR=uintptr_t;
using HANDLE=void*;
constexpr uint64_t DROIDVM_DRM_FORMAT_MOD_LINEAR=0;
constexpr uint32_t DROIDVM_NATIVE_SURFACE_LINEAR_ALIAS=1;
constexpr unsigned PIPE_BIND_SHARED=1, PIPE_BIND_LINEAR=2;
constexpr unsigned WINSYS_HANDLE_TYPE_WIN32_HANDLE=7;
constexpr unsigned PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE=2;
#define DebugPrintf(...) ((void)0)
struct pipe_resource { unsigned width0, height0, bind, format; };
struct winsys_handle {
 unsigned type; HANDLE handle; unsigned stride; uint64_t offset, modifier, format, size;
};
struct pipe_screen {
 pipe_resource *(*resource_from_handle)(pipe_screen*,const pipe_resource*,winsys_handle*,unsigned);
};
struct pipe_context { pipe_screen *screen; };
VIOGPU_WDDM_NATIVE_SURFACE response;
unsigned allocations, imports, frees;
bool allocationOk=true, importOk=true;
pipe_resource texture{};
bool NativeSurfaceRequest(pipe_screen*,VIOGPU_WDDM_NATIVE_SURFACE *s) {
 if (s->Opcode==VIOGPU_WDDM_ESCAPE_FREE_NATIVE_SURFACE) {
  ++frees;
  assert(s->ShareKey==response.ShareKey && s->ResourceId==response.ResourceId);
  assert(s->ExpectedResetGeneration==response.ResetGeneration);
  return true;
 }
 ++allocations;
 assert(s->Opcode==VIOGPU_WDDM_ESCAPE_ALLOCATE_NATIVE_SURFACE);
 assert(s->ContextId==0 && s->ExpectedResetGeneration==0 && s->Flags==0);
 assert(s->ShareKey==0 && s->Size==0 && s->Stride==0 && s->ResourceId==0);
 if (!allocationOk) return false;
 *s=response;
 return true;
}
pipe_resource *importTexture(pipe_screen*,const pipe_resource *t,winsys_handle *h,unsigned usage) {
 ++imports;
 assert(h->size==response.Size && h->stride==response.Stride);
 assert(h->offset==0 && h->modifier==0 && h->format==t->format);
 assert(uintptr_t(h->handle)==response.ShareKey);
 assert((t->bind&(PIPE_BIND_SHARED|PIPE_BIND_LINEAR))==(PIPE_BIND_SHARED|PIPE_BIND_LINEAR));
 assert(usage==PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE);
 return importOk ? &texture : nullptr;
}
// FUNCTIONS
int main() {
 pipe_screen screen{importTexture}; pipe_context pipe{&screen};
 pipe_resource t{101,37,0,19}; VIOGPU_WDDM_NATIVE_SURFACE result{};
 VIOGPU_WDDM_NATIVE_SURFACE good{};
 good.Header={VIOGPU_WDDM_ABI_MAGIC,VIOGPU_WDDM_ABI_VERSION,sizeof(good),0};
 good.Opcode=VIOGPU_WDDM_ESCAPE_ALLOCATE_NATIVE_SURFACE;
 good.ShareKey=11; good.Size=20480; good.ResetGeneration=3; good.ResourceId=5;
 good.Width=101; good.Height=37; good.Fourcc=0x34325241;
 good.Stride=512; good.PlaneCount=1; good.LayoutFlags=1;
 for (unsigned scenario=0; scenario<16; ++scenario) {
  response=good; imports=frees=0; allocationOk=importOk=true;
  switch(scenario) {
  case 1: response.Stride=403; break;
  case 2: response.Stride=510; break;
  case 3: response.Modifier=1; break;
  case 4: response.PlaneOffset=4; break;
  case 5: response.PlaneCount=2; break;
  case 6: response.ResetGeneration=0; break;
  case 7: response.ContextId=1; break;
  case 8: response.Width=102; break;
  case 9: response.Fourcc=0x34324241; break;
  case 10: response.Size=4096; break;
  case 11: response.Size=20479; break;
  case 12: response.ExpectedResetGeneration=1; break;
  case 13: response.LayoutFlags=3; break;
  case 14: importOk=false; break;
  case 15: allocationOk=false; break;
  }
  pipe_resource *r=CreateNativeHostSurfaceTexture(&pipe,&t,good.Fourcc,&result);
  assert((r!=nullptr)==(scenario==0));
  assert(imports==unsigned(scenario==0 || scenario==14));
  assert(frees==unsigned(scenario>0 && scenario<15));
 }
 puts("PASS native host allocation/import: 16 production-code scenarios");
 assert(SharedAllocationFlags(false,false)==VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE);
 assert(SharedAllocationFlags(true,false)==0);
 assert(SharedAllocationFlags(false,true)==VIOGPU_WDDM_ALLOCATION_PRIMARY);
 assert(SharedAllocationFlags(true,true)==VIOGPU_WDDM_ALLOCATION_PRIMARY);
 for (unsigned scenario=0; scenario<26; ++scenario) {
  VIOGPU_WDDM_ALLOCATION_INFO info{};
  info.Header={VIOGPU_WDDM_ABI_MAGIC,VIOGPU_WDDM_ABI_VERSION,sizeof(info),0};
  info.Size=good.Size; info.Width=good.Width; info.Height=good.Height;
  info.Pitch=good.Stride; info.RefreshRateNumerator=60; info.RefreshRateDenominator=1;
  VIOGPU_WDDM_RESOURCE_SHARE share{};
  share.Header={VIOGPU_WDDM_ABI_MAGIC,VIOGPU_WDDM_ABI_VERSION,sizeof(share),0};
  share.ShareKey=good.ShareKey; share.Stride=good.Stride;
  share.Flags=VIOGPU_WDDM_RESOURCE_SHARE_NATIVE_SURFACE;
  const VIOGPU_WDDM_RESOURCE_SHARE *metadata=&share;
  bool valid=false;
  switch(scenario) {
  case 0: valid=true; break;
  case 1: info.Flags=VIOGPU_WDDM_ALLOCATION_PRIMARY; valid=true; break;
  case 2: info.Flags=VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE; break;
  case 3: info.Flags=VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE|VIOGPU_WDDM_ALLOCATION_PRIMARY; break;
  case 4: metadata=nullptr; break;
  case 5: share.Header.Magic=0; break;
  case 6: share.Stride+=64; break;
  case 7: share.Flags|=0x80000000; break;
  case 8: share.ShareKey=0; break;
  case 9: share.Flags=0; break;
  case 10: share.Flags=0; info.Flags=VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE; valid=true; break;
  case 11: share.Flags=0; info.Flags=VIOGPU_WDDM_ALLOCATION_PRIMARY; valid=true; break;
  case 12: metadata=nullptr; info.Flags=VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE; valid=true; break;
  case 13: metadata=nullptr; info.Flags=VIOGPU_WDDM_ALLOCATION_PRIMARY; valid=true; break;
  case 14: info.Flags=VIOGPU_WDDM_ALLOCATION_PRIMARY; info.RefreshRateNumerator=0; break;
  case 15: info.Flags=VIOGPU_WDDM_ALLOCATION_PRIMARY; info.RefreshRateDenominator=0; break;
  case 16: info.Header.Version=VIOGPU_WDDM_ABI_VERSION+1; break;
  case 17: info.Height=0; break;
  case 18: info.Width=0; break;
  case 19: info.Pitch=info.Width*4-1; break;
  case 20: info.Size=uint64_t(info.Pitch)*info.Height-1; break;
  case 21: info.Header.Size--; break;
  case 22: info.Header.Reserved=1; break;
  case 23: share.Reserved[0]=1; break;
  case 24: info.Pitch=share.Stride=510; break;
  case 25: info.Flags=VIOGPU_WDDM_ALLOCATION_NATIVE; break;
  }
  if (IsValidSharedAllocation(&info,metadata)!=valid) {
   std::fprintf(stderr,"allocation validation scenario %u failed\n",scenario);
   return 1;
  }
 }
 puts("PASS native non-CPU-visible wrapper/open: 26 production-code scenarios");
}
'''
fixture = fixture.replace('// FUNCTIONS', '\n'.join(
    extract(name) for name in ('IsValidResourceShare', 'SharedAllocationFlags',
                              'IsValidSharedAllocation', 'FreeNativeHostSurface',
                              'CreateNativeHostSurfaceTexture')))
for function, guarded_call in (('ResourceMap', 'pipe->buffer_map('),
                               ('EnsureSharedCopy', 'pfnAllocateCb')):
    body = extract(function)
    assert body.index('native_host_backing') < body.index(guarded_call)
    assert body.index('DXGI_DDI_ERR_UNSUPPORTED') < body.index(guarded_call)
opened = extract('OpenResource')
assert opened.index('IsValidSharedAllocation(') < opened.index('resource_from_handle(')
with tempfile.TemporaryDirectory(prefix='native-host-surface-') as temporary:
    out = Path(temporary)
    (out / 'test.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'freedreno/vulkan'), str(out / 'test.cpp'),
                    '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)

print("PASS D3D10 native host surface import contract")
