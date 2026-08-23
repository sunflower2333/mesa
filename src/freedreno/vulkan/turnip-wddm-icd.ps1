[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
   [ValidateSet('Install', 'Uninstall')]
   [string]$Action = 'Install',

   [string]$BundleRoot = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$manifestName = 'freedreno_icd.arm64.json'
$libraryName = 'vulkan_freedreno.dll'
$dependencyName = 'z-1.dll'
$hashListName = 'SHA256SUMS.txt'

function Assert-Administrator
{
   $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
   $principal = [Security.Principal.WindowsPrincipal]::new($identity)
   if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
      throw 'Turnip ICD registration requires an elevated 64-bit PowerShell session.'
   }
}

function Assert-Arm64Host
{
   if (-not [Environment]::Is64BitProcess -or
       $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') {
      throw 'Turnip WDDM ICD installation requires native 64-bit PowerShell on Windows ARM64.'
   }
}

function Get-PeMachine
{
   param([Parameter(Mandatory = $true)][string]$Path)

   $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                             [IO.FileShare]::Read)
   try {
      if ($stream.Length -lt 0x40) {
         throw "$Path is too small to be a PE image."
      }
      $reader = [IO.BinaryReader]::new($stream)
      if ($reader.ReadUInt16() -ne 0x5a4d) {
         throw "$Path does not have an MZ header."
      }
      $stream.Position = 0x3c
      $peOffset = $reader.ReadUInt32()
      if ($peOffset -gt $stream.Length - 6) {
         throw "$Path has an invalid PE header offset."
      }
      $stream.Position = $peOffset
      if ($reader.ReadUInt32() -ne 0x00004550) {
         throw "$Path does not have a PE signature."
      }
      return $reader.ReadUInt16()
   }
   finally {
      $stream.Dispose()
   }
}

function Get-Sha256
{
   param([Parameter(Mandatory = $true)][string]$Path)

   $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
                             [IO.FileShare]::Read)
   try {
      $sha256 = [Security.Cryptography.SHA256]::Create()
      try {
         $hash = $sha256.ComputeHash($stream)
         return [BitConverter]::ToString($hash).Replace('-', '').ToLowerInvariant()
      }
      finally {
         $sha256.Dispose()
      }
   }
   finally {
      $stream.Dispose()
   }
}

function Assert-BundleIntegrity
{
   param([Parameter(Mandatory = $true)][string]$Root)

   $hashPath = Join-Path $Root $hashListName
   if (-not (Test-Path -LiteralPath $hashPath -PathType Leaf)) {
      throw "Missing bundle hash list: $hashPath"
   }

   $entries = @{}
   foreach ($line in (Get-Content -LiteralPath $hashPath)) {
      if ($line -notmatch '^([0-9a-f]{64})  ([^\\/]+)$') {
         throw "Malformed SHA256SUMS entry: $line"
      }
      $expected = $Matches[1]
      $name = $Matches[2]
      if ($entries.ContainsKey($name)) {
         throw "Duplicate SHA256SUMS entry: $name"
      }
      $path = Join-Path $Root $name
      if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
         throw "SHA256SUMS references a missing file: $name"
      }
      $actual = Get-Sha256 -Path $path
      if ($actual -ne $expected) {
         throw "Bundle hash mismatch for $name"
      }
      $entries[$name] = $true
   }

   foreach ($required in @($manifestName, $libraryName, $dependencyName,
                            'turnip-wddm-icd.ps1')) {
      if (-not $entries.ContainsKey($required)) {
         throw "SHA256SUMS does not cover required bundle file: $required"
      }
   }
}

function Open-VulkanDriverKey
{
   param([Parameter(Mandatory = $true)][bool]$Writable)

   $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
      [Microsoft.Win32.RegistryHive]::LocalMachine,
      [Microsoft.Win32.RegistryView]::Registry64)
   try {
      if ($Writable) {
         return $base.CreateSubKey('SOFTWARE\Khronos\Vulkan\Drivers', $true)
      }
      return $base.OpenSubKey('SOFTWARE\Khronos\Vulkan\Drivers', $false)
   }
   finally {
      $base.Dispose()
   }
}

