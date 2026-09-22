#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the native rotation implementation with refcounted Gallium mocks."""
from pathlib import Path
import subprocess
import tempfile


def function(source, name, result):
    start = source.index('\n' + name + '(')
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return result + ' ' + source[start:end]


here = Path(__file__).resolve().parent
dxgi = (here / 'DxgiFns.cpp').read_text()
state = (here / 'State.h').read_text()
shader = (here / 'Shader.cpp').read_text()
output = (here / 'OutputMerger.cpp').read_text()
definitions = []
for name in ('Resource', 'RenderTargetView', 'ShaderResourceView'):
    start = state.index('struct ' + name + '\n{')
    definitions.append(state[start:state.index('\n};', start) + 3])

fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using UINT = unsigned;
using UINT64 = uint64_t;
using D3DKMT_HANDLE = unsigned;
using HANDLE = void *;
using DXGI_FORMAT = unsigned;
using HRESULT = int;
constexpr HRESULT S_OK=0, E_INVALIDARG=-1, E_OUTOFMEMORY=-2, DXGI_DDI_ERR_UNSUPPORTED=-3;
constexpr unsigned PIPE_TEXTURE_2D=2, MESA_SHADER_STAGES=6, PIPE_MAX_SHADER_SAMPLER_VIEWS=8;
enum mesa_shader_stage { MESA_SHADER_VERTEX, MESA_SHADER_FRAGMENT };
struct pipe_resource {
 unsigned target=2, format=3, width0=32, height0=24, depth0=1, array_size=1;
 unsigned last_level=0, nr_samples=0, nr_storage_samples=0, nr_sparse_levels=0;
 unsigned compression_rate=0, usage=0, bind=3, flags=0;
 int refs=1;
};
struct pipe_transfer {};
struct pipe_stream_output_target {};
struct pipe_surface { pipe_resource *texture; unsigned format, level, first_layer, last_layer; };
struct pipe_sampler_view { pipe_resource *texture; unsigned format, swizzle; int refs; };
struct Resource;
struct RenderTargetView;
struct ShaderResourceView;
using D3D10DDI_HRTRENDERTARGETVIEW = unsigned;
// DEFINITIONS
using DXGI_DDI_HRESOURCE = Resource *;
using D3D10DDI_HRENDERTARGETVIEW = RenderTargetView *;
using D3D10DDI_HSHADERRESOURCEVIEW = ShaderResourceView *;
struct pipe_framebuffer_state { unsigned nr_cbufs; pipe_surface cbufs[4]; };
struct pipe_context {
 pipe_sampler_view *(*create_sampler_view)(pipe_context *, pipe_resource *, const pipe_sampler_view *);
 void (*sampler_view_release)(pipe_context *, pipe_sampler_view *);
 void (*set_framebuffer_state)(pipe_context *, const pipe_framebuffer_state *);
 void (*set_sampler_views)(pipe_context *, mesa_shader_stage, unsigned, unsigned, unsigned, pipe_sampler_view **);
};
struct Device {
 pipe_context *pipe;
 pipe_framebuffer_state fb;
 pipe_sampler_view *sampler_views[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS];
};
using D3D10DDI_HDEVICE = Device *;
Resource *CastResource(Resource *r) { return r; }
RenderTargetView *CastRenderTargetView(RenderTargetView *v) { return v; }
ShaderResourceView *CastShaderResourceView(ShaderResourceView *v) { return v; }
Device *CastDevice(Device *d) { return d; }
#define LOG_ENTRYPOINT() ((void)0)
void pipe_resource_reference(pipe_resource **out, pipe_resource *in) {
 if (in) ++in->refs;
 if (*out) { assert((*out)->refs>0); --(*out)->refs; }
 *out=in;
}
unsigned calls, failAt, framebufferCalls, samplerCalls, liveViews;
bool failArray;
pipe_sampler_view *driverViews[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS];
pipe_framebuffer_state driverFb{};
void release(pipe_context *, pipe_sampler_view *view) {
 assert(view && view->refs>0);
 if (--view->refs) return;
 pipe_resource_reference(&view->texture, nullptr);
 --liveViews;
 delete view;
}
pipe_sampler_view *create(pipe_context *, pipe_resource *image, const pipe_sampler_view *desc) {
 if (++calls==failAt) return nullptr;
 auto *view=new pipe_sampler_view{nullptr,desc->format,desc->swizzle,1};
 pipe_resource_reference(&view->texture,image);
 ++liveViews;
 return view;
}
void bindFramebuffer(pipe_context *, const pipe_framebuffer_state *fb) {
 ++framebufferCalls;
 for (unsigned i=0; i<4; ++i) {
  pipe_resource_reference(&driverFb.cbufs[i].texture,fb->cbufs[i].texture);
  driverFb.cbufs[i]=fb->cbufs[i];
 }
 driverFb.nr_cbufs=fb->nr_cbufs;
}
void bindSamplers(pipe_context *pipe, mesa_shader_stage stage, unsigned first,
                  unsigned count, unsigned unbind, pipe_sampler_view **views) {
 assert(first==0 && count==PIPE_MAX_SHADER_SAMPLER_VIEWS && unbind==0);
 ++samplerCalls;
 for (unsigned i=0; i<count; ++i) {
  if (views[i]) ++views[i]->refs;
  if (driverViews[stage][i]) release(pipe,driverViews[stage][i]);
  driverViews[stage][i]=views[i];
 }
}
void *test_calloc(size_t n, size_t size) { return failArray ? nullptr : calloc(n,size); }
#define calloc test_calloc
// FUNCTIONS
#undef calloc

