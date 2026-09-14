param(
    [ValidateRange(5, 300)]
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Verify the flat payload before creating a process; never install or register DLLs.
$receipt = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'bundle.json') -Raw | ConvertFrom-Json
if ($receipt.schema -ne 1 -or $receipt.architecture -ne 'arm64' -or
    $receipt.native_freedreno_gallium -ne $false) {
    throw 'Unexpected bundle format'
}
$expected = @('opengl32.dll', 'libgallium_wgl.dll', 'vulkan_freedreno.dll', 'z-1.dll',
              'zink_wgl_probe.exe', 'freedreno_icd.arm64.json', 'run-probe.ps1', 'README.md')
$names = @($receipt.files.PSObject.Properties.Name)
if (@(Compare-Object ($expected | Sort-Object) ($names | Sort-Object)).Count -ne 0) {
    throw 'Bundle receipt has an unexpected inventory'
}
foreach ($name in $expected) {
    $path = Join-Path $PSScriptRoot $name
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Bundle path must be a plain file: $name"
    }
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $receipt.files.$name) { throw "Bundle hash mismatch: $name" }
}
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne
    [Runtime.InteropServices.Architecture]::Arm64) {
    throw 'This candidate requires Windows ARM64, not x64/x86 emulation'
}

# Environment changes apply only to the child, not the shell or system registry.
$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = Join-Path $PSScriptRoot 'zink_wgl_probe.exe'
$info.Arguments = '--render'
$info.WorkingDirectory = $PSScriptRoot
$info.UseShellExecute = $false
$info.EnvironmentVariables['GALLIUM_DRIVER'] = 'zink'
$icd = Join-Path $PSScriptRoot 'freedreno_icd.arm64.json'
$info.EnvironmentVariables['VK_DRIVER_FILES'] = $icd
$info.EnvironmentVariables['VK_ICD_FILENAMES'] = $icd
foreach ($name in @('LIBGL_ALWAYS_SOFTWARE', 'MESA_GL_VERSION_OVERRIDE',
                    'MESA_GLSL_VERSION_OVERRIDE', 'MESA_EXTENSION_OVERRIDE',
                    'VK_ADD_DRIVER_FILES', 'TU_WDDM_DEFERRED_BO_DESTROY')) {
    $info.EnvironmentVariables.Remove($name)
}
$process = [Diagnostics.Process]::new()
$process.StartInfo = $info
try {
    if (-not $process.Start()) { throw 'Probe did not start' }
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        throw 'Probe timed out; target GPU/display acceptance FAILED'
    }
    if ($process.ExitCode -ne 0) { throw "Probe failed with exit code $($process.ExitCode)" }
}
finally {
    $process.Dispose()
}