function Get-VulkanDriverValueSnapshot
{
   param([Parameter(Mandatory = $true)][string]$Name)

   $key = Open-VulkanDriverKey -Writable $false
   if ($null -eq $key) {
      return [pscustomobject]@{ Exists = $false; Data = $null; Kind = $null }
   }
   try {
      if ($key.GetValueNames() -notcontains $Name) {
         return [pscustomobject]@{ Exists = $false; Data = $null; Kind = $null }
      }
      return [pscustomobject]@{
         Exists = $true
         Data = $key.GetValue(
            $Name, $null,
            [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
         Kind = $key.GetValueKind($Name)
      }
   }
   finally {
      $key.Dispose()
   }
}

function Restore-VulkanDriverValue
{
   param(
      [Parameter(Mandatory = $true)][string]$Name,
      [Parameter(Mandatory = $true)]$Snapshot
   )

   $key = Open-VulkanDriverKey -Writable $true
   try {
      if ($Snapshot.Exists) {
         $key.SetValue($Name, $Snapshot.Data, $Snapshot.Kind)
      }
      else {
         $key.DeleteValue($Name, $false)
      }
   }
   finally {
      $key.Dispose()
   }
}

function New-IcdTransactionRoot
{
   param([Parameter(Mandatory = $true)][string]$Parent)

   New-Item -ItemType Directory -Path $Parent -Force | Out-Null
   $leaf = '.turnip-icd-{0}-{1}' -f $PID, [Guid]::NewGuid().ToString('N')
   $path = Join-Path $Parent $leaf
   New-Item -ItemType Directory -Path $path | Out-Null
   return $path
}

Assert-Arm64Host
Assert-Administrator

$programFiles = [Environment]::GetFolderPath([System.Environment+SpecialFolder]::ProgramFiles)
$installRoot = Join-Path $programFiles 'DroidVM\Turnip'
$installedManifest = Join-Path $installRoot $manifestName
$installedLibrary = Join-Path $installRoot $libraryName
$installedDependency = Join-Path $installRoot $dependencyName
$ownedFiles = @($installedManifest, $installedLibrary, $installedDependency)
$transactionParent = Split-Path $installRoot -Parent

if ($Action -eq 'Uninstall') {
   if ($PSCmdlet.ShouldProcess($installedManifest, 'Unregister and remove the DroidVM Turnip ICD')) {
      $registrySnapshot = Get-VulkanDriverValueSnapshot -Name $installedManifest
      if ($registrySnapshot.Exists -and [int]$registrySnapshot.Data -ne 0) {
         throw "Refusing to delete an enabled-state value with unexpected data: $($registrySnapshot.Data)"
      }

      $transactionRoot = New-IcdTransactionRoot -Parent $transactionParent
      $backupRoot = Join-Path $transactionRoot 'backup'
      New-Item -ItemType Directory -Path $backupRoot | Out-Null
      $backedUp = @{}
      try {
         foreach ($path in $ownedFiles) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
               $name = Split-Path $path -Leaf
               Copy-Item -LiteralPath $path -Destination (Join-Path $backupRoot $name)
               $backedUp[$path] = $name
            }
         }

         $key = Open-VulkanDriverKey -Writable $true
         try {
            $key.DeleteValue($installedManifest, $false)
         }
         finally {
            $key.Dispose()
         }

         foreach ($path in $ownedFiles) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
               Remove-Item -LiteralPath $path -Force
            }
         }
      }
      catch {
         $failure = $_
         try {
            Restore-VulkanDriverValue -Name $installedManifest -Snapshot $registrySnapshot
            New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
            foreach ($path in $backedUp.Keys) {
               Copy-Item -LiteralPath (Join-Path $backupRoot $backedUp[$path]) `
                  -Destination $path -Force
            }
         }
         catch {
            throw "Turnip ICD uninstall failed and rollback also failed: $($failure.Exception.Message); $($_.Exception.Message)"
         }
         throw $failure
      }
      finally {
         Remove-Item -LiteralPath $transactionRoot -Recurse -Force -ErrorAction SilentlyContinue
      }

      if (Test-Path -LiteralPath $installRoot -PathType Container) {
         $remaining = @(Get-ChildItem -LiteralPath $installRoot -Force)
         if ($remaining.Count -ne 0) {
            Write-Warning "Preserving $installRoot because it contains files not owned by this installer."
         }
         else {
            Remove-Item -LiteralPath $installRoot -Force -ErrorAction SilentlyContinue
         }
      }
   }
   return
}

if ([string]::IsNullOrWhiteSpace($BundleRoot)) {
   $BundleRoot = $PSScriptRoot
}
$BundleRoot = [IO.Path]::GetFullPath($BundleRoot)
if (-not (Test-Path -LiteralPath $BundleRoot -PathType Container)) {
   throw "Bundle root does not exist: $BundleRoot"
}

Assert-BundleIntegrity -Root $BundleRoot
$sourceManifest = Join-Path $BundleRoot $manifestName
$sourceLibrary = Join-Path $BundleRoot $libraryName
$sourceDependency = Join-Path $BundleRoot $dependencyName

$manifest = Get-Content -LiteralPath $sourceManifest -Raw | ConvertFrom-Json
if ($manifest.file_format_version -ne '1.0.1' -or
    $manifest.ICD.library_path -ne '.\vulkan_freedreno.dll' -or
    $manifest.ICD.library_arch -ne '64' -or
    $manifest.ICD.api_version -notmatch '^1\.4\.[0-9]+$') {
   throw 'The bundle does not contain the expected ARM64 Turnip ICD manifest.'
}
foreach ($path in @($sourceLibrary, $sourceDependency)) {
   if ((Get-PeMachine -Path $path) -ne 0xaa64) {
      throw "$path is not an ARM64 PE image."
   }
}

if ($PSCmdlet.ShouldProcess($installedManifest, 'Install and register the DroidVM Turnip ICD')) {
   $registrySnapshot = Get-VulkanDriverValueSnapshot -Name $installedManifest
   $transactionRoot = New-IcdTransactionRoot -Parent $transactionParent
   $stageRoot = Join-Path $transactionRoot 'stage'
   $backupRoot = Join-Path $transactionRoot 'backup'
   New-Item -ItemType Directory -Path $stageRoot, $backupRoot | Out-Null
   $backedUp = @{}
   try {
      $stagedManifest = Join-Path $stageRoot $manifestName
      $stagedLibrary = Join-Path $stageRoot $libraryName
      $stagedDependency = Join-Path $stageRoot $dependencyName
      Copy-Item -LiteralPath $sourceLibrary -Destination $stagedLibrary
      Copy-Item -LiteralPath $sourceDependency -Destination $stagedDependency

      $installed = [ordered]@{
         file_format_version = '1.0.1'
         ICD = [ordered]@{
            library_path = $installedLibrary
            api_version = $manifest.ICD.api_version
            library_arch = '64'
         }
      }
      $json = $installed | ConvertTo-Json -Depth 4
      [IO.File]::WriteAllText($stagedManifest, $json,
                              [Text.UTF8Encoding]::new($false))

      foreach ($path in $ownedFiles) {
         if (Test-Path -LiteralPath $path -PathType Leaf) {
            $name = Split-Path $path -Leaf
            Copy-Item -LiteralPath $path -Destination (Join-Path $backupRoot $name)
            $backedUp[$path] = $name
         }
      }

      New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
      Copy-Item -LiteralPath $stagedLibrary -Destination $installedLibrary -Force
      Copy-Item -LiteralPath $stagedDependency -Destination $installedDependency -Force
      Copy-Item -LiteralPath $stagedManifest -Destination $installedManifest -Force

      $key = Open-VulkanDriverKey -Writable $true
      try {
         $key.SetValue($installedManifest, 0,
                       [Microsoft.Win32.RegistryValueKind]::DWord)
      }
      finally {
         $key.Dispose()
      }
   }
   catch {
      $failure = $_
      try {
         Restore-VulkanDriverValue -Name $installedManifest -Snapshot $registrySnapshot
         foreach ($path in $ownedFiles) {
            if (Test-Path -LiteralPath $path -PathType Leaf) {
               Remove-Item -LiteralPath $path -Force
            }
         }
         foreach ($path in $backedUp.Keys) {
            Copy-Item -LiteralPath (Join-Path $backupRoot $backedUp[$path]) `
               -Destination $path -Force
         }
      }
      catch {
         throw "Turnip ICD install failed and rollback also failed: $($failure.Exception.Message); $($_.Exception.Message)"
      }
      throw $failure
   }
   finally {
      Remove-Item -LiteralPath $transactionRoot -Recurse -Force -ErrorAction SilentlyContinue
   }
}
