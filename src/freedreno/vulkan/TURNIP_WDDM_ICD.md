# Turnip WDDM ICD bundle

The ARM64 workflow stages `vulkan_freedreno.dll`, `z-1.dll`, an app-local ICD
manifest, `SHA256SUMS.txt`, and `turnip-wddm-icd.ps1`. The script verifies every
staged hash and both PE machine types before changing the system Vulkan ICD
registry. Install, upgrade, and uninstall use a transaction directory beside
the final install root; any file or registry failure restores the previous
three-file set and exact registry value before returning an error.

The WDDM build exposes `VK_KHR_win32_surface` and uses Mesa's existing CPU/GDI
Win32 WSI path. It deliberately does not select the generic D3D12/DXGI
swapchain backend; the KMD/guest path remains responsible for GPU work and the
first Win32 present baseline is CPU copy.

This baseline exercises Turnip rendering and a CPU-visible WDDM allocation, then
copies the mapped image into the window DC. It does not exercise the KMD
`DxgkDdiPresent` callback; that callback remains a separate Windows display
runtime gate.

The current CI evidence is ARM64 compile/package evidence. It does not prove
Win32 surface creation, swapchain resize behavior, or runtime present.

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
