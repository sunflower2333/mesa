param(
    [Parameter(Mandatory = $true)]
    [string]$Output,
    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'arm64')]
    [string]$Architecture,
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Check every native exit code before using any generated file.
function Check-Exit {
    param([string]$Operation)
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed: $LASTEXITCODE" }
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$legacyTests = Join-Path $root '../vulkan/tests'
if (Test-Path -LiteralPath $Output) { throw 'Output already exists; refusing stale object files' }
New-Item -ItemType Directory -Path $Output | Out-Null
$destination = (Resolve-Path -LiteralPath $Output).Path
Get-Command cl.exe -ErrorAction Stop | Out-Null
Get-Command lib.exe -ErrorAction Stop | Out-Null
Get-Command dumpbin.exe -ErrorAction Stop | Out-Null
Push-Location -LiteralPath $destination
try {
    $common = @('/nologo', '/W4', '/WX', '/MT', "/I$root", '/D_WIN32_WINNT=0x0A00',
                '/DNTDDI_VERSION=0x0A00000C')
    & cl @common /std:c++17 /EHsc /c "$root/freedreno_wddm.cc" "$root/tu_wddm_dispatch.cc"
    Check-Exit 'native transport C++ compile'
    & cl @common /std:c11 /c "$root/freedreno_wddm_submit.c"
    Check-Exit 'native packet C compile'
    & lib /nologo /out:freedreno_wddm.lib freedreno_wddm.obj tu_wddm_dispatch.obj freedreno_wddm_submit.obj
    Check-Exit 'native static library'
    & cl @common /std:c++17 /EHsc "$PSScriptRoot/native_link_test.cpp" freedreno_wddm.lib /Fe:native_link_test.exe
    Check-Exit 'Vulkan-free native link'
    & cl @common /std:c11 "$PSScriptRoot/packet_test.c" freedreno_wddm.lib /Fe:packet_test.exe
    Check-Exit 'native packet tests link'
    # Existing fixture injects dispatch functions itself; do not link real dispatch here.
    & cl @common /std:c++17 /EHsc "$legacyTests/tu_wddm_render_test.cpp" freedreno_wddm.obj /Fe:transport_test.exe
    Check-Exit 'existing fake-dispatch fixture link'
    $expectedMachine = if ($Architecture -eq 'arm64') { 'AA64' } else { '8664' }
    foreach ($name in @('native_link_test.exe', 'packet_test.exe', 'transport_test.exe')) {
        $headers = (& dumpbin /nologo /headers $name 2>&1 | Out-String)
        Check-Exit "PE check $name"
        if ($headers -notmatch "(?i)\b$expectedMachine machine") { throw "Wrong architecture: $name" }
        $imports = (& dumpbin /nologo /dependents $name 2>&1 | Out-String)
        Check-Exit "import check $name"
        if ($imports -match '(?i)\b(?:vulkan-1|vulkan_freedreno|opengl32|libgallium_wgl)\.dll\b') {
            throw "Unexpected graphics-API dependency in $name"
        }
    }
    if ($RunTests) {
        if ($Architecture -ne 'x64') { throw 'This x64 runner does not execute ARM64 tests' }
        & .\native_link_test.exe
        Check-Exit 'native link test execution'
        & .\packet_test.exe
        Check-Exit 'native packet test execution'
        & .\transport_test.exe
        Check-Exit 'fake-dispatch test execution'
    }
    Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination README.md
    Write-Host 'Built native KMT transport only; this is not an OpenGL DLL or GPU acceptance result.'
}
finally {
    Pop-Location
}
