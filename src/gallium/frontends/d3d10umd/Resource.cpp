/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

/*
 * Resource.cpp --
 *    Functions that manipulate GPU resources.
 */


#include "Resource.h"
#include "tu_wddm_abi.h"
#include "Format.h"
#include "State.h"
#include "Query.h"

#include "Debug.h"
#include <limits.h>

#include "util/u_math.h"
#include "util/u_rect.h"
#include "util/u_surface.h"

static VIOGPU_WDDM_UINT32
SharedPrivateFormat(DXGI_FORMAT format)
{
   switch (format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM:
      return VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM;
   case DXGI_FORMAT_R8G8B8A8_UNORM:
      return VIOGPU_WDDM_FORMAT_R8G8B8A8_UNORM;
   default:
      return VIOGPU_WDDM_FORMAT_NONE;
   }
}

static DXGI_FORMAT
SharedDxgiFormat(VIOGPU_WDDM_UINT32 format)
{
   switch (format) {
   case VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
   case VIOGPU_WDDM_FORMAT_R8G8B8A8_UNORM:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
   default:
      return DXGI_FORMAT_UNKNOWN;
   }
}

static struct pipe_resource *
CreateSharedTextureCache(struct pipe_screen *screen, const struct pipe_resource *templat)
{
   // These caches cross the CPU-visible WDDM backing on every ownership
   // transition. On UMA a linear image can be mapped after its GPU usage
   // retires, avoiding the extra tiled-image-to-staging copy. The normal
   // synchronized map and kernel LockCb still govern visibility and access.
   struct pipe_resource linear = *templat;
   linear.bind |= PIPE_BIND_LINEAR;
   struct pipe_resource *resource = screen->resource_create(screen, &linear);
   if (resource)
      return resource;
   // A backend may not support the requested render/sample usage with linear
   // tiling. Its existing tiled cache remains a valid, synchronized fallback.
   return screen->resource_create(screen, templat);
}

static void
LogSharedCopyFailure(const char *stage, HRESULT hr)
{
   if (SUCCEEDED(hr))
      return;
   HANDLE log = CreateFileA("C:\\Users\\Public\\umd_shared.log", FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
   if (log != INVALID_HANDLE_VALUE) {
      char line[128];
      int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                          "pid=%lu shared-copy stage=%s hr=0x%08lx\r\n",
                          GetCurrentProcessId(), stage, (unsigned long)hr);
      DWORD written;
      if (n > 0)
         WriteFile(log, line, (DWORD)n, &written, NULL);
      CloseHandle(log);
   }
}

static HRESULT
EnsureSharedCopy(Device *device, Resource *resource)
{
   if (!device->KTCallbacks.pfnCreateContextCb || !device->KTCallbacks.pfnDestroyContextCb ||
       !device->KTCallbacks.pfnAllocateCb || !device->KTCallbacks.pfnDeallocateCb ||
       !device->KTCallbacks.pfnRenderCb || !device->KTCallbacks.pfnLockCb ||
       !device->KTCallbacks.pfnUnlockCb)
      return E_NOTIMPL;

   if (!device->shared_copy_context.hContext) {
      D3DDDICB_CREATECONTEXT create = {};
      create.EngineAffinity = 1;
      HRESULT hr = device->KTCallbacks.pfnCreateContextCb(device->hDevice, &create);
      LogSharedCopyFailure("create-context", hr);
      if (FAILED(hr))
         return hr;
      device->shared_copy_context = create;
   }
   if (!resource->allocation_lockable && !resource->shared_staging_allocation) {
      VIOGPU_WDDM_ALLOCATION_INFO info = {};
      info.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      info.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      info.Header.Size = sizeof(info);
      info.Width = resource->resource->width0;
      info.Height = resource->resource->height0;
      info.Pitch = resource->shared_pitch;
      info.Size = (VIOGPU_WDDM_UINT64)info.Pitch * info.Height;
      info.Alignment = 4096;
      info.Flags = VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
      info.Format = SharedPrivateFormat(resource->Format);
      D3DDDI_ALLOCATIONINFO allocation = {};
      allocation.pPrivateDriverData = &info;
      allocation.PrivateDriverDataSize = sizeof(info);
      D3DDDICB_ALLOCATE allocate = {};
      allocate.NumAllocations = 1;
      allocate.pAllocationInfo = &allocation;
      // Private staging belongs to this device, not to the shared resource.
      HRESULT hr = device->KTCallbacks.pfnAllocateCb(device->hDevice, &allocate);
      LogSharedCopyFailure("allocate-staging", hr);
      if (FAILED(hr))
         return hr;
      if (!allocation.hAllocation)
         return E_FAIL;
      resource->shared_staging_allocation = allocation.hAllocation;
   }
   return S_OK;
}

static HRESULT
SubmitSharedCopy(Device *device, Resource *resource, bool publish)
{
   D3DDDICB_CREATECONTEXT *context = &device->shared_copy_context;
   if (!context->hContext || !context->pCommandBuffer ||
       context->CommandBufferSize < sizeof(VIOGPU_WDDM_ALLOCATION_COPY) ||
       !context->pAllocationList || context->AllocationListSize < 2 ||
       !context->pPatchLocationList || context->PatchLocationListSize < 2)
      return E_FAIL;

   VIOGPU_WDDM_ALLOCATION_COPY copy = {};
   copy.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
   copy.Header.Version = VIOGPU_WDDM_ABI_VERSION;
   copy.Header.Size = sizeof(copy);
   copy.Opcode = VIOGPU_WDDM_RENDER_ALLOCATION_COPY;
   copy.Width = resource->resource->width0;
   copy.Height = resource->resource->height0;
   memcpy(context->pCommandBuffer, &copy, sizeof(copy));
   memset(context->pAllocationList, 0, 2 * sizeof(*context->pAllocationList));
   context->pAllocationList[0].hAllocation = publish ? resource->shared_staging_allocation : resource->hAllocation;
   context->pAllocationList[1].hAllocation = publish ? resource->hAllocation : resource->shared_staging_allocation;
   context->pAllocationList[1].WriteOperation = 1;
   D3DDDICB_RENDER render = {};
   render.hContext = context->hContext;
   render.CommandLength = sizeof(copy);
   render.NumAllocations = 2;
   // The KMD creates the two relocations in its own DMA packet.
   render.NewCommandBufferSize = context->CommandBufferSize;
   render.NewAllocationListSize = context->AllocationListSize;
   render.NewPatchLocationListSize = context->PatchLocationListSize;
   HRESULT hr = device->KTCallbacks.pfnRenderCb(device->hDevice, &render);
   LogSharedCopyFailure(publish ? "submit-publish" : "submit-refresh", hr);
   if (SUCCEEDED(hr)) {
      context->pCommandBuffer = render.pNewCommandBuffer;
      context->CommandBufferSize = render.NewCommandBufferSize;
      context->pAllocationList = render.pNewAllocationList;
      context->AllocationListSize = render.NewAllocationListSize;
      context->pPatchLocationList = render.pNewPatchLocationList;
      context->PatchLocationListSize = render.NewPatchLocationListSize;
   }
   return hr;
}

/* WDDM owns the inter-process backing. Gallium textures are GPU caches of that
 * allocation until Turnip implements external-memory import. Flush/ownership
 * release publishes GPU writes; a resource read refreshes a clean GPU cache.
 * Never replace a shared allocation with a private texture and report success.
 */