struct Scenario {
 pipe_context pipe{create,release,bindFramebuffer,bindSamplers};
 Device device{};
 Resource resources[3]{};
 Resource *handles[3]{};
 pipe_resource images[3];
 pipe_transfer *transfers[3]{};
 RenderTargetView rtvs[3][2]{};
 ShaderResourceView srvs[3][2]{};
 pipe_sampler_view *pending[3]{};
 unsigned count;
 Scenario(unsigned n, bool views=true, bool opened=false): count(n) {
  assert(liveViews==0);
  calls=failAt=framebufferCalls=samplerCalls=0; failArray=false;
  device.pipe=&pipe;
  for (unsigned i=0; i<n; ++i) {
   Resource &r=resources[i]; handles[i]=&r;
   r.resource=&images[i]; r.MipLevels=r.NumSubResources=1;
   r.hAllocation=10+i; r.hKMResource=20+i;
   r.hRTResourceHandle=opened ? nullptr : reinterpret_cast<void *>(uintptr_t(30+i));
   r.shared_next=i+1<n ? &resources[i+1] : nullptr;
   r.Format=5; r.shared_pitch=128; r.transfers=&transfers[i];
   r.native_host_backing=r.zero_copy=true; r.zero_copy_key=100+i;
   r.native_host_surface_live=!opened; memset(r.native_host_surface,i+1,128);
   r.shared_dirty=i%2; r.allocation_resident=i!=1; r.allocation_lockable=i==1;
   if (!views) continue;
   for (unsigned j=0; j<2; ++j) {
    auto &rt=rtvs[i][j]; rt.owner=&r; rt.next=r.render_target_views;
    r.render_target_views=&rt;
    pipe_resource_reference(&rt.surface.texture,r.resource); rt.surface.format=77+j;
    auto &sr=srvs[i][j]; sr.owner=&r; sr.next=r.shader_resource_views;
    r.shader_resource_views=&sr;
    pipe_sampler_view desc{nullptr,88+j,99+j,0};
    sr.handle=create(&pipe,r.resource,&desc);
   }
   pending[i]=srvs[i][0].handle; ++pending[i]->refs;
   pipe_resource_reference(&device.fb.cbufs[i].texture,r.resource);
   device.fb.cbufs[i].format=77;
   for (unsigned stage=0; stage<MESA_SHADER_STAGES; ++stage)
    device.sampler_views[stage][i]=srvs[i][0].handle;
  }
  if (views) {
   device.fb.nr_cbufs=n; bindFramebuffer(&pipe,&device.fb);
   for (unsigned stage=0; stage<MESA_SHADER_STAGES; ++stage)
    bindSamplers(&pipe,static_cast<mesa_shader_stage>(stage),0,PIPE_MAX_SHADER_SAMPLER_VIEWS,0,
                 device.sampler_views[stage]);
  }
  calls=framebufferCalls=samplerCalls=0;
 }
 void verify(unsigned rotations, bool views=true, bool opened=false) {
  for (unsigned i=0; i<count; ++i) {
   unsigned identity=(i+rotations)%count;
   Resource &r=resources[i];
   assert(r.resource==&images[identity] && r.hAllocation==10+identity && r.hKMResource==20+identity);
   assert(r.zero_copy_key==100+identity && r.native_host_surface_live==!opened);
   for (unsigned j=0; j<128; ++j) assert(r.native_host_surface[j]==identity+1);
   assert(r.shared_dirty==bool(identity%2) && r.allocation_resident==(identity!=1));
   assert(r.allocation_lockable==(identity==1));
   assert(r.hRTResourceHandle==(opened ? nullptr : reinterpret_cast<void *>(uintptr_t(30+i))));
   assert(r.transfers==&transfers[i] && r.shared_next==(i+1<count ? &resources[i+1] : nullptr));
   if (!views) continue;
   assert(r.render_target_views==&rtvs[i][1] && r.shader_resource_views==&srvs[i][1]);
   for (unsigned j=0; j<2; ++j) {
    assert(rtvs[i][j].owner==&r && rtvs[i][j].surface.texture==r.resource);
    assert(rtvs[i][j].surface.format==77+j);
    assert(srvs[i][j].owner==&r && srvs[i][j].handle->texture==r.resource);
    assert(srvs[i][j].handle->format==88+j && srvs[i][j].handle->swizzle==99+j);
   }
   assert(device.fb.cbufs[i].texture==r.resource && driverFb.cbufs[i].texture==r.resource);
   for (unsigned stage=0; stage<MESA_SHADER_STAGES; ++stage) {
    assert(device.sampler_views[stage][i]==srvs[i][0].handle);
    assert(driverViews[stage][i]==srvs[i][0].handle);
   }
   assert(pending[i]->texture==&images[i]); // Already submitted batches keep their identity.
  }
 }
 ~Scenario() {
  for (unsigned stage=0; stage<MESA_SHADER_STAGES; ++stage)
   for (unsigned i=0; i<PIPE_MAX_SHADER_SAMPLER_VIEWS; ++i)
    if (driverViews[stage][i]) { release(&pipe,driverViews[stage][i]); driverViews[stage][i]=nullptr; }
  for (unsigned i=0; i<count; ++i) {
   pipe_resource_reference(&driverFb.cbufs[i].texture,nullptr);
   pipe_resource_reference(&device.fb.cbufs[i].texture,nullptr);
   for (unsigned j=0; j<2; ++j) {
    DestroyRenderTargetView(&device,&rtvs[i][j]);
    DestroyShaderResourceView(&device,&srvs[i][j]);
   }
   assert(!resources[i].render_target_views && !resources[i].shader_resource_views);
   if (pending[i]) release(&pipe,pending[i]);
  }
  // Resource owning references were permuted, never allocated or destroyed.
  for (unsigned i=0; i<count; ++i) assert(images[i].refs==1);
  assert(liveViews==0);
 }
};
int main() {
 unsigned scenarios=0;
 for (unsigned n : {2u,3u}) for (bool opened : {false,true}) {
  Scenario s(n,true,opened);
  for (unsigned turn=1; turn<=n*4; ++turn) {
   assert(RotateNativeResourceIdentities(&s.device,n,s.handles)==S_OK);
   s.verify(turn,true,opened); ++scenarios;
  }
 }
 for (unsigned failure=0; failure<=6; ++failure) {
  Scenario s(3); Resource before[3]; memcpy(before,s.resources,sizeof before);
  pipe_sampler_view *old[3][2]; int refs[3];
  for (unsigned i=0; i<3; ++i) {
   refs[i]=s.images[i].refs;
   for (unsigned j=0; j<2; ++j) old[i][j]=s.srvs[i][j].handle;
  }
  failArray=failure==0; failAt=failure;
  assert(RotateNativeResourceIdentities(&s.device,3,s.handles)==E_OUTOFMEMORY);
  assert(memcmp(before,s.resources,sizeof before)==0);
  assert(framebufferCalls==0 && samplerCalls==0 && liveViews==6);
  for (unsigned i=0; i<3; ++i) {
   assert(refs[i]==s.images[i].refs);
   for (unsigned j=0; j<2; ++j) assert(old[i][j]==s.srvs[i][j].handle);
  }
  s.verify(0); ++scenarios;
 }
 for (unsigned invalid=0; invalid<14; ++invalid) {
  Scenario s(3); Resource saved=s.resources[1]; auto &r=s.resources[1];
  auto *oldView=s.srvs[1][1].handle;
  switch (invalid) {
   case 0: s.handles[1]=s.handles[0]; break;
   case 1: r.zero_copy_key=s.resources[0].zero_copy_key; break;
   case 2: r.native_host_backing=false; break;
   case 3: s.transfers[1]=reinterpret_cast<pipe_transfer *>(uintptr_t(1)); break;
   case 4: r.shared_pitch++; break;
   case 5: r.hAllocation=0; break;
   case 6: r.resource=s.images; break;
   case 7: r.zero_copy_owner=true; break;
   case 8: r.zero_copy_local=true; break;
   case 9: r.shared_staging_allocation=17; break;
   case 10: r.hRTResourceHandle=nullptr; break;
   case 11: r.Format++; break;
   case 12: s.srvs[1][1].handle=nullptr; break;
   case 13: s.handles[1]=nullptr; break;
  }
  Resource before[3]; memcpy(before,s.resources,sizeof before);
  assert(RotateNativeResourceIdentities(&s.device,3,s.handles)<0);
  assert(memcmp(before,s.resources,sizeof before)==0 && calls==0);
  assert(framebufferCalls==0 && samplerCalls==0);
  r=saved; s.handles[1]=&r; s.transfers[1]=nullptr; s.srvs[1][1].handle=oldView;
  s.verify(0); ++scenarios;
 }
 {
  Scenario s(3,false); failArray=true;
  assert(RotateNativeResourceIdentities(&s.device,3,s.handles)==S_OK);
  assert(calls==0 && framebufferCalls==0 && samplerCalls==0);
  s.verify(1,false); ++scenarios;
 }
 std::printf("native rotation: %u production scenarios passed\n",scenarios);
}
'''
functions = '\n'.join(function(dxgi, name, result) for name, result in (
    ('RotationScratchMatches', 'bool'), ('AssignNativeBacking', 'void'),
    ('RotateNativeResourceIdentities', 'HRESULT')))
functions += function(output, 'DestroyRenderTargetView', 'void')
functions += function(shader, 'DestroyShaderResourceView', 'void')
fixture = fixture.replace('// DEFINITIONS', '\n'.join(definitions)).replace('// FUNCTIONS', functions)

# Both DDI SRV creators and RTV creation must attach views to the runtime object.
for name in ('CreateShaderResourceView', 'CreateShaderResourceView1'):
    creator = function(shader, name, 'void')
    assert 'pSRView->owner->shader_resource_views = pSRView;' in creator
assert 'pRTView->owner->render_target_views = pRTView;' in function(output, 'CreateRenderTargetView', 'void')
dispatch = function(dxgi, '_RotateResourceIdentities', 'HRESULT')
assert dispatch.index('RotateNativeResourceIdentities(') < dispatch.index('resource_copy_region(')

with tempfile.TemporaryDirectory(prefix='native-rotation-') as temporary:
    out = Path(temporary)
    (out / 'test.cpp').write_text(fixture)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(out / 'test.cpp'),
                    '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
