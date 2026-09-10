/* Validate actual shared pixels and DirectComposition, not just valid handles.
 * Run one bounded mode per process on the VIOGPU adapter: --local, --shared,
 * --keyed, --nt or --dcomp. The harness must enforce an outer GPU-hang timeout.
 */
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cwchar>

#pragma comment(lib, "dwmapi.lib")

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

// Reference behavior only. This mode never counts as VIOGPU acceptance.
static bool warpControl;
static DXGI_FORMAT sharedTextureFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

static void PrintClientIdentity()
{
   using QueryMachine = BOOL (WINAPI *)(HANDLE, USHORT *, USHORT *);
   auto query = reinterpret_cast<QueryMachine>(GetProcAddress(
      GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
   USHORT processMachine = 0, nativeMachine = 0;
   if (query && query(GetCurrentProcess(), &processMachine, &nativeMachine))
      printf("client: pointer_bits=%u process_machine=0x%04x native_machine=0x%04x\n",
             unsigned(sizeof(void *) * 8), processMachine, nativeMachine);
   else
      printf("client: pointer_bits=%u machine_query_unavailable\n", unsigned(sizeof(void *) * 8));
}

static void PrintDriverModules()
{
   const wchar_t *names[] = {L"viogpud3d.dll", L"viogpud3dec.dll",
                            L"vulkan-1.dll", L"vulkan_freedreno.dll", L"d3d10warp.dll"};
   for (const wchar_t *name : names) {
      char path[MAX_PATH] = {};
      HMODULE module = GetModuleHandleW(name);
      if (module && GetModuleFileNameA(module, path, ARRAYSIZE(path)))
         printf("module: %ls=%s\n", name, path);
   }
}

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
   HRESULT hr = D3D11CreateDevice(warpControl ? NULL : adapter,
                          warpControl ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN, NULL,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                          D3D11_SDK_VERSION, &d.device, &level, &d.context);
   PrintDriverModules();
   Check(hr, "D3D11CreateDevice");
   char path[MAX_PATH] = {};
   HMODULE umd = GetModuleHandleW(L"viogpud3d.dll");
   if (umd)
      GetModuleFileNameA(umd, path, ARRAYSIZE(path));
   printf("device: feature_level=0x%x umd=%s reference_warp=%d\n", level, path, warpControl);
   if (!warpControl && !umd)
      Check(E_FAIL, "VIOGPU UMD must be loaded");
   return d;
}

static const float colors[][4] = {
   {1, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1},
};

static void PixelBytes(DXGI_FORMAT format, const float color[4], unsigned char bytes[4])
{
   if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM)
      Check(E_INVALIDARG, "Four-channel UNORM readback format");
   const bool rgba = format == DXGI_FORMAT_R8G8B8A8_UNORM;
   bytes[0] = (unsigned char)(255 * color[rgba ? 0 : 2]);
   bytes[1] = (unsigned char)(255 * color[1]);
   bytes[2] = (unsigned char)(255 * color[rgba ? 2 : 0]);
   bytes[3] = (unsigned char)(255 * color[3]);
}

static void CheckSharedFormat(ID3D11Texture2D *texture)
{
   D3D11_TEXTURE2D_DESC desc;
   texture->GetDesc(&desc);
   printf("shared format=%u expected=%u size=%ux%u\n", unsigned(desc.Format),
          unsigned(sharedTextureFormat), desc.Width, desc.Height);
   if (desc.Format != sharedTextureFormat)
      Check(E_FAIL, "Shared texture must retain its format when opened");
}

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
   unsigned char expected[4];
   PixelBytes(desc.Format, color, expected);
   unsigned char insetExpected[4] = {};
   if (inset)
      PixelBytes(desc.Format, inset, insetExpected);
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

static void SampleTexture(Device &d, ID3D11Texture2D *source, const float color[4],
                          bool releaseBeforeReadback = false)
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
   // RGBA shared inputs must be interpreted by the sampler before BGRA
   // composition. A same-format raw copy would miss swapped channel metadata.
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
   // A nonzero start also checks the frontend's zero-based SV_VertexID.
   d.context->Draw(3, releaseBeforeReadback ? 1 : 0);
   if (releaseBeforeReadback) {
      // The D3D frontend substitutes its empty shaders for these NULL binds.
      // Delete the old pair without drawing with the replacements, then let
      // readback retire their last GPU batch before device teardown.
      d.context->VSSetShader(NULL, NULL, 0);
      d.context->PSSetShader(NULL, NULL, 0);
      vs.Reset();
      ps.Reset();
   }
   VerifyPixels(d, output.Get(), color, "sampled shared texture");
   d.context->ClearState();
}

