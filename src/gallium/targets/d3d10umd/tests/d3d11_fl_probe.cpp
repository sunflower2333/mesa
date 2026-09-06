/*
 * Native ARM64 D3D11 feature-level probe for the VioGPU native-context adapter.
 *
 * Two things make this probe necessary rather than convenient.
 *
 * First, process architecture.  On Windows ARM64 a .NET Framework harness built
 * with Framework64\csc.exe runs as an *emulated x64* process, and this driver
 * publishes no UserModeDriverNameWow, so D3D11CreateDevice there can only ever
 * return DXGI_ERROR_UNSUPPORTED without loading a user-mode driver at all.
 * IsWow64Process2 does not make that obvious: Windows 11's x64-on-ARM64
 * emulation is not classic WOW64, so an emulated x64 process reports
 * processMachine = IMAGE_FILE_MACHINE_UNKNOWN and looks native.  This probe is
 * built for aarch64-pc-windows-msvc and prints what it actually is, so no
 * measurement taken with it can be misread that way again.
 *
 * Second, feature level.  Adapter.cpp's GetCaps reports
 * D3D11DDICAPS_3DPIPELINESUPPORT with 9_1..10_1 only - tessellation, compute,
 * UAV and indirect draw are unimplemented - so a caller that asks for 11_0, as
 * the default D3D11CreateDevice feature-level array does, cannot be satisfied.
 * Each level is therefore requested on its own and reported separately.
 */

#include <windows.h>
#include <winternl.h>   /* UNICODE_STRING, NTSTATUS */
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

