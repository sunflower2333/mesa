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
 *
 **************************************************************************/

/*
 * Hardware backend for the D3D10 WDDM user-mode driver.
 *
 * The frontend in src/gallium/frontends/d3d10umd is winsys-agnostic: it asks
 * the target for one pipe_screen through d3d10_create_screen().  The swrast
 * target answers with llvmpipe or softpipe, which is what makes
 * libgallium_d3d10.dll a WARP-like software device.  This target answers with
 * zink instead, so the same frontend renders on whatever Vulkan driver the
 * loader resolves - on a Windows guest with the Turnip ICD registered, that is
 * the physical Adreno behind the virtio-gpu Native Context.
 *
 * Presentation still goes through the GDI software winsys, exactly as the
 * swrast target does: the D3D10 UMD DDI hands the frontend a D3DKMT_PRESENT
 * carrying an HWND, and zink reads the rendered surface back into it.  That
 * keeps this change confined to screen creation and leaves the frontend, the
 * present path, and the shipped software target untouched.
 */

#include "util/u_debug.h"
#include "target-helpers/inline_debug_helper.h"
#include "zink/zink_public.h"
#include "sw/gdi/gdi_sw_winsys.h"

#include "winddk_compat.h"
#include <d3dkmthk.h>

extern struct pipe_screen *
d3d10_create_screen(void);

static HDC
d3d10_zink_acquire_hdc(void *winsys_drawable_handle) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   return GetDC(hWnd);
}

static void
d3d10_zink_release_hdc(void *winsys_drawable_handle, HDC hDC) {
   D3DKMT_PRESENT *pPresentInfo = (D3DKMT_PRESENT *)winsys_drawable_handle;

   HWND hWnd = pPresentInfo->hWindow;
   ReleaseDC(hWnd, hDC);
}

struct pipe_screen *
d3d10_create_screen(void)
{
   struct pipe_screen *screen = NULL;
   struct sw_winsys *winsys;

   winsys = gdi_create_sw_winsys(d3d10_zink_acquire_hdc,
                                 d3d10_zink_release_hdc);
   if (!winsys)
      goto no_winsys;

   screen = zink_create_screen(winsys, NULL);
   if (screen == NULL)
      goto no_screen;

   return debug_screen_wrap(screen);

no_screen:
   winsys->destroy(winsys);
no_winsys:
   return NULL;
}
