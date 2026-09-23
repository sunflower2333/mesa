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

assert 'const bool nativeHostSurface = pResource->scanout_primary && NativeHostSurfaceEnabled();' in resource
assert 'DefaultEnabledUnlessDisabled("VIOGPU_NATIVE_HOST_SURFACE")' in resource
assert 'DefaultEnabledUnlessDisabled("VIOGPU_DWM_FLIP")' in resource
assert 'viogpu-native-host-surface' not in resource
assert 'viogpu-dwm-flip' not in resource

create = resource[resource.index("void APIENTRY\nCreateResource"):resource.index("SIZE_T APIENTRY\nCalcPrivateOpenedResourceSize")]
assert create.index("if (nativeHostSurface)") < create.index("CreateSharedTextureCache")
assert "native host surface allocation failed" in create
assert create.index('pfnAllocateCb(') < create.index('CreateNativeHostSurfaceTexture(')
assert 'if (!pResource->resource && !nativeHostSurface)' in create
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
#include <cstdarg>
using UINT64=uint64_t;
using ULONG_PTR=uintptr_t;
using HANDLE=void*;
using D3DKMT_HANDLE=unsigned;
using NTSTATUS=int32_t;
using DWORD=unsigned long;
using LONG=long;
struct OBJECT_ATTRIBUTES {
 unsigned Length; HANDLE RootDirectory; void *ObjectName; unsigned Attributes;
 void *SecurityDescriptor, *SecurityQualityOfService;
};
constexpr unsigned SHARED_ALLOCATION_ALL_ACCESS=0x000f0001;
constexpr unsigned FILE_APPEND_DATA=1, FILE_SHARE_READ=2, FILE_SHARE_WRITE=4;
constexpr unsigned OPEN_ALWAYS=1, FILE_ATTRIBUTE_NORMAL=1, _TRUNCATE=0;
#define NT_SUCCESS(s) ((s)>=0)
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
unsigned closedHandles, shareCalls;
bool shareOk=true;
LONG InterlockedIncrement(volatile LONG *n) { return *n+=1; }
unsigned long GetCurrentProcessId() { return 1; }
HANDLE CreateFileA(const char*,unsigned,unsigned,void*,unsigned,unsigned,void*) {
 return INVALID_HANDLE_VALUE;
}
int _snprintf_s(char*,size_t,unsigned,const char*,...) { return 0; }
bool WriteFile(HANDLE,const void*,DWORD,DWORD*,void*) { return true; }
bool CloseHandle(HANDLE h) { assert(uintptr_t(h)==42); ++closedHandles; return true; }
NTSTATUS ShareObjects(unsigned n,const D3DKMT_HANDLE *r,OBJECT_ATTRIBUTES *attr,DWORD rights,HANDLE *out) {
 assert(n==1 && *r==37 && rights==SHARED_ALLOCATION_ALL_ACCESS); ++shareCalls;
 assert(attr && attr->Length==sizeof(*attr));
 assert(!attr->RootDirectory && !attr->ObjectName && !attr->Attributes);
 assert(!attr->SecurityDescriptor && !attr->SecurityQualityOfService);
 *out=shareOk ? (HANDLE)42 : nullptr; return shareOk ? 0 : -1;
}
struct NativeSurfaceDispatch { decltype(&ShareObjects) share=ShareObjects; };
constexpr uint64_t DROIDVM_DRM_FORMAT_MOD_LINEAR=0;
constexpr uint32_t DROIDVM_NATIVE_SURFACE_LINEAR_ALIAS=1;
constexpr unsigned PIPE_BIND_SHARED=1, PIPE_BIND_LINEAR=2;
constexpr unsigned WINSYS_HANDLE_TYPE_WIN32_HANDLE=7;
constexpr unsigned WINSYS_HANDLE_TYPE_WIN32_NT_HANDLE=8;
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
unsigned resolveCalls, resolveFault;
NTSTATUS NativeSurfaceEscape(pipe_screen*,void *data,unsigned size) {
 assert(size==sizeof(VIOGPU_WDDM_NATIVE_SURFACE_RESOURCE));
 auto *r=static_cast<VIOGPU_WDDM_NATIVE_SURFACE_RESOURCE*>(data);
 assert(r->Header.Magic==VIOGPU_WDDM_ABI_MAGIC && r->Header.Size==sizeof(*r));
 assert(r->Header.Version==VIOGPU_WDDM_ABI_VERSION && !r->Header.Reserved);
 assert(r->Opcode==VIOGPU_WDDM_ESCAPE_QUERY_NATIVE_SURFACE_RESOURCE && !r->Flags);
 assert(r->AllocationHandle==12 && !r->ResourceHandle && !r->Reserved);
 assert(r->ShareKey==11 && r->Size==20480 && r->ResetGeneration==3);
 ++resolveCalls;
 r->ResourceHandle=37;
 switch(resolveFault) {
 case 1: return -1;
 case 2: r->ResourceHandle=0; break;
 case 3: ++r->Header.Magic; break;
 case 4: ++r->Header.Version; break;
 case 5: ++r->Header.Size; break;
 case 6: ++r->Header.Reserved; break;
 case 7: ++r->Opcode; break;
 case 8: ++r->Flags; break;
 case 9: ++r->AllocationHandle; break;
 case 10: ++r->ShareKey; break;
 case 11: ++r->Size; break;
 case 12: ++r->ResetGeneration; break;
 case 13: ++r->Reserved; break;
 }
 return 0;
}
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
 assert(uintptr_t(h->handle)==42 && h->type==WINSYS_HANDLE_TYPE_WIN32_NT_HANDLE);
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
  case 14: response.ShareKey=0; break;
  case 15: allocationOk=false; break;
  }
  bool ok=AllocateNativeHostSurface(&pipe,&t,good.Fourcc,&result);
  assert(ok==(scenario==0));
  assert(imports==0);
  assert(frees==unsigned(scenario>0 && scenario<14));
 }
 puts("PASS native host allocation: 16 production-code scenarios");
 for (unsigned scenario=0; scenario<16; scenario++) {
  resolveCalls=0; resolveFault=scenario;
  auto h=ResolveNativeHostSurfaceResource(&screen,scenario==14 ? 0 : 12,
                                          good.ShareKey,good.Size,scenario==15 ? 0 : 3);
  assert(h==(scenario==0 ? 37 : 0));
  assert(resolveCalls==unsigned(scenario<14));
 }
 puts("PASS native parent resolution: 16 exact identity and malformed response scenarios");
 for (unsigned scenario=0; scenario<8; scenario++) {
  response=good; imports=closedHandles=shareCalls=resolveCalls=0;
  resolveFault=scenario==5 ? 1 : scenario==7 ? 10 : 0;
  shareOk=scenario!=1; importOk=scenario!=2;
  screen.resource_from_handle=scenario==3 ? nullptr : importTexture;
  pipe_resource *r=CreateNativeHostSurfaceTexture(&pipe,&t,scenario>=4 ? 0 : 37,12,
                              2,"create",good.Size,good.Stride,good.ShareKey,scenario==6 ? 0 : 3);
  assert((r!=nullptr)==(scenario==0 || scenario==4));
  assert(resolveCalls==unsigned(scenario>=4 && scenario!=6));
  assert(shareCalls==unsigned(scenario<5));
  assert(closedHandles==unsigned(scenario==0 || scenario==2 || scenario==3 || scenario==4));
  assert(imports==unsigned(scenario==0 || scenario==2 || scenario==4));
 }
 puts("PASS native NT import: shared handle success, failure, unavailable import and lifetime");
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
                              'AllocateNativeHostSurface', 'ResolveNativeHostSurfaceResource',
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
