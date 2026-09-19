// SPDX-License-Identifier: MIT
#include "State.h"
#include "Resource.h"
#include <stdlib.h>

void
RegisterResourceView(Device *device, ResourceView *view, Resource *owner,
                     struct pipe_surface *surface, struct pipe_sampler_view **sampler)
{
   view->owner = owner;
   view->surface = surface;
   view->sampler = sampler;
   view->next = device->resource_views;
   device->resource_views = view;
}

void
UnregisterResourceView(Device *device, ResourceView *view)
{
   for (ResourceView **p = &device->resource_views; *p; p = &(*p)->next) {
      if (*p == view) {
         *p = view->next;
         break;
      }
   }
   for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i)
      if (device->fb_views[i] == view)
         device->fb_views[i] = NULL;
   if (device->zs_view == view)
      device->zs_view = NULL;
   for (unsigned s = 0; s < MESA_SHADER_STAGES; ++s)
      for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++slot)
         if (device->sampler_resource_views[s][slot] == view)
            device->sampler_resource_views[s][slot] = NULL;
}

static Resource *
NextIdentity(Resource *owner, UINT count, const DXGI_DDI_HRESOURCE *resources)
{
   for (UINT i = 0; i < count; ++i)
      if (CastResource(resources[i]) == owner)
         return CastResource(resources[(i + 1) % count]);
   return NULL;
}

struct PreparedRotationView {
   PreparedRotationView *next;
   ResourceView *view;
   struct pipe_sampler_view *sampler;
};

/* Rotate physical identity without touching runtime handles or copying pixels.
 * Views point at runtime objects, not at physical allocations. Prepare every
 * fallible sampler creation before changing any live state. */
HRESULT
RotateNativeResourceIdentities(Device *device, UINT count, const DXGI_DDI_HRESOURCE *resources)
{
   struct pipe_context *pipe = device->pipe;
   if (count < 2 || !resources)
      return E_INVALIDARG;
   for (UINT i = 0; i < count; ++i) {
      Resource *r = CastResource(resources[i]);
      if (!r || !r->resource || !r->hAllocation || r->so_target)
         return E_INVALIDARG;
      for (UINT j = 0; j < i; ++j)
         if (CastResource(resources[j]) == r)
            return E_INVALIDARG;
      for (UINT j = 0; r->transfers && j < r->NumSubResources; ++j)
         if (r->transfers[j])
            return E_INVALIDARG;
   }

   PreparedRotationView *prepared = NULL;
   for (ResourceView *view = device->resource_views; view; view = view->next) {
      Resource *next = NextIdentity(view->owner, count, resources);
      if (!next || !view->sampler)
         continue;
      PreparedRotationView *item = (PreparedRotationView *)calloc(1, sizeof(*item));
      if (!item)
         goto failed;
      item->next = prepared;
      item->view = view;
      prepared = item;
      item->sampler = pipe->create_sampler_view(pipe, next->resource, *view->sampler);
      if (!item->sampler)
         goto failed;
   }

   // Surfaces require no allocation. Update by owner, not texture address:
   // another opened Resource can alias the same texture without rotating.
   for (ResourceView *view = device->resource_views; view; view = view->next) {
      Resource *next = NextIdentity(view->owner, count, resources);
      if (next && view->surface)
         pipe_resource_reference(&view->surface->texture, next->resource);
   }
   {
      bool stages[MESA_SHADER_STAGES] = {};
      while (prepared) {
         PreparedRotationView *item = prepared;
         prepared = item->next;
         struct pipe_sampler_view *old = *item->view->sampler;
         for (unsigned s = 0; s < MESA_SHADER_STAGES; ++s)
            for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++slot)
               if (device->sampler_resource_views[s][slot] == item->view) {
                  device->sampler_views[s][slot] = item->sampler;
                  stages[s] = true;
               }
         *item->view->sampler = item->sampler;
         pipe->sampler_view_release(pipe, old);
         free(item);
      }

      // Move only physical ownership. Runtime handle, intrusive lists and
      // reusable staging buffers remain attached to their Resource objects.
      Resource first = *CastResource(resources[0]);
      for (UINT i = 0; i < count; ++i) {
         Resource *dst = CastResource(resources[i]);
         const Resource *src = i + 1 < count ? CastResource(resources[i + 1]) : &first;
         dst->resource = src->resource;
         dst->hAllocation = src->hAllocation;
         dst->hKMResource = src->hKMResource;
         dst->shared_dirty = src->shared_dirty;
         dst->allocation_lockable = src->allocation_lockable;
         dst->allocation_resident = src->allocation_resident;
         dst->zero_copy = src->zero_copy;
         dst->zero_copy_owner = src->zero_copy_owner;
         dst->zero_copy_local = src->zero_copy_local;
         dst->zero_copy_key = src->zero_copy_key;
      }

      bool framebufferChanged = false;
      for (unsigned i = 0; i < PIPE_MAX_COLOR_BUFS; ++i) {
         ResourceView *view = device->fb_views[i];
         if (view && NextIdentity(view->owner, count, resources)) {
            pipe_resource_reference(&device->fb.cbufs[i].texture, view->surface->texture);
            framebufferChanged = true;
         }
      }
      ResourceView *view = device->zs_view;
      if (view && NextIdentity(view->owner, count, resources)) {
         pipe_resource_reference(&device->fb.zsbuf.texture, view->surface->texture);
         framebufferChanged = true;
      }
      if (framebufferChanged)
         pipe->set_framebuffer_state(pipe, &device->fb);
      for (unsigned s = 0; s < MESA_SHADER_STAGES; ++s)
         if (stages[s])
            pipe->set_sampler_views(pipe, (mesa_shader_stage)s, 0,
                                    PIPE_MAX_SHADER_SAMPLER_VIEWS, 0, device->sampler_views[s]);
   }
   return S_OK;

failed:
   while (prepared) {
      PreparedRotationView *item = prepared;
      prepared = item->next;
      if (item->sampler)
         pipe->sampler_view_release(pipe, item->sampler);
      free(item);
   }
   return E_OUTOFMEMORY;
}
