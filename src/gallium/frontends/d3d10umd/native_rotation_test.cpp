// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
using UINT = unsigned;
using HRESULT = int;
constexpr HRESULT S_OK=0, E_INVALIDARG=-1, E_OUTOFMEMORY=-2;
constexpr unsigned PIPE_MAX_COLOR_BUFS=4, PIPE_MAX_SHADER_SAMPLER_VIEWS=8, MESA_SHADER_STAGES=3;
enum mesa_shader_stage { VS, PS, GS };
struct pipe_resource { int refs=1; unsigned id; };
struct pipe_surface { pipe_resource *texture=nullptr; };
struct pipe_sampler_view { pipe_resource *texture; unsigned format; };
struct Resource {
   pipe_resource *resource=nullptr;
   unsigned hAllocation=0, hKMResource=0;
   bool shared_dirty=false, allocation_lockable=false, allocation_resident=false;
   bool zero_copy=false, zero_copy_owner=false, zero_copy_local=false;
   uint64_t zero_copy_key=0;
   void *so_target=nullptr;
   void **transfers=nullptr;
   unsigned NumSubResources=1;
   uintptr_t hRTResourceHandle=0, shared_staging_allocation=0;
   Resource *shared_next=nullptr;
};
using DXGI_DDI_HRESOURCE = Resource *;
Resource *CastResource(Resource *r) { return r; }
struct ResourceView {
   ResourceView *next;
   Resource *owner;
   pipe_surface *surface;
   pipe_sampler_view **sampler;
};
struct pipe_framebuffer_state { pipe_surface cbufs[PIPE_MAX_COLOR_BUFS], zsbuf; };
struct pipe_context {
   pipe_sampler_view *(*create_sampler_view)(pipe_context *, pipe_resource *, const pipe_sampler_view *);
   void (*sampler_view_release)(pipe_context *, pipe_sampler_view *);
   void (*set_framebuffer_state)(pipe_context *, const pipe_framebuffer_state *);
   void (*set_sampler_views)(pipe_context *, mesa_shader_stage, unsigned, unsigned, unsigned, pipe_sampler_view **);
};
struct Device {
   pipe_context *pipe;
   ResourceView *resource_views=nullptr;
   ResourceView *fb_views[PIPE_MAX_COLOR_BUFS]={}, *zs_view=nullptr;
   ResourceView *sampler_resource_views[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS]={};
   pipe_sampler_view *sampler_views[MESA_SHADER_STAGES][PIPE_MAX_SHADER_SAMPLER_VIEWS]={};
   pipe_framebuffer_state fb;
};
void pipe_resource_reference(pipe_resource **dst, pipe_resource *src) {
   if (src) { assert(src->refs>0); ++src->refs; }
   if (*dst) { assert((*dst)->refs>0); --(*dst)->refs; }
   *dst=src;
}
int failAt=-1, creates=0, liveSamplers=0, framebufferCalls=0, samplerCalls=0;
pipe_sampler_view *create(pipe_context *, pipe_resource *r, const pipe_sampler_view *desc) {
   if (++creates==failAt) return nullptr;
   auto *v=new pipe_sampler_view{nullptr,desc->format};
   pipe_resource_reference(&v->texture,r); ++liveSamplers; return v;
}
void release(pipe_context *, pipe_sampler_view *v) {
   pipe_resource_reference(&v->texture,nullptr); delete v; --liveSamplers;
}
void framebuffer(pipe_context *, const pipe_framebuffer_state *fb) {
   ++framebufferCalls; assert(fb->cbufs[0].texture && fb->zsbuf.texture);
}
void samplers(pipe_context *, mesa_shader_stage, unsigned, unsigned count, unsigned, pipe_sampler_view **views) {
   ++samplerCalls; assert(count==PIPE_MAX_SHADER_SAMPLER_VIEWS);
   for (unsigned i=0;i<count;++i) if (views[i]) assert(views[i]->texture->refs>0);
}

// PRODUCTION