static void SharedSampleReuseTest(IDXGIAdapter *adapter, const char *mode)
{
   const bool unbind = !strcmp(mode, "--sample-reuse-unbind");
   const bool wait = !strcmp(mode, "--sample-reuse-wait");
   // Reuse one sampled shared image across CPU/GPU ownership transitions.
   // Read the consumer only after all draws: a readback between draws would
   // introduce transfer barriers and hide a missing host-to-sampler dependency.
   Device producer = CreateDevice(adapter), consumer = CreateDevice(adapter);
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
   ComPtr<ID3D11Texture2D> texture, opened, output;
   Check(producer.device->CreateTexture2D(&desc, NULL, &texture), "Create reused shared texture");
   ComPtr<IDXGIResource> shared;
   Check(texture.As(&shared), "Reused IDXGIResource");
   HANDLE handle = NULL;
   Check(shared->GetSharedHandle(&handle), "Reused shared handle");
   Check(consumer.device->OpenSharedResource(handle, IID_PPV_ARGS(&opened)), "Open reused texture");
   ComPtr<IDXGIKeyedMutex> producerMutex, consumerMutex;
   Check(texture.As(&producerMutex), "Reused producer mutex");
   Check(opened.As(&consumerMutex), "Reused consumer mutex");
   ComPtr<ID3D11RenderTargetView> producerTarget, consumerTarget;
   Check(producer.device->CreateRenderTargetView(texture.Get(), NULL, &producerTarget), "Reused producer RTV");
   desc.Width *= ARRAYSIZE(colors);
   desc.MiscFlags = 0;
   Check(consumer.device->CreateTexture2D(&desc, NULL, &output), "Create stripe output");
   Check(consumer.device->CreateRenderTargetView(output.Get(), NULL, &consumerTarget), "Stripe output RTV");
   ComPtr<ID3D11ShaderResourceView> input;
   Check(consumer.device->CreateShaderResourceView(opened.Get(), NULL, &input), "Reused shared SRV");
   const char *vsSource = "float4 main(uint id:SV_VertexID):SV_Position {"
      "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
   const char *psSource = "Texture2D<float4> tex:register(t0);"
      "float4 main(float4 p:SV_Position):SV_Target{return tex.Load(int3((int2)p.xy & 63,0));}";
   ComPtr<ID3DBlob> vsCode, psCode, errors;
   Check(D3DCompile(vsSource, strlen(vsSource), NULL, NULL, NULL, "main", "vs_4_1",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsCode, &errors), "Compile reuse VS");
   errors.Reset();
   Check(D3DCompile(psSource, strlen(psSource), NULL, NULL, NULL, "main", "ps_4_1",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &psCode, &errors), "Compile reuse PS");
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   Check(consumer.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), NULL, &vs), "Create reuse VS");
   Check(consumer.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), NULL, &ps), "Create reuse PS");
   ComPtr<ID3D11Query> completion;
   if (wait) {
      const D3D11_QUERY_DESC query = {D3D11_QUERY_EVENT, 0};
      Check(consumer.device->CreateQuery(&query, &completion), "Create reuse completion event");
   }
   ID3D11RenderTargetView *rtv = consumerTarget.Get();
   ID3D11ShaderResourceView *srv = input.Get();
   consumer.context->OMSetRenderTargets(1, &rtv, NULL);
   consumer.context->PSSetShaderResources(0, 1, &srv);
   consumer.context->VSSetShader(vs.Get(), NULL, 0);
   consumer.context->PSSetShader(ps.Get(), NULL, 0);
   consumer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   for (unsigned frame = 0; frame < ARRAYSIZE(colors); ++frame) {
      Acquire(producerMutex.Get(), 0, "Reuse producer acquire");
      producer.context->ClearRenderTargetView(producerTarget.Get(), colors[frame]);
      producer.context->Flush();
      Check(producerMutex->ReleaseSync(1), "Reuse producer release");
      Acquire(consumerMutex.Get(), 1, "Reuse consumer acquire");
      // ReleaseSync unsets shared resources from the runtime pipeline. Keep
      // the same SRV object, but bind it anew after every ownership acquire.
      consumer.context->PSSetShaderResources(0, 1, &srv);
      D3D11_VIEWPORT viewport = {(float)(frame * 64), 0, 64, 64, 0, 1};
      consumer.context->RSSetViewports(1, &viewport);
      consumer.context->Draw(3, 0);
      CheckDevice(consumer, "Reuse consumer after Draw");
      if (unbind) {
         ID3D11ShaderResourceView *empty = NULL;
         consumer.context->PSSetShaderResources(0, 1, &empty);
      }
      if (wait)
         consumer.context->End(completion.Get());
      consumer.context->Flush();
      CheckDevice(consumer, "Reuse consumer after Flush");
      if (wait) {
         const ULONGLONG deadline = GetTickCount64() + 5000;
         BOOL complete = FALSE;
         HRESULT hr;
         do {
            hr = consumer.context->GetData(completion.Get(), &complete, sizeof complete,
                                           D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr != S_FALSE)
               break;
            Sleep(1);
         } while (GetTickCount64() < deadline);
         CheckDeviceCall(consumer, hr, "Reuse completion GetData");
         Check(hr == S_OK && complete ? S_OK : E_FAIL, "Reuse GPU completion before release");
      }
      Check(consumerMutex->ReleaseSync(0), "Reuse consumer release");
   }
   desc.Usage = D3D11_USAGE_STAGING;
   desc.BindFlags = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> readback;
   Check(consumer.device->CreateTexture2D(&desc, NULL, &readback), "Create stripe readback");
   consumer.context->CopyResource(readback.Get(), output.Get());
   D3D11_MAPPED_SUBRESOURCE map = {};
   CheckDeviceCall(consumer, consumer.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Read all stripes");
   unsigned mismatches = 0;
   for (UINT y = 0; y < desc.Height; ++y) {
      const unsigned char *row = (const unsigned char *)map.pData + y * map.RowPitch;
      for (UINT x = 0; x < desc.Width; ++x) {
         const float *color = colors[x / 64];
         const unsigned char expected[] = {
            (unsigned char)(255 * color[2]), (unsigned char)(255 * color[1]),
            (unsigned char)(255 * color[0]), (unsigned char)(255 * color[3]),
         };
         mismatches += memcmp(row + x * 4, expected, sizeof expected) != 0;
      }
   }
   consumer.context->Unmap(readback.Get(), 0);
   printf("reused shared sampling: mismatches=%u/%u\n", mismatches, desc.Width * desc.Height);
   consumer.context->ClearState();
   if (mismatches)
      Check(E_FAIL, "Repeated shared sampling contents");
   CheckDevice(producer, "Reuse producer final");
   CheckDevice(consumer, "Reuse consumer final");
}

static const char bufferFetchVS[] =
   "struct O{float4 p:SV_Position;float4 c:COLOR;};"
   "O main(float4 c:POSITION,uint id:SV_VertexID){O o;"
   "float2 p=float2((id<<1)&2,id&2);"
   "o.p=float4(p*float2(2,-2)+float2(-1,1),0,1);o.c=c;return o;}";
static const char bufferFetchPS[] =
   // Keep the complete VS output layout, including the unused position.
   // D3D stage linkage shares registers; COLOR alone would land in v0,
   // while the VS writes COLOR to o1. User semantic names cannot fix that.
   "struct O{float4 p:SV_Position;float4 c:COLOR;};"
   "float4 main(O i):SV_Target{return i.c;}";

static bool BufferSignaturesCompatible(ID3DBlob *vsCode, ID3DBlob *psCode)
{
   ComPtr<ID3D11ShaderReflection> vs, ps;
   Check(D3DReflect(vsCode->GetBufferPointer(), vsCode->GetBufferSize(),
                    IID_PPV_ARGS(&vs)), "Reflect buffer VS");
   Check(D3DReflect(psCode->GetBufferPointer(), psCode->GetBufferSize(),
                    IID_PPV_ARGS(&ps)), "Reflect buffer PS");
   D3D11_SHADER_DESC vsDesc = {}, psDesc = {};
   Check(vs->GetDesc(&vsDesc), "Get buffer VS signature");
   Check(ps->GetDesc(&psDesc), "Get buffer PS signature");
   bool compatible = true;
   for (UINT i = 0; i < psDesc.InputParameters; ++i) {
      D3D11_SIGNATURE_PARAMETER_DESC input = {};
      Check(ps->GetInputParameterDesc(i, &input), "Get buffer PS input");
      bool matched = false;
      for (UINT j = 0; j < vsDesc.OutputParameters; ++j) {
         D3D11_SIGNATURE_PARAMETER_DESC output = {};
         Check(vs->GetOutputParameterDesc(j, &output), "Get buffer VS output");
         if (!_stricmp(input.SemanticName, output.SemanticName) &&
             input.SemanticIndex == output.SemanticIndex) {
            printf("buffer linkage %s%u: VS=o%u mask=%x PS=v%u mask=%x\n",
                   input.SemanticName, input.SemanticIndex, output.Register,
                   output.Mask, input.Register, input.Mask);
            matched = input.Register == output.Register &&
               (input.Mask & output.Mask) == input.Mask &&
               input.ComponentType == output.ComponentType &&
               input.SystemValueType == output.SystemValueType;
            break;
         }
      }
      compatible &= matched;
   }
   return compatible;
}

static void BufferSignatureTest()
{
   // This SDK-only mode runs without a GPU in CI. The negative control must
   // detect the old invalid pair; a positive result is not rendering proof.
   const char invalidPS[] = "float4 main(float4 c:COLOR):SV_Target{return c;}";
   ComPtr<ID3DBlob> vs, ps, invalid;
   Check(D3DCompile(bufferFetchVS, strlen(bufferFetchVS), NULL, NULL, NULL,
                    "main", "vs_4_1", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, NULL),
         "Compile signature VS");
   Check(D3DCompile(bufferFetchPS, strlen(bufferFetchPS), NULL, NULL, NULL,
                    "main", "ps_4_1", D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, NULL),
         "Compile signature PS");
   Check(D3DCompile(invalidPS, strlen(invalidPS), NULL, NULL, NULL,
                    "main", "ps_4_1", D3DCOMPILE_ENABLE_STRICTNESS, 0, &invalid, NULL),
         "Compile invalid signature control");
   Check(BufferSignaturesCompatible(vs.Get(), ps.Get()) ? S_OK : E_FAIL,
         "Matching full signature");
   Check(!BufferSignaturesCompatible(vs.Get(), invalid.Get()) ? S_OK : E_FAIL,
         "Reject old mismatched signature");
}

