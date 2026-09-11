# Windows Zink OpenGL and OpenGL ES candidate

Each archive is a single architecture. ARM64 is native; x64 is actual AMD64
code executed by Windows emulation on ARM64; x86 is actual 32-bit code. Do not
mix DLLs across archives. This is Zink -> Vulkan -> Turnip, with no ANGLE,
DirectX translation, or software Gallium driver compiled into the package.

Extract to a new directory. Keep every DLL together. No global ICD registry
change is required. `verify-windows-opengl.ps1 -Architecture arm64` (or x64/x86)
checks payload hashes and PE types without loading code.
`run-windows-opengl.ps1 -Mode load-only` checks DLL loading
and public entrypoints without creating a GPU device. On the target device,
run `-Mode wgl`, `-Mode gles2`, and `-Mode gles1` separately in a coordinated
test window from a normal, non-elevated interactive user session. Windows SSH
commonly runs elevated; Vulkan ignores VK_DRIVER_FILES there. For remote GPU
testing use an Interactive scheduled task with the same process environment.
The launcher rejects elevated GPU runs to prevent testing a different ICD.
Each GPU probe renders and checks pixels and prints the actual
GL renderer; software renderers or a non-Turnip Zink renderer fail. A successful
probe is bounded raster evidence, not prolonged FurMark or desktop acceptance.

For application testing, stage the complete matching architecture DLL set
beside a separate copy of the application, then launch with GALLIUM_DRIVER=zink
and VK_DRIVER_FILES set to the absolute freedreno_icd.json path. Preserve all
original application files. Some applications explicitly load the system
opengl32.dll; those need separate ICD installation and are not covered by
this application-local package. Never install the x64 DLL as a WOW32 ICD.

The archive records Mesa and Vulkan loader commits plus SHA256SUMS.txt.
CI verifies machine types, public exports, and real x86/x64 load execution;
ARM64 load and all accelerated rendering require the ARM64 target.
