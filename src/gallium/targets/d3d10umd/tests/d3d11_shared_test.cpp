/* Validate actual shared pixels and DirectComposition, not just valid handles.
 * Run one bounded mode per process on the VIOGPU adapter: --local, --shared,
 * --keyed, --nt or --dcomp. The harness must enforce an outer GPU-hang timeout.
 */
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr, const char *operation)
{
   printf("%s: hr=0x%08lx\n", operation, (unsigned long)hr);
   if (FAILED(hr))
      throw hr;
}

static void Acquire(IDXGIKeyedMutex *mutex, UINT64 key, const char *operation)
{
   HRESULT hr = mutex->AcquireSync(key, 5000);
   // WAIT_TIMEOUT and WAIT_ABANDONED are not negative HRESULTs.
   printf("%s: hr=0x%08lx\n", operation, (unsigned long)hr);
   if (hr != S_OK)
      throw FAILED(hr) ? hr : E_FAIL;
}

struct Device {
   ComPtr<ID3D11Device> device;
   ComPtr<ID3D11DeviceContext> context;
};

static void CheckDevice(Device &d, const char *operation)
{
   HRESULT reason = d.device->GetDeviceRemovedReason();
   printf("%s: GetDeviceRemovedReason=0x%08lx\n", operation, (unsigned long)reason);
   if (FAILED(reason))
      throw reason;
}

static void CheckDeviceCall(Device &d, HRESULT hr, const char *operation)
{
   printf("%s: hr=0x%08lx\n", operation, (unsigned long)hr);
   if (FAILED(hr)) {
      HRESULT reason = d.device->GetDeviceRemovedReason();
      printf("%s: GetDeviceRemovedReason=0x%08lx\n", operation, (unsigned long)reason);
      throw hr;
   }
}

static Device CreateDevice(IDXGIAdapter *adapter)
{
   Device d;
   const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
   };
   D3D_FEATURE_LEVEL level;
   Check(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                          D3D11_SDK_VERSION, &d.device, &level, &d.context),
         "D3D11CreateDevice");
   char path[MAX_PATH] = {};
   HMODULE umd = GetModuleHandleW(L"viogpud3d.dll");
   if (umd)
      GetModuleFileNameA(umd, path, ARRAYSIZE(path));
   printf("device: feature_level=0x%x umd=%s\n", level, path);
   if (!umd)
      Check(E_FAIL, "VIOGPU UMD must be loaded");
   return d;
}

static const float colors[][4] = {
   {1, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1},
};

static void VerifyPixels(Device &d, ID3D11Texture2D *texture,
                         const float color[4], const char *label,
                         const float *inset = NULL)
{
   D3D11_TEXTURE2D_DESC desc;
   texture->GetDesc(&desc);
   desc.Usage = D3D11_USAGE_STAGING;
   desc.BindFlags = 0;
   desc.MiscFlags = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> staging;
   Check(d.device->CreateTexture2D(&desc, NULL, &staging), "Create readback");
   d.context->CopyResource(staging.Get(), texture);
   CheckDevice(d, "after CopyResource");
   D3D11_MAPPED_SUBRESOURCE map;
   CheckDeviceCall(d, d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map),
                   "Map readback");
   const unsigned char expected[] = {
      (unsigned char)(255 * color[2]), (unsigned char)(255 * color[1]),
      (unsigned char)(255 * color[0]), (unsigned char)(255 * color[3]),
   };
   unsigned char insetExpected[4] = {};
   if (inset) {
      insetExpected[0] = (unsigned char)(255 * inset[2]);
      insetExpected[1] = (unsigned char)(255 * inset[1]);
      insetExpected[2] = (unsigned char)(255 * inset[0]);
      insetExpected[3] = (unsigned char)(255 * inset[3]);
   }
   unsigned mismatches = 0;
   unsigned char first[4];
   memcpy(first, map.pData, sizeof first);
   for (UINT y = 0; y < desc.Height; ++y) {
      const unsigned char *row = (const unsigned char *)map.pData + y * map.RowPitch;
      for (UINT x = 0; x < desc.Width; ++x) {
         const bool inside = inset && x >= 16 && x < 48 && y >= 16 && y < 48;
         mismatches += memcmp(row + x * 4, inside ? insetExpected : expected, sizeof expected) != 0;
      }
   }
   d.context->Unmap(staging.Get(), 0);
   printf("%s: first=%u,%u,%u,%u expected=%u,%u,%u,%u mismatches=%u/%u\n",
          label, first[0], first[1], first[2], first[3],
          expected[0], expected[1], expected[2], expected[3],
          mismatches, desc.Width * desc.Height);
   if (mismatches)
      Check(E_FAIL, "Shared or local pixel contents");
}

