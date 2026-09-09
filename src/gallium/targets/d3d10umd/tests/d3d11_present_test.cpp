/*
 * A real Direct3D 11 render-and-present test.
 *
 * Creating a device at a feature level proves only that the adapter answered.
 * The desktop needs the GPU to actually draw and the result to reach the
 * display, so this opens a real window, renders through a real DXGI swapchain
 * and presents.  Each frame is read back from the back buffer, which shows
 * whether the GPU wrote the pixels regardless of what reaches the screen, and
 * the colours are distinct per frame so a stale frame cannot pass.
 */

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dcomp.h>
#include <stdio.h>

static const struct { float r, g, b; unsigned char br, bg, bb; const char *name; } kFrames[] = {
   {1.0f, 0.0f, 1.0f, 255, 0, 255, "magenta"},
   {0.0f, 1.0f, 0.0f, 0, 255, 0, "green"},
   {0.0f, 0.0f, 1.0f, 0, 0, 255, "blue"},
   {1.0f, 1.0f, 0.0f, 255, 255, 0, "yellow"},
};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
   if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
   return DefWindowProcW(hwnd, msg, wp, lp);
}

static void PumpMessages(void)
{
   MSG msg;
   while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
}

static void Step(const char *what)
{
   printf("[step] %s\n", what);
   fflush(stdout);
}

