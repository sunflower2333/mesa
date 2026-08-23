# Turnip WDDM ICD bundle

The ARM64 workflow stages `vulkan_freedreno.dll`, `z-1.dll`, an app-local ICD
manifest, `SHA256SUMS.txt`, and `turnip-wddm-icd.ps1`. The script verifies every
staged hash and both PE machine types before changing the system Vulkan ICD
registry. Install, upgrade, and uninstall use a transaction directory beside
the final install root; any file or registry failure restores the previous
three-file set and exact registry value before returning an error.
The ICD, zlib, and workload probes use the static MSVC runtime so the bundle
does not depend on a separately installed ARM64 Visual C++ redistributable.
`-WhatIf` still performs the read-only hash and PE validation before declining
the registry and file transaction.

The WDDM build exposes `VK_KHR_win32_surface` and uses Mesa's existing CPU/GDI
Win32 WSI path. It deliberately does not select the generic D3D12/DXGI
swapchain backend; the KMD/guest path remains responsible for GPU work and the
first Win32 present baseline is CPU copy.

This baseline exercises Turnip rendering and a CPU-visible WDDM allocation, then
copies the mapped image into the window DC. It does not exercise the KMD
`DxgkDdiPresent` callback; that callback remains a separate Windows display
runtime gate.

The unprotected-VM backend allocates pageable guest RAM through VidMm; it does
not consume a fixed restricted-DMA pool. Vulkan heap size and budget therefore
use Mesa's system-memory estimate, while heap usage charges each WDDM BO's
page-rounded backing extent. If KMT Unlock or DestroyAllocation fails, both the
BO owner and its heap charge remain live until final destruction succeeds.

Legacy MSM command streams can expose raw IOVAs through buffer device address,
so a submit cannot prove an exact resource dependency closure. Every live WDDM
BO is included in each nonempty submit. The matching UMD/KMD contract supports
1024 references and up to 256 command records inside the 64 KiB DMA buffer;
allocation creation returns `VK_ERROR_OUT_OF_DEVICE_MEMORY` before the live BO
set could exceed that capacity. The temporary reference arrays are allocated
to the actual count. This preserves residency correctness but remains a
bounded scalability limit.

The ARM64 bundle also contains a direct KMT diagnostic and four runtime probes.
`tu_wddm_kmt_probe_arm64.exe` enumerates adapters through
`D3DKMTEnumAdapters2`, claims only the DroidVM private endpoint, and reports
the exact status of adapter, device, context, and context-info bring-up while
closing every acquired handle. It does not load Vulkan or a D3D UMD.
`tu_wddm_vulkan_probe_arm64.exe`
checks ICD loading, adapter identity, queue discovery, and device creation;
`tu_wddm_vulkan_compute_probe_arm64.exe` takes `tu_wddm_compute.spv`, submits a
real storage-buffer compute dispatch, waits on a Vulkan fence, and verifies all
256 results; `tu_wddm_vulkan_graphics_probe_arm64.exe` takes
`tu_wddm_graphics.vert.spv` and `tu_wddm_graphics.frag.spv`, renders a
fullscreen triangle into a 64x64 optimal RGBA8 image, copies it to a
host-visible buffer, waits on a real fence, and verifies every pixel is
`rgba(255,0,0,255)`; `tu_wddm_win32_probe_arm64.exe` creates a Win32 window,
queries surface support/formats/present modes, and creates a CPU/GDI swapchain.
It acquires an image with a fence, records a clear and both image-layout
transitions, submits the command buffer, and calls `vkQueuePresentKHR`. It then
resizes, minimizes, restores, recreates the swapchain through `oldSwapchain`,
and presents a second frame. The CI jobs only cross-compile and package these
inputs. They do not execute them or prove Windows/KMT/Host runtime behavior.

Run the offscreen graphics workload from the extracted app-local bundle with:

```powershell
.\tu_wddm_vulkan_graphics_probe_arm64.exe `
  .\tu_wddm_graphics.vert.spv .\tu_wddm_graphics.frag.spv
```

Run installation only after the matching DroidVM Native Context KMD is active:

```powershell
pwsh -File .\turnip-wddm-icd.ps1 -Action Install
```

The installed files live under `%ProgramFiles%\DroidVM\Turnip`. The registry
value is written through the 64-bit view at
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers`.

Uninstall removes only that exact registry value and the three files owned by
the installer. An unexpected file keeps the install directory intact:

```powershell
pwsh -File .\turnip-wddm-icd.ps1 -Action Uninstall
```

Use `-WhatIf` with either action to inspect the requested changes without
modifying the machine.