static void SampleTexture(Device &d, ID3D11Texture2D *source, const float color[4])
{
   const char *vsSource = "float4 main(uint id:SV_VertexID):SV_Position {"
      "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
   const char *psSource = "Texture2D<float4> tex:register(t0);"
      "float4 main(float4 p:SV_Position):SV_Target{return tex.Load(int3(p.xy,0));}";
   ComPtr<ID3DBlob> vsCode, psCode, errors;
   Check(D3DCompile(vsSource, strlen(vsSource), NULL, NULL, NULL, "main", "vs_4_1",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsCode, &errors), "Compile VS");
   errors.Reset();
   Check(D3DCompile(psSource, strlen(psSource), NULL, NULL, NULL, "main", "ps_4_1",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &psCode, &errors), "Compile PS");
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   Check(d.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), NULL, &vs), "Create VS");
   Check(d.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), NULL, &ps), "Create PS");
   D3D11_TEXTURE2D_DESC desc;
   source->GetDesc(&desc);
   desc.MiscFlags = 0;
   ComPtr<ID3D11Texture2D> output;
   Check(d.device->CreateTexture2D(&desc, NULL, &output), "Create sample output");
   ComPtr<ID3D11RenderTargetView> target;
   ComPtr<ID3D11ShaderResourceView> input;
   Check(d.device->CreateRenderTargetView(output.Get(), NULL, &target), "Create sample RTV");
   Check(d.device->CreateShaderResourceView(source, NULL, &input), "Create shared SRV");
   ID3D11RenderTargetView *rtv = target.Get();
   ID3D11ShaderResourceView *srv = input.Get();
   d.context->OMSetRenderTargets(1, &rtv, NULL);
   d.context->PSSetShaderResources(0, 1, &srv);
   d.context->VSSetShader(vs.Get(), NULL, 0);
   d.context->PSSetShader(ps.Get(), NULL, 0);
   d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   D3D11_VIEWPORT viewport = {0, 0, (float)desc.Width, (float)desc.Height, 0, 1};
   d.context->RSSetViewports(1, &viewport);
   d.context->Draw(3, 0);
   VerifyPixels(d, output.Get(), color, "sampled shared texture");
   d.context->ClearState();
}