/* LdrRegisterDllNotification is not in any import library. */
typedef struct _LDR_DLL_NOTIFICATION_DATA_UNION {
   ULONG Flags;
   const UNICODE_STRING *FullDllName;
   const UNICODE_STRING *BaseDllName;
   PVOID DllBase;
   ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA_UNION;

typedef VOID(CALLBACK *PLDR_DLL_NOTIFICATION)(ULONG reason,
                                              const LDR_DLL_NOTIFICATION_DATA_UNION *data,
                                              PVOID context);
typedef NTSTATUS(NTAPI *PFN_LDR_REGISTER)(ULONG flags, PLDR_DLL_NOTIFICATION cb,
                                          PVOID context, PVOID *cookie);

static bool g_umd_loaded = false;
static wchar_t g_umd_path[MAX_PATH] = {0};

static VOID CALLBACK
dll_notify(ULONG reason, const LDR_DLL_NOTIFICATION_DATA_UNION *data, PVOID)
{
   if (reason != 1 /* LOADED */ || data == NULL || data->FullDllName == NULL)
      return;
   const UNICODE_STRING *u = data->FullDllName;
   if (u->Buffer == NULL)
      return;
   /* Case-insensitive search for the user-mode driver by name. */
   wchar_t buf[MAX_PATH];
   size_t n = u->Length / sizeof(wchar_t);
   if (n >= MAX_PATH)
      n = MAX_PATH - 1;
   memcpy(buf, u->Buffer, n * sizeof(wchar_t));
   buf[n] = 0;
   for (wchar_t *p = buf; *p; p++)
      *p = (wchar_t)towlower(*p);
   if (wcsstr(buf, L"viogpud3d") != NULL) {
      g_umd_loaded = true;
      if (g_umd_path[0] == 0) {
         memcpy(g_umd_path, u->Buffer, n * sizeof(wchar_t));
         g_umd_path[n] = 0;
      }
   }
}

static const char *
machine_name(USHORT m)
{
   switch (m) {
   case 0:      return "UNKNOWN";
   case 0x8664: return "AMD64/x64";
   case 0xAA64: return "ARM64";
   case 0x14c:  return "I386/x86";
   default:     return "other";
   }
}

static void
report_architecture(void)
{
   /* Compile-time truth first: this file is built for ARM64. */
#if defined(_M_ARM64)
   const char *built_for = "ARM64 (_M_ARM64)";
#elif defined(_M_AMD64)
   const char *built_for = "x64 (_M_AMD64)";
#elif defined(_M_IX86)
   const char *built_for = "x86 (_M_IX86)";
#else
   const char *built_for = "unknown";
#endif
   printf("process:\n");
   printf("  built for              = %s\n", built_for);

   typedef BOOL(WINAPI * PFN_ISWOW64_2)(HANDLE, USHORT *, USHORT *);
   HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
   PFN_ISWOW64_2 fn =
      k32 ? (PFN_ISWOW64_2)GetProcAddress(k32, "IsWow64Process2") : NULL;
   if (fn) {
      USHORT pm = 0, nm = 0;
      if (fn(GetCurrentProcess(), &pm, &nm)) {
         printf("  IsWow64Process2        = processMachine %s, nativeMachine %s\n",
                machine_name(pm), machine_name(nm));
         printf("                           (processMachine UNKNOWN means native; note that\n");
         printf("                            emulated x64 on ARM64 also reports UNKNOWN)\n");
      }
   }
   SYSTEM_INFO si;
   GetNativeSystemInfo(&si);
   printf("  GetNativeSystemInfo    = arch id %u%s\n", si.wProcessorArchitecture,
          si.wProcessorArchitecture == 12 ? " (ARM64)"
                                          : (si.wProcessorArchitecture == 9 ? " (AMD64 - emulated view)" : ""));
}

struct LevelName {
   D3D_FEATURE_LEVEL level;
   const char *name;
};

static const LevelName kLevels[] = {
   { D3D_FEATURE_LEVEL_12_1, "12_1" }, { D3D_FEATURE_LEVEL_12_0, "12_0" },
   { D3D_FEATURE_LEVEL_11_1, "11_1" }, { D3D_FEATURE_LEVEL_11_0, "11_0" },
   { D3D_FEATURE_LEVEL_10_1, "10_1" }, { D3D_FEATURE_LEVEL_10_0, "10_0" },
   { D3D_FEATURE_LEVEL_9_3,  "9_3"  }, { D3D_FEATURE_LEVEL_9_2,  "9_2"  },
   { D3D_FEATURE_LEVEL_9_1,  "9_1"  },
};

/*
 * A device that cannot colour a pixel is not a working device, so the highest
 * level that creates is also asked to clear a render target and read the result
 * back through a staging copy.
 */
static bool
run_clear_readback(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
   D3D11_TEXTURE2D_DESC td = {};
   td.Width = 64;
   td.Height = 64;
   td.MipLevels = 1;
   td.ArraySize = 1;
   td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   td.SampleDesc.Count = 1;
   td.Usage = D3D11_USAGE_DEFAULT;
   td.BindFlags = D3D11_BIND_RENDER_TARGET;

   ID3D11Texture2D *rt = NULL;
   HRESULT hr = dev->CreateTexture2D(&td, NULL, &rt);
   if (FAILED(hr)) {
      printf("    CreateTexture2D(render target) hr=0x%08lX\n", (unsigned long)hr);
      return false;
   }

   ID3D11RenderTargetView *rtv = NULL;
   hr = dev->CreateRenderTargetView(rt, NULL, &rtv);
   if (FAILED(hr)) {
      printf("    CreateRenderTargetView hr=0x%08lX\n", (unsigned long)hr);
      rt->Release();
      return false;
   }

   const float colour[4] = { 0.25f, 0.50f, 0.75f, 1.0f };
   ctx->ClearRenderTargetView(rtv, colour);

   D3D11_TEXTURE2D_DESC sd = td;
   sd.Usage = D3D11_USAGE_STAGING;
   sd.BindFlags = 0;
   sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
   ID3D11Texture2D *staging = NULL;
   hr = dev->CreateTexture2D(&sd, NULL, &staging);
   if (FAILED(hr)) {
      printf("    CreateTexture2D(staging) hr=0x%08lX\n", (unsigned long)hr);
      rtv->Release();
      rt->Release();
      return false;
   }

   ctx->CopyResource(staging, rt);
   ctx->Flush();

   D3D11_MAPPED_SUBRESOURCE map = {};
   hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
   bool ok = false;
   if (SUCCEEDED(hr)) {
      const uint8_t *px = (const uint8_t *)map.pData;
      /* BGRA8: expect B=191, G=128, R=64, A=255 within rounding. */
      int b = px[0], g = px[1], r = px[2], a = px[3];
      ok = abs(b - 191) <= 2 && abs(g - 128) <= 2 && abs(r - 64) <= 2 && a == 255;
      printf("    readback pixel BGRA = %3d %3d %3d %3d  -> %s\n", b, g, r, a,
             ok ? "MATCHES the cleared colour" : "does NOT match");
      ctx->Unmap(staging, 0);
   } else {
      printf("    Map(staging) hr=0x%08lX\n", (unsigned long)hr);
   }

   staging->Release();
   rtv->Release();
   rt->Release();
   return ok;
}

int
main(void)
{
   report_architecture();

   HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
   PFN_LDR_REGISTER reg =
      ntdll ? (PFN_LDR_REGISTER)GetProcAddress(ntdll, "LdrRegisterDllNotification") : NULL;
   PVOID cookie = NULL;
   if (reg)
      reg(0, dll_notify, NULL, &cookie);
   printf("  LdrRegisterDllNotification = %s\n\n", reg ? "armed" : "UNAVAILABLE");

   IDXGIFactory1 *factory = NULL;
   HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
   if (FAILED(hr)) {
      printf("CreateDXGIFactory1 hr=0x%08lX\n", (unsigned long)hr);
      return 2;
   }

   int probed = 0, created = 0, workload_ok = 0;

   for (UINT i = 0;; i++) {
      IDXGIAdapter1 *adapter = NULL;
      /* Break on any failure, not just DXGI_ERROR_NOT_FOUND: on any other error
       * adapter is left null and would be dereferenced below. */
      HRESULT ehr = factory->EnumAdapters1(i, &adapter);
      if (FAILED(ehr) || adapter == NULL) {
         if (ehr != DXGI_ERROR_NOT_FOUND)
            printf("EnumAdapters1(%u) hr=0x%08lX\n", i, (unsigned long)ehr);
         break;
      }

      DXGI_ADAPTER_DESC1 desc = {};
      adapter->GetDesc1(&desc);
      printf("adapter[%u] %ls\n", i, desc.Description);
      printf("  vendor=0x%04X device=0x%04X luid=0x%08lX%08lX flags=0x%X\n",
             desc.VendorId, desc.DeviceId, (unsigned long)desc.AdapterLuid.HighPart,
             (unsigned long)desc.AdapterLuid.LowPart, desc.Flags);

      /* Only the virtio adapter is the subject; the rest are WARP. */
      if (desc.VendorId != 0x1AF4) {
         printf("  (not the VioGPU adapter, skipped)\n\n");
         adapter->Release();
         continue;
      }
      probed++;

      D3D_FEATURE_LEVEL best = (D3D_FEATURE_LEVEL)0;
      ID3D11Device *best_dev = NULL;
      ID3D11DeviceContext *best_ctx = NULL;

      for (size_t k = 0; k < sizeof(kLevels) / sizeof(kLevels[0]); k++) {
         g_umd_loaded = false;
         D3D_FEATURE_LEVEL want = kLevels[k].level;
         D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
         ID3D11Device *dev = NULL;
         ID3D11DeviceContext *ctx = NULL;

         HRESULT chr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                                         &want, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);

         printf("  %-5s hr=0x%08lX  umd_loaded=%-3s", kLevels[k].name,
                (unsigned long)chr, g_umd_loaded ? "yes" : "no");
         if (SUCCEEDED(chr)) {
            printf("  created at 0x%X\n", (unsigned)got);
            created++;
            if (best_dev == NULL) {
               best = got;
               best_dev = dev;
               best_ctx = ctx;
               dev = NULL;
               ctx = NULL;
            }
         } else {
            printf("\n");
         }
         if (ctx)
            ctx->Release();
         if (dev)
            dev->Release();
      }

      if (g_umd_path[0])
         printf("  user-mode driver loaded: %ls\n", g_umd_path);

      if (best_dev) {
         printf("  running clear+readback at feature level 0x%X:\n", (unsigned)best);
         if (run_clear_readback(best_dev, best_ctx))
            workload_ok++;
         best_ctx->Release();
         best_dev->Release();
      } else {
         printf("  no feature level produced a device on this adapter\n");
      }
      printf("\n");
      adapter->Release();
   }

   factory->Release();

   if (probed == 0) {
      printf("RESULT: the VioGPU adapter was not enumerated by DXGI\n");
      return 3;
   }
   if (created == 0) {
      printf("RESULT: FAIL - no D3D11 device could be created on the VioGPU adapter\n");
      return 1;
   }
   if (workload_ok == 0) {
      printf("RESULT: PARTIAL - a device was created but the clear/readback did not verify\n");
      return 1;
   }
   printf("RESULT: PASS - D3D11 device created on the VioGPU adapter and a cleared "
          "render target read back correctly\n");
   return 0;
}