void run(int injectedFailure) {
   failAt=-1; creates=framebufferCalls=samplerCalls=0;
   pipe_context pipe{create,release,framebuffer,samplers};
   Device device{}; device.pipe=&pipe;
   pipe_resource textures[3]={{1,0},{1,1},{1,2}};
   Resource resources[3]; Resource *handles[3];
   for (unsigned i=0;i<3;++i) {
      Resource &r=resources[i]; handles[i]=&r; r.resource=&textures[i];
      r.hAllocation=100+i; r.hKMResource=200+i; r.zero_copy_key=300+i;
      r.zero_copy=true; r.zero_copy_owner=i!=1; r.zero_copy_local=i==1;
      r.shared_dirty=i==2; r.allocation_lockable=i==1; r.allocation_resident=true;
      r.hRTResourceHandle=400+i; r.shared_staging_allocation=500+i;
      r.shared_next=i<2 ? &resources[i+1] : nullptr;
   }
   Resource alias{}; alias.resource=&textures[0]; // Different runtime object, same texture.
   pipe_surface surfaces[4]; ResourceView surfaceViews[4]{};
   pipe_sampler_view *views[4]; ResourceView samplerViews[4]{};
   pipe_sampler_view desc{nullptr,77};
   for (unsigned i=0;i<4;++i) {
      Resource *owner=i<3?&resources[i]:&alias;
      pipe_resource_reference(&surfaces[i].texture,owner->resource);
      RegisterResourceView(&device,&surfaceViews[i],owner,&surfaces[i],nullptr);
      views[i]=create(&pipe,owner->resource,&desc);
      RegisterResourceView(&device,&samplerViews[i],owner,nullptr,&views[i]);
   }
   device.fb_views[0]=&surfaceViews[0]; device.zs_view=&surfaceViews[1];
   pipe_resource_reference(&device.fb.cbufs[0].texture,surfaces[0].texture);
   pipe_resource_reference(&device.fb.zsbuf.texture,surfaces[1].texture);
   device.sampler_views[PS][2]=views[0]; device.sampler_resource_views[PS][2]=&samplerViews[0];
   device.sampler_views[VS][4]=views[1]; device.sampler_resource_views[VS][4]=&samplerViews[1];
   device.sampler_views[PS][3]=views[3]; device.sampler_resource_views[PS][3]=&samplerViews[3];
   creates=0; failAt=injectedFailure;
   Resource before[3]; memcpy(before,resources,sizeof before);
   auto result=RotateNativeResourceIdentities(&device,3,handles);
   const bool failed=injectedFailure>0;
   assert(result==(failed?E_OUTOFMEMORY:S_OK));
   if (failed) {
      assert(memcmp(before,resources,sizeof before)==0);
      assert(framebufferCalls==0 && samplerCalls==0);
   } else {
      assert(framebufferCalls==1 && samplerCalls==2);
   }
   for (unsigned i=0;i<3;++i) {
      unsigned n=failed?i:(i+1)%3;
      const Resource &r=resources[i];
      assert(r.resource==&textures[n] && r.hAllocation==100+n && r.hKMResource==200+n);
      assert(r.zero_copy_key==300+n && r.zero_copy_owner==(n!=1) && r.zero_copy_local==(n==1));
      assert(r.shared_dirty==(n==2) && r.allocation_lockable==(n==1) && r.allocation_resident);
      assert(r.hRTResourceHandle==400+i && r.shared_staging_allocation==500+i);
      assert(r.shared_next==before[i].shared_next);
      assert(surfaces[i].texture==&textures[n] && views[i]->texture==&textures[n]);
      assert(views[i]->format==77);
   }
   assert(surfaces[3].texture==&textures[0] && views[3]->texture==&textures[0]);
   assert(device.sampler_views[PS][2]==views[0] && device.sampler_views[VS][4]==views[1]);
   assert(device.sampler_views[PS][3]==views[3]);
   assert(device.fb.cbufs[0].texture==surfaces[0].texture && device.fb.zsbuf.texture==surfaces[1].texture);
   assert(liveSamplers==4);
   failAt=-1;
   if (!failed) {
      for (unsigned i=0;i<2;++i) assert(RotateNativeResourceIdentities(&device,3,handles)==S_OK);
      for (unsigned i=0;i<3;++i) assert(resources[i].resource==&textures[i]);
   }
   Resource *duplicate[3]={&resources[0],&resources[1],&resources[0]};
   assert(RotateNativeResourceIdentities(&device,3,duplicate)==E_INVALIDARG);
   void *mapping=&resources[0]; resources[0].transfers=&mapping;
   assert(RotateNativeResourceIdentities(&device,3,handles)==E_INVALIDARG);
   resources[0].transfers=nullptr;
   for (unsigned i=0;i<4;++i) {
      UnregisterResourceView(&device,&surfaceViews[i]);
      UnregisterResourceView(&device,&samplerViews[i]);
      pipe_resource_reference(&surfaces[i].texture,nullptr); release(&pipe,views[i]);
   }
   assert(!device.resource_views && !device.fb_views[0] && !device.zs_view);
   assert(!device.sampler_resource_views[PS][2]);
   pipe_resource_reference(&device.fb.cbufs[0].texture,nullptr);
   pipe_resource_reference(&device.fb.zsbuf.texture,nullptr);
   assert(!liveSamplers);
   for (auto &texture:textures) assert(texture.refs==1);
}
int main() {
   run(-1);
   for (int i=1;i<=3;++i) run(i);
   puts("PASS native 3-buffer rotation: identity, bound/unbound views, aliases, dirty/residency, full cycle, atomic failures, lifetimes");
}
