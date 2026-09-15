$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Locate required host tools; the CI setup step supplies the Windows SDK/WDK.
function Find-ShaderCompiler {
    $command = Get-Command glslangValidator.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($root in @('C:\VulkanSDK', "$env:ProgramFiles\VulkanSDK")) {
        if (Test-Path -LiteralPath $root) {
            $file = Get-ChildItem -LiteralPath $root -Filter glslangValidator.exe -Recurse -File |
                Where-Object { $_.Directory.Name -eq 'Bin' } | Select-Object -First 1
            if ($file) { return $file.FullName }
        }
    }
    throw 'Host glslangValidator.exe is required'
}

# Check native commands immediately rather than accepting leftover build outputs.
function Check-Exit([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed with exit code $LASTEXITCODE" }
}

$revision = (& git rev-parse HEAD).Trim()
Check-Exit 'git revision'
if ($revision -ne $env:GITHUB_SHA) { throw 'Checked-out source differs from workflow revision' }
$shader = Find-ShaderCompiler
$env:PATH = "$(Split-Path -Parent $shader);$env:PATH"
$flex = Get-Command win_flex.exe -ErrorAction Stop
$bison = Get-Command win_bison.exe -ErrorAction Stop
Write-Host "Shader compiler: $shader"
Write-Host "Flex: $($flex.Source)"
Write-Host "Bison: $($bison.Source)"
& clang-cl --version
Check-Exit 'clang-cl'

$build = Join-Path $env:RUNNER_TEMP 'droidvm-zink-turnip-arm64'
$stage = Join-Path $env:RUNNER_TEMP 'droidvm-opengl-arm64'
if ((Test-Path -LiteralPath $build) -or (Test-Path -LiteralPath $stage)) {
    throw 'Candidate directories already exist; refusing stale outputs'
}
$cross = Join-Path $env:RUNNER_TEMP 'droidvm-opengl-arm64.ini'
@(
    '[binaries]',
    "c = ['clang-cl', '--target=aarch64-pc-windows-msvc']",
    "cpp = ['clang-cl', '--target=aarch64-pc-windows-msvc']",
    "ar = 'llvm-lib'", '',
    '[host_machine]', "system = 'windows'", "cpu_family = 'aarch64'",
    "cpu = 'arm64'", "endian = 'little'", '',
    '[properties]', 'needs_exe_wrapper = true'
) | Set-Content -LiteralPath $cross -Encoding ascii
$options = @(
    'setup', $build, '.', '--cross-file', $cross, '--backend=ninja', '--buildtype=release',
    '-Dplatforms=windows', '-Dvulkan-drivers=freedreno', '-Dfreedreno-kmds=wddm',
    '-Dgallium-drivers=zink', '-Dopengl=true', '-Dgallium-wgl-dll-name=libgallium_wgl',
    '-Dgallium-d3d10umd=false', '-Dgles1=disabled', '-Dgles2=disabled',
    '-Degl=disabled', '-Dglx=disabled', '-Dgbm=disabled', '-Dllvm=disabled',
    '-Dshader-cache=disabled', '-Dzlib=enabled', '-Dzstd=disabled',
    '-Db_vscrt=static_from_buildtype', '-Dtools=[]', '-Dbuild-tests=false', '-Dwerror=false'
)
& meson @options
Check-Exit 'Meson configure'
& meson compile -C $build --ninja-args=-k0 opengl32 libgallium_wgl vulkan_freedreno freedreno_icd
Check-Exit 'combined OpenGL/Turnip compile'

$testRoot = 'src/gallium/drivers/zink/tests/droidvm'
$probe = Join-Path $env:RUNNER_TEMP 'zink_wgl_probe.exe'
& cl /nologo /std:c++17 /W4 /WX /EHsc /MT /D_WIN32_WINNT=0x0A00 `
    "$testRoot/wgl_probe.cpp" "/Fe:$probe" user32.lib gdi32.lib
Check-Exit 'WGL probe compile'
$exports = @{
    'opengl32.dll' = @('wglCreateContext', 'wglChoosePixelFormat', 'wglSetPixelFormat', 'glReadPixels', 'glBegin');
    'libgallium_wgl.dll' = @('DrvCreateContext', 'DrvSetPixelFormat');
    'vulkan_freedreno.dll' = @('vk_icdGetInstanceProcAddr', 'vk_icdNegotiateLoaderICDInterfaceVersion')
}
foreach ($name in $exports.Keys) {
    $files = @(Get-ChildItem -LiteralPath $build -Recurse -File -Filter $name)
    if ($files.Count -ne 1) { throw "Expected exactly one $name" }
    $text = (& dumpbin /nologo /exports $files[0].FullName 2>&1 | Out-String)
    Check-Exit "dumpbin exports $name"
    foreach ($symbol in $exports[$name]) {
        if ($text -notmatch "\b$([regex]::Escape($symbol))\b") { throw "Missing $name export $symbol" }
    }
}
& python "$testRoot/stage_bundle.py" --build $build --probe $probe --output $stage --revision $revision
Check-Exit 'stage and verify candidate'
foreach ($file in Get-ChildItem -LiteralPath $stage -File | Where-Object { $_.Extension -in @('.exe', '.dll') }) {
    $imports = (& dumpbin /nologo /dependents $file.FullName 2>&1 | Out-String)
    Check-Exit "dumpbin imports $($file.Name)"
    if ($imports -match '(?i)\b(?:msvcp|vcruntime)[0-9_]*\.dll\b') {
        throw "Undeclared dynamic MSVC runtime in $($file.Name)"
    }
    if ($file.Name -eq 'zink_wgl_probe.exe' -and $imports -match '(?i)\bopengl32\.dll\b') {
        throw 'Probe must explicitly load app-local OpenGL, not import system OpenGL'
    }
}
$tokens = $null
$parseErrors = $null
[System.Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $stage 'run-probe.ps1'), [ref]$tokens, [ref]$parseErrors) | Out-Null
if ($parseErrors.Count) { throw "Probe launcher parse error: $parseErrors" }
Write-Host 'PASS: combined ARM64 app-local candidate built; no GPU/display operation executed in CI.'
