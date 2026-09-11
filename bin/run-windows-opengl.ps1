param([ValidateSet('load-only','wgl','gles2','gles1')][string]$Mode = 'load-only')
$ErrorActionPreference = 'Stop'
if ($Mode -ne 'load-only') {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run the GPU probe as the normal interactive user: the Vulkan loader ignores candidate environment overrides in elevated processes'
    }
}
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
    $process = Start-Process (Join-Path $PSScriptRoot opengl-probe.exe) -ArgumentList "--$Mode" -NoNewWindow -PassThru
    # Windows PowerShell can otherwise lose the native handle and report a
    # null ExitCode after WaitForExit, even when the process has finished.
    $null = $process.Handle
    if (!$process.WaitForExit(30000)) {
        $process.Kill()
        $process.WaitForExit()
        throw 'OpenGL probe exceeded its 30 second bound'
    }
    $process.Refresh()
    $result = $process.ExitCode
    if ($null -eq $result) { throw 'OpenGL probe exit code was not captured' }
} finally {
    Pop-Location
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
exit $result