static void SharedTest(IDXGIAdapter *adapter, const char *mode)
{
   const bool local = strcmp(mode, "--local") == 0;
   const bool nt = strcmp(mode, "--nt") == 0;
   const bool keyed = nt || strcmp(mode, "--keyed") == 0;
   const bool sample = strcmp(mode, "--sample") == 0;
   const bool partial = strcmp(mode, "--partial") == 0;
   Device producer = CreateDevice(adapter);
   Device consumer;
   if (!local)
      consumer = CreateDevice(adapter);

   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   desc.MiscFlags = local ? 0 : keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                                   : D3D11_RESOURCE_MISC_SHARED;
   if (nt)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
   ComPtr<ID3D11Texture2D> texture, opened;
   Check(producer.device->CreateTexture2D(&desc, NULL, &texture), "Create producer texture");
   ComPtr<ID3D11RenderTargetView> target;
   Check(producer.device->CreateRenderTargetView(texture.Get(), NULL, &target), "Create producer RTV");
   ComPtr<IDXGIKeyedMutex> producerMutex, consumerMutex;
   if (keyed)
      Check(texture.As(&producerMutex), "Producer keyed mutex");

   for (unsigned frame = 0; frame < ARRAYSIZE(colors); ++frame) {
      printf("FRAME %u\n", frame);
      if (keyed)
         Acquire(producerMutex.Get(), 0, "Producer AcquireSync");
      producer.context->ClearRenderTargetView(target.Get(), colors[frame]);
      producer.context->Flush();
      CheckDevice(producer, "producer after Flush");
      // Blocking readback proves the producer finished before a legacy shared read.
      VerifyPixels(producer, texture.Get(), colors[frame], "producer");
      if (keyed)
         Check(producerMutex->ReleaseSync(frame + 1), "Producer ReleaseSync");
      if (local)
         continue;

      if (!opened) {
         HANDLE handle = NULL;
         if (nt) {
            ComPtr<IDXGIResource1> resource;
            Check(texture.As(&resource), "IDXGIResource1");
            Check(resource->CreateSharedHandle(NULL,
                  DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &handle),
                  "CreateSharedHandle");
            ComPtr<ID3D11Device1> device1;
            HRESULT hr = consumer.device.As(&device1);
            if (SUCCEEDED(hr))
               hr = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&opened));
            CloseHandle(handle);
            Check(hr, "OpenSharedResource1");
         } else {
            ComPtr<IDXGIResource> resource;
            Check(texture.As(&resource), "IDXGIResource");
            Check(resource->GetSharedHandle(&handle), "GetSharedHandle");
            if (!handle)
               Check(E_FAIL, "Non-null shared handle");
            Check(consumer.device->OpenSharedResource(handle, IID_PPV_ARGS(&opened)),
                  "OpenSharedResource");
         }
         if (keyed)
            Check(opened.As(&consumerMutex), "Consumer keyed mutex");
      }
      if (keyed)
         Acquire(consumerMutex.Get(), frame + 1, "Consumer AcquireSync");
      if (sample)
         SampleTexture(consumer, opened.Get(), colors[frame]);
      else
         VerifyPixels(consumer, opened.Get(), colors[frame], "consumer");
      if (partial) {
         const float *changed = colors[(frame + 1) % ARRAYSIZE(colors)];
         unsigned char data[32 * 32 * 4];
         for (unsigned p = 0; p < 32 * 32; ++p) {
            data[p * 4] = (unsigned char)(255 * changed[2]);
            data[p * 4 + 1] = (unsigned char)(255 * changed[1]);
            data[p * 4 + 2] = (unsigned char)(255 * changed[0]);
            data[p * 4 + 3] = 255;
         }
         const D3D11_BOX box = {16, 16, 0, 48, 48, 1};
         consumer.context->UpdateSubresource(opened.Get(), 0, &box, data, 32 * 4, 0);
         consumer.context->Flush();
         CheckDevice(consumer, "consumer after partial Flush");
         VerifyPixels(producer, texture.Get(), colors[frame], "reverse partial update", changed);
      }
      if (keyed)
         Check(consumerMutex->ReleaseSync(0), "Consumer ReleaseSync");
   }
   if (!strcmp(mode, "--lifetime")) {
      producer.context->ClearState();
      target.Reset();
      texture.Reset();
      producer.context.Reset();
      producer.device.Reset();
      VerifyPixels(consumer, opened.Get(), colors[3], "consumer after producer destruction");
   }
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
   return DefWindowProcW(hwnd, msg, w, l);
}