int main(int argc, char **argv)
{
   /* A crash here loses buffered output entirely, which is how this test
    * previously exited with an empty file.  Report every step as it happens. */
   setvbuf(stdout, NULL, _IONBF, 0);

   int width = 640, height = 480, frames = (int)(sizeof kFrames / sizeof kFrames[0]);
   bool fullscreenSize = false;
   int fpsSeconds = 0;
   for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--full")) fullscreenSize = true;
      /* Sustained present rate, which is the number the display path is judged
       * on.  The staged frames below sleep between presents, so they measure
       * correctness and say nothing about throughput. */
      else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fpsSeconds = atoi(argv[++i]);
   }

   if (fullscreenSize) {
      width = GetSystemMetrics(SM_CXSCREEN);
      height = GetSystemMetrics(SM_CYSCREEN);
      if (width <= 0 || height <= 0) { width = 1280; height = 1024; }
   }

   printf("session: window %dx%d\n", width, height);

   Step("register window class");
   WNDCLASSEXW wc = {};
   wc.cbSize = sizeof wc;
   wc.lpfnWndProc = WndProc;
   wc.hInstance = GetModuleHandleW(NULL);
   wc.lpszClassName = L"VioGpuPresentTest";
   if (!RegisterClassExW(&wc)) { printf("RESULT: FAIL - RegisterClassEx %lu\n", GetLastError()); return 2; }

   Step("create window");
   HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VioGPU present test",
                               WS_POPUP | WS_VISIBLE, 0, 0, width, height,
                               NULL, NULL, wc.hInstance, NULL);
   if (hwnd == NULL) { printf("RESULT: FAIL - CreateWindowEx %lu\n", GetLastError()); return 2; }
   ShowWindow(hwnd, SW_SHOW);
   UpdateWindow(hwnd);
   PumpMessages();

   Step("describe swapchain");
   /* Residency probe: the runtime tracks each resource by the kernel allocation
    * the user-mode driver is supposed to have created for it.  If none was ever
    * created, SetEvictionPriority drives the runtime into
    * NDXGI::CDevice::SetPriorityCB against an uninitialised handle.  Do this on
    * a device with no swapchain so the two failures cannot be confused. */
   if (!fullscreenSize) {
      ID3D11Device *probeDev = NULL;
      ID3D11DeviceContext *probeCtx = NULL;
      D3D_FEATURE_LEVEL probeFl = (D3D_FEATURE_LEVEL)0;
      const D3D_FEATURE_LEVEL probeWanted[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
      Step("residency: create device");
      HRESULT phr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                      probeWanted, (UINT)(sizeof probeWanted / sizeof probeWanted[0]),
                                      D3D11_SDK_VERSION, &probeDev, &probeFl, &probeCtx);
      printf("[step] residency: device hr=0x%08lX\n", (unsigned long)phr);
      if (SUCCEEDED(phr)) {
         D3D11_TEXTURE2D_DESC td = {};
         td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1;
         td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
         td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
         ID3D11Texture2D *tex = NULL;
         Step("residency: create texture");
         phr = probeDev->CreateTexture2D(&td, NULL, &tex);
         printf("[step] residency: texture hr=0x%08lX\n", (unsigned long)phr);
         if (SUCCEEDED(phr)) {
            IDXGIResource *dxgiRes = NULL;
            phr = tex->QueryInterface(__uuidof(IDXGIResource), (void **)&dxgiRes);
            printf("[step] residency: QueryInterface hr=0x%08lX\n", (unsigned long)phr);
            if (SUCCEEDED(phr)) {
               Step("residency: SetEvictionPriority  <-- expected fault point");
               phr = dxgiRes->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_MAXIMUM);
               printf("[step] residency: SetEvictionPriority hr=0x%08lX (survived)\n", (unsigned long)phr);
               dxgiRes->Release();
            }
            tex->Release();
         }
         /* DirectComposition builds its surfaces as shared textures, and
          * LogonUI/dwm fault inside DCompSurface::InitializeSurface on this
          * driver. Exercise the same shapes so the failing one is named. */
         struct { UINT misc; const char *name; } kShared[] = {
            { D3D11_RESOURCE_MISC_SHARED,                 "SHARED" },
            { D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,      "SHARED_KEYEDMUTEX" },
            { D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
              D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,      "SHARED_NTHANDLE|KEYEDMUTEX" },
         };
         for (int k = 0; k < (int)(sizeof kShared / sizeof kShared[0]); ++k) {
            D3D11_TEXTURE2D_DESC shd = {};
            shd.Width = 256; shd.Height = 256; shd.MipLevels = 1; shd.ArraySize = 1;
            shd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; shd.SampleDesc.Count = 1;
            shd.Usage = D3D11_USAGE_DEFAULT;
            shd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            shd.MiscFlags = kShared[k].misc;
            ID3D11Texture2D *shtex = NULL;
            HRESULT shr = probeDev->CreateTexture2D(&shd, NULL, &shtex);
            printf("[step] shared(%s): CreateTexture2D hr=0x%08lX\n",
                   kShared[k].name, (unsigned long)shr);
            fflush(stdout);
            if (SUCCEEDED(shr)) {
               IDXGIResource *shres = NULL;
               shr = shtex->QueryInterface(__uuidof(IDXGIResource), (void **)&shres);
               if (SUCCEEDED(shr)) {
                  HANDLE shared = NULL;
                  shr = shres->GetSharedHandle(&shared);
                  printf("[step] shared(%s): GetSharedHandle hr=0x%08lX handle=%p\n",
                         kShared[k].name, (unsigned long)shr, shared);
                  shres->Release();
               }
               shtex->Release();
            }
            fflush(stdout);
         }

         /* DirectComposition is what LogonUI and dwm use, and what faults at
          * DCompSurface::InitializeSurface on this driver. Drive the same flow
          * here, where the failing call and its HRESULT are visible. */
         IDXGIDevice *dxgiDev = NULL;
         HRESULT dhr = probeDev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDev);
         printf("[step] dcomp: QueryInterface(IDXGIDevice) hr=0x%08lX\n", (unsigned long)dhr);
         fflush(stdout);
         if (SUCCEEDED(dhr)) {
            IDCompositionDevice *dcomp = NULL;
            dhr = DCompositionCreateDevice(dxgiDev, __uuidof(IDCompositionDevice), (void **)&dcomp);
            printf("[step] dcomp: DCompositionCreateDevice hr=0x%08lX\n", (unsigned long)dhr);
            fflush(stdout);
            if (SUCCEEDED(dhr)) {
               IDCompositionVisual *visual = NULL;
               dhr = dcomp->CreateVisual(&visual);
               printf("[step] dcomp: CreateVisual hr=0x%08lX\n", (unsigned long)dhr);
               fflush(stdout);

               IDCompositionSurface *surface = NULL;
               dhr = dcomp->CreateSurface(256, 256, DXGI_FORMAT_B8G8R8A8_UNORM,
                                          DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
               printf("[step] dcomp: CreateSurface hr=0x%08lX  <-- LogonUI/dwm fault here\n",
                      (unsigned long)dhr);
               fflush(stdout);
               if (SUCCEEDED(dhr)) {
                  ID3D11Texture2D *dcTex = NULL;
                  POINT offset = {};
                  dhr = surface->BeginDraw(NULL, __uuidof(ID3D11Texture2D), (void **)&dcTex, &offset);
                  printf("[step] dcomp: BeginDraw hr=0x%08lX\n", (unsigned long)dhr);
                  fflush(stdout);
                  if (SUCCEEDED(dhr)) {
                     surface->EndDraw();
                     if (dcTex) dcTex->Release();
                  }
                  surface->Release();
               }
               if (visual) visual->Release();
               dcomp->Release();
            }
            dxgiDev->Release();
         }

         if (probeCtx) probeCtx->Release();
         if (probeDev) probeDev->Release();
      }
   }

   DXGI_SWAP_CHAIN_DESC scd = {};
   scd.BufferCount = 2;
   scd.BufferDesc.Width = width;
   scd.BufferDesc.Height = height;
   scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   scd.BufferDesc.RefreshRate.Numerator = 60;
   scd.BufferDesc.RefreshRate.Denominator = 1;
   scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
   scd.OutputWindow = hwnd;
   scd.SampleDesc.Count = 1;
   scd.Windowed = TRUE;
   scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

   const D3D_FEATURE_LEVEL wanted[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
   IDXGISwapChain *swap = NULL;
   ID3D11Device *dev = NULL;
   ID3D11DeviceContext *ctx = NULL;
   D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;

   Step("D3D11CreateDeviceAndSwapChain");
   HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                              wanted, (UINT)(sizeof wanted / sizeof wanted[0]),
                                              D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx);
   if (FAILED(hr)) { printf("RESULT: FAIL - D3D11CreateDeviceAndSwapChain hr=0x%08lX\n", (unsigned long)hr); return 3; }
   printf("device: feature level 0x%04X\n", (unsigned)got);

   Step("GetBuffer");
   ID3D11Texture2D *back = NULL;
   hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
   if (FAILED(hr)) { printf("RESULT: FAIL - GetBuffer hr=0x%08lX\n", (unsigned long)hr); return 4; }

   Step("CreateRenderTargetView");
   ID3D11RenderTargetView *rtv = NULL;
   hr = dev->CreateRenderTargetView(back, NULL, &rtv);
   if (FAILED(hr)) { printf("RESULT: FAIL - CreateRenderTargetView hr=0x%08lX\n", (unsigned long)hr); return 5; }

   Step("create staging texture");
   D3D11_TEXTURE2D_DESC sd = {};
   back->GetDesc(&sd);
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   sd.MiscFlags = 0;
   ID3D11Texture2D *staging = NULL;
   hr = dev->CreateTexture2D(&sd, NULL, &staging);
   if (FAILED(hr)) { printf("RESULT: FAIL - staging CreateTexture2D hr=0x%08lX\n", (unsigned long)hr); return 6; }

   if (fpsSeconds > 0) {
      /* Render and present as fast as the path allows, with a colour that
       * changes every frame so nothing downstream can treat the surface as
       * unchanged and skip it. */
      LARGE_INTEGER freq, start, now;
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&start);
      long long count = 0;
      double elapsed = 0.0;
      HRESULT presentHr = S_OK;
      for (;;) {
         const float sweep[4] = { (float)((count % 90) / 90.0), 0.25f, 0.65f, 1.0f };
         ctx->ClearRenderTargetView(rtv, sweep);
         presentHr = swap->Present(0, 0);
         if (FAILED(presentHr)) {
            printf("present failed at frame %lld hr=0x%08lX\n", count, (unsigned long)presentHr);
            break;
         }
         ++count;
         PumpMessages();
         QueryPerformanceCounter(&now);
         elapsed = (double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart;
         if (elapsed >= (double)fpsSeconds) break;
      }
      const double fps = elapsed > 0.0 ? (double)count / elapsed : 0.0;
      printf("FPS: frames=%lld seconds=%.2f fps=%.2f\n", count, elapsed, fps);
      printf("RESULT: %s\n", (SUCCEEDED(presentHr) && fps > 60.0) ? "PASS" : "FAIL");
      if (staging) staging->Release();
      if (rtv) rtv->Release();
      if (back) back->Release();
      if (swap) swap->Release();
      if (ctx) ctx->Release();
      if (dev) dev->Release();
      return 0;
   }

   int rendered = 0, presented = 0;
   for (int f = 0; f < frames; ++f) {
      printf("[step] frame %d: clear\n", f);
      const float colour[4] = { kFrames[f].r, kFrames[f].g, kFrames[f].b, 1.0f };
      ctx->ClearRenderTargetView(rtv, colour);
      ctx->Flush();

      /* Did the GPU actually write the back buffer? */
      ctx->CopyResource(staging, back);
      D3D11_MAPPED_SUBRESOURCE map = {};
      hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
      if (SUCCEEDED(hr)) {
         const unsigned char *px = (const unsigned char *)map.pData;
         /* B8G8R8A8 */
         bool ok = px[0] == kFrames[f].bb && px[1] == kFrames[f].bg && px[2] == kFrames[f].br;
         printf("frame %d (%-7s): readback b=%3u g=%3u r=%3u %s\n", f, kFrames[f].name,
                px[0], px[1], px[2], ok ? "OK" : "MISMATCH");
         if (ok) ++rendered;
         ctx->Unmap(staging, 0);
      } else {
         printf("frame %d (%-7s): readback Map failed hr=0x%08lX\n", f, kFrames[f].name, (unsigned long)hr);
      }

      printf("[step] frame %d: Present\n", f);
      hr = swap->Present(0, 0);
      if (SUCCEEDED(hr)) ++presented;
      else printf("frame %d: Present hr=0x%08lX\n", f, (unsigned long)hr);

      PumpMessages();
      Sleep(700);
   }

   /* Leave the last colour on screen long enough to be captured. */
   const float last[4] = { kFrames[frames - 1].r, kFrames[frames - 1].g, kFrames[frames - 1].b, 1.0f };
   for (int i = 0; i < 40; ++i) {
      ctx->ClearRenderTargetView(rtv, last);
      swap->Present(0, 0);
      PumpMessages();
      Sleep(100);
   }

   printf("rendered=%d/%d presented=%d/%d\n", rendered, frames, presented, frames);
   printf("RESULT: %s\n", (rendered == frames && presented == frames) ? "PASS" : "FAIL");

   if (staging) staging->Release();
   if (rtv) rtv->Release();
   if (back) back->Release();
   if (swap) swap->Release();
   if (ctx) ctx->Release();
   if (dev) dev->Release();
   DestroyWindow(hwnd);
   return (rendered == frames && presented == frames) ? 0 : 1;
}
