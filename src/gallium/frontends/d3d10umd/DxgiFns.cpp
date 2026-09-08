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
 * DxgiFns.cpp --
 *    DXGI related functions.
 */

#include <stdio.h>

#include "DxgiFns.h"
#include "Resource.h"
#include "util/u_memory.h"
#ifndef UMDF_USING_NTSTATUS
#define UMDF_USING_NTSTATUS
#endif
#include <winternl.h>
#include <d3dkmthk.h>
#include "tu_wddm_abi.h"
#include "Format.h"
#include "State.h"

#include "Debug.h"

#include "util/format/u_format.h"


/*
 * ----------------------------------------------------------------------
 *
 * _Present --
 *
 *    This is turned into kernel callbacks rather than directly emitted
 *    as fifo packets.
 *
 * ----------------------------------------------------------------------
 */

/*
 * KmtEscape / PresentAdapter --
 *
 *    Reach the miniport the way turnip does, through the runtime thunks in
 *    gdi32 with a real adapter handle, rather than through the device callback
 *    table whose layout this build cannot verify.
 */

static PFND3DKMT_ESCAPE
KmtEscape(void)
{
   static PFND3DKMT_ESCAPE escape = NULL;
   static bool resolved = false;
   if (!resolved) {
      resolved = true;
      HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (gdi != NULL)
         escape = (PFND3DKMT_ESCAPE)GetProcAddress(gdi, "D3DKMTEscape");
   }
   return escape;
}

static D3DKMT_HANDLE
PresentAdapter(void)
{
   static D3DKMT_HANDLE adapter = 0;
   static bool resolved = false;
   if (resolved)
      return adapter;
   resolved = true;

   HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (gdi == NULL)
      return 0;
   PFND3DKMT_OPENADAPTERFROMHDC open_from_hdc =
      (PFND3DKMT_OPENADAPTERFROMHDC)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
   if (open_from_hdc == NULL)
      return 0;

   /* The adapter driving the primary display is the one scanning out. */
   HDC hdc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
   if (hdc == NULL)
      return 0;

   D3DKMT_OPENADAPTERFROMHDC open;
   memset(&open, 0, sizeof open);
   open.hDc = hdc;
   if (NT_SUCCESS(open_from_hdc(&open)))
      adapter = open.hAdapter;
   DeleteDC(hdc);
   return adapter;
}


/*
 * PublishPresentFrame --
 *
 *    Hand a finished frame to the display miniport.
 *
 *    This driver renders on the host, so the guest pages behind the back
 *    buffer are never written and the miniport's display path copies zeros
 *    no matter how it publishes them.  Read the frame back into a staging
 *    surface and send the pixels through the miniport's frame publication
 *    escape, which blits them into the surface it scans out.
 */