static void DynamicBufferReuseTest(IDXGIAdapter *adapter, const char *mode)
{
   const bool rebind = !strcmp(mode, "--buffer-rebind");
   const bool single = strcmp(mode, "--buffer-reuse") && !rebind;
   const bool immutable = !strcmp(mode, "--buffer-static");
   const bool vertexId = !strcmp(mode, "--buffer-vertexid");
   const bool fetch = !strcmp(mode, "--buffer-fetch") || !strcmp(mode, "--buffer-fetch-large");
   const bool large = !strcmp(mode, "--buffer-large") || !strcmp(mode, "--buffer-fetch-large");
   const bool noCull = !strcmp(mode, "--buffer-nocull");
   const bool arrays = !strcmp(mode, "--buffer-arrays");
   const bool zeroOffset = !strcmp(mode, "--buffer-zero-offset");
   const unsigned indexPadding = zeroOffset ? 0 : 2;
   // Keep bindings fixed while DISCARD replaces vertex/constant storage and
   // NO_OVERWRITE appends indices. No intermediate readbacks may serialize
   // the draws or hide stale buffer descriptors after a rename.
   Device d = CreateDevice(adapter);
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> output;
   ComPtr<ID3D11RenderTargetView> target;
   Check(d.device->CreateTexture2D(&desc, NULL, &output), "Create buffer reuse target");
   Check(d.device->CreateRenderTargetView(output.Get(), NULL, &target), "Buffer reuse RTV");
   const char *vsSource = "float4 main(float4 p:POSITION):SV_Position{return p;}";
   if (vertexId)
      vsSource = "float4 main(uint id:SV_VertexID):SV_Position {"
         "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
   const char *psSource = "cbuffer C:register(b0){float4 color;}"
      "float4 main():SV_Target{return color;}";
   if (fetch) {
      // Generate coverage independently, but pass real VB data through the
      // VS/PS interface. This separates vertex fetching from position output.
      vsSource = bufferFetchVS;
      psSource = bufferFetchPS;
   }
   ComPtr<ID3DBlob> vsCode, psCode, errors;
   Check(D3DCompile(vsSource, strlen(vsSource), NULL, NULL, NULL, "main", "vs_4_1",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsCode, &errors), "Compile buffer VS");
   errors.Reset();
   Check(D3DCompile(psSource, strlen(psSource), NULL, NULL, NULL, "main", "ps_4_1",
                    D3DCOMPILE_ENABLE_STRICTNESS, 0, &psCode, &errors), "Compile buffer PS");
   Check(BufferSignaturesCompatible(vsCode.Get(), psCode.Get()) ? S_OK : E_FAIL,
         "Buffer shader linkage");
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> ps;
   ComPtr<ID3D11InputLayout> layout;
   Check(d.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), NULL, &vs), "Create buffer VS");
   Check(d.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), NULL, &ps), "Create buffer PS");
   const D3D11_INPUT_ELEMENT_DESC element = {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
      0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
   if (!vertexId)
      Check(d.device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout), "Create buffer layout");
   D3D11_BUFFER_DESC bd = {};
   bd.Usage = D3D11_USAGE_DYNAMIC;
   bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
   // Four MiB exceeds Zink's one-MiB slab entry limit, isolating buffer
   // suballocation without changing binding offsets or populated geometry.
   bd.ByteWidth = large ? 4 * 1024 * 1024 : 5 * 16;
   bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
   ComPtr<ID3D11Buffer> vertex, index, constant;
   Check(d.device->CreateBuffer(&bd, NULL, &vertex), "Create dynamic VB");
   bd.ByteWidth = large ? 4 * 1024 * 1024 : (2 + 4 * 6) * sizeof(uint16_t);
   bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
   Check(d.device->CreateBuffer(&bd, NULL, &index), "Create dynamic IB");
   bd.ByteWidth = 16;
   bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   Check(d.device->CreateBuffer(&bd, NULL, &constant), "Create dynamic CB");
   ID3D11RenderTargetView *rtv = target.Get();
   ID3D11Buffer *vb = vertex.Get(), *cb = constant.Get();
   const UINT stride = 16, offset = zeroOffset ? 0 : 16;
   d.context->OMSetRenderTargets(1, &rtv, NULL);
   d.context->IASetInputLayout(layout.Get());
   d.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
   d.context->IASetIndexBuffer(index.Get(), DXGI_FORMAT_R16_UINT, indexPadding * 2);
   d.context->IASetPrimitiveTopology(arrays ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                          : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   d.context->VSSetShader(vs.Get(), NULL, 0);
   d.context->PSSetShader(ps.Get(), NULL, 0);
   d.context->PSSetConstantBuffers(0, 1, &cb);
   D3D11_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
   d.context->RSSetViewports(1, &viewport);
   ComPtr<ID3D11RasterizerState> rasterizer;
   if (noCull) {
      D3D11_RASTERIZER_DESC rs = {};
      rs.FillMode = D3D11_FILL_SOLID;
      rs.CullMode = D3D11_CULL_NONE;
      rs.DepthClipEnable = TRUE;
      Check(d.device->CreateRasterizerState(&rs, &rasterizer), "Create no-cull state");
      d.context->RSSetState(rasterizer.Get());
   }
   const float black[4] = {0, 0, 0, 1};
   d.context->ClearRenderTargetView(target.Get(), black);
   for (unsigned draw = 0; draw < (single ? 4u : 32u); ++draw) {
      const unsigned stripe = draw % 4;
      const float left = -1.0f + stripe * 0.5f, right = left + 0.5f;
      float vertices[5][4] = {{100, 100, 0, 1},
         {left, 1, 0, 1}, {right, 1, 0, 1}, {left, -1, 0, 1}, {right, -1, 0, 1}};
      if (fetch) {
         for (unsigned i = 1; i < ARRAYSIZE(vertices); ++i)
            memcpy(vertices[i], colors[stripe], sizeof vertices[i]);
      }
      const uint16_t indices[] = {0, 1, 2, 2, 1, 3};
      if (immutable) {
         // Each draw owns initialized, immutable buffers. Keep the same VB/IB
         // offsets and shaders while removing Map/DISCARD/renaming entirely.
         D3D11_BUFFER_DESC fixed = {};
         fixed.Usage = D3D11_USAGE_IMMUTABLE;
         fixed.ByteWidth = sizeof vertices;
         fixed.BindFlags = D3D11_BIND_VERTEX_BUFFER;
         D3D11_SUBRESOURCE_DATA initial = {vertices, 0, 0};
         vertex.Reset();
         Check(d.device->CreateBuffer(&fixed, &initial, &vertex), "Immutable VB");
         uint16_t allIndices[2 + 4 * 6] = {};
         memcpy(allIndices + 2 + stripe * 6, indices, sizeof indices);
         fixed.ByteWidth = sizeof allIndices;
         fixed.BindFlags = D3D11_BIND_INDEX_BUFFER;
         initial.pSysMem = allIndices;
         index.Reset();
         Check(d.device->CreateBuffer(&fixed, &initial, &index), "Immutable IB");
         fixed.ByteWidth = sizeof colors[0];
         fixed.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
         initial.pSysMem = colors[stripe];
         constant.Reset();
         Check(d.device->CreateBuffer(&fixed, &initial, &constant), "Immutable CB");
         vb = vertex.Get();
         cb = constant.Get();
      } else {
         D3D11_MAPPED_SUBRESOURCE map = {};
         CheckDeviceCall(d, d.context->Map(vertex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map), "Discard VB");
         if (zeroOffset)
            memcpy(map.pData, vertices + 1, 4 * sizeof vertices[0]);
         else
            memcpy(map.pData, vertices, sizeof vertices);
         d.context->Unmap(vertex.Get(), 0);
         CheckDeviceCall(d, d.context->Map(constant.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map), "Discard CB");
         memcpy(map.pData, colors[stripe], sizeof colors[0]);
         d.context->Unmap(constant.Get(), 0);
         CheckDeviceCall(d, d.context->Map(index.Get(), 0,
            stripe ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, 0, &map), "Append IB");
         memcpy((uint16_t *)map.pData + indexPadding + stripe * 6, indices, sizeof indices);
         d.context->Unmap(index.Get(), 0);
      }
      if (rebind || immutable) {
         d.context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
         d.context->IASetIndexBuffer(index.Get(), DXGI_FORMAT_R16_UINT, indexPadding * 2);
         d.context->PSSetConstantBuffers(0, 1, &cb);
      }
      if (vertexId || fetch) {
         viewport.TopLeftX = (float)(stripe * 16);
         viewport.Width = 16;
         d.context->RSSetViewports(1, &viewport);
         d.context->Draw(3, 0);
      } else if (arrays) {
         d.context->Draw(4, 0);
      } else {
         d.context->DrawIndexed(6, stripe * 6, 0);
      }
   }
   desc.Usage = D3D11_USAGE_STAGING;
   desc.BindFlags = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> readback;
   Check(d.device->CreateTexture2D(&desc, NULL, &readback), "Create buffer reuse readback");
   d.context->CopyResource(readback.Get(), output.Get());
   D3D11_MAPPED_SUBRESOURCE map = {};
   CheckDeviceCall(d, d.context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &map), "Read buffer reuse stripes");
   unsigned mismatches = 0;
   for (unsigned stripe = 0; stripe < 4; ++stripe) {
      const unsigned char *pixel = (const unsigned char *)map.pData + 32 * map.RowPitch + (stripe * 16 + 8) * 4;
      printf("buffer stripe %u: BGRA=%u,%u,%u,%u\n", stripe, pixel[0], pixel[1], pixel[2], pixel[3]);
   }
   for (UINT y = 0; y < desc.Height; ++y) {
      const unsigned char *row = (const unsigned char *)map.pData + y * map.RowPitch;
      for (UINT x = 0; x < desc.Width; ++x) {
         const float *color = colors[x / 16];
         const unsigned char expected[] = {(unsigned char)(255 * color[2]),
            (unsigned char)(255 * color[1]), (unsigned char)(255 * color[0]), 255};
         mismatches += memcmp(row + x * 4, expected, sizeof expected) != 0;
      }
   }
   d.context->Unmap(readback.Get(), 0);
   printf("dynamic buffer reuse: mismatches=%u/4096 draws=%u rebind=%d immutable=%d vertexId=%d noCull=%d\n",
          mismatches, single ? 4 : 32, rebind, immutable, vertexId, noCull);
   // Only inspect the upload after all draws and the image check, so this
   // diagnostic cannot serialize the buffer reuse workload being tested.
   vertex->GetDesc(&bd);
   bd.Usage = D3D11_USAGE_STAGING;
   bd.BindFlags = 0;
   bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Buffer> vertexReadback;
   Check(d.device->CreateBuffer(&bd, NULL, &vertexReadback), "Create VB readback");
   d.context->CopyResource(vertexReadback.Get(), vertex.Get());
   CheckDeviceCall(d, d.context->Map(vertexReadback.Get(), 0, D3D11_MAP_READ, 0, &map), "Read VB upload");
   const float *uploaded = (const float *)((const char *)map.pData + offset);
   for (unsigned i = 0; i < 4; ++i)
      printf("uploaded vertex %u: %.3f,%.3f,%.3f,%.3f\n", i,
             uploaded[i * 4], uploaded[i * 4 + 1], uploaded[i * 4 + 2], uploaded[i * 4 + 3]);
   d.context->Unmap(vertexReadback.Get(), 0);
   d.context->ClearState();
   if (mismatches)
      Check(E_FAIL, "Dynamic buffer reuse contents");
   CheckDevice(d, "Dynamic buffer reuse final");
}

