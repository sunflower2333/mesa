#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute production shared publication with injected fence/reset outcomes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def extract(source, name):
    start = source.index('\n' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return 'HRESULT ' + source[start:end]


parser = argparse.ArgumentParser()
parser.add_argument('--revision')
args = parser.parse_args()
here = Path(__file__).resolve().parent
source = (subprocess.check_output(
    ['git', 'show', args.revision + ':src/gallium/frontends/d3d10umd/Resource.cpp'],
    cwd=here, text=True) if args.revision else (here / 'Resource.cpp').read_text())
dxgi = (subprocess.check_output(
    ['git', 'show', args.revision + ':src/gallium/frontends/d3d10umd/DxgiFns.cpp'],
    cwd=here, text=True) if args.revision else (here / 'DxgiFns.cpp').read_text())
fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
using HRESULT = int;
constexpr HRESULT S_OK=0, E_FAIL=-1, D3DDDIERR_DEVICEREMOVED=-2, DXGI_DDI_ERR_UNSUPPORTED=-3;
#define SUCCEEDED(x) ((x)>=0)
#define FAILED(x) ((x)<0)
constexpr uint64_t OS_TIMEOUT_INFINITE=UINT64_MAX;
enum pipe_reset_status { PIPE_NO_RESET, PIPE_UNKNOWN_CONTEXT_RESET };
struct pipe_context;
struct pipe_fence_handle {};
struct pipe_screen {
 bool (*fence_finish)(pipe_screen*,pipe_context*,pipe_fence_handle*,uint64_t);
 void (*fence_reference)(pipe_screen*,pipe_fence_handle**,pipe_fence_handle*);
};
struct pipe_context {
 pipe_screen* screen;
 void (*flush)(pipe_context*,pipe_fence_handle**,unsigned);
 pipe_reset_status (*get_device_reset_status)(pipe_context*);
};
struct Resource {
 bool shared_dirty, zero_copy;
 bool zero_copy_owner=true;
 bool native_host_backing=false;
 Resource* shared_next=nullptr;
 unsigned hAllocation=1;
};
struct Device { pipe_context* pipe; Resource* shared_resources=nullptr; };
struct DXGI_DDI_ARG_RESOLVESHAREDRESOURCE { Device* hDevice; Resource* hResource; };
Device* CastDevice(Device* d) { return d; }
Resource* CastResource(Resource* r) { return r; }
#define LOG_ENTRYPOINT() ((void)0)
bool ready, lost, noFence;
unsigned releases;
unsigned transfers;
HRESULT transferResult=S_OK;
pipe_context* observed;
pipe_fence_handle handle;
void flush(pipe_context*,pipe_fence_handle** f,unsigned) { if (f) *f=noFence ? nullptr : &handle; }
bool finish(pipe_screen*,pipe_context* p,pipe_fence_handle*,uint64_t t) {
 assert(t==OS_TIMEOUT_INFINITE); observed=p; return ready;
}
void release(pipe_screen*,pipe_fence_handle** f,pipe_fence_handle*) {
 assert(*f==&handle); ++releases; *f=nullptr;
}
pipe_reset_status reset(pipe_context*) {
 return lost ? PIPE_UNKNOWN_CONTEXT_RESET : PIPE_NO_RESET;
}
HRESULT TransferSharedResource(Device*,Resource*,bool publish) {
 assert(publish); ++transfers; return transferResult;
}
HRESULT EnsureSharedCopy(Device*,Resource*) { assert(false); return E_FAIL; }
HRESULT EnsureSharedPresentContext(Device*) { return S_OK; }
HRESULT RefreshSharedResource(Device*,Resource*) { assert(false); return E_FAIL; }
// FUNCTIONS
int main() {
 pipe_screen screen{finish,release}; pipe_context pipe{&screen,flush,reset};
 Device device{&pipe}; unsigned failures=0, ownershipFailures=0, contextFailures=0;
 for (unsigned scenario=0; scenario<4; ++scenario) {
  ready=scenario&1; lost=scenario&2; releases=0; observed=nullptr;
  Resource resource{true,true};
  HRESULT hr=PublishSharedResource(&device,&resource);
  HRESULT expected=lost ? D3DDDIERR_DEVICEREMOVED : ready ? S_OK : E_FAIL;
  if (hr!=expected || resource.shared_dirty!=(expected!=S_OK)) ++ownershipFailures;
  if (observed!=&pipe) ++contextFailures;
  if (releases!=1) ++failures;
 }
 failures += ownershipFailures + contextFailures;
 std::printf("shared completion: 4 scenarios, %u failures (ownership=%u, context=%u)\n",
             failures, ownershipFailures, contextFailures);
 unsigned handoffFailures=0;
 for (unsigned scenario=0; scenario<8; ++scenario) {
  ready=scenario&1; lost=scenario&2; releases=0; observed=nullptr;
  const bool dirty=scenario&4;
  Resource resource{dirty,true};
  DXGI_DDI_ARG_RESOLVESHAREDRESOURCE args{&device,&resource};
  HRESULT hr=_ResolveSharedResource(&args);
  HRESULT expected=lost ? D3DDDIERR_DEVICEREMOVED : ready ? S_OK : E_FAIL;
  if (hr!=expected || resource.shared_dirty!=(dirty && expected!=S_OK) ||
      releases!=1 || observed!=&pipe) ++handoffFailures;
 }
 std::printf("shared read/write handoff: 8 scenarios, %u failures\n", handoffFailures);
 failures += handoffFailures;
 unsigned shadowFailures=0;
 for (unsigned route=0; route<3; ++route) {
  for (bool fail : {false,true}) {
   ready=true; lost=false; releases=transfers=0;
   transferResult=fail ? E_FAIL : S_OK;
   Resource imported{true,true,false}; device.shared_resources=&imported;
   DXGI_DDI_ARG_RESOLVESHAREDRESOURCE args{&device,&imported};
   HRESULT hr=route==0 ? PublishSharedResource(&device,&imported) :
              route==1 ? PublishSharedResources(&device) : _ResolveSharedResource(&args);
   if (hr!=transferResult || imported.shared_dirty!=fail || transfers!=1)
    ++shadowFailures;
  }
 }
 std::printf("imported writes shadow publication: 6 scenarios, %u failures\n", shadowFailures);
 failures += shadowFailures;
 unsigned nativeFailures=0;
 for (unsigned route=0; route<4; ++route) {
  for (unsigned scenario=0; scenario<8; ++scenario) {
   ready=scenario&1; lost=scenario&2; noFence=scenario&4;
   releases=transfers=0; observed=nullptr;
   Resource native{true,true,false,true}; device.shared_resources=&native;
   DXGI_DDI_ARG_RESOLVESHAREDRESOURCE args{&device,&native};
   HRESULT hr=route==0 ? PublishSharedResource(&device,&native) :
              route==1 ? PublishSharedResources(&device) :
              route==2 ? _ResolveSharedResource(&args) :
                         PreparePresentResource(&device,&native,false,true);
   HRESULT expected=lost ? D3DDDIERR_DEVICEREMOVED : ready && !noFence ? S_OK : E_FAIL;
   if (hr!=expected || native.shared_dirty!=(expected!=S_OK) || transfers!=0)
    ++nativeFailures;
  }
 }
 Resource native{true,true,false,true};
 assert(PreparePresentResource(&device,&native,false,false)==DXGI_DDI_ERR_UNSUPPORTED);
 std::printf("native host publication/present: 32 scenarios, %u failures\n", nativeFailures);
 failures += nativeFailures;
 return failures ? 1 : 0;
}
'''
fixture = fixture.replace('// FUNCTIONS', '\n'.join(
    extract(source, name) for name in ('FinishZeroCopyWrites', 'PublishSharedResource', 'PublishSharedResources', 'PreparePresentResource')) +
    (extract(source, 'ResolveSharedResourceAccess') if '\nResolveSharedResourceAccess(' in source else '') +
    extract(dxgi, '_ResolveSharedResource'))
with tempfile.TemporaryDirectory(prefix='shared-finish-') as temporary:
    out = Path(temporary)
    (out / 'test.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
    raise SystemExit(subprocess.run([str(out / 'test')]).returncode)
