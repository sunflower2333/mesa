param(
    [Parameter(Mandatory=$true)][ValidateSet('arm64','x64','x86')][string]$Architecture,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$LoaderBuild,
    [Parameter(Mandatory=$true)][string]$Stage
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
New-Item -ItemType Directory -Path $Stage | Out-Null
$Stage = (Resolve-Path $Stage).Path
$payload = @('opengl32.dll','libgallium_wgl.dll','libEGL.dll','libGLESv1_CM.dll','libGLESv2.dll','vulkan_freedreno.dll','z-1.dll')
foreach ($name in $payload + @('vulkan-1.dll')) {
    $root = if ($name -eq 'vulkan-1.dll') { $LoaderBuild } else { $Build }
    $found = @(Get-ChildItem $root -Recurse -File -Filter $name)
    if ($found.Count -ne 1) { throw "Missing/ambiguous payload $name : $($found.Count)" }
    Copy-Item $found[0].FullName $Stage
    $pdb = [IO.Path]::ChangeExtension($found[0].FullName, '.pdb')
    if (Test-Path $pdb) { Copy-Item $pdb $Stage }
}
cl /nologo /W4 /WX /EHsc /MT /I include bin/windows-opengl-probe.cpp /Fe:"$Stage/opengl-probe.exe" user32.lib gdi32.lib
if ($LASTEXITCODE) { throw 'OpenGL probe compilation failed' }
$machine = @{arm64='AA64'; x64='8664'; x86='14C'}[$Architecture]
foreach ($file in Get-ChildItem $Stage -File | Where-Object {$_.Extension -in '.exe','.dll'}) {
    $headers = (& dumpbin /headers $file.FullName) -join "`n"
    if ($LASTEXITCODE -or $headers -notmatch "(?im)^\s*$machine machine") { throw "Wrong architecture: $($file.Name)" }
    $imports = (& dumpbin /dependents $file.FullName) -join "`n"
    if ($LASTEXITCODE -or $imports -match '(?i)\b(?:msvcp|vcruntime)[0-9_]*\.dll\b') { throw "Undeclared CRT import: $($file.Name)" }
    $imports | Set-Content "$Stage/$($file.Name).imports.txt"
}
$required = @{
    'opengl32.dll' = @('wglCreateContext','wglMakeCurrent','wglGetProcAddress','glGetString','glReadPixels')
    'libgallium_wgl.dll' = @('DrvCreateContext','DrvGetProcAddress','DrvSetContext','DrvSwapBuffers')
    'libEGL.dll' = @('eglGetDisplay','eglInitialize','eglCreateContext','eglMakeCurrent','eglGetProcAddress')
    'libGLESv1_CM.dll' = @('glGetString','glDrawArrays','glReadPixels')
    'libGLESv2.dll' = @('glGetString','glCreateShader','glDrawArrays','glReadPixels')
    'vulkan_freedreno.dll' = @('vk_icdNegotiateLoaderICDInterfaceVersion','vk_icdGetInstanceProcAddr','vk_icdGetPhysicalDeviceProcAddr')
    'vulkan-1.dll' = @('vkGetInstanceProcAddr','vkGetDeviceProcAddr')
}
foreach ($dll in $required.Keys) {
    $exports = (& dumpbin /exports "$Stage/$dll") -join "`n"
    if ($LASTEXITCODE) { throw "Cannot read exports: $dll" }
    $exports | Set-Content "$Stage/$dll.exports.txt"
    foreach ($symbol in $required[$dll]) {
        $pattern = '(?m)^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+' + [regex]::Escape($symbol) + '(?:\s*=\s*\S+)?\s*$'
        if ($exports -notmatch $pattern) { throw "Missing undecorated export $dll : $symbol" }
    }
}
$pointerSize = if ($Architecture -eq 'x86') { 4 } else { 8 }
python src/vulkan/util/vk_icd_gen.py --api-version 1.4 --xml src/vulkan/registry/vk.xml `
    --icd-lib-path . --icd-filename vulkan_freedreno.dll --sizeof-pointer $pointerSize --use-backslash `
    --out "$Stage/freedreno_icd.json"
if ($LASTEXITCODE) { throw 'ICD manifest generation failed' }
Copy-Item bin/run-windows-opengl.ps1,bin/verify-windows-opengl.ps1,bin/windows-opengl-snapshot.ps1,bin/windows-opengl-candidate.md $Stage
git rev-parse HEAD | Set-Content "$Stage/source-commit.txt"
git -C external/vulkan-loader rev-parse HEAD | Set-Content "$Stage/loader-source-commit.txt"
if ($Architecture -ne 'arm64') {
    Push-Location $Stage
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ./run-windows-opengl.ps1 -Mode load-only
        if ($LASTEXITCODE) { throw 'Real architecture loader/export execution failed' }
    } finally { Pop-Location }
}
Get-ChildItem $Stage -File | Sort-Object Name | ForEach-Object {
    (Get-FileHash $_.FullName).Hash.ToLowerInvariant() + '  ' + $_.Name
} | Set-Content "$Stage/SHA256SUMS.txt" -Encoding ascii
Write-Host 'Candidate only: GPU raster/readback/presentation and stability still require the target device.'
