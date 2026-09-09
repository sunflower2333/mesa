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
PresentAdapter(bool reopen)
{
   static D3DKMT_HANDLE adapter = 0;
   static bool resolved = false;
   if (reopen) {
      /* Restarting the display device invalidates this handle. Holding the
       * first one for the life of the process turns a driver reinstall into a
       * desktop that never publishes another frame. */
      if (adapter != 0) {
         HMODULE gdiClose = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
         if (gdiClose != NULL) {
            PFND3DKMT_CLOSEADAPTER close_adapter =
               (PFND3DKMT_CLOSEADAPTER)GetProcAddress(gdiClose, "D3DKMTCloseAdapter");
            if (close_adapter != NULL) {
               D3DKMT_CLOSEADAPTER closeArgs;
               memset(&closeArgs, 0, sizeof closeArgs);
               closeArgs.hAdapter = adapter;
               close_adapter(&closeArgs);
            }
         }
      }
      adapter = 0;
      resolved = false;
   }
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
static HRESULT
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

   if (!enabled || src == NULL) {
      return S_OK;
   }

   /* A failed publication used to disable this path for the life of the
    * device. The commonest cause -- a stale adapter handle after the display
    * device restarts -- is recoverable, and latching on it leaves the desktop
    * frozen on whatever was last published. Back off and try again instead,
    * re-opening the handle each time, so a transient failure costs frames
    * rather than the display. */
   if (device->present_publish_retry > 0) {
      --device->present_publish_retry;
      return S_OK;
   }

   /* The miniport's scanout is B8G8R8A8; anything else would need a
    * conversion this path deliberately does not attempt. */
   if (src->format != PIPE_FORMAT_B8G8R8A8_UNORM &&
       src->format != PIPE_FORMAT_B8G8R8X8_UNORM) {
      return S_OK;
   }

   struct pipe_context *pipe = device->pipe;
   struct pipe_screen *screen = pipe->screen;
   const unsigned width = src->width0;
   const unsigned height = src->height0;

   if (width == 0 || height == 0) {
      return S_OK;
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
      /* The readback has to land somewhere the host writes and the guest can
       * read.  A tiled staging surface is not that: the driver resolves it
       * through a buffer the host never fills, so every frame read back as
       * zeroes and the miniport published a black desktop while reporting
       * success on full-size frames.  A linear surface is host-visible, and
       * its pixels are the real ones. */
      templat.bind = PIPE_BIND_LINEAR;
      device->present_staging = screen->resource_create(screen, &templat);
      if (device->present_staging == NULL) {
         return E_OUTOFMEMORY;
      }
   }

   const unsigned rowBytes = width * 4;
   const unsigned payloadSize = rowBytes * height;
   const unsigned requestSize = sizeof(VIOGPU_WDDM_PRESENT_BLIT) + payloadSize;

   /* Keep the request buffer for the life of the device.  A frame is several
    * megabytes, and taking it from the heap and giving it back on every
    * present cost more than the blit it carries. */
   if (device->present_request != NULL &&
       device->present_request_size != requestSize) {
      FREE(device->present_request);
      device->present_request = NULL;
      device->present_request_size = 0;
   }

   if (device->present_request == NULL) {
      device->present_request = (uint8_t *)MALLOC(requestSize);
      if (device->present_request == NULL) {
         return E_OUTOFMEMORY;
      }
      device->present_request_size = requestSize;
   }

   LARGE_INTEGER readbackFrequency;
   LARGE_INTEGER readbackStart;
   QueryPerformanceFrequency(&readbackFrequency);
   QueryPerformanceCounter(&readbackStart);

   struct pipe_box box;
   memset(&box, 0, sizeof box);
   box.width = width;
   box.height = height;
   box.depth = 1;

   /* A Present owns this source's current contents. Publishing the previous
    * device-global slot loses the first and final frames, and can mix two
    * swapchains of equal size. Complete this copy before reading the same
    * staging surface. Use an explicit finite fence wait and a nonblocking map
    * so a failed host submission cannot hang the compositor in texture_map.
    * The bridge remains a correctness fallback pending standard DXGI output;
    * a timeout must be reported, never counted as a successful publication. */
   pipe->resource_copy_region(pipe, device->present_staging, 0, 0, 0, 0,
                              src, 0, &box);
   struct pipe_fence_handle *fence = NULL;
   pipe->flush(pipe, &fence, 0);
   if (fence == NULL) {
      return E_FAIL;
   }
   const bool ready = screen->fence_finish(screen, pipe, fence, 250000000ull);
   screen->fence_reference(screen, &fence, NULL);
   if (!ready)
      return DXGI_ERROR_WAS_STILL_DRAWING;

   struct pipe_transfer *transfer = NULL;
   void *map = pipe->texture_map(pipe, device->present_staging, 0,
                                 PIPE_MAP_READ | PIPE_MAP_DONTBLOCK, &box,
                                 &transfer);
   if (map == NULL) {
      /* Reserve adapter backoff for an escape that actually failed. */
      return DXGI_ERROR_WAS_STILL_DRAWING;
   }

   uint8_t *request = device->present_request;
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
   if (transfer->stride == rowBytes) {
      memcpy(payload, map, payloadSize);
   } else {
      for (unsigned row = 0; row < height; ++row) {
         memcpy(payload + (size_t)row * rowBytes,
                (const uint8_t *)map + (size_t)row * transfer->stride,
                rowBytes);
      }
   }

   /* Release the mapping before the escape: the miniport copies the whole
    * frame twice more on the other side of it, and nothing there needs the
    * resource to stay mapped. */
   pipe_texture_unmap(pipe, transfer);

   LARGE_INTEGER readbackEnd;
   QueryPerformanceCounter(&readbackEnd);
   /* Tell the miniport how long the host took to hand this frame back.  It
    * publishes the value next to its own timing, which is the only way to say
    * which side of the escape a slow frame was spent on. */
   blit->ReadbackUsec =
      readbackFrequency.QuadPart > 0
         ? (uint32_t)(((readbackEnd.QuadPart - readbackStart.QuadPart) * 1000000LL) /
                      readbackFrequency.QuadPart)
         : 0;

   /* Send this through the runtime's D3DKMTEscape rather than
    * KTCallbacks.pfnEscapeCb: calling that table entry faults inside
    * NDXGI::CDevice::SetPriorityCB -- a neighbouring callback -- and kills
    * the caller before the request ever reaches the miniport. */
   D3DKMT_ESCAPE escape;
   memset(&escape, 0, sizeof escape);
   escape.hAdapter = PresentAdapter(false);
   escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
   escape.pPrivateDriverData = request;
   escape.PrivateDriverDataSize = requestSize;

   NTSTATUS status = escape.hAdapter != 0 && KmtEscape() != NULL
                        ? KmtEscape()(&escape)
                        : STATUS_UNSUCCESSFUL;
   if (!NT_SUCCESS(status)) {
      /* Re-open the adapter: the likeliest reason the escape never reached the
       * miniport is that the device restarted under us. Back off so a miniport
       * that genuinely does not speak this endpoint costs one readback per
       * backoff period rather than one per frame. */
      PresentAdapter(true);
      const unsigned doubled = device->present_publish_backoff * 2u;
      device->present_publish_backoff = device->present_publish_backoff == 0u ? 8u
                                        : (doubled > 240u ? 240u : doubled);
      device->present_publish_retry = device->present_publish_backoff;
   } else {
      device->present_publish_backoff = 0;
   }
   return S_OK;
}