static void
PublishPresentFrame(struct Device *device, Resource *pSrcResource)
{
   struct pipe_resource *src = pSrcResource ? pSrcResource->resource : NULL;

   /* Publishing is opt-in until the callback contract is verified.  DWM faults
    * inside NDXGI::CDevice::SetPriorityCB -- another entry of this same
    * callback table -- on every present, while the miniport records no
    * publication arriving at all, which is what calling the wrong slot of
    * D3DDDI_DEVICECALLBACKS would look like.  Default to the behaviour that
    * kept the compositor alive. */
   static int enabled = -1;
   if (enabled < 0) {
      char buf[8];
      enabled = GetEnvironmentVariableA("VIOGPU_PRESENT_BRIDGE", buf, sizeof buf) > 0 &&
                buf[0] == '1';
   }

   if (!enabled || device->present_publish_failed || src == NULL) {
      return;
   }

   /* The miniport's scanout is B8G8R8A8; anything else would need a
    * conversion this path deliberately does not attempt. */
   if (src->format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       src->format != PIPE_FORMAT_B8G8R8X8_UNORM) {
      return;
   }

   struct pipe_context *pipe = device->pipe;
   struct pipe_screen *screen = pipe->screen;
   const unsigned width = src->width0;
   const unsigned height = src->height0;

   if (width == 0 || height == 0) {
      return;
   }

   if (device->present_staging != NULL &&
       (device->present_staging->width0 != width ||
        device->present_staging->height0 != height ||
        device->present_staging->format != src->format)) {
      pipe_resource_reference(&device->present_staging, NULL);
   }

   if (device->present_staging == NULL) {
      struct pipe_resource templat;
      memset(&templat, 0, sizeof templat);
      templat.target = PIPE_TEXTURE_2D;
      templat.format = src->format;
      templat.width0 = width;
      templat.height0 = height;
      templat.depth0 = 1;
      templat.array_size = 1;
      templat.last_level = 0;
      templat.nr_samples = 1;
      templat.usage = PIPE_USAGE_STAGING;
      templat.bind = PIPE_BIND_RENDER_TARGET;
      device->present_staging = screen->resource_create(screen, &templat);
      if (device->present_staging == NULL) {
         device->present_publish_failed = true;
         return;
      }
   }

   struct pipe_box box;
   memset(&box, 0, sizeof box);
   box.width = width;
   box.height = height;
   box.depth = 1;
   pipe->resource_copy_region(pipe, device->present_staging, 0, 0, 0, 0, src, 0, &box);

   struct pipe_transfer *transfer = NULL;
   void *map = pipe->texture_map(pipe, device->present_staging, 0, PIPE_MAP_READ,
                                 &box, &transfer);
   if (map == NULL) {
      device->present_publish_failed = true;
      return;
   }

   const unsigned rowBytes = width * 4;
   const unsigned payloadSize = rowBytes * height;
   const unsigned requestSize = sizeof(VIOGPU_WDDM_PRESENT_BLIT) + payloadSize;
   uint8_t *request = (uint8_t *)MALLOC(requestSize);

   if (request != NULL) {
      VIOGPU_WDDM_PRESENT_BLIT *blit = (VIOGPU_WDDM_PRESENT_BLIT *)request;
      memset(blit, 0, sizeof *blit);
      blit->Header.Magic = VIOGPU_WDDM_ABI_MAGIC;
      blit->Header.Version = VIOGPU_WDDM_ABI_VERSION;
      blit->Header.Size = sizeof *blit;
      blit->Opcode = VIOGPU_WDDM_ESCAPE_PRESENT_BLIT;
      blit->Flags = VIOGPU_WDDM_ESCAPE_FLAGS_NONE;
      blit->Width = width;
      blit->Height = height;
      blit->SourcePitch = rowBytes;
      blit->Format = VIOGPU_WDDM_FORMAT_B8G8R8A8_UNORM;
      blit->PayloadSize = payloadSize;

      uint8_t *payload = request + sizeof(VIOGPU_WDDM_PRESENT_BLIT);
      for (unsigned row = 0; row < height; ++row) {
         memcpy(payload + (size_t)row * rowBytes,
                (const uint8_t *)map + (size_t)row * transfer->stride,
                rowBytes);
      }

      /* Send this through the runtime's D3DKMTEscape rather than
       * KTCallbacks.pfnEscapeCb: calling that table entry faults inside
       * NDXGI::CDevice::SetPriorityCB -- a neighbouring callback -- and kills
       * the caller before the request ever reaches the miniport. */
      D3DKMT_ESCAPE escape;
      memset(&escape, 0, sizeof escape);
      escape.hAdapter = PresentAdapter();
      escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
      escape.pPrivateDriverData = request;
      escape.PrivateDriverDataSize = requestSize;

      NTSTATUS status = escape.hAdapter != 0 && KmtEscape() != NULL
                           ? KmtEscape()(&escape)
                           : STATUS_UNSUCCESSFUL;
      if (!NT_SUCCESS(status)) {
         /* One refusal is enough: the miniport either speaks this endpoint or
          * it does not, and retrying every frame would only add a readback. */
         device->present_publish_failed = true;
      }

      FREE(request);
   }

   pipe_texture_unmap(pipe, transfer);
}


HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);

   HRESULT hr = PublishSharedResources(device);
   if (SUCCEEDED(hr))
      hr = RefreshSharedResource(device, pSrcResource);
   if (FAILED(hr))
      return hr;
   device->pipe->flush(device->pipe, NULL, 0);
   PublishPresentFrame(device, pSrcResource);
   device->pipe->screen->flush_frontbuffer(device->pipe->screen, device->pipe, 
      pSrcResource->resource, 0, 0, pPresentData->pDXGIContext, 0, NULL);

   return S_OK;
}