static HRESULT
TransferSharedResource(Device *device, Resource *resource, bool publish)
{
   static volatile LONG sequence;
   const LONG transfer_sequence = InterlockedIncrement(&sequence);
   /* This sits on the present path, ahead of the scanout publication, and the
    * two of them together set the frame rate.  Time it and report a running
    * average so the frame budget can be split between them. */
   LARGE_INTEGER transferFreq, transferStart;
   QueryPerformanceFrequency(&transferFreq);
   QueryPerformanceCounter(&transferStart);
   // Sample the existing timing log without adding per-draw file traffic.
   // Separate GPU mapping, VidSch/LockCb waits and CPU copy costs before
   // changing any of the shared-resource ownership or completion rules.
   // Mixed refresh/publish cycles can alias a single global modulo counter
   // and almost never sample publish. Keep the same cadence per operation.
   static volatile LONG operation_sequence[2];
   const unsigned operation = publish ? 1 : 0;
   const bool timing_sample = (InterlockedIncrement(&operation_sequence[operation]) % 64) == 0;
   LARGE_INTEGER phaseStart = transferStart;
   LONGLONG phaseUsec[6] = {};
   const auto recordPhase = [&](unsigned phase) {
      if (!timing_sample)
         return;
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (transferFreq.QuadPart > 0)
         phaseUsec[phase] = ((now.QuadPart - phaseStart.QuadPart) * 1000000LL) /
                            transferFreq.QuadPart;
      phaseStart = now;
   };
   void *pixels = NULL;
   struct pipe_transfer *transfer = NULL;
   HRESULT lock_hr = E_UNEXPECTED;
   HRESULT unlock_hr = E_UNEXPECTED;
   void *lock_data = NULL;
   unsigned transfer_stride = 0;
   enum pipe_reset_status reset_status = PIPE_NO_RESET;

   if (resource->hAllocation == 0)
      return S_OK;
   HRESULT setup_hr = EnsureSharedCopy(device, resource);
   if (FAILED(setup_hr))
      return setup_hr;

   struct pipe_context *pipe = device->pipe;
   struct pipe_resource *texture = resource->resource;
   struct pipe_box box = {};
   box.width = texture->width0;
   box.height = texture->height0;
   box.depth = 1;
   if (!device->KTCallbacks.pfnLockCb || !device->KTCallbacks.pfnUnlockCb) {
      lock_hr = E_NOTIMPL;
   } else {
      // A synchronized map waits for the GPU before publishing its pixels.
      pixels = pipe->texture_map(pipe, texture, 0,
                                publish ? PIPE_MAP_READ : PIPE_MAP_WRITE,
                                &box, &transfer);
      recordPhase(0);
      if (!pixels) {
         reset_status = pipe->get_device_reset_status
                           ? pipe->get_device_reset_status(pipe)
                           : PIPE_NO_RESET;
         lock_hr = reset_status == PIPE_NO_RESET ? E_OUTOFMEMORY : D3DDDIERR_DEVICEREMOVED;
      } else {
         transfer_stride = transfer->stride;
         D3DDDICB_LOCK lock = {};
         // LockCb synchronizes CPU-visible allocations created by this device
         // directly with VidSch. Opened handles and non-lockable primaries
         // still need a scheduled copy through device-owned private staging.
         lock.hAllocation = resource->allocation_lockable
                               ? resource->hAllocation : resource->shared_staging_allocation;
         lock.Flags.LockEntire = 1;
         lock.Flags.ReadOnly = !publish;
         lock.Flags.WriteOnly = publish;
         lock_hr = publish || resource->allocation_lockable
                      ? S_OK : SubmitSharedCopy(device, resource, false);
         if (SUCCEEDED(lock_hr))
            lock_hr = device->KTCallbacks.pfnLockCb(device->hDevice, &lock);
         recordPhase(1);
         lock_data = lock.pData;
         if (SUCCEEDED(lock_hr)) {
            if (!lock.pData) {
               lock_hr = E_FAIL;
            } else {
               const size_t row_bytes = (size_t)texture->width0 * 4;
               for (unsigned y = 0; y < texture->height0; ++y) {
                  void *gpu = (char *)pixels + (size_t)y * transfer_stride;
                  void *shared = (char *)lock.pData + (size_t)y * resource->shared_pitch;
                  if (publish)
                     memcpy(shared, gpu, row_bytes);
                  else
                     memcpy(gpu, shared, row_bytes);
               }
            }
            recordPhase(2);
            D3DDDICB_UNLOCK unlock = {};
            unlock.NumAllocations = 1;
            unlock.phAllocations = &lock.hAllocation;
            unlock_hr = device->KTCallbacks.pfnUnlockCb(device->hDevice, &unlock);
            recordPhase(3);
            if (publish && !resource->allocation_lockable &&
                SUCCEEDED(lock_hr) && SUCCEEDED(unlock_hr)) {
               unlock_hr = SubmitSharedCopy(device, resource, true);
               if (SUCCEEDED(unlock_hr)) {
                  // A read/write lock waits for the copy's read of staging too.
                  // Ownership may be released only after the destination is current.
                  D3DDDICB_LOCK wait = {};
                  wait.hAllocation = resource->shared_staging_allocation;
                  wait.Flags.LockEntire = 1;
                  unlock_hr = device->KTCallbacks.pfnLockCb(device->hDevice, &wait);
                  if (SUCCEEDED(unlock_hr)) {
                     unlock.phAllocations = &wait.hAllocation;
                     unlock_hr = device->KTCallbacks.pfnUnlockCb(device->hDevice, &unlock);
                  }
               }
            }
            recordPhase(4);
         }
         pipe_texture_unmap(pipe, transfer);
         reset_status = pipe->get_device_reset_status
                           ? pipe->get_device_reset_status(pipe)
                           : PIPE_NO_RESET;
         recordPhase(5);
      }
   }

   HRESULT hr = FAILED(lock_hr) ? lock_hr : unlock_hr;
   if (SUCCEEDED(lock_hr) && unlock_hr == E_UNEXPECTED)
      hr = E_FAIL;
   if (reset_status != PIPE_NO_RESET)
      hr = D3DDDIERR_DEVICEREMOVED;
   {
      LARGE_INTEGER transferEnd;
      QueryPerformanceCounter(&transferEnd);
      static volatile LONG64 total_usec[2];
      static volatile LONG samples[2];
      const LONG64 usec = transferFreq.QuadPart > 0
         ? ((transferEnd.QuadPart - transferStart.QuadPart) * 1000000LL) / transferFreq.QuadPart
         : 0;
      const LONG64 running = InterlockedAdd64(&total_usec[operation], usec);
      const LONG n = InterlockedIncrement(&samples[operation]);
      if (timing_sample) {
         HANDLE log = CreateFileA("C:\\Users\\Public\\umd_timing.log",
                                  FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
         if (log != INVALID_HANDLE_VALUE) {
            char line[512];
            int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                                  "shared op=%s samples=%ld last=%lldus mean=%lldus "
                                  "pid=%lu tick=%llu size=%ux%u allocation=0x%x linear=%u direct=%u hr=0x%08lx "
                                  "prepare_map=%lld lock=%lld copy=%lld unlock=%lld publish=%lld unmap=%lldus\r\n",
                                  publish ? "publish" : "refresh", n, usec, running / n,
                                  GetCurrentProcessId(), GetTickCount64(),
                                  texture->width0, texture->height0, resource->hAllocation,
                                  (texture->bind & PIPE_BIND_LINEAR) ? 1u : 0u,
                                  resource->allocation_lockable ? 1u : 0u,
                                  (unsigned long)hr, phaseUsec[0], phaseUsec[1], phaseUsec[2],
                                  phaseUsec[3], phaseUsec[4], phaseUsec[5]);
            DWORD written = 0;
            if (len > 0) WriteFile(log, line, (DWORD)len, &written, NULL);
            CloseHandle(log);
         }
      }
   }
   /* This opens, writes and closes a file on the present path, so it records
    * enough transfers to show the path working and then gets out of the way. */
   if (transfer_sequence <= 32 || FAILED(hr)) {
      HANDLE log = CreateFileA("C:\\Users\\Public\\umd_shared.log",
                               FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (log != INVALID_HANDLE_VALUE) {
         char line[320];
         int n = _snprintf_s(line, sizeof line, _TRUNCATE,
                             "seq=%ld op=%s resource=%p hAllocation=0x%x map=%p transfer=%p stride=%u lock=0x%08lx data=%p unlock=0x%08lx reset=%u result=0x%08lx\r\n",
                             transfer_sequence, publish ? "publish" : "refresh",
                             resource, resource->hAllocation, pixels, transfer,
                             transfer_stride, (unsigned long)lock_hr,
                             lock_data, (unsigned long)unlock_hr, (unsigned)reset_status,
                             (unsigned long)hr);
         DWORD written = 0;
         if (n > 0)
            WriteFile(log, line, (DWORD)n, &written, NULL);
         CloseHandle(log);
      }
   }
   if (FAILED(hr))
      DebugPrintf("shared %s hAllocation=0x%x map=%p lock=0x%08lx unlock=0x%08lx reset=%u hr=0x%08lx\n",
                  publish ? "publish" : "refresh", resource->hAllocation,
                  pixels, (unsigned long)lock_hr, (unsigned long)unlock_hr,
                  (unsigned)reset_status, (unsigned long)hr);
   return hr;
}

HRESULT
RefreshSharedResource(Device *device, Resource *resource)
{
   if (!resource || resource->hAllocation == 0 || resource->shared_dirty)
      return S_OK;
   return TransferSharedResource(device, resource, false);
}

HRESULT
PublishSharedResource(Device *device, Resource *resource)
{
   if (!resource || !resource->shared_dirty)
      return S_OK;
   HRESULT hr = TransferSharedResource(device, resource, true);
   if (SUCCEEDED(hr))
      resource->shared_dirty = false;
   return hr;
}

HRESULT
PublishSharedResources(Device *device)
{
   for (Resource *resource = device->shared_resources; resource; resource = resource->shared_next) {
      HRESULT hr = PublishSharedResource(device, resource);
      if (FAILED(hr))
         return hr;
   }
   return S_OK;
}

HRESULT
PreparePresentResource(Device *device, Resource *resource, bool refreshCache)
{
   if (!resource || !resource->hAllocation)
      return DXGI_DDI_ERR_UNSUPPORTED;
   HRESULT hr = EnsureSharedCopy(device, resource);
   if (FAILED(hr))
      return hr;
   // PresentCb consumes the kernel allocation. Clean allocation contents are
   // already authoritative, so only a subsequent UMD readback needs its GPU
   // cache refreshed. Dirty writes still publish and wait before PresentCb.
   if (resource->shared_dirty)
      return PublishSharedResource(device, resource);
   return refreshCache ? RefreshSharedResource(device, resource) : S_OK;
}

void
MarkSharedResourceWritten(Device *device, struct pipe_resource *texture)
{
   for (Resource *resource = device->shared_resources; resource; resource = resource->shared_next) {
      if (resource->resource == texture) {
         resource->shared_dirty = true;
         return;
      }
   }
}

bool
PrepareSharedDraw(Device *device)
{
   // Only refresh resources referenced by this draw: unrelated keyed resources
   // may be owned by another device. Dirty caches already include our writes.
   for (Resource *resource = device->shared_resources; resource; resource = resource->shared_next) {
      bool input = false, output = false;
      for (unsigned stage = 0; stage < MESA_SHADER_STAGES; ++stage) {
         for (unsigned slot = 0; slot < PIPE_MAX_SHADER_SAMPLER_VIEWS; ++slot) {
            struct pipe_sampler_view *view = device->sampler_views[stage][slot];
            input |= view && view->texture == resource->resource;
         }
      }
      for (unsigned slot = 0; slot < device->fb.nr_cbufs; ++slot)
         output |= device->fb.cbufs[slot].texture == resource->resource;
      if (input || output) {
         HRESULT hr = RefreshSharedResource(device, resource);
         if (FAILED(hr)) {
            device->UMCallbacks.pfnSetErrorCb(device->hRTCoreLayer, hr);
            return false;
         }
      }
      if (output)
         resource->shared_dirty = true;
   }
   return true;
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateResourceSize --
 *
 *    The CalcPrivateResourceSize function determines the size of
 *    the user-mode display driver's private region of memory
 *    (that is, the size of internal driver structures, not the
 *    size of the resource video memory).
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateResourceSize(D3D10DDI_HDEVICE hDevice,                                // IN
                        __in const D3D10DDIARG_CREATERESOURCE *pCreateResource)  // IN
{
   LOG_ENTRYPOINT();
   return sizeof(Resource);
}


static pipe_resource_usage
translate_resource_usage(UINT usage)
{
   pipe_resource_usage resource_usage = PIPE_USAGE_DEFAULT;

   switch (usage) {
   case D3D10_DDI_USAGE_DEFAULT:
      resource_usage = PIPE_USAGE_DEFAULT;
      break;
   case D3D10_DDI_USAGE_IMMUTABLE:
      resource_usage = PIPE_USAGE_IMMUTABLE;
      break;
   case D3D10_DDI_USAGE_DYNAMIC:
      resource_usage = PIPE_USAGE_DYNAMIC;
      break;
   case D3D10_DDI_USAGE_STAGING:
      resource_usage = PIPE_USAGE_STAGING;
      break;
   default:
      assert(0);
      break;
   }

   return resource_usage;
}


static unsigned
translate_resource_flags(UINT flags)
{
   unsigned bind = 0;

   if (flags & D3D10_DDI_BIND_VERTEX_BUFFER)
      bind |= PIPE_BIND_VERTEX_BUFFER;

   if (flags & D3D10_DDI_BIND_INDEX_BUFFER)
      bind |= PIPE_BIND_INDEX_BUFFER;

   if (flags & D3D10_DDI_BIND_CONSTANT_BUFFER)
      bind |= PIPE_BIND_CONSTANT_BUFFER;

   if (flags & D3D10_DDI_BIND_SHADER_RESOURCE)
      bind |= PIPE_BIND_SAMPLER_VIEW;

   if (flags & D3D10_DDI_BIND_RENDER_TARGET)
      bind |= PIPE_BIND_RENDER_TARGET;

   if (flags & D3D10_DDI_BIND_DEPTH_STENCIL)
      bind |= PIPE_BIND_DEPTH_STENCIL;

   if (flags & D3D10_DDI_BIND_STREAM_OUTPUT)
      bind |= PIPE_BIND_STREAM_OUTPUT;

   if (flags & D3D10_DDI_BIND_PRESENT)
      bind |= PIPE_BIND_DISPLAY_TARGET;

   return bind;
}


static enum pipe_texture_target
translate_texture_target( D3D10DDIRESOURCE_TYPE ResourceDimension,
                             UINT ArraySize)
{
   assert(ArraySize >= 1);
   switch(ResourceDimension) {
   case D3D10DDIRESOURCE_BUFFER:
      assert(ArraySize == 1);
      return PIPE_BUFFER;
   case D3D10DDIRESOURCE_TEXTURE1D:
      return ArraySize > 1 ? PIPE_TEXTURE_1D_ARRAY : PIPE_TEXTURE_1D;
   case D3D10DDIRESOURCE_TEXTURE2D:
      return ArraySize > 1 ? PIPE_TEXTURE_2D_ARRAY : PIPE_TEXTURE_2D;
   case D3D10DDIRESOURCE_TEXTURE3D:
      assert(ArraySize == 1);
      return PIPE_TEXTURE_3D;
   case D3D10DDIRESOURCE_TEXTURECUBE:
      assert(ArraySize % 6 == 0);
      return ArraySize > 6 ? PIPE_TEXTURE_CUBE_ARRAY : PIPE_TEXTURE_CUBE;
   default:
      assert(0);
      return PIPE_TEXTURE_1D;
   }
}


static void
subResourceBox(struct pipe_resource *resource, // IN
                 UINT SubResource,  // IN
                 unsigned *pLevel, // OUT
                 struct pipe_box *pBox)   // OUT
{
   UINT MipLevels = resource->last_level + 1;
   unsigned layer;
   unsigned width;
   unsigned height;
   unsigned depth;

   *pLevel = SubResource % MipLevels;
   layer = SubResource / MipLevels;

   width  = u_minify(resource->width0,  *pLevel);
   height = u_minify(resource->height0, *pLevel);
   depth  = u_minify(resource->depth0,  *pLevel);

   pBox->x = 0;
   pBox->y = 0;
   pBox->z = 0 + layer;
   pBox->width  = width;
   pBox->height = height;
   pBox->depth  = depth;
}


/*
 * ----------------------------------------------------------------------
 *
 * CreateResource --
 *
 *    The CreateResource function creates a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
CreateResource(D3D10DDI_HDEVICE hDevice,                                // IN
               __in const D3D10DDIARG_CREATERESOURCE *pCreateResource,  // IN
               D3D10DDI_HRESOURCE hResource,                            // IN
               D3D10DDI_HRTRESOURCE hRTResource)                        // IN
{
   LOG_ENTRYPOINT();

   if (pCreateResource->ResourceDimension == D3D10DDIRESOURCE_BUFFER ||
       (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) ||
       (pCreateResource->pPrimaryDesc &&
        pCreateResource->pPrimaryDesc->Flags & DXGI_DDI_PRIMARY_OPTIONAL)) {

      DebugPrintf("%s(%dx%dx%d hResource=%p)\n",
	       __func__,
	       pCreateResource->pMipInfoList[0].TexelWidth,
	       pCreateResource->pMipInfoList[0].TexelHeight,
	       pCreateResource->pMipInfoList[0].TexelDepth,
	       hResource.pDrvPrivate);
      DebugPrintf("  ResourceDimension = %u\n",
	       pCreateResource->ResourceDimension);
      DebugPrintf("  Usage = %u\n",
	       pCreateResource->Usage);
      DebugPrintf("  BindFlags = 0x%x\n",
	       pCreateResource->BindFlags);
      DebugPrintf("  MapFlags = 0x%x\n",
	       pCreateResource->MapFlags);
      DebugPrintf("  MiscFlags = 0x%x\n",
	       pCreateResource->MiscFlags);
      DebugPrintf("  Format = %s\n",
	       FormatToName(pCreateResource->Format));
      DebugPrintf("  SampleDesc.Count = %u\n", pCreateResource->SampleDesc.Count);
      DebugPrintf("  SampleDesc.Quality = %u\n", pCreateResource->SampleDesc.Quality);
      DebugPrintf("  MipLevels = %u\n", pCreateResource->MipLevels);
      DebugPrintf("  ArraySize = %u\n", pCreateResource->ArraySize);
      DebugPrintf("  pPrimaryDesc = %p\n", pCreateResource->pPrimaryDesc);
      if (pCreateResource->pPrimaryDesc) {
	 DebugPrintf("    Flags = 0x%x\n",
		  pCreateResource->pPrimaryDesc->Flags);
	 DebugPrintf("    VidPnSourceId = %u\n", pCreateResource->pPrimaryDesc->VidPnSourceId);
	 DebugPrintf("    ModeDesc.Width = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.Width);
	 DebugPrintf("    ModeDesc.Height = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.Height);
	 DebugPrintf("    ModeDesc.Format = %u)\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Format);
	 DebugPrintf("    ModeDesc.RefreshRate.Numerator = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Numerator);
	 DebugPrintf("    ModeDesc.RefreshRate.Denominator = %u\n", pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Denominator);
	 DebugPrintf("    ModeDesc.ScanlineOrdering = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.ScanlineOrdering);
	 DebugPrintf("    ModeDesc.Rotation = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Rotation);
	 DebugPrintf("    ModeDesc.Scaling = %u\n",
		  pCreateResource->pPrimaryDesc->ModeDesc.Scaling);
	 DebugPrintf("    DriverFlags = 0x%x\n",
		  pCreateResource->pPrimaryDesc->DriverFlags);
      }

   }

   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;

   Resource *pResource = CastResource(hResource);

   memset(pResource, 0, sizeof *pResource);

   if (pCreateResource->pPrimaryDesc) {
      DXGI_DDI_PRIMARY_DESC *primary = pCreateResource->pPrimaryDesc;
      const bool optional = (primary->Flags & DXGI_DDI_PRIMARY_OPTIONAL) != 0;
      // Optional app buffers retain copy-style presentation. DWM requires a
      // real primary: allocate the miniport's standard scanout backing below.
      primary->DriverFlags = optional ? DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT : 0;
      pResource->scanout_primary = !optional;
      static volatile LONG primaryCount;
      const LONG sample = InterlockedIncrement(&primaryCount);
      if (sample <= 16) {
         HANDLE log = CreateFileA("C:\\Users\\Public\\umd_dxgi.log", FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, NULL);
         if (log != INVALID_HANDLE_VALUE) {
            char line[192];
            int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                                  "primary pid=%lu flags=0x%x driverFlags=0x%x optional=%u size=%ux%u\r\n",
                                  GetCurrentProcessId(), primary->Flags, primary->DriverFlags,
                                  optional ? 1u : 0u, primary->ModeDesc.Width, primary->ModeDesc.Height);
            DWORD written;
            if (len > 0)
               WriteFile(log, line, (DWORD)len, &written, NULL);
            CloseHandle(log);
         }
      }
      if (!optional &&
          (!CastDevice(hDevice)->runtime_present || primary->VidPnSourceId != 0 ||
           (primary->Flags & ~DXGI_DDI_PRIMARY_NONPREROTATED) != 0 ||
           primary->ModeDesc.Rotation != DXGI_DDI_MODE_ROTATION_IDENTITY ||
           primary->ModeDesc.Width != pCreateResource->pMipInfoList[0].TexelWidth ||
           primary->ModeDesc.Height != pCreateResource->pMipInfoList[0].TexelHeight ||
           primary->ModeDesc.Format != pCreateResource->Format ||
           primary->ModeDesc.RefreshRate.Numerator == 0 ||
           primary->ModeDesc.RefreshRate.Denominator == 0)) {
         SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
         return;
      }
   }

   pResource->Format = pCreateResource->Format;
   pResource->MipLevels = pCreateResource->MipLevels;

   struct pipe_resource templat;

   memset(&templat, 0, sizeof templat);

   templat.target     = translate_texture_target( pCreateResource->ResourceDimension,
                                                  pCreateResource->ArraySize );
   pResource->buffer = templat.target == PIPE_BUFFER;

   if (pCreateResource->Format == DXGI_FORMAT_UNKNOWN) {
      assert(pCreateResource->ResourceDimension == D3D10DDIRESOURCE_BUFFER);
      templat.format = PIPE_FORMAT_R8_UINT;
   } else {
      BOOL bindDepthStencil = !!(pCreateResource->BindFlags & D3D10_DDI_BIND_DEPTH_STENCIL);
      templat.format = FormatTranslate(pCreateResource->Format, bindDepthStencil);
   }

   templat.width0     = pCreateResource->pMipInfoList[0].TexelWidth;
   templat.height0    = pCreateResource->pMipInfoList[0].TexelHeight;
   templat.depth0     = pCreateResource->pMipInfoList[0].TexelDepth;
   templat.array_size = pCreateResource->ArraySize;
   templat.last_level = pCreateResource->MipLevels - 1;
   templat.nr_samples = pCreateResource->SampleDesc.Count;
   templat.nr_storage_samples = pCreateResource->SampleDesc.Count;
   templat.bind       = translate_resource_flags(pCreateResource->BindFlags);
   templat.usage      = translate_resource_usage(pCreateResource->Usage);

   const bool shared = (pCreateResource->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0;
   const bool present = CastDevice(hDevice)->runtime_present &&
                        (pCreateResource->BindFlags & D3D10_DDI_BIND_PRESENT);
   const bool kernelBacked = shared || present || pResource->scanout_primary;
   const VIOGPU_WDDM_UINT32 sharedFormat = SharedPrivateFormat(pCreateResource->Format);
   if (kernelBacked && (templat.target != PIPE_TEXTURE_2D ||
                  sharedFormat == VIOGPU_WDDM_FORMAT_NONE ||
                  templat.width0 == 0 || templat.width0 > UINT_MAX / 4 ||
                  templat.height0 == 0 || templat.array_size != 1 ||
                  templat.last_level != 0 || templat.nr_samples != 1 ||
                  templat.usage != PIPE_USAGE_DEFAULT)) {
      SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
      return;
   }

   if (templat.target != PIPE_BUFFER) {
      if (!screen->is_format_supported(screen,
                                       templat.format,
                                       templat.target,
                                       templat.nr_samples,
                                       templat.nr_storage_samples,
                                       templat.bind)) {
         debug_printf("%s: unsupported format %s\n",
                     __func__, util_format_name(templat.format));
         SetError(hDevice, E_OUTOFMEMORY);
         return;
      }
   }

   pResource->resource = kernelBacked ? CreateSharedTextureCache(screen, &templat)
                                     : screen->resource_create(screen, &templat);
   if (!pResource->resource) {
      DebugPrintf("%s: failed to create resource\n", __func__);
      SetError(hDevice, E_OUTOFMEMORY);
      return;
   }

   pResource->NumSubResources = pCreateResource->MipLevels * pCreateResource->ArraySize;
   pResource->transfers = (struct pipe_transfer **)calloc(pResource->NumSubResources,
                                                          sizeof *pResource->transfers);
   if (!pResource->transfers) {
      pipe_resource_reference(&pResource->resource, NULL);
      SetError(hDevice, E_OUTOFMEMORY);
      return;
   }

   /* A shared/present resource must have a real kernel allocation behind it. Without
    * one the runtime still reports success from GetSharedHandle and hands back
    * a NULL handle, which DirectComposition trusts and then faults on -- that
    * is what crash-loops LogonUI.exe and dwm.exe on this driver. If the
    * allocation cannot be made, refuse the resource rather than return a
    * handle that was never real. */
   if (kernelBacked) {
      Device *pDevice = CastDevice(hDevice);
      const unsigned shared_width = pCreateResource->pMipInfoList[0].TexelWidth;
      const unsigned shared_height = pCreateResource->pMipInfoList[0].TexelHeight;
      const unsigned shared_pitch = shared_width * 4;

      VIOGPU_WDDM_ALLOCATION_INFO privateData;
      memset(&privateData, 0, sizeof privateData);
      privateData.Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      privateData.Header.Version = VIOGPU_WDDM_ABI_VERSION;
      privateData.Header.Size = sizeof privateData;
      privateData.Size = (VIOGPU_WDDM_UINT64)shared_pitch * shared_height;
      privateData.Alignment = 4096;
      privateData.Flags = VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE;
      if (pResource->scanout_primary) {
         // Primary backing is scheduler-managed, not directly locked by the
         // UMD. The scheduled staging copy fills it before PresentCb.
         privateData.Flags = VIOGPU_WDDM_ALLOCATION_PRIMARY;
         privateData.RefreshRateNumerator = pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Numerator;
         privateData.RefreshRateDenominator = pCreateResource->pPrimaryDesc->ModeDesc.RefreshRate.Denominator;
      }
      // The allocation's format is also returned to OpenResource in another
      // process. Preserve RGBA rather than interpreting its bytes as BGRA.
      privateData.Format = sharedFormat;
      privateData.Width = shared_width;
      privateData.Height = shared_height;
      privateData.Pitch = shared_pitch;

      D3DDDI_ALLOCATIONINFO allocationInfo;
      memset(&allocationInfo, 0, sizeof allocationInfo);
      allocationInfo.pPrivateDriverData = &privateData;
      allocationInfo.PrivateDriverDataSize = sizeof privateData;
      allocationInfo.Flags.Primary = pResource->scanout_primary;
      if (pResource->scanout_primary)
         allocationInfo.VidPnSourceId = pCreateResource->pPrimaryDesc->VidPnSourceId;

      D3DDDICB_ALLOCATE allocate;
      memset(&allocate, 0, sizeof allocate);
      allocate.hResource = (HANDLE)(UINT_PTR)hRTResource.handle;
      allocate.NumAllocations = 1;
      allocate.pAllocationInfo = &allocationInfo;

      HRESULT ahr = pDevice->KTCallbacks.pfnAllocateCb(pDevice->hDevice, &allocate);

      /* The miniport reports refusing none of these, so a failure happens
       * inside the runtime and leaves no trace this driver can read back.
       * Record it where it can be collected. */
      {
         static unsigned attempts = 0, failures = 0;
         ++attempts;
         if (FAILED(ahr) || allocationInfo.hAllocation == 0)
            ++failures;
         HANDLE log = attempts <= 32 || FAILED(ahr) || allocationInfo.hAllocation == 0
                         ? CreateFileA("C:\\Users\\Public\\umd_alloc.log",
                                       FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)
                         : INVALID_HANDLE_VALUE;
         if (log != INVALID_HANDLE_VALUE) {
            char line[256];
            int n = _snprintf_s(line, sizeof line, _TRUNCATE,
                                "alloc hr=0x%08lx hAlloc=0x%x dim=%u %ux%u fmt=%u attempts=%u failures=%u\r\n",
                                (unsigned long)ahr, allocationInfo.hAllocation,
                                pCreateResource->ResourceDimension,
                                shared_width, shared_height,
                                (unsigned)pCreateResource->Format,
                                attempts, failures);
            DWORD written = 0;
            if (n > 0)
               WriteFile(log, line, (DWORD)n, &written, NULL);
            CloseHandle(log);
         }
      }

      if (FAILED(ahr) || allocationInfo.hAllocation == 0) {
         DebugPrintf("%s: shared allocation failed hr=0x%08lx\n",
                     __func__, (unsigned long)ahr);
         pipe_resource_reference(&pResource->resource, NULL);
         free(pResource->transfers);
         pResource->transfers = NULL;
         SetError(hDevice, DXGI_DDI_ERR_UNSUPPORTED);
         return;
      }
      pResource->hKMResource = allocate.hKMResource;
      pResource->hAllocation = allocationInfo.hAllocation;
      pResource->hRTResourceHandle = allocate.hResource;
      pResource->allocation_lockable = !pResource->scanout_primary;
      pResource->shared_pitch = shared_pitch;
      pResource->shared_next = pDevice->shared_resources;
      pDevice->shared_resources = pResource;
      // A fresh resource has no defined contents until the client writes it.
      pResource->shared_dirty = pCreateResource->pInitialDataUP != NULL;
   }

   if (pCreateResource->pInitialDataUP) {
      if (pResource->buffer) {
         assert(pResource->NumSubResources == 1);
         const D3D10_DDIARG_SUBRESOURCE_UP* pInitialDataUP =
               &pCreateResource->pInitialDataUP[0];

         unsigned level;
         struct pipe_box box;
         subResourceBox(pResource->resource, 0, &level, &box);

         struct pipe_transfer *transfer;
         void *map;
         map = pipe->buffer_map(pipe,
                                pResource->resource,
                                level,
                                PIPE_MAP_WRITE |
                                PIPE_MAP_UNSYNCHRONIZED,
                                &box,
                                &transfer);
         assert(map);
         if (map) {
            memcpy(map, pInitialDataUP->pSysMem, box.width);
            pipe_buffer_unmap(pipe, transfer);
         }
      } else {
         for (UINT SubResource = 0; SubResource < pResource->NumSubResources; ++SubResource) {
            const D3D10_DDIARG_SUBRESOURCE_UP* pInitialDataUP =
                  &pCreateResource->pInitialDataUP[SubResource];

            unsigned level;
            struct pipe_box box;
            subResourceBox(pResource->resource, SubResource, &level, &box);

            struct pipe_transfer *transfer;
            void *map;
            map = pipe->texture_map(pipe,
                                    pResource->resource,
                                    level,
                                    PIPE_MAP_WRITE |
                                    PIPE_MAP_UNSYNCHRONIZED,
                                    &box,
                                    &transfer);
            assert(map);
            if (map) {
               for (int z = 0; z < box.depth; ++z) {
                  uint8_t *dst = (uint8_t*)map + z*transfer->layer_stride;
                  const uint8_t *src = (const uint8_t*)pInitialDataUP->pSysMem + z*pInitialDataUP->SysMemSlicePitch;
                  util_copy_rect(dst,
                                 templat.format,
                                 transfer->stride,
                                 0, 0, box.width, box.height,
                                 src,
                                 pInitialDataUP->SysMemPitch,
                                 0, 0);
               }
               pipe_texture_unmap(pipe, transfer);
            }
         }
      }
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * CalcPrivateOpenedResourceSize --
 *
 *    The CalcPrivateOpenedResourceSize function determines the size
 *    of the user-mode display driver's private shared region of memory
 *    (that is, the size of internal driver structures, not the size
 *    of the resource video memory) for an opened resource.
 *
 * ----------------------------------------------------------------------
 */

SIZE_T APIENTRY
CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE hDevice,                             // IN
                              __in const D3D10DDIARG_OPENRESOURCE *pOpenResource)   // IN
{
   return sizeof(Resource);
}


/*
 * ----------------------------------------------------------------------
 *
 * OpenResource --
 *
 *    The OpenResource function opens a shared resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
OpenResource(D3D10DDI_HDEVICE hDevice,                            // IN
             __in const D3D10DDIARG_OPENRESOURCE *pOpenResource,  // IN
             D3D10DDI_HRESOURCE hResource,                        // IN
             D3D10DDI_HRTRESOURCE hRTResource)                    // IN
{
   LOG_ENTRYPOINT();

   /* Reconstruct the GPU cache of the existing WDDM backing. Its actual pixels
    * are refreshed before use, including uses after another owner updates it. */
   struct pipe_context *pipe = CastPipeContext(hDevice);
   struct pipe_screen *screen = pipe->screen;
   Resource *pResource = CastResource(hResource);

   memset(pResource, 0, sizeof *pResource);

   /* Record the ABI shape independently of the shared-content transfer. */
   {
      HANDLE log = CreateFileA("C:\\Users\\Public\\umd_open.log",
                               FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (log != INVALID_HANDLE_VALUE) {
         char line[256];
         int n = _snprintf_s(line, sizeof line, _TRUNCATE,
                             "open numAlloc=%u privSize=%u expect=%u\r\n",
                             pOpenResource ? pOpenResource->NumAllocations : 0u,
                             (pOpenResource && pOpenResource->NumAllocations && pOpenResource->pOpenAllocationInfo)
                                ? pOpenResource->pOpenAllocationInfo[0].PrivateDriverDataSize : 0u,
                             (unsigned)sizeof(VIOGPU_WDDM_ALLOCATION_INFO));
         DWORD written = 0;
         if (n > 0)
            WriteFile(log, line, (DWORD)n, &written, NULL);
         CloseHandle(log);
      }
   }

   if (pOpenResource == NULL || pOpenResource->NumAllocations != 1 ||
       pOpenResource->pOpenAllocationInfo == NULL) {
      DebugPrintf("%s: unexpected open shape\n", __func__);
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   const D3DDDI_OPENALLOCATIONINFO *openInfo = &pOpenResource->pOpenAllocationInfo[0];
   if (openInfo->pPrivateDriverData == NULL ||
       openInfo->PrivateDriverDataSize != sizeof(VIOGPU_WDDM_ALLOCATION_INFO)) {
      DebugPrintf("%s: allocation private data is %u bytes, expected %u\n", __func__,
                  (unsigned)openInfo->PrivateDriverDataSize,
                  (unsigned)sizeof(VIOGPU_WDDM_ALLOCATION_INFO));
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   VIOGPU_WDDM_ALLOCATION_INFO info;
   memcpy(&info, openInfo->pPrivateDriverData, sizeof info);
   const DXGI_FORMAT sharedFormat = SharedDxgiFormat(info.Format);
   if (info.Header.Magic != VIOGPU_WDDM_ABI_MAGIC ||
       info.Header.Version != VIOGPU_WDDM_ABI_VERSION ||
       info.Header.Size != sizeof info || info.Header.Reserved != 0 ||
       (info.Flags != VIOGPU_WDDM_ALLOCATION_CPU_VISIBLE &&
        info.Flags != VIOGPU_WDDM_ALLOCATION_PRIMARY) ||
       (info.Flags == VIOGPU_WDDM_ALLOCATION_PRIMARY &&
        (info.RefreshRateNumerator == 0 || info.RefreshRateDenominator == 0)) ||
       sharedFormat == DXGI_FORMAT_UNKNOWN ||
       info.Width == 0 || info.Width > UINT_MAX / 4 || info.Height == 0 ||
       info.Pitch < info.Width * 4 ||
       (VIOGPU_WDDM_UINT64)info.Pitch * info.Height > info.Size ||
       openInfo->hAllocation == 0) {
      DebugPrintf("%s: allocation private data is not this driver's\n", __func__);
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   struct pipe_resource templat;
   memset(&templat, 0, sizeof templat);
   templat.target = PIPE_TEXTURE_2D;
   templat.format = FormatTranslate(sharedFormat, false);
   templat.width0 = info.Width;
   templat.height0 = info.Height;
   templat.depth0 = 1;
   templat.array_size = 1;
   templat.last_level = 0;
   templat.nr_samples = 1;
   templat.usage = PIPE_USAGE_DEFAULT;
   templat.bind = PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_RENDER_TARGET;

   pResource->resource = CreateSharedTextureCache(screen, &templat);
   if (pResource->resource == NULL) {
      DebugPrintf("%s: could not create the backing resource\n", __func__);
      SetError(hDevice, E_OUTOFMEMORY);
      return;
   }

   pResource->Format = sharedFormat;
   pResource->MipLevels = 1;
   pResource->NumSubResources = 1;
   pResource->buffer = false;
   pResource->transfers = (struct pipe_transfer **)calloc(1, sizeof *pResource->transfers);
   if (!pResource->transfers) {
      pipe_resource_reference(&pResource->resource, NULL);
      SetError(hDevice, E_OUTOFMEMORY);
      return;
   }
   pResource->hAllocation = openInfo->hAllocation;
   pResource->scanout_primary = info.Flags == VIOGPU_WDDM_ALLOCATION_PRIMARY;
   pResource->hKMResource = 0;
   /* The allocation belongs to whoever created it; this device only holds a
    * view, so destruction here must not deallocate it. */
   pResource->hRTResourceHandle = NULL;
   Device *device = CastDevice(hDevice);
   pResource->shared_pitch = info.Pitch;
   pResource->shared_next = device->shared_resources;
   device->shared_resources = pResource;

   DebugPrintf("%s: opened %ux%u hAllocation=0x%x\n", __func__,
               info.Width, info.Height, openInfo->hAllocation);
}


/*
 * ----------------------------------------------------------------------
 *
 * DestroyResource --
 *
 *    The DestroyResource function destroys the specified resource
 *    object. The resource object can be destoyed only if it is not
 *    currently bound to a display device, and if all views that
 *    refer to the resource are also destroyed.
 *
 * ----------------------------------------------------------------------
 */


void APIENTRY
DestroyResource(D3D10DDI_HDEVICE hDevice,       // IN
                D3D10DDI_HRESOURCE hResource)   // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);
   Device *device = CastDevice(hDevice);
   if (pResource->hAllocation) {
      if (pResource->shared_dirty) {
         pipe->flush(pipe, NULL, 0);
         HRESULT hr = PublishSharedResource(device, pResource);
         if (FAILED(hr))
            DebugPrintf("DestroyResource: final shared publication failed hr=0x%08lx\n",
                        (unsigned long)hr);
      }
      Resource **entry = &device->shared_resources;
      while (*entry && *entry != pResource)
         entry = &(*entry)->shared_next;
      if (*entry)
         *entry = pResource->shared_next;
   }
   if (pResource->shared_staging_allocation) {
      D3DDDICB_DEALLOCATE deallocate = {};
      deallocate.NumAllocations = 1;
      deallocate.HandleList = &pResource->shared_staging_allocation;
      HRESULT hr = device->KTCallbacks.pfnDeallocateCb(device->hDevice, &deallocate);
      if (FAILED(hr))
         DebugPrintf("DestroyResource: staging deallocation failed hr=0x%08lx\n", (unsigned long)hr);
      pResource->shared_staging_allocation = 0;
   }

   /* Only free an allocation this device created. An opened resource holds a
    * view of another process's allocation and must not deallocate it. */
   if (pResource->hAllocation != 0 && pResource->hRTResourceHandle != NULL) {
      Device *pDevice = CastDevice(hDevice);
      D3DDDICB_DEALLOCATE deallocate;
      memset(&deallocate, 0, sizeof deallocate);
      /* With a resource handle set, the runtime frees the resource and every
       * allocation under it, so no handle list is passed. */
      deallocate.hResource = pResource->hRTResourceHandle;
      deallocate.NumAllocations = 0;
      deallocate.HandleList = NULL;
      pDevice->KTCallbacks.pfnDeallocateCb(pDevice->hDevice, &deallocate);
      pResource->hAllocation = 0;
      pResource->hKMResource = 0;
      pResource->hRTResourceHandle = NULL;
   }

   if (pResource->so_target) {
      pipe_so_target_reference(&pResource->so_target, NULL);
   }

   for (UINT SubResource = 0; SubResource < pResource->NumSubResources; ++SubResource) {
      if (pResource->transfers[SubResource]) {
         if (pResource->buffer) {
            pipe_buffer_unmap(pipe, pResource->transfers[SubResource]);
         } else {
            pipe_texture_unmap(pipe, pResource->transfers[SubResource]);
         }
         pResource->transfers[SubResource] = NULL;
      }
   }
   free(pResource->transfers);

   pipe_resource_reference(&pResource->resource, NULL);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceMap --
 *
 *    The ResourceMap function maps a subresource of a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceMap(D3D10DDI_HDEVICE hDevice,                                // IN
            D3D10DDI_HRESOURCE hResource,                            // IN
            UINT SubResource,                                        // IN
            D3D10_DDI_MAP DDIMap,                                    // IN
            UINT Flags,                                              // IN
            __out D3D10DDI_MAPPED_SUBRESOURCE *pMappedSubResource)   // OUT
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);
   struct pipe_resource *resource = pResource->resource;

   unsigned usage;
   switch (DDIMap) {
   case D3D10_DDI_MAP_READ:
      usage = PIPE_MAP_READ;
      break;
   case D3D10_DDI_MAP_READWRITE:
      usage = PIPE_MAP_READ | PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE:
      usage = PIPE_MAP_WRITE;
      break;
   case D3D10_DDI_MAP_WRITE_DISCARD:
      usage = PIPE_MAP_WRITE;
      if (resource->last_level == 0 && resource->array_size == 1) {
         usage |= PIPE_MAP_DISCARD_WHOLE_RESOURCE;
      } else {
         usage |= PIPE_MAP_DISCARD_RANGE;
      }
      break;
   case D3D10_DDI_MAP_WRITE_NOOVERWRITE:
      usage = PIPE_MAP_WRITE | PIPE_MAP_UNSYNCHRONIZED;
      break;
   default:
      assert(0);
      return;
   }

   assert(SubResource < pResource->NumSubResources);

   unsigned level;
   struct pipe_box box;
   subResourceBox(resource, SubResource, &level, &box);

   assert(!pResource->transfers[SubResource]);

   void *map;
   if (pResource->buffer) {
      map = pipe->buffer_map(pipe,
                             resource,
                             level,
                             usage,
                             &box,
                             &pResource->transfers[SubResource]);
   } else {
      map = pipe->texture_map(pipe,
                              resource,
                              level,
                              usage,
                              &box,
                              &pResource->transfers[SubResource]);
   }
   if (!map) {
      DebugPrintf("%s: failed to map resource\n", __func__);
      SetError(hDevice, E_FAIL);
      return;
   }

   pMappedSubResource->pData = map;
   pMappedSubResource->RowPitch = pResource->transfers[SubResource]->stride;
   pMappedSubResource->DepthPitch = pResource->transfers[SubResource]->layer_stride;
   if (pResource->buffer)
      DebugPrintf("buffer map: res=%p mode=%u usage=%x bytes=%u map=%p layers=%u levels=%u\n",
                  resource, (unsigned)DDIMap, usage, (unsigned)box.width,
                  map, resource->array_size, resource->last_level + 1);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceUnmap --
 *
 *    The ResourceUnmap function unmaps a subresource of a resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceUnmap(D3D10DDI_HDEVICE hDevice,      // IN
              D3D10DDI_HRESOURCE hResource,  // IN
              UINT SubResource)              // IN
{
   LOG_ENTRYPOINT();

   struct pipe_context *pipe = CastPipeContext(hDevice);
   Resource *pResource = CastResource(hResource);

   assert(SubResource < pResource->NumSubResources);

   if (pResource->transfers[SubResource]) {
      if (pResource->buffer) {
         pipe_buffer_unmap(pipe, pResource->transfers[SubResource]);
      } else {
         pipe_texture_unmap(pipe, pResource->transfers[SubResource]);
      }
      pResource->transfers[SubResource] = NULL;
   }
}


/*
 *----------------------------------------------------------------------
 *
 * areResourcesCompatible --
 *
 *      Check whether two resources can be safely passed to
 *      pipe_context::resource_copy_region method.
 *
 * Results:
 *      As above.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

static bool
areResourcesCompatible(const struct pipe_resource *src_resource, // IN
                       const struct pipe_resource *dst_resource) // IN
{
   if (src_resource->format == dst_resource->format) {
      /*
       * Trivial.
       */

      return true;
   } else if (src_resource->target == PIPE_BUFFER &&
              dst_resource->target == PIPE_BUFFER) {
      /*
       * Buffer resources are merely a collection of bytes.
       */

      return true;
   } else {
      /*
       * Check whether the formats are supported by
       * the resource_copy_region method.
       */

      const struct util_format_description *src_format_desc;
      const struct util_format_description *dst_format_desc;

      src_format_desc = util_format_description(src_resource->format);
      dst_format_desc = util_format_description(dst_resource->format);

      assert(src_format_desc->block.width  == dst_format_desc->block.width);
      assert(src_format_desc->block.height == dst_format_desc->block.height);
      assert(src_format_desc->block.bits   == dst_format_desc->block.bits);

      return util_is_format_compatible(src_format_desc, dst_format_desc);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceCopy --
 *
 *    The ResourceCopy function copies an entire source
 *    resource to a destination resource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceCopy(D3D10DDI_HDEVICE hDevice,          // IN
             D3D10DDI_HRESOURCE hDstResource,   // IN
             D3D10DDI_HRESOURCE hSrcResource)   // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   Resource *pSrcResource = CastResource(hSrcResource);
   struct pipe_resource *dst_resource = pDstResource->resource;
   struct pipe_resource *src_resource = pSrcResource->resource;
   bool compatible;

   HRESULT shared_hr = RefreshSharedResource(pDevice, pSrcResource);
   if (FAILED(shared_hr)) {
      SetError(hDevice, shared_hr);
      return;
   }
   MarkSharedResourceWritten(pDevice, dst_resource);

   assert(dst_resource->target == src_resource->target);
   assert(dst_resource->width0 == src_resource->width0);
   assert(dst_resource->height0 == src_resource->height0);
   assert(dst_resource->depth0 == src_resource->depth0);
   assert(dst_resource->last_level == src_resource->last_level);
   assert(dst_resource->array_size == src_resource->array_size);

   compatible = areResourcesCompatible(src_resource, dst_resource);
   if (pSrcResource->buffer)
      DebugPrintf("buffer copy: src=%p dst=%p bytes=%u layers=%u levels=%u compatible=%u\n",
                  src_resource, dst_resource, dst_resource->width0,
                  dst_resource->array_size, dst_resource->last_level + 1,
                  (unsigned)compatible);

   /* could also use one 3d copy for arrays */
   for (unsigned layer = 0; layer < dst_resource->array_size; ++layer) {
      for (unsigned level = 0; level <= dst_resource->last_level; ++level) {
         struct pipe_box box;
         box.x = 0;
         box.y = 0;
         box.z = 0 + layer;
         box.width  = u_minify(dst_resource->width0,  level);
         box.height = u_minify(dst_resource->height0, level);
         box.depth  = u_minify(dst_resource->depth0,  level);

	 if (compatible) {
            pipe->resource_copy_region(pipe,
                                       dst_resource, level,
                                       0, 0, layer,
                                       src_resource, level,
                                       &box);
         } else {
            util_resource_copy_region(pipe,
                                      dst_resource, level,
                                      0, 0, layer,
                                      src_resource, level,
                                      &box);
         }
      }
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceCopyRegion --
 *
 *    The ResourceCopyRegion function copies a source subresource
 *    region to a location on a destination subresource.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceCopyRegion(D3D10DDI_HDEVICE hDevice,                // IN
                   D3D10DDI_HRESOURCE hDstResource,         // IN
                   UINT DstSubResource,                     // IN
                   UINT DstX,                               // IN
                   UINT DstY,                               // IN
                   UINT DstZ,                               // IN
                   D3D10DDI_HRESOURCE hSrcResource,         // IN
                   UINT SrcSubResource,                     // IN
                   __in_opt const D3D10_DDI_BOX *pSrcBox)   // IN (optional)
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   Resource *pSrcResource = CastResource(hSrcResource);
   struct pipe_resource *dst_resource = pDstResource->resource;
   struct pipe_resource *src_resource = pSrcResource->resource;

   unsigned dst_level = DstSubResource % (dst_resource->last_level + 1);
   unsigned dst_layer = DstSubResource / (dst_resource->last_level + 1);
   unsigned src_level = SrcSubResource % (src_resource->last_level + 1);
   unsigned src_layer = SrcSubResource / (src_resource->last_level + 1);

   HRESULT shared_hr = RefreshSharedResource(pDevice, pSrcResource);
   if (SUCCEEDED(shared_hr))
      shared_hr = RefreshSharedResource(pDevice, pDstResource);
   if (FAILED(shared_hr)) {
      SetError(hDevice, shared_hr);
      return;
   }
   MarkSharedResourceWritten(pDevice, dst_resource);

   struct pipe_box src_box;
   if (pSrcBox) {
      src_box.x = pSrcBox->left;
      src_box.y = pSrcBox->top;
      src_box.z = pSrcBox->front + src_layer;
      src_box.width  = pSrcBox->right  - pSrcBox->left;
      src_box.height = pSrcBox->bottom - pSrcBox->top;
      src_box.depth  = pSrcBox->back   - pSrcBox->front;
   } else {
      src_box.x = 0;
      src_box.y = 0;
      src_box.z = 0 + src_layer;
      src_box.width  = u_minify(src_resource->width0,  src_level);
      src_box.height = u_minify(src_resource->height0, src_level);
      src_box.depth  = u_minify(src_resource->depth0,  src_level);
   }

   if (areResourcesCompatible(src_resource, dst_resource)) {
      pipe->resource_copy_region(pipe,
                                 dst_resource, dst_level,
                                 DstX, DstY, DstZ + dst_layer,
                                 src_resource, src_level,
                                 &src_box);
   } else {
      util_resource_copy_region(pipe,
                                dst_resource, dst_level,
                                DstX, DstY, DstZ + dst_layer,
                                src_resource, src_level,
                                &src_box);
   }
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceResolveSubResource --
 *
 *    The ResourceResolveSubResource function resolves
 *    multiple samples to one pixel.
 *
 * ----------------------------------------------------------------------
 */

static bool
resolveFormatCompatible(DXGI_FORMAT resource, DXGI_FORMAT typed)
{
   // A typed resource must match exactly. A typeless resource may use a
   // typed member of its DXGI family, not any format of the same block size.
   switch (resource) {
   case DXGI_FORMAT_R32G32B32A32_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R32G32B32A32_SINT;
   case DXGI_FORMAT_R32G32B32_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R32G32B32_SINT;
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R16G16B16A16_SINT;
   case DXGI_FORMAT_R32G32_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R32G32_SINT;
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R10G10B10A2_UINT;
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R8G8B8A8_SINT;
   case DXGI_FORMAT_R16G16_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R16G16_SINT;
   case DXGI_FORMAT_R32_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R32_SINT;
   case DXGI_FORMAT_R8G8_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R8G8_SINT;
   case DXGI_FORMAT_R16_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R16_SINT;
   case DXGI_FORMAT_R8_TYPELESS:
      return typed > resource && typed <= DXGI_FORMAT_R8_SINT;
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return typed == DXGI_FORMAT_B8G8R8A8_UNORM || typed == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
      return typed == DXGI_FORMAT_B8G8R8X8_UNORM || typed == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
   default:
      return typed == resource;
   }
}

void APIENTRY
ResourceResolveSubResource(D3D10DDI_HDEVICE hDevice,        // IN
                           D3D10DDI_HRESOURCE hDstResource, // IN
                           UINT DstSubResource,             // IN
                           D3D10DDI_HRESOURCE hSrcResource, // IN
                           UINT SrcSubResource,             // IN
                           DXGI_FORMAT ResolveFormat)       // IN
{
   LOG_ENTRYPOINT();
   Device *device = CastDevice(hDevice);
   Resource *src = CastResource(hSrcResource);
   Resource *dst = CastResource(hDstResource);
   const enum pipe_format format = FormatTranslate(ResolveFormat, false);
   if (!src || !dst || !src->resource || !dst->resource ||
       SrcSubResource >= src->NumSubResources || DstSubResource >= dst->NumSubResources ||
       src->buffer || dst->buffer || src->resource->nr_samples <= 1 ||
       dst->resource->nr_samples > 1 || format == PIPE_FORMAT_NONE ||
       util_format_is_depth_or_stencil(format) || util_format_is_pure_integer(format) ||
       util_format_is_compressed(format) ||
       !resolveFormatCompatible(src->Format, ResolveFormat) ||
       !resolveFormatCompatible(dst->Format, ResolveFormat)) {
      SetError(hDevice, E_INVALIDARG);
      return;
   }

   struct pipe_blit_info blit = {};
   subResourceBox(src->resource, SrcSubResource, &blit.src.level, &blit.src.box);
   subResourceBox(dst->resource, DstSubResource, &blit.dst.level, &blit.dst.box);
   if (blit.src.box.width != blit.dst.box.width ||
       blit.src.box.height != blit.dst.box.height ||
       blit.src.box.depth != 1 || blit.dst.box.depth != 1 ||
       util_format_get_blocksize(format) != util_format_get_blocksize(src->resource->format) ||
       util_format_get_blocksize(format) != util_format_get_blocksize(dst->resource->format)) {
      SetError(hDevice, E_INVALIDARG);
      return;
   }
   // Resolve is unpredicated. Preserve shared backing before touching one
   // subresource, and publish the resolved destination through normal ownership.
   HRESULT hr = RefreshSharedResource(device, src);
   if (SUCCEEDED(hr))
      hr = RefreshSharedResource(device, dst);
   if (FAILED(hr)) {
      SetError(hDevice, hr);
      return;
   }
   blit.src.resource = src->resource;
   blit.dst.resource = dst->resource;
   blit.src.format = blit.dst.format = format;
   blit.mask = PIPE_MASK_RGBA;
   blit.filter = PIPE_TEX_FILTER_NEAREST;
   device->pipe->blit(device->pipe, &blit);
   MarkSharedResourceWritten(device, dst->resource);
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceIsStagingBusy --
 *
 *    The ResourceIsStagingBusy function determines whether a
 *    resource is currently being used by the graphics pipeline.
 *
 * ----------------------------------------------------------------------
 */

BOOL APIENTRY
ResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice,       // IN
                      D3D10DDI_HRESOURCE hResource)   // IN
{
   LOG_ENTRYPOINT();

   /* ignore */

   return false;
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceReadAfterWriteHazard --
 *
 *    The ResourceReadAfterWriteHazard function informs the user-mode
 *    display driver that the specified resource was used as an output
 *    from the graphics processing unit (GPU) and that the resource
 *    will be used as an input to the GPU.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE hDevice,      // IN
                             D3D10DDI_HRESOURCE hResource)  // IN
{
   LOG_ENTRYPOINT();

   /* Not actually necessary */
}


/*
 * ----------------------------------------------------------------------
 *
 * ResourceUpdateSubResourceUP --
 *
 *    The ResourceUpdateSubresourceUP function updates a
 *    destination subresource region from a source
 *    system memory region.
 *
 * ----------------------------------------------------------------------
 */

void APIENTRY
ResourceUpdateSubResourceUP(D3D10DDI_HDEVICE hDevice,                // IN
                            D3D10DDI_HRESOURCE hDstResource,         // IN
                            UINT DstSubResource,                     // IN
                            __in_opt const D3D10_DDI_BOX *pDstBox,   // IN
                            __in const void *pSysMemUP,              // IN
                            UINT RowPitch,                           // IN
                            UINT DepthPitch)                         // IN
{
   LOG_ENTRYPOINT();

   Device *pDevice = CastDevice(hDevice);
   if (!CheckPredicate(pDevice)) {
      return;
   }

   struct pipe_context *pipe = pDevice->pipe;
   Resource *pDstResource = CastResource(hDstResource);
   struct pipe_resource *dst_resource = pDstResource->resource;

   HRESULT shared_hr = RefreshSharedResource(pDevice, pDstResource);
   if (FAILED(shared_hr)) {
      SetError(hDevice, shared_hr);
      return;
   }
   MarkSharedResourceWritten(pDevice, dst_resource);

   unsigned level;
   struct pipe_box box;

   if (pDstBox) {
      UINT DstMipLevels = dst_resource->last_level + 1;
      level = DstSubResource % DstMipLevels;
      unsigned dst_layer = DstSubResource / DstMipLevels;
      box.x = pDstBox->left;
      box.y = pDstBox->top;
      box.z = pDstBox->front + dst_layer;
      box.width  = pDstBox->right  - pDstBox->left;
      box.height = pDstBox->bottom - pDstBox->top;
      box.depth  = pDstBox->back   - pDstBox->front;
   } else {
      subResourceBox(dst_resource, DstSubResource, &level, &box);
   }

   struct pipe_transfer *transfer;
   void *map;
   if (pDstResource->buffer) {
      map = pipe->buffer_map(pipe,
                              dst_resource,
                              level,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box,
                              &transfer);
   } else {
      map = pipe->texture_map(pipe,
                              dst_resource,
                              level,
                              PIPE_MAP_WRITE | PIPE_MAP_DISCARD_RANGE,
                              &box,
                              &transfer);
   }
   assert(map);
   if (map) {
      for (int z = 0; z < box.depth; ++z) {
         uint8_t *dst = (uint8_t*)map + z*transfer->layer_stride;
         const uint8_t *src = (const uint8_t*)pSysMemUP + z*DepthPitch;
         util_copy_rect(dst,
                        dst_resource->format,
                        transfer->stride,
                        0, 0, box.width, box.height,
                        src,
                        RowPitch,
                        0, 0);
      }
      if (pDstResource->buffer) {
         pipe_buffer_unmap(pipe, transfer);
      } else {
         pipe_texture_unmap(pipe, transfer);
      }
   }
}
