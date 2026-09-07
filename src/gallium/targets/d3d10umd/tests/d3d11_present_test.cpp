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

int main(int argc, char **argv)
{
   int width = 640, height = 480, frames = (int)(sizeof kFrames / sizeof kFrames[0]);
   bool fullscreenSize = false;
   for (int i = 1; i < argc; ++i) {
      if (!strcmp(argv[i], "--full")) fullscreenSize = true;
   }

   if (fullscreenSize) {
      width = GetSystemMetrics(SM_CXSCREEN);
      height = GetSystemMetrics(SM_CYSCREEN);
      if (width <= 0 || height <= 0) { width = 1280; height = 1024; }
   }

   printf("session: window %dx%d\n", width, height);

   WNDCLASSEXW wc = {};
   wc.cbSize = sizeof wc;
   wc.lpfnWndProc = WndProc;
   wc.hInstance = GetModuleHandleW(NULL);
   wc.lpszClassName = L"VioGpuPresentTest";
   if (!RegisterClassExW(&wc)) { printf("RESULT: FAIL - RegisterClassEx %lu\n", GetLastError()); return 2; }

   HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VioGPU present test",
                               WS_POPUP | WS_VISIBLE, 0, 0, width, height,
                               NULL, NULL, wc.hInstance, NULL);
   if (hwnd == NULL) { printf("RESULT: FAIL - CreateWindowEx %lu\n", GetLastError()); return 2; }
   ShowWindow(hwnd, SW_SHOW);
   UpdateWindow(hwnd);
   PumpMessages();

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

   HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                              wanted, (UINT)(sizeof wanted / sizeof wanted[0]),
                                              D3D11_SDK_VERSION, &scd, &swap, &dev, &got, &ctx);
   if (FAILED(hr)) { printf("RESULT: FAIL - D3D11CreateDeviceAndSwapChain hr=0x%08lX\n", (unsigned long)hr); return 3; }
   printf("device: feature level 0x%04X\n", (unsigned)got);

   ID3D11Texture2D *back = NULL;
   hr = swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
   if (FAILED(hr)) { printf("RESULT: FAIL - GetBuffer hr=0x%08lX\n", (unsigned long)hr); return 4; }

   ID3D11RenderTargetView *rtv = NULL;
   hr = dev->CreateRenderTargetView(back, NULL, &rtv);
   if (FAILED(hr)) { printf("RESULT: FAIL - CreateRenderTargetView hr=0x%08lX\n", (unsigned long)hr); return 5; }

   D3D11_TEXTURE2D_DESC sd = {};
   back->GetDesc(&sd);
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   sd.MiscFlags = 0;
   ID3D11Texture2D *staging = NULL;
   hr = dev->CreateTexture2D(&sd, NULL, &staging);
   if (FAILED(hr)) { printf("RESULT: FAIL - staging CreateTexture2D hr=0x%08lX\n", (unsigned long)hr); return 6; }

   int rendered = 0, presented = 0;
   for (int f = 0; f < frames; ++f) {
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