static void ShaderLifetimeTest(IDXGIAdapter *adapter)
{
   for (unsigned iteration = 0; iteration < 8; ++iteration) {
      printf("SHADER LIFETIME %u\n", iteration);
      Device d = CreateDevice(adapter);
      D3D11_TEXTURE2D_DESC desc = {};
      desc.Width = desc.Height = 64;
      desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      ComPtr<ID3D11Texture2D> texture;
      Check(d.device->CreateTexture2D(&desc, NULL, &texture), "Create lifetime texture");
      ComPtr<ID3D11RenderTargetView> target;
      Check(d.device->CreateRenderTargetView(texture.Get(), NULL, &target), "Create lifetime RTV");
      const float *color = colors[iteration % ARRAYSIZE(colors)];
      d.context->ClearRenderTargetView(target.Get(), color);
      SampleTexture(d, texture.Get(), color, true);
      // Exercise batch reuse without another draw selecting a new program.
      for (unsigned retire = 0; retire < 3; ++retire)
         VerifyPixels(d, texture.Get(), color, "post-delete batch retirement");
      CheckDevice(d, "before shader lifetime teardown");
      target.Reset();
      texture.Reset();
      d.context->ClearState();
      d.context->Flush();
      d.context.Reset();
      d.device.Reset();
      printf("SHADER LIFETIME %u teardown completed\n", iteration);
   }
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
   desc.Format = sharedTextureFormat;
   if (sharedTextureFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
      // Match the Explorer atlas that failed in CreateResource.
      desc.Width = 1280;
      desc.Height = 224;
   }
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
      if (local) {
         if (sharedTextureFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
            SampleTexture(producer, texture.Get(), colors[frame]);
         continue;
      }

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
         CheckSharedFormat(opened.Get());
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
         for (unsigned p = 0; p < 32 * 32; ++p)
            PixelBytes(desc.Format, changed, data + p * 4);
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

static void CheckWin(BOOL result, const char *operation)
{
   if (!result)
      Check(HRESULT_FROM_WIN32(GetLastError()), operation);
}

static HANDLE ParseHandle(const char *text)
{
   char *end = NULL;
   errno = 0;
   unsigned long long value = strtoull(text, &end, 10);
   if (errno || !value || end == text || *end || value > UINTPTR_MAX)
      Check(E_INVALIDARG, "Child IPC handle");
   return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
}

static void WaitEvent(HANDLE event, const char *operation)
{
   DWORD result = WaitForSingleObject(event, 5000);
   printf("%s: wait=%lu\n", operation, result);
   if (result != WAIT_OBJECT_0)
      Check(result == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL, operation);
}

static void ProcessConsumer(IDXGIAdapter *adapter, HANDLE shared, HANDLE ready, HANDLE done)
{
   Device consumer = CreateDevice(adapter);
   ComPtr<ID3D11Texture2D> opened;
   for (unsigned frame = 0; frame < ARRAYSIZE(colors); ++frame) {
      WaitEvent(ready, "Child waits for producer");
      if (!opened) {
         Check(consumer.device->OpenSharedResource(shared, IID_PPV_ARGS(&opened)),
               "Child OpenSharedResource");
         CheckSharedFormat(opened.Get());
      }
      printf("CHILD FRAME %u pid=%lu\n", frame, GetCurrentProcessId());
      VerifyPixels(consumer, opened.Get(), colors[frame], "cross-process consumer");
      if (sharedTextureFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
         SampleTexture(consumer, opened.Get(), colors[frame]);
      CheckWin(SetEvent(done), "Child signals readback complete");
   }
}

struct ChildLifetime {
   HANDLE job = NULL, ready = NULL, done = NULL;
   PROCESS_INFORMATION process = {};
   ~ChildLifetime()
   {
      // Parent exceptions and outer harness termination must not orphan a GPU child.
      if (job) CloseHandle(job);
      if (process.hThread) CloseHandle(process.hThread);
      if (process.hProcess) CloseHandle(process.hProcess);
      if (ready) CloseHandle(ready);
      if (done) CloseHandle(done);
   }
};

static void ProcessSharedTest(IDXGIAdapter *adapter)
{
   Device producer = CreateDevice(adapter);
   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = desc.Height = 64;
   desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
   desc.Format = sharedTextureFormat;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
   ComPtr<ID3D11Texture2D> texture;
   ComPtr<ID3D11RenderTargetView> target;
   ComPtr<IDXGIResource> resource;
   Check(producer.device->CreateTexture2D(&desc, NULL, &texture), "Process producer texture");
   Check(producer.device->CreateRenderTargetView(texture.Get(), NULL, &target), "Process producer RTV");
   Check(texture.As(&resource), "Process IDXGIResource");
   HANDLE shared = NULL;
   Check(resource->GetSharedHandle(&shared), "Process GetSharedHandle");
   if (!shared)
      Check(E_FAIL, "Process shared handle must be non-null");

   ChildLifetime child;
   SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
   child.ready = CreateEventW(&inherit, FALSE, FALSE, NULL);
   CheckWin(child.ready != NULL, "Create producer event");
   child.done = CreateEventW(&inherit, FALSE, FALSE, NULL);
   CheckWin(child.done != NULL, "Create consumer event");
   child.job = CreateJobObjectW(NULL, NULL); // Not inheritable by the child.
   CheckWin(child.job != NULL, "Create child lifetime job");
   JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
   limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
   CheckWin(SetInformationJobObject(child.job, JobObjectExtendedLimitInformation,
                                    &limits, sizeof(limits)), "Set child job limits");
   wchar_t executable[MAX_PATH] = {};
   DWORD length = GetModuleFileNameW(NULL, executable, ARRAYSIZE(executable));
   if (!length || length >= ARRAYSIZE(executable))
      Check(E_FAIL, "Resolve child executable");
   wchar_t command[MAX_PATH + 160];
   const wchar_t *childMode = sharedTextureFormat == DXGI_FORMAT_R8G8B8A8_UNORM ?
      L"--rgba-process-child" : L"--process-child";
   if (swprintf_s(command, L"\"%ls\" %ls %llu %llu %llu", executable, childMode,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(shared)),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(child.ready)),
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(child.done))) < 0)
      Check(E_FAIL, "Format child command");
   STARTUPINFOW startup = {};
   startup.cb = sizeof(startup);
   CheckWin(CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_SUSPENDED,
                           NULL, NULL, &startup, &child.process), "Create consumer process");
   if (!AssignProcessToJobObject(child.job, child.process.hProcess)) {
      DWORD error = GetLastError();
      TerminateProcess(child.process.hProcess, 1); // It has never executed.
      Check(HRESULT_FROM_WIN32(error), "Assign consumer lifetime job");
   }
   if (ResumeThread(child.process.hThread) == static_cast<DWORD>(-1))
      Check(HRESULT_FROM_WIN32(GetLastError()), "Resume consumer");
   printf("PROCESS PAIR producer=%lu consumer=%lu\n", GetCurrentProcessId(), child.process.dwProcessId);

   for (unsigned frame = 0; frame < ARRAYSIZE(colors); ++frame) {
      printf("PARENT FRAME %u\n", frame);
      producer.context->ClearRenderTargetView(target.Get(), colors[frame]);
      producer.context->Flush();
      CheckDevice(producer, "Process producer after Flush");
      VerifyPixels(producer, texture.Get(), colors[frame], "cross-process producer");
      CheckWin(SetEvent(child.ready), "Signal producer completion");
      HANDLE waitFor[] = {child.done, child.process.hProcess};
      DWORD wait = WaitForMultipleObjects(ARRAYSIZE(waitFor), waitFor, FALSE, 5000);
      if (wait != WAIT_OBJECT_0)
         Check(wait == WAIT_FAILED ? HRESULT_FROM_WIN32(GetLastError()) : E_FAIL,
               "Consumer readback timeout or early exit");
   }
   WaitEvent(child.process.hProcess, "Wait consumer exit");
   DWORD exitCode = STILL_ACTIVE;
   CheckWin(GetExitCodeProcess(child.process.hProcess, &exitCode), "Get consumer exit code");
   printf("CONSUMER_EXIT=%lu\n", exitCode);
   if (exitCode != 0)
      Check(E_FAIL, "Cross-process consumer must pass");
}