static HRESULT
RecordRuntimePresent(Device *device, const DXGI_DDI_ARG_PRESENT *present,
                     Resource *src, Resource *dst, const char *stage, HRESULT hr,
                     ULONGLONG started)
{
   const unsigned sample = device->runtime_present_count;
   // Bounded startup/error sampling: DWM has no console for stderr, and a
   // file write per frame would contaminate the responsiveness measurement.
   if (sample <= 16 || (sample % 64) == 0) {
      HANDLE log = CreateFileA("C:\\Users\\Public\\umd_dxgi.log", FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
      if (log != INVALID_HANDLE_VALUE) {
         char line[384];
         int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                               "present tick=%llu pid=%lu n=%u stage=%s src=0x%x dst=0x%x "
                               "size=%ux%u flags=0x%x hr=0x%08lx elapsed=%llums\r\n",
                               GetTickCount64(), GetCurrentProcessId(), sample, stage,
                               src ? src->hAllocation : 0, dst ? dst->hAllocation : 0,
                               src && src->resource ? src->resource->width0 : 0,
                               src && src->resource ? src->resource->height0 : 0,
                               present->Flags.Value, (unsigned long)hr, GetTickCount64() - started);
         DWORD written;
         if (len > 0)
            WriteFile(log, line, (DWORD)len, &written, NULL);
         CloseHandle(log);
      }
   }
   return hr;
}

