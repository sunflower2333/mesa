param([ValidateSet('arm64','x64','x86')][string]$Architecture = 'arm64')
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$expectedMachine = @{arm64=0xAA64; x64=0x8664; x86=0x014c}[$Architecture]
$count = 0
foreach ($line in Get-Content (Join-Path $PSScriptRoot SHA256SUMS.txt)) {
    if ($line -notmatch '^([0-9a-fA-F]{64})  ([^/\\]+)$') { throw "Invalid manifest entry: $line" }
    $expectedHash = $Matches[1]
    $name = $Matches[2]
    $path = Join-Path $PSScriptRoot $name
    $actualHash = (Get-FileHash -Algorithm SHA256 $path).Hash
    if ($actualHash -ne $expectedHash) { throw "Hash mismatch: $name" }
    if ([IO.Path]::GetExtension($name) -in '.exe','.dll') {
        $stream = [IO.File]::OpenRead($path)
        $reader = New-Object IO.BinaryReader($stream)
        try {
            $stream.Position = 0x3c
            $peOffset = $reader.ReadUInt32()
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $name" }
            $machine = $reader.ReadUInt16()
            if ($machine -ne $expectedMachine) { throw "Wrong machine in $name : $machine" }
        } finally { $reader.Dispose(); $stream.Dispose() }
        Write-Output "$name SHA256=$actualHash PE=$('{0:X4}' -f $machine)"
    }
    $count++
}
Write-Output "PASS VerifyOnly architecture=$Architecture files=$count source=$((Get-Content (Join-Path $PSScriptRoot source-commit.txt)).Trim())"