static void CompositionTest(IDXGIAdapter *adapter)
{
   const UINT width = sharedTextureFormat == DXGI_FORMAT_R8G8B8A8_UNORM ? 1280 : 256;
   const UINT height = sharedTextureFormat == DXGI_FORMAT_R8G8B8A8_UNORM ? 224 : 256;
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
                              100, 100, int(width + 64), int(height + 64), NULL, NULL, wc.hInstance, NULL);
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
   Check(comp->CreateSurface(width, height, sharedTextureFormat,
                            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface), "CreateSurface");
   Check(visual->SetContent(surface.Get()), "SetContent");
   Check(target->SetRoot(visual.Get()), "SetRoot");
   for (unsigned frame = 0; frame < ARRAYSIZE(colors); ++frame) {
      printf("DCOMP FRAME %u\n", frame);
      ComPtr<ID3D11Texture2D> texture;
      POINT offset;
      Check(surface->BeginDraw(NULL, IID_PPV_ARGS(&texture), &offset), "BeginDraw");
      CheckSharedFormat(texture.Get());
      ComPtr<ID3D11RenderTargetView> view;
      HRESULT drawHr = d.device->CreateRenderTargetView(texture.Get(), NULL, &view);
      if (SUCCEEDED(drawHr)) {
         const D3D11_RECT rect = {offset.x, offset.y, offset.x + LONG(width), offset.y + LONG(height)};
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

static void ReadQuery(Device &d, ID3D11Query *query, void *data, UINT size)
{
   const ULONGLONG deadline = GetTickCount64() + 5000;
   HRESULT hr;
   do {
      hr = d.context->GetData(query, data, size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (hr == S_OK)
         return;
      if (FAILED(hr))
         CheckDeviceCall(d, hr, "Query GetData");
      Sleep(1);
   } while (GetTickCount64() < deadline);
   Check(E_FAIL, "Query completion timed out");
}

static void QueryPollTest(IDXGIAdapter *adapter)
{
   Device d = CreateDevice(adapter);
   D3D11_TEXTURE2D_DESC texture = {};
   texture.Width = texture.Height = 64;
   texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
   texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   texture.BindFlags = D3D11_BIND_RENDER_TARGET;
   ComPtr<ID3D11Texture2D> source, destination;
   ComPtr<ID3D11RenderTargetView> view;
   Check(d.device->CreateTexture2D(&texture, NULL, &source), "Create query source");
   Check(d.device->CreateTexture2D(&texture, NULL, &destination), "Create query destination");
   Check(d.device->CreateRenderTargetView(source.Get(), NULL, &view), "Create query RTV");
   D3D11_QUERY_DESC desc = {D3D11_QUERY_EVENT, 0};
   ComPtr<ID3D11Query> query;
   Check(d.device->CreateQuery(&desc, &query), "Create polling event");
   unsigned pending = 0, completedEarly = 0;
   LARGE_INTEGER hz;
   QueryPerformanceFrequency(&hz);
   double maxPollMs = 0;
   for (unsigned iteration = 0; iteration < 8; ++iteration) {
      d.context->ClearRenderTargetView(view.Get(), colors[iteration % ARRAYSIZE(colors)]);
      d.context->CopyResource(destination.Get(), source.Get());
      d.context->End(query.Get());
      for (unsigned poll = 0; poll < 8; ++poll) {
         BOOL completed = 0x5a5a5a5a;
         LARGE_INTEGER begin, end;
         QueryPerformanceCounter(&begin);
         HRESULT hr = d.context->GetData(query.Get(), &completed, sizeof completed,
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH);
         QueryPerformanceCounter(&end);
         double ms = double(end.QuadPart - begin.QuadPart) * 1000 / double(hz.QuadPart);
         if (ms > maxPollMs)
            maxPollMs = ms;
         if (hr == S_FALSE) {
            ++pending;
            if (completed != 0x5a5a5a5a)
               Check(E_FAIL, "Pending query modified output data");
         } else {
            CheckDeviceCall(d, hr, "No-flush query");
            if (hr != S_OK || !completed)
               Check(E_FAIL, "Invalid completed event result");
            ++completedEarly;
            break;
         }
      }
      d.context->Flush();
      BOOL completed = FALSE;
      ReadQuery(d, query.Get(), &completed, sizeof completed);
      if (!completed)
         Check(E_FAIL, "Flushed event must complete");

      d.context->CopyResource(destination.Get(), source.Get());
      d.context->End(query.Get());
      const ULONGLONG deadline = GetTickCount64() + 5000;
      HRESULT hr;
      do {
         hr = d.context->GetData(query.Get(), &completed, sizeof completed, 0);
         if (hr != S_FALSE)
            break;
         Sleep(1);
      } while (GetTickCount64() < deadline);
      if (hr != S_OK || !completed)
         Check(E_FAIL, "Default GetData must submit and complete pending work");
   }
   printf("query-poll pending=%u early-complete=%u max-poll=%.3fms\n",
          pending, completedEarly, maxPollMs);
   if (!pending || maxPollMs >= 100)
      Check(E_FAIL, "Controlled pre-flush query polls did not stay pending and bounded");
   CheckDevice(d, "query polling test");
}

static void TimestampTest(IDXGIAdapter *adapter)
{
   Device d = CreateDevice(adapter);
   ComPtr<ID3D11Query> disjoint, first, last, event;
   D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
   Check(d.device->CreateQuery(&desc, &disjoint), "Create timestamp disjoint");
   desc.Query = D3D11_QUERY_TIMESTAMP;
   Check(d.device->CreateQuery(&desc, &first), "Create first timestamp");
   Check(d.device->CreateQuery(&desc, &last), "Create last timestamp");
   desc.Query = D3D11_QUERY_EVENT;
   Check(d.device->CreateQuery(&desc, &event), "Create completion event");

   LARGE_INTEGER hz, begin, end;
   QueryPerformanceFrequency(&hz);
   for (unsigned iteration = 0; iteration < 3; ++iteration) {
      d.context->Begin(disjoint.Get());
      QueryPerformanceCounter(&begin);
      d.context->End(first.Get());
      d.context->End(event.Get());
      d.context->Flush();
      BOOL completed = FALSE;
      ReadQuery(d, event.Get(), &completed, sizeof completed);
      if (!completed)
         Check(E_FAIL, "Event returned false after S_OK");
      // Ensure the GPU interval includes a known wall-clock interval, not just
      // command execution time. The first timestamp has completed before sleep.
      Sleep(200);
      d.context->End(last.Get());
      d.context->End(disjoint.Get());
      d.context->End(event.Get());
      d.context->Flush();
      ReadQuery(d, event.Get(), &completed, sizeof completed);
      QueryPerformanceCounter(&end);
      UINT64 startTicks = 0, endTicks = 0;
      D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result = {};
      ReadQuery(d, first.Get(), &startTicks, sizeof startTicks);
      ReadQuery(d, last.Get(), &endTicks, sizeof endTicks);
      ReadQuery(d, disjoint.Get(), &result, sizeof result);
      if (result.Disjoint || !result.Frequency || endTicks <= startTicks)
         Check(E_FAIL, "Invalid/disjoint timestamp interval");
      const double gpuSeconds = double(endTicks - startTicks) / double(result.Frequency);
      const double wallSeconds = double(end.QuadPart - begin.QuadPart) / double(hz.QuadPart);
      printf("timestamp iteration=%u frequency=%llu delta=%llu gpu=%.6fs wall=%.6fs ratio=%.3f\n",
             iteration, (unsigned long long)result.Frequency,
             (unsigned long long)(endTicks - startTicks), gpuSeconds, wallSeconds,
             gpuSeconds / wallSeconds);
      if (gpuSeconds < 0.15 || gpuSeconds > wallSeconds * 1.5 ||
          gpuSeconds < wallSeconds * 0.5)
         Check(E_FAIL, "GPU timestamp seconds disagree with wall clock");
   }
   CheckDevice(d, "timestamp test");
}

static void CompositionTimingSample()
{
   LARGE_INTEGER frequency;
   QueryPerformanceFrequency(&frequency);
   printf("DWM observation only; collected counters do not prove visible rendering or fps.\n");
   printf("qpc-frequency=%lld timing-size=%zu\n", frequency.QuadPart, sizeof(DWM_TIMING_INFO));
   const ULONGLONG start = GetTickCount64();
   for (unsigned sample = 0; sample <= 20; ++sample) {
      DWM_TIMING_INFO info = {};
      info.cbSize = sizeof(info);
      HRESULT hr = DwmGetCompositionTimingInfo(NULL, &info);
      if (FAILED(hr))
         Check(hr, "Get DWM timing");
      printf("dwm elapsed=%llu refresh=%u/%u compose=%u/%u period=%.3fms "
             "refresh-count=%llu frame=%llu submitted=%llu confirmed=%llu "
             "displayed=%llu complete=%llu pending=%llu late=%llu outstanding=%u "
             "dx-present=%u dx-confirmed=%u\n",
             GetTickCount64() - start,
             info.rateRefresh.uiNumerator, info.rateRefresh.uiDenominator,
             info.rateCompose.uiNumerator, info.rateCompose.uiDenominator,
             1000.0 * static_cast<double>(info.qpcRefreshPeriod) / static_cast<double>(frequency.QuadPart),
             info.cRefresh, info.cFrame, info.cFrameSubmitted, info.cFrameConfirmed,
             info.cFramesDisplayed, info.cFramesComplete, info.cFramesPending,
             info.cFramesLate, info.cFramesOutstanding,
             info.cDXPresent, info.cDXPresentConfirmed);
      if (sample < 20)
         Sleep(1000);
   }
}

static void FormatCapsTest(IDXGIAdapter *adapter)
{
   Device d = CreateDevice(adapter);
   bool valid = true;
   const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
      DXGI_FORMAT_D24_UNORM_S8_UINT};
   for (DXGI_FORMAT format : formats) {
      UINT caps = 0;
      Check(d.device->CheckFormatSupport(format, &caps), "CheckFormatSupport");
      printf("format=%u caps=0x%08x render_target=%d msaa_target=%d msaa_resolve=%d\n",
             unsigned(format), caps, !!(caps & D3D11_FORMAT_SUPPORT_RENDER_TARGET),
             !!(caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET),
             !!(caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE));
      const UINT sampleCounts[] = {1, 2, 4, 8};
      for (UINT count : sampleCounts) {
         UINT quality = 0xdeadbeef;
         HRESULT hr = d.device->CheckMultisampleQualityLevels(format, count, &quality);
         printf("format=%u samples=%u quality=%u hr=0x%08lx\n",
                unsigned(format), count, quality, (unsigned long)hr);
         if (FAILED(hr) || (count == 1 && quality != 1))
            valid = false;
      }
   }
   Check(valid ? S_OK : E_FAIL, "Single-sample quality contract");
   CheckDevice(d, "Format capability queries");
}

static ComPtr<ID3DBlob> CompileMsaaShader(const char *source, const char *profile)
{
   ComPtr<ID3DBlob> code, errors;
   HRESULT hr = D3DCompile(source, strlen(source), NULL, NULL, NULL, "main", profile,
                           D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
   if (errors)
      printf("%s\n", static_cast<const char *>(errors->GetBufferPointer()));
   Check(hr, "Compile MSAA shader");
   return code;
}

static void VerifyMsaaPixels(Device &d, ID3D11Texture2D *texture, UINT subresource,
                             UINT samples, int sample, const char *label)
{
   D3D11_TEXTURE2D_DESC desc;
   texture->GetDesc(&desc);
   const UINT width = desc.Width >> (subresource % desc.MipLevels);
   const UINT height = desc.Height >> (subresource % desc.MipLevels);
   desc.Width = width;
   desc.Height = height;
   desc.MipLevels = desc.ArraySize = 1;
   desc.Usage = D3D11_USAGE_STAGING;
   desc.BindFlags = desc.MiscFlags = 0;
   desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ComPtr<ID3D11Texture2D> staging;
   Check(d.device->CreateTexture2D(&desc, NULL, &staging), "Create MSAA readback");
   d.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, subresource, NULL);
   D3D11_MAPPED_SUBRESOURCE map = {};
   Check(d.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map MSAA readback");
   UINT mismatches = 0;
   for (UINT y = 0; y < height; ++y) {
      const auto *row = static_cast<const unsigned char *>(map.pData) + y * map.RowPitch;
      for (UINT x = 0; x < width; ++x) {
         const float checker = float((x ^ y) & 1);
         // Sample loads deliberately swizzle BGRA; resolves retain RGBA.
         float expected[] = {sample < 0 ? 0.5f : checker,
            sample < 0 ? (samples == 4 ? 0.5f : 0.0f) : float((sample >> 1) & 1),
            sample < 0 ? checker : float(sample & 1), 1.0f};
         if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
            const float red = expected[0];
            expected[0] = expected[2];
            expected[2] = red;
         }
         bool wrong = false;
         for (UINT c = 0; c < 4; ++c) {
            if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
               const auto *half = reinterpret_cast<const uint16_t *>(row);
               const uint16_t wanted = expected[c] == 1 ? 0x3c00 :
                                        expected[c] == 0.5f ? 0x3800 : 0;
               wrong |= half[x * 4 + c] != wanted;
            } else {
               const int wanted = int(expected[c] * 255 + 0.5f);
               wrong |= abs(int(row[x * 4 + c]) - wanted) > 1;
            }
         }
         mismatches += wrong;
      }
   }
   d.context->Unmap(staging.Get(), 0);
   printf("%s format=%u samples=%u selected=%d mismatches=%u/%u\n",
          label, unsigned(desc.Format), samples, sample, mismatches, width * height);
   Check(mismatches ? E_FAIL : S_OK, "MSAA pixel contents");
}

static void MultisampleTest(IDXGIAdapter *adapter)
{
   Device d = CreateDevice(adapter);
   const char *vsSource = "float4 main(uint id:SV_VertexID):SV_Position {"
      "float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}";
   const char *drawSource = "cbuffer Params:register(b0){uint4 choice;}"
      "float4 main(float4 p:SV_Position):SV_Target{"
      "return float4(choice.x&1,(choice.x>>1)&1,(uint(p.x)^uint(p.y))&1,1);}";
   auto vsCode = CompileMsaaShader(vsSource, "vs_4_1");
   auto drawCode = CompileMsaaShader(drawSource, "ps_4_1");
   ComPtr<ID3D11VertexShader> vs;
   ComPtr<ID3D11PixelShader> draw;
   Check(d.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), NULL, &vs), "Create MSAA VS");
   Check(d.device->CreatePixelShader(drawCode->GetBufferPointer(), drawCode->GetBufferSize(), NULL, &draw), "Create MSAA PS");
   D3D11_BUFFER_DESC cbDesc = {};
   cbDesc.ByteWidth = 16;
   cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   ComPtr<ID3D11Buffer> constants;
   Check(d.device->CreateBuffer(&cbDesc, NULL, &constants), "Create MSAA parameters");
   ID3D11Buffer *cb = constants.Get();
   d.context->PSSetConstantBuffers(0, 1, &cb);
   d.context->VSSetShader(vs.Get(), NULL, 0);
   d.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   D3D11_VIEWPORT viewport = {0, 0, 16, 16, 0, 1};
   d.context->RSSetViewports(1, &viewport);
   D3D11_RASTERIZER_DESC rsDesc = {};
   rsDesc.FillMode = D3D11_FILL_SOLID;
   rsDesc.CullMode = D3D11_CULL_NONE;
   rsDesc.DepthClipEnable = TRUE;
   // FL10_1+ must multisample triangles even when MultisampleEnable is false.
   ComPtr<ID3D11RasterizerState> rs;
   Check(d.device->CreateRasterizerState(&rsDesc, &rs), "Create MSAA rasterizer");
   d.context->RSSetState(rs.Get());
   const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT,
      DXGI_FORMAT_R8G8B8A8_TYPELESS};
   const UINT counts[] = {2, 4};
   for (DXGI_FORMAT storage : formats) {
      const DXGI_FORMAT format = storage == DXGI_FORMAT_R8G8B8A8_TYPELESS ?
                                  DXGI_FORMAT_R8G8B8A8_UNORM : storage;
      for (UINT count : counts) {
         printf("MSAA case storage=%u typed=%u samples=%u\n", unsigned(storage), unsigned(format), count);
         UINT quality = 0, caps = 0;
         Check(d.device->CheckFormatSupport(format, &caps), "Query MSAA format");
         Check(d.device->CheckMultisampleQualityLevels(format, count, &quality), "Query MSAA quality");
         Check(quality && (caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) &&
               (caps & D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD) ? S_OK : E_FAIL,
               "Real MSAA support required (unsupported is not a pass)");
         D3D11_TEXTURE2D_DESC desc = {};
         desc.Width = desc.Height = 16;
         desc.MipLevels = 1;
         desc.ArraySize = 2;
         desc.SampleDesc.Count = count;
         desc.Format = storage;
         desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
         ComPtr<ID3D11Texture2D> source, output, resolved;
         Check(d.device->CreateTexture2D(&desc, NULL, &source), "Create multisample array");
         D3D11_RENDER_TARGET_VIEW_DESC rtDesc = {};
         rtDesc.Format = format;
         rtDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
         rtDesc.Texture2DMSArray.FirstArraySlice = 1;
         rtDesc.Texture2DMSArray.ArraySize = 1;
         ComPtr<ID3D11RenderTargetView> msTarget, target;
         Check(d.device->CreateRenderTargetView(source.Get(), &rtDesc, &msTarget), "Create MSAA array RTV");
         D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
         srvDesc.Format = format;
         srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
         srvDesc.Texture2DMSArray.ArraySize = 2;
         ComPtr<ID3D11ShaderResourceView> input;
         Check(d.device->CreateShaderResourceView(source.Get(), &srvDesc, &input), "Create MSAA array SRV");
         desc.Format = format;
         desc.ArraySize = desc.SampleDesc.Count = 1;
         Check(d.device->CreateTexture2D(&desc, NULL, &output), "Create sample-load output");
         Check(d.device->CreateRenderTargetView(output.Get(), NULL, &target), "Create sample-load RTV");
         desc.Width = desc.Height = 32;
         desc.ArraySize = desc.MipLevels = 2;
         Check(d.device->CreateTexture2D(&desc, NULL, &resolved), "Create mip-array resolve destination");
         ID3D11RenderTargetView *rtv = msTarget.Get();
         d.context->OMSetRenderTargets(1, &rtv, NULL);
         const float black[] = {0, 0, 0, 0};
         d.context->ClearRenderTargetView(rtv, black);
         d.context->PSSetShader(draw.Get(), NULL, 0);
         for (UINT sample = 0; sample < count; ++sample) {
            const UINT params[] = {sample, 0, 0, 0};
            d.context->UpdateSubresource(cb, 0, NULL, params, 0, 0);
            d.context->OMSetBlendState(NULL, NULL, 1u << sample);
            d.context->Draw(3, 0);
         }
         d.context->OMSetRenderTargets(0, NULL, NULL);
         d.context->ResolveSubresource(resolved.Get(), 3, source.Get(), 1, format);
         VerifyMsaaPixels(d, resolved.Get(), 3, count, -1, "resolve to array1/mip1");
         char loadSource[512];
         sprintf_s(loadSource, "Texture2DMSArray<float4,%u> tex:register(t0);"
            "cbuffer Params:register(b0){uint4 choice;}"
            "float4 main(float4 p:SV_Position):SV_Target{"
            "return tex.Load(int3(int2(p.xy)-int2(1,0),1),choice.x,int2(1,0)).bgra;}", count);
         auto loadCode = CompileMsaaShader(loadSource, "ps_4_1");
         ComPtr<ID3D11PixelShader> load;
         Check(d.device->CreatePixelShader(loadCode->GetBufferPointer(), loadCode->GetBufferSize(), NULL, &load), "Create LD_MS PS");
         rtv = target.Get();
         ID3D11ShaderResourceView *srv = input.Get();
         d.context->OMSetRenderTargets(1, &rtv, NULL);
         d.context->OMSetBlendState(NULL, NULL, ~0u);
         d.context->PSSetShaderResources(0, 1, &srv);
         d.context->PSSetShader(load.Get(), NULL, 0);
         for (UINT sample = 0; sample < count; ++sample) {
            const UINT params[] = {sample, 0, 0, 0};
            d.context->UpdateSubresource(cb, 0, NULL, params, 0, 0);
            d.context->Draw(3, 0);
            VerifyMsaaPixels(d, output.Get(), 0, count, int(sample), "load array1/offset/swizzle");
         }
         srv = NULL;
         d.context->PSSetShaderResources(0, 1, &srv);
         d.context->OMSetRenderTargets(0, NULL, NULL);
         CheckDevice(d, "MSAA case");
      }
   }
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   PrintClientIdentity();
   const char *mode = argc >= 2 ? argv[1] : "--shared";
   const char *requestedMode = mode;
   const struct { const char *requested; const char *operation; } rgbaModes[] = {
      {"--rgba-shared", "--shared"}, {"--rgba-sample", "--sample"},
      {"--rgba-keyed", "--keyed"}, {"--rgba-nt", "--nt"},
      {"--rgba-partial", "--partial"}, {"--rgba-process", "--process"},
      {"--rgba-process-child", "--process-child"}, {"--rgba-dcomp", "--dcomp"},
      {"--rgba-local-warp", "--local"},
   };
   for (const auto &entry : rgbaModes) {
      if (!strcmp(mode, entry.requested)) {
         sharedTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
         mode = entry.operation;
         break;
      }
   }
   const bool child = !strcmp(mode, "--process-child") && argc == 5;
   if (!child && ((argc != 1 && argc != 2) ||
       (strcmp(mode, "--local") && strcmp(mode, "--shared") && strcmp(mode, "--process") &&
       strcmp(mode, "--keyed") && strcmp(mode, "--nt") && strcmp(mode, "--dcomp") &&
       strcmp(mode, "--sample") && strcmp(mode, "--sample-reuse") &&
       strcmp(mode, "--sample-reuse-unbind") && strcmp(mode, "--sample-reuse-wait") &&
       strcmp(mode, "--sample-reuse-warp") && strcmp(mode, "--buffer-reuse") &&
       strcmp(mode, "--buffer-rebind") && strcmp(mode, "--buffer-first") &&
       strcmp(mode, "--buffer-static") && strcmp(mode, "--buffer-vertexid") && strcmp(mode, "--buffer-nocull") &&
       strcmp(mode, "--buffer-arrays") && strcmp(mode, "--buffer-zero-offset") &&
       strcmp(mode, "--buffer-fetch") && strcmp(mode, "--buffer-large") && strcmp(mode, "--buffer-fetch-large") &&
       strcmp(mode, "--buffer-signatures") && strcmp(mode, "--format-caps") &&
       strcmp(mode, "--msaa") && strcmp(mode, "--msaa-warp") &&
       strcmp(mode, "--partial") && strcmp(mode, "--lifetime") &&
       strcmp(mode, "--shader-lifetime") && strcmp(mode, "--timestamp") &&
       strcmp(mode, "--query-poll") && strcmp(mode, "--dwm-timing")))) {
      printf("Usage: d3d11_shared_test [--local|--shared|--process|--keyed|--nt|--sample|--sample-reuse|--buffer-reuse|--buffer-rebind|--buffer-first|--buffer-static|--buffer-vertexid|--buffer-nocull|--buffer-arrays|--buffer-zero-offset|--partial|--lifetime|--shader-lifetime|--dcomp|--timestamp|--query-poll|--dwm-timing]\n");
      printf("Additional buffer isolation: --buffer-fetch|--buffer-large|--buffer-fetch-large|--buffer-signatures\n");
      printf("Shared sampling controls: --sample-reuse-unbind|--sample-reuse-wait|--sample-reuse-warp\n");
      printf("Format capability contract: --format-caps\n");
      printf("Multisample render/resolve/load: --msaa|--msaa-warp (reference only)\n");
      printf("RGBA shared atlas: --rgba-shared|--rgba-sample|--rgba-keyed|--rgba-nt|--rgba-partial|--rgba-process|--rgba-dcomp|--rgba-local-warp\n");
      return 2;
   }
   DWORD session;
   ProcessIdToSessionId(GetCurrentProcessId(), &session);
   printf("mode=%s session=%lu pid=%lu\n", requestedMode, session, GetCurrentProcessId());
   warpControl = !strcmp(mode, "--sample-reuse-warp") || !strcmp(mode, "--msaa-warp") ||
                 !strcmp(requestedMode, "--rgba-local-warp");
   try {
      if (!strcmp(mode, "--buffer-signatures")) {
         BufferSignatureTest();
         printf("RESULT: PASS compiled signatures only; no GPU rendering tested\n");
         return 0;
      }
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
      if (!adapter && !warpControl)
         Check(E_FAIL, "VIOGPU hardware adapter required");
      if (child)
         ProcessConsumer(adapter.Get(), ParseHandle(argv[2]), ParseHandle(argv[3]), ParseHandle(argv[4]));
      else if (!strcmp(mode, "--process"))
         ProcessSharedTest(adapter.Get());
      else if (!strcmp(mode, "--dcomp"))
         CompositionTest(adapter.Get());
      else if (!strcmp(mode, "--format-caps"))
         FormatCapsTest(adapter.Get());
      else if (!strcmp(mode, "--msaa") || !strcmp(mode, "--msaa-warp"))
         MultisampleTest(adapter.Get());
      else if (!strcmp(mode, "--shader-lifetime"))
         ShaderLifetimeTest(adapter.Get());
      else if (!strcmp(mode, "--sample-reuse") || !strcmp(mode, "--sample-reuse-unbind") ||
               !strcmp(mode, "--sample-reuse-wait") || !strcmp(mode, "--sample-reuse-warp"))
         SharedSampleReuseTest(adapter.Get(), mode);
      else if (!strncmp(mode, "--buffer-", 9))
         DynamicBufferReuseTest(adapter.Get(), mode);
      else if (!strcmp(mode, "--timestamp"))
         TimestampTest(adapter.Get());
      else if (!strcmp(mode, "--query-poll"))
         QueryPollTest(adapter.Get());
      else if (!strcmp(mode, "--dwm-timing"))
         CompositionTimingSample();
      else
         SharedTest(adapter.Get(), mode);
      printf("RESULT: PASS mode=%s%s\n", requestedMode,
             warpControl ? " reference WARP only; not VIOGPU validation" : "");
      return 0;
   } catch (HRESULT hr) {
      printf("RESULT: FAIL mode=%s hr=0x%08lx\n", requestedMode, (unsigned long)hr);
      return 1;
   }
}