HRESULT APIENTRY
_Present(DXGI_DDI_ARG_PRESENT *pPresentData)
{

   LOG_ENTRYPOINT();

   struct Device *device = CastDevice(pPresentData->hDevice);
   Resource *pSrcResource = CastResource(pPresentData->hSurfaceToPresent);
   Resource *pDstResource = CastResource(pPresentData->hDstResource);

   if (device->runtime_present) {
      const ULONGLONG started = GetTickCount64();
      ++device->runtime_present_count;
      if (!device->pDXGIBaseCallbacks || !device->pDXGIBaseCallbacks->pfnPresentCb ||
          !pSrcResource || pPresentData->SrcSubResourceIndex != 0 ||
          pPresentData->DstSubResourceIndex != 0 ||
          (pDstResource && !pDstResource->hAllocation))
         return RecordRuntimePresent(device, pPresentData, pSrcResource, pDstResource,
                                     "validate", DXGI_DDI_ERR_UNSUPPORTED, started);
      device->pipe->flush(device->pipe, NULL, 0);
      HRESULT hr = PreparePresentResource(device, pSrcResource);
      if (FAILED(hr))
         return RecordRuntimePresent(device, pPresentData, pSrcResource, pDstResource,
                                     "prepare", hr, started);

      DXGIDDICB_PRESENT present = {};
      present.hSrcAllocation = pSrcResource->hAllocation;
      present.hDstAllocation = pDstResource ? pDstResource->hAllocation : 0;
      present.pDXGIContext = pPresentData->pDXGIContext;
      // Use the same VidSch context that submitted the host-to-allocation copy.
      present.hContext = device->shared_copy_context.hContext;
      hr = device->pDXGIBaseCallbacks->pfnPresentCb(device->hDevice, &present);
      const unsigned sample = device->runtime_present_count;
      if (sample <= 16 || FAILED(hr)) {
         fprintf(stderr, "DXGI runtime Present n=%u src=0x%x dst=0x%x flags=0x%x hr=0x%08lx\n",
                 sample, present.hSrcAllocation, present.hDstAllocation,
                 pPresentData->Flags.Value, (unsigned long)hr);
      }
      // An explicit destination belongs to the runtime's composition path.
      // Its pixels must never be sent straight to the global scanout escape.
      if (SUCCEEDED(hr) && !pDstResource && !pSrcResource->scanout_primary)
         hr = PublishPresentFrame(device, pSrcResource);
      return RecordRuntimePresent(device, pPresentData, pSrcResource, pDstResource,
                                  "callback-and-publication", hr, started);
   }

   /* Split the present into its three parts and report a running average.
    * The scanout publication and the shared transfer together account for
    * well under half the frame, so the rest is in here somewhere. */
   LARGE_INTEGER presentFreq, tEnter, tFlush, tShared, tPublish, tFrontbuffer;
   QueryPerformanceFrequency(&presentFreq);
   QueryPerformanceCounter(&tEnter);

   device->pipe->flush(device->pipe, NULL, 0);
   QueryPerformanceCounter(&tFlush);
   HRESULT hr = pSrcResource->shared_dirty
                   ? PublishSharedResource(device, pSrcResource)
                   : RefreshSharedResource(device, pSrcResource);
   QueryPerformanceCounter(&tShared);
   if (FAILED(hr))
      return hr;
   hr = PublishPresentFrame(device, pSrcResource);
   if (FAILED(hr))
      return hr;
   QueryPerformanceCounter(&tPublish);
   device->pipe->screen->flush_frontbuffer(device->pipe->screen, device->pipe,
      pSrcResource->resource, 0, 0, pPresentData->pDXGIContext, 0, NULL);
   QueryPerformanceCounter(&tFrontbuffer);
   {
      static volatile LONG64 flush_usec, shared_usec, publish_usec, frontbuffer_usec;
      static volatile LONG present_samples;
      const LONGLONG hz = presentFreq.QuadPart > 0 ? presentFreq.QuadPart : 1;
      const LONG64 f = ((tFlush.QuadPart - tEnter.QuadPart) * 1000000LL) / hz;
      const LONG64 s2 = ((tShared.QuadPart - tFlush.QuadPart) * 1000000LL) / hz;
      const LONG64 p2 = ((tPublish.QuadPart - tShared.QuadPart) * 1000000LL) / hz;
      const LONG64 fb = ((tFrontbuffer.QuadPart - tPublish.QuadPart) * 1000000LL) / hz;
      const LONG64 tf = InterlockedAdd64(&flush_usec, f);
      const LONG64 ts = InterlockedAdd64(&shared_usec, s2);
      const LONG64 tp = InterlockedAdd64(&publish_usec, p2);
      const LONG64 tfb = InterlockedAdd64(&frontbuffer_usec, fb);
      const LONG n = InterlockedIncrement(&present_samples);
      const bool slow = f + s2 + p2 + fb >= 100000;
      if ((n % 64) == 0 || slow) {
         HANDLE log = CreateFileA("C:\\Users\\Public\\umd_timing.log",
                                  FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
         if (log != INVALID_HANDLE_VALUE) {
            char line[320];
            int len = _snprintf_s(
               line, sizeof line, _TRUNCATE,
               "present tick=%llu pid=%lu tid=%lu n=%ld slow=%u last=%lld/%lld/%lld/%lldus "
               "mean=%lld/%lld/%lld/%lldus\r\n",
               GetTickCount64(), GetCurrentProcessId(), GetCurrentThreadId(), n, slow ? 1u : 0u,
               f, s2, p2, fb, tf / n, ts / n, tp / n, tfb / n);
            DWORD written = 0;
            if (len > 0) WriteFile(log, line, (DWORD)len, &written, NULL);
            CloseHandle(log);
         }
      }
   }

   return S_OK;
}

HRESULT APIENTRY
_ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *resolve)
{
   LOG_ENTRYPOINT();
   Device *device = CastDevice(resolve->hDevice);
   Resource *resource = CastResource(resolve->hResource);
   device->pipe->flush(device->pipe, NULL, 0);
   return PublishSharedResource(device, resource);
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
   LOG_ENTRYPOINT();
   if (!SetDisplayMode || SetDisplayMode->SubResourceIndex != 0)
      return E_INVALIDARG;
   Device *device = CastDevice(SetDisplayMode->hDevice);
   Resource *resource = CastResource(SetDisplayMode->hResource);
   if (!device || !resource || !resource->scanout_primary || !resource->hAllocation ||
       !device->KTCallbacks.pfnSetDisplayModeCb)
      return DXGI_DDI_ERR_UNSUPPORTED;
   D3DDDICB_SETDISPLAYMODE mode = {};
   mode.hPrimaryAllocation = resource->hAllocation;
   HRESULT hr = device->KTCallbacks.pfnSetDisplayModeCb(device->hDevice, &mode);
   static volatile LONG modeCount;
   if (InterlockedIncrement(&modeCount) <= 16) {
      HANDLE log = CreateFileA("C:\\Users\\Public\\umd_dxgi.log", FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
      if (log != INVALID_HANDLE_VALUE) {
         char line[160];
         int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                               "set-mode pid=%lu allocation=0x%x hr=0x%08lx\r\n",
                               GetCurrentProcessId(), resource->hAllocation, (unsigned long)hr);
         DWORD written;
         if (len > 0)
            WriteFile(log, line, (DWORD)len, &written, NULL);
         CloseHandle(log);
      }
   }
   return hr;
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

   if (!RotateResourceIdentities || !RotateResourceIdentities->hDevice ||
       (RotateResourceIdentities->Resources && !RotateResourceIdentities->pResources))
      return E_INVALIDARG;
   if (RotateResourceIdentities->Resources <= 1) {
      return S_OK;
   }

   Device *device = CastDevice(RotateResourceIdentities->hDevice);
   struct pipe_context *pipe = device->pipe;
   struct pipe_screen *screen = pipe->screen;
   Resource *first = CastResource(RotateResourceIdentities->pResources[0]);
   if (!first || !first->resource)
      return E_INVALIDARG;
   const ULONGLONG started = GetTickCount64();
   unsigned dirtyBefore = 0;
   // Kernel-backed back buffers must rotate as a homogeneous set. Validate
   // before copying pixels or changing any allocation identity.
   for (UINT i = 0; i < RotateResourceIdentities->Resources; ++i) {
      Resource *current = CastResource(RotateResourceIdentities->pResources[i]);
      if (!current || !current->resource ||
          bool(current->hAllocation) != bool(first->hAllocation))
         return E_INVALIDARG;
      dirtyBefore += current->shared_dirty;
      if (first->hAllocation &&
          (current->resource->target != PIPE_TEXTURE_2D || current->MipLevels != 1 ||
           current->resource->array_size != 1 || current->Format != first->Format ||
           current->resource->width0 != first->resource->width0 ||
           current->resource->height0 != first->resource->height0 ||
           current->shared_pitch != first->shared_pitch ||
           current->scanout_primary != first->scanout_primary))
         return DXGI_DDI_ERR_UNSUPPORTED;
   }

   for (UINT i = 0; i < RotateResourceIdentities->Resources; ++i) {
      HRESULT hr = RefreshSharedResource(device,
                                        CastResource(RotateResourceIdentities->pResources[i]));
      if (FAILED(hr))
         return hr;
   }

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

   // DXGI rotates kernel identities, while the runtime handles stay attached
   // to their Resource objects. Pixel copies above preserve existing views.
   const D3DKMT_HANDLE firstAllocation = first->hAllocation;
   const D3DKMT_HANDLE firstKMResource = first->hKMResource;
   const bool firstDirty = first->shared_dirty;
   // The copied GPU contents and the kernel allocation rotate together. A
   // clean source already matches that allocation; copying it into another
   // cache is not a new write to publish back through VidSch. Pending writes
   // must follow their original allocation instead of making every buffer
   // dirty and forcing redundant full-frame GPU readbacks on the next Flush.
   for (UINT i = 0; i + 1 < RotateResourceIdentities->Resources; ++i) {
      Resource *current = CastResource(RotateResourceIdentities->pResources[i]);
      Resource *next = CastResource(RotateResourceIdentities->pResources[i + 1]);
      current->hAllocation = next->hAllocation;
      current->hKMResource = next->hKMResource;
      current->shared_dirty = next->shared_dirty;
   }
   Resource *last = CastResource(RotateResourceIdentities->pResources[RotateResourceIdentities->Resources - 1]);
   last->hAllocation = firstAllocation;
   last->hKMResource = firstKMResource;
   last->shared_dirty = firstDirty;

   static volatile LONG rotations;
   const LONG rotation = InterlockedIncrement(&rotations);
   if (rotation <= 8 || (rotation % 64) == 0) {
      HANDLE log = CreateFileA("C:\\Users\\Public\\umd_dxgi.log", FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
      if (log != INVALID_HANDLE_VALUE) {
         char line[192];
         int len = _snprintf_s(line, sizeof line, _TRUNCATE,
                              "rotate tick=%llu pid=%lu n=%ld buffers=%u dirty=%u elapsed=%llums\r\n",
                              GetTickCount64(), GetCurrentProcessId(), rotation,
                              RotateResourceIdentities->Resources, dirtyBefore,
                              GetTickCount64() - started);
         DWORD written;
         if (len > 0) WriteFile(log, line, (DWORD)len, &written, NULL);
         CloseHandle(log);
      }
   }

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
   LOG_ENTRYPOINT();

   if (!Blt)
      return E_INVALIDARG;
   Device *device = CastDevice(Blt->hDevice);
   Resource *src = CastResource(Blt->hSrcResource);
   Resource *dst = CastResource(Blt->hDstResource);
   if (!device || !src || !dst || !src->resource || !dst->resource ||
       src->resource == dst->resource ||
       Blt->SrcSubresource >= src->NumSubResources ||
       Blt->DstSubresource >= dst->NumSubResources)
      return E_INVALIDARG;
   if ((src->resource->target != PIPE_TEXTURE_2D && src->resource->target != PIPE_TEXTURE_2D_ARRAY) ||
       (dst->resource->target != PIPE_TEXTURE_2D && dst->resource->target != PIPE_TEXTURE_2D_ARRAY) ||
       (Blt->Flags.Value & ~0xfu) ||
       (Blt->Rotate != DXGI_DDI_MODE_ROTATION_IDENTITY &&
        Blt->Rotate != DXGI_DDI_MODE_ROTATION_ROTATE180))
      return DXGI_DDI_ERR_UNSUPPORTED;

   const unsigned srcLevel = Blt->SrcSubresource % src->MipLevels;
   const unsigned dstLevel = Blt->DstSubresource % dst->MipLevels;
   const unsigned srcWidth = u_minify(src->resource->width0, srcLevel);
   const unsigned srcHeight = u_minify(src->resource->height0, srcLevel);
   const unsigned dstWidth = u_minify(dst->resource->width0, dstLevel);
   const unsigned dstHeight = u_minify(dst->resource->height0, dstLevel);
   if (Blt->DstLeft >= Blt->DstRight || Blt->DstTop >= Blt->DstBottom ||
       Blt->DstRight > dstWidth || Blt->DstBottom > dstHeight ||
       (uint64_t)Blt->DstLeft > dstWidth || (uint64_t)Blt->DstTop > dstHeight)
      return E_INVALIDARG;

   HRESULT hr = RefreshSharedResource(device, src);
   // Preserve pixels outside a partial destination rectangle. A full overwrite
   // has no dependency on the old destination contents.
   if (SUCCEEDED(hr) && (Blt->DstLeft || Blt->DstTop ||
                        Blt->DstRight != dstWidth || Blt->DstBottom != dstHeight))
      hr = RefreshSharedResource(device, dst);
   if (FAILED(hr))
      return hr;

   struct pipe_blit_info info = {};
   info.src.resource = src->resource;
   info.src.level = srcLevel;
   info.src.format = util_format_linear(src->resource->format);
   info.src.box.z = Blt->SrcSubresource / src->MipLevels;
   info.src.box.width = srcWidth;
   info.src.box.height = srcHeight;
   info.src.box.depth = 1;
   if (Blt->Rotate == DXGI_DDI_MODE_ROTATION_ROTATE180) {
      info.src.box.x = srcWidth;
      info.src.box.y = srcHeight;
      info.src.box.width = -(int)srcWidth;
      info.src.box.height = -(int)srcHeight;
   }
   info.dst.resource = dst->resource;
   info.dst.level = dstLevel;
   info.dst.format = util_format_linear(dst->resource->format);
   info.dst.box.x = Blt->DstLeft;
   info.dst.box.y = Blt->DstTop;
   info.dst.box.z = Blt->DstSubresource / dst->MipLevels;
   info.dst.box.width = Blt->DstRight - Blt->DstLeft;
   info.dst.box.height = Blt->DstBottom - Blt->DstTop;
   info.dst.box.depth = 1;
   info.mask = PIPE_MASK_RGBA;
   info.filter = PIPE_TEX_FILTER_LINEAR;
   // DXGI requires a single resolve/convert/stretch pass for presentation.
   // sRGB source bits are preserved rather than decoded into a linear target.
   device->pipe->blit(device->pipe, &info);
   MarkSharedResourceWritten(device, dst->resource);

   // The destination can be DWM's shared composition surface, not scanout.
   // Publish its allocation contents without invoking the display bridge.
   if (Blt->Flags.Present)
      hr = PublishSharedResource(device, dst);

   static volatile LONG count;
   const LONG sample = InterlockedIncrement(&count);
   if (sample <= 16 || (sample % 64) == 0) {
      fprintf(stderr, "DXGI Blt pid=%lu n=%ld src=%p dst=%p %ux%u->%ux%u flags=0x%x hr=0x%08lx\n",
              GetCurrentProcessId(), sample, src, dst, srcWidth, srcHeight,
              info.dst.box.width, info.dst.box.height, Blt->Flags.Value, (unsigned long)hr);
   }
   return hr;
}
