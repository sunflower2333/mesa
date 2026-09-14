# DroidVM ARM64 OpenGL candidate: Zink over Turnip/WDDM

This is an app-local **Windows ARM64** candidate, not a native Freedreno Gallium
WDDM port. The route is OpenGL -> Mesa WGL/state tracker -> Zink -> Vulkan ->
Turnip -> VIOGPU WDDM. Turnip uses shared Freedreno compiler/layout code; it is
not a Vulkan-to-another-OpenGL-driver translation layer. LLVMpipe is not required.

## A8xx investigation and remaining native work

At baseline Mesa `241131efe0daea2157e67a2f018feb5104f754fa`:

* `src/gallium/drivers/freedreno/freedreno_screen.c` already dispatches generation
  8 to `fd6_screen_init`; the `a6xx` directory contains A8XX-specialized emission,
  rasterization, image and GMEM paths. No new `a8xx` directory is required to
  demonstrate their existence. This is not a conformance claim for every A8xx.
* `meson.build` explicitly rejects Freedreno Gallium with `freedreno-kmds=wddm`.
  `src/freedreno/drm/meson.build` builds MSM and optional virtio support, not a
  Windows Gallium WDDM backend. WGL currently selects Zink rather than Gallium
  Freedreno for this route.
* The earlier Turnip workflow disabled OpenGL and Gallium. The separate D3D10
  Zink UMD workflow is not an OpenGL build. Neither proves a native migration.

A genuine native port still needs a Gallium-compatible WDDM device/pipe/BO/submit
and fence backend, WGL screen/scanout integration, residency and reset lifetime
handling, and device validation. Removing the Meson rejection, renaming a DLL,
or pointing GALLIUM_DRIVER at freedreno does not implement those contracts.
The existing rejection is retained. A8xx image/SSBO/UBWC code is not replaced
with A6xx behavior or altered without a reproducible failing workload.

## Changes and reproducible gates

The new workflow builds **opengl32.dll, libgallium_wgl.dll and
vulkan_freedreno.dll from the same commit**, plus the zlib dependency and a WGL
probe. It verifies ARM64 PE identities, relevant exports and dynamic runtime
imports before staging. `bundle.json` records the revision, route and hashes.
It does not include a Vulkan loader or a KMD; compatible installed versions of
both remain prerequisites. It does not register DLLs, modify DriverStore or
System32, change the system OpenGL ICD, or enable experimental BO retirement.

The last-pipeline fast path in `zink_program_state.hpp` now checks the hash
*and* the existing table's semantic equality predicate. Previously different
baked states with the same 32-bit hash could select a stale pipeline across a
program switch. Both mesh and vertex-input-dynamic paths use the new predicate;
inline and legacy-shadow exclusions are retained. This is a correctness fix,
not a measured speedup. The one-entry shortcut still avoids a hash-table search
for equal states, while candidates incur a semantic comparison.

```sh
python src/gallium/drivers/zink/tests/droidvm/run_cache_test.py
python src/gallium/drivers/zink/tests/droidvm/run_cache_test.py --negative-control
python -m unittest discover -s src/gallium/drivers/zink/tests/droidvm -p 'test_*.py' -v
```

The predicate test extracts that production function, but uses explicitly fake
key/program structures and a fake equality callback. It checks candidate versus
collision decisions, dynamic-only differences, exclusions and four cache slots.
A hash-only mutation must fail at the intended assertion. It does not model GPU
concurrency or execute the full pipeline creation code. The complete Zink source
is compiled/linked separately in the Windows ARM64 gate.

## Target smoke test

Unzip the candidate into a separate directory on a recoverable Windows ARM64
DroidVM guest. Keep it separate from active game directories and system DLLs.
Run in a normal non-elevated shell with an interactive desktop:

```powershell
.\zink_wgl_probe.exe --describe
powershell -NoProfile -File .\run-probe.ps1
```

`--describe` is inert. Only the launcher/`--render` mode loads OpenGL and submits
GPU work. The launcher validates the payload hashes, confines driver-selection
variables to a child process, and enforces a process timeout. It does not change
machine policy or permanently alter shell environment variables. Run without
elevation: Vulkan loader driver-file overrides can be ignored in elevated apps.
A missing loader, unsupported KMD or failed context produces a failure, not a
software fallback PASS. Capture the printed renderer/version and confirm the
actual device model; the probe only requires Zink over Turnip, not a specific GPU.

The render probe performs 16 alternating red/blue clears, 16 triangle draws,
32 center-pixel checks and 16 swaps. It resolves WGL/GL functions from the
app-local opengl32.dll rather than linking system OpenGL. A successful test is
basic render/readback/presentation evidence, not OpenGL conformance, A8xx-wide
support, reset/TDR acceptance, or an FPS comparison. It intentionally performs
readbacks and is not suitable as a performance benchmark.

Plain ARM64 DLLs cannot be injected into x86/x64 applications. ARM64EC/ARM64X,
x86 and x64 OpenGL packaging remain separate deliverables. Shader disk cache,
NIR pass order, descriptor layout policy, and GPU synchronization are unchanged.
