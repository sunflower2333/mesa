param([ValidateSet('load-only','wgl','gles2','gles1')][string]$Mode = 'load-only')
$ErrorActionPreference = 'Stop'
$saved = @{}
foreach ($name in @('VK_DRIVER_FILES','VK_ICD_FILENAMES','GALLIUM_DRIVER','LIBGL_ALWAYS_SOFTWARE','PATH')) {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
Push-Location $PSScriptRoot
try {
    $env:VK_DRIVER_FILES = Join-Path $PSScriptRoot freedreno_icd.json
    $env:VK_ICD_FILENAMES = $env:VK_DRIVER_FILES
    $env:GALLIUM_DRIVER = 'zink'
    $env:LIBGL_ALWAYS_SOFTWARE = 'false'
    $env:PATH = "$PSScriptRoot;$env:PATH"
    & ./opengl-probe.exe "--$Mode"
    $result = $LASTEXITCODE
} finally {
    Pop-Location
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
exit $result