HRESULT APIENTRY
_ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *resolve)
{
   LOG_ENTRYPOINT();
   Device *device = CastDevice(resolve->hDevice);
   HRESULT hr = PublishSharedResources(device);
   if (FAILED(hr))
      return hr;
   device->pipe->flush(device->pipe, NULL, 0);
   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _GetGammaCaps --
 *
 *    Return gamma capabilities.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_GetGammaCaps( DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *GetCaps )
{
   LOG_ENTRYPOINT();

   DXGI_GAMMA_CONTROL_CAPABILITIES *pCaps;

   pCaps = GetCaps->pGammaCapabilities;

   pCaps->ScaleAndOffsetSupported = false;
   pCaps->MinConvertedValue = 0.0;
   pCaps->MaxConvertedValue = 1.0;
   pCaps->NumGammaControlPoints = 17;

   for (UINT i = 0; i < pCaps->NumGammaControlPoints; i++) {
      pCaps->ControlPointPositions[i] = (float)i / (float)(pCaps->NumGammaControlPoints - 1);
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetDisplayMode --
 *
 *    Set the resource that is used to scan out to the display.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetDisplayMode( DXGI_DDI_ARG_SETDISPLAYMODE *SetDisplayMode )
{
   LOG_UNSUPPORTED_ENTRYPOINT();

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _SetResourcePriority --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_SetResourcePriority( DXGI_DDI_ARG_SETRESOURCEPRIORITY *SetResourcePriority )
{
   LOG_ENTRYPOINT();

   /* ignore */

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _QueryResourceResidency --
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_QueryResourceResidency( DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *QueryResourceResidency )
{
   LOG_ENTRYPOINT();

   for (UINT i = 0; i < QueryResourceResidency->Resources; ++i) {
      QueryResourceResidency->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   }

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _RotateResourceIdentities --
 *
 *    Rotate a list of resources by recreating their views with
 *    the updated rotations.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_RotateResourceIdentities( DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *RotateResourceIdentities )
{
   LOG_ENTRYPOINT();

   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }

   struct pipe_context *pipe = CastPipeDevice(RotateResourceIdentities->hDevice);
   struct pipe_screen *screen = pipe->screen;

   struct pipe_resource *resource0 = CastPipeResource(RotateResourceIdentities->pResources[0]);

   assert(resource0);
   LOG_UNSUPPORTED(resource0->last_level);

   /*
    * XXX: Copying is not very efficient, but it is much simpler than the
    * alternative of recreating all views.
    */

   struct pipe_resource *temp_resource;
   temp_resource = screen->resource_create(screen, resource0);
   assert(temp_resource);
   if (!temp_resource) {
      return E_OUTOFMEMORY;
   }

   struct pipe_box src_box;
   src_box.x = 0;
   src_box.y = 0;
   src_box.z = 0;
   src_box.width  = resource0->width0;
   src_box.height = resource0->height0;
   src_box.depth  = resource0->depth0;

   for (UINT i = 0; i < RotateResourceIdentities->Resources + 1; ++i) {
      struct pipe_resource *src_resource;
      struct pipe_resource *dst_resource;

      if (i < RotateResourceIdentities->Resources) {
         src_resource = CastPipeResource(RotateResourceIdentities->pResources[i]);
      } else {
         src_resource = temp_resource;
      }

      if (i > 0) {
         dst_resource = CastPipeResource(RotateResourceIdentities->pResources[i - 1]);
      } else {
         dst_resource = temp_resource;
      }

      assert(dst_resource);
      assert(src_resource);

      pipe->resource_copy_region(pipe,
                                 dst_resource,
                                 0, // dst_level
                                 0, 0, 0, // dst_x,y,z
                                 src_resource,
                                 0, // src_level
                                 &src_box);
   }

   pipe_resource_reference(&temp_resource, NULL);

   return S_OK;
}


/*
 * ----------------------------------------------------------------------
 *
 * _Blt --
 *
 *    Do a blt between two subresources. Apply MSAA resolve, format
 *    conversion and stretching.
 *
 * ----------------------------------------------------------------------
 */

HRESULT APIENTRY
_Blt(DXGI_DDI_ARG_BLT *Blt)
{
   LOG_UNSUPPORTED_ENTRYPOINT();

   return S_OK;
}