static void CompositionTest(IDXGIAdapter *adapter)
{
   Device d = CreateDevice(adapter);
   ComPtr<ID3D11DeviceContext1> context1;
   Check(d.context.As(&context1), "ID3D11DeviceContext1");
   WNDCLASSW wc = {};
   wc.hInstance = GetModuleHandleW(NULL);
   wc.lpfnWndProc = WindowProc;
   wc.lpszClassName = L"VioGpuSharedCompositionTest";
   if (!RegisterClassW(&wc))
      Check(HRESULT_FROM_WIN32(GetLastError()), "RegisterClass");
   HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName,
                              L"VIOGPU DirectComposition", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 320, 320, NULL, NULL, wc.hInstance, NULL);
   if (!hwnd)
      Check(HRESULT_FROM_WIN32(GetLastError()), "CreateWindow");
   ComPtr<IDXGIDevice> dxgi;
   Check(d.device.As(&dxgi), "IDXGIDevice");
   ComPtr<IDCompositionDevice> comp;
   Check(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&comp)), "DCompositionCreateDevice");
   ComPtr<IDCompositionTarget> target;
   ComPtr<IDCompositionVisual> visual;
   ComPtr<IDCompositionSurface> surface;
   Check(comp->CreateTargetForHwnd(hwnd, TRUE, &target), "CreateTargetForHwnd");
   Check(comp->CreateVisual(&visual), "CreateVisual");
   Check(comp->CreateSurface(256, 256, DXGI_FORMAT_B8G8R8A8_UNORM,
                            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface), "CreateSurface");
   Check(visual->SetContent(surface.Get()), "SetContent");
   Check(target->SetRoot(visual.Get()), "SetRoot");
   for (unsigned frame = 0; frame < ARRAYSIZE(colors); ++frame) {
      printf("DCOMP FRAME %u\n", frame);
      ComPtr<ID3D11Texture2D> texture;
      POINT offset;
      Check(surface->BeginDraw(NULL, IID_PPV_ARGS(&texture), &offset), "BeginDraw");
      ComPtr<ID3D11RenderTargetView> view;
      HRESULT drawHr = d.device->CreateRenderTargetView(texture.Get(), NULL, &view);
      if (SUCCEEDED(drawHr)) {
         const D3D11_RECT rect = {offset.x, offset.y, offset.x + 256, offset.y + 256};
         context1->ClearView(view.Get(), colors[frame], &rect, 1);
         d.context->Flush();
      }
      HRESULT endHr = surface->EndDraw();
      Check(drawHr, "DComp RTV and clear");
      Check(endHr, "EndDraw");
      Check(comp->Commit(), "Commit");
      Check(comp->WaitForCommitCompletion(), "WaitForCommitCompletion");
      Check(d.device->GetDeviceRemovedReason(), "GetDeviceRemovedReason");
      const ULONGLONG until = GetTickCount64() + 1500;
      while (GetTickCount64() < until) {
         MSG msg;
         while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
         }
         Sleep(10);
      }
   }
   DestroyWindow(hwnd);
   printf("Composition API sequence completed; visible output needs independent scanout verification.\n");
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   const char *mode = argc == 2 ? argv[1] : "--shared";
   if (strcmp(mode, "--local") && strcmp(mode, "--shared") &&
       strcmp(mode, "--keyed") && strcmp(mode, "--nt") && strcmp(mode, "--dcomp") &&
       strcmp(mode, "--sample") && strcmp(mode, "--partial") && strcmp(mode, "--lifetime")) {
      printf("Usage: d3d11_shared_test [--local|--shared|--keyed|--nt|--sample|--partial|--lifetime|--dcomp]\n");
      return 2;
   }
   DWORD session;
   ProcessIdToSessionId(GetCurrentProcessId(), &session);
   printf("mode=%s session=%lu pid=%lu\n", mode, session, GetCurrentProcessId());
   try {
      ComPtr<IDXGIFactory1> factory;
      Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
      ComPtr<IDXGIAdapter1> adapter;
      for (UINT index = 0;; ++index) {
         ComPtr<IDXGIAdapter1> candidate;
         HRESULT hr = factory->EnumAdapters1(index, &candidate);
         if (hr == DXGI_ERROR_NOT_FOUND)
            break;
         Check(hr, "EnumAdapters1");
         DXGI_ADAPTER_DESC1 desc;
         Check(candidate->GetDesc1(&desc), "GetDesc1");
         printf("adapter %u: %ls vendor=%04x device=%04x luid=%08lx:%08lx flags=%x\n",
                index, desc.Description, desc.VendorId, desc.DeviceId,
                (unsigned long)desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, desc.Flags);
         if (desc.VendorId == 0x1af4 && desc.DeviceId == 0x1050 &&
             !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter = candidate;
            break;
         }
      }
      if (!adapter)
         Check(E_FAIL, "VIOGPU hardware adapter required");
      if (!strcmp(mode, "--dcomp"))
         CompositionTest(adapter.Get());
      else
         SharedTest(adapter.Get(), mode);
      printf("RESULT: PASS mode=%s\n", mode);
      return 0;
   } catch (HRESULT hr) {
      printf("RESULT: FAIL mode=%s hr=0x%08lx\n", mode, (unsigned long)hr);
      return 1;
   }
}
