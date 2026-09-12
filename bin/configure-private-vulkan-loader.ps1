# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory)][ValidateSet('arm64','x64','x86')][string]$Architecture,
    [Parameter(Mandatory)][string]$Source
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Patch the pinned loader build source before CMake/link, including its .lib
# identity. No rename or binary rewriting can repair DLL import tables.
$path = Join-Path $Source 'loader/CMakeLists.txt'
$text = [IO.File]::ReadAllText($path)
$old = 'OUTPUT_NAME ${API_TYPE}-1)'
if (($text.Split(@($old), [StringSplitOptions]::None)).Count -ne 2) {
    throw 'Pinned Vulkan loader output-name source changed'
}
$text = $text.Replace($old, "OUTPUT_NAME viogpu_gl_loader_$Architecture)")
[IO.File]::WriteAllText($path, $text)
$path = Join-Path $Source 'loader/vulkan-1.def'
$text = [IO.File]::ReadAllText($path)
$old = 'LIBRARY vulkan-1.dll'
if (($text.Split(@($old), [StringSplitOptions]::None)).Count -ne 2) {
    throw 'Pinned Vulkan loader module-definition source changed'
}
[IO.File]::WriteAllText($path, $text.Replace($old, "LIBRARY viogpu_gl_loader_$Architecture.dll"))
