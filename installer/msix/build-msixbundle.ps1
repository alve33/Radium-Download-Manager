<#
.SYNOPSIS
    Bundles x64 and ARM64 MSIX packages into a single multi-architecture .msixbundle.
.DESCRIPTION
    Uses Microsoft Windows SDK MakeAppx.exe to combine architecture-specific .msix
    packages into a Microsoft Store compatible .msixbundle and validates the bundle.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputDir,

    [string]$OutputDir = "",

    [string]$Version = "",

    [string[]]$RequireArchitectures = @()
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path

if (-not $Version) {
    if ($env:RADIUM_VERSION) {
        $Version = $env:RADIUM_VERSION
    } else {
        $versionFilePath = Join-Path $RepoRoot "VERSION"
        $Version = if (Test-Path $versionFilePath) { (Get-Content $versionFilePath -Raw).Trim() } else { "1.0.0" }
    }
}
if ($Version -match '^v?(\d+)\.(\d+)\.(\d+)\.(\d+)$') {
    $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$($Matches[4])"
} elseif ($Version -match '^v?(\d+)\.(\d+)\.(\d+)(?:[.-].+)?$') {
    $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
} else {
    throw "Invalid version format '$Version'. Expected SemVer or 4-part MSIX version."
}

if (-not (Test-Path $InputDir)) {
    throw "Input directory not found: $InputDir"
}
if (-not $OutputDir) {
    $OutputDir = $InputDir
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

# Locate Windows SDK makeappx.exe
$makeappxExe = $null
foreach ($root in @("C:\Program Files (x86)\Windows Kits\10\bin", "C:\Program Files\Windows Kits\10\bin")) {
    if (Test-Path $root) {
        $versions = Get-ChildItem -Path $root -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } | Sort-Object Name -Descending
        foreach ($v in $versions) {
            $cand = Join-Path $v.FullName "x64\makeappx.exe"
            if (Test-Path $cand) {
                $makeappxExe = $cand
                break
            }
        }
    }
    if ($makeappxExe) { break }
}
if (-not $makeappxExe) {
    throw "Windows SDK makeappx.exe was not found."
}

$msixFiles = Get-ChildItem -Path $InputDir -Filter "*.msix" -Recurse
if (-not $msixFiles -or $msixFiles.Count -eq 0) {
    throw "No .msix packages found in $InputDir"
}

$stageBundleDir = Join-Path $env:TEMP "radium_msixbundle_stage"
if (Test-Path $stageBundleDir) { Remove-Item -Recurse -Force $stageBundleDir }
New-Item -ItemType Directory -Path $stageBundleDir -Force | Out-Null

foreach ($f in $msixFiles) {
    Copy-Item $f.FullName (Join-Path $stageBundleDir $f.Name) -Force
    Write-Host "[+] Included in bundle: $($f.Name) ($($f.Length) bytes)" -ForegroundColor Green
}

$bundleName = "Radium-$Version.msixbundle"
$bundlePath = Join-Path $OutputDir $bundleName
if (Test-Path $bundlePath) { Remove-Item -Force $bundlePath }

Write-Host "==> Creating MSIX bundle: $bundlePath..." -ForegroundColor Yellow
& $makeappxExe bundle /d $stageBundleDir /p $bundlePath /bv $Version /o
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $bundlePath)) {
    throw "MakeAppx bundle failed with exit code $LASTEXITCODE"
}

# Validate bundle via unbundle round-trip and verify architectures + PE machine types
$verifyBundleDir = Join-Path $env:TEMP "radium_msixbundle_verify"
if (Test-Path $verifyBundleDir) { Remove-Item -Recurse -Force $verifyBundleDir }
& $makeappxExe unbundle /p $bundlePath /d $verifyBundleDir /o | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx unbundle validation failed for $bundlePath (exit code $LASTEXITCODE)"
}
$unbundledMsix = Get-ChildItem -Path $verifyBundleDir -Filter "*.msix"
if ($unbundledMsix.Count -ne $msixFiles.Count) {
    throw "Bundle validation mismatch: expected $($msixFiles.Count) .msix files, found $($unbundledMsix.Count)"
}

$foundArchitectures = @()
foreach ($pkg in $unbundledMsix) {
    $pkgVerifyDir = Join-Path $verifyBundleDir ("inspect_" + [System.IO.Path]::GetFileNameWithoutExtension($pkg.Name))
    & $makeappxExe unpack /p $pkg.FullName /d $pkgVerifyDir /o | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "MakeAppx unpack failed for bundled package $($pkg.Name)"
    }
    [xml]$mXml = Get-Content (Join-Path $pkgVerifyDir "AppxManifest.xml") -Raw
    $pkgArch = $mXml.Package.Identity.ProcessorArchitecture
    $pkgId = $mXml.Package.Identity.Name
    $pkgVer = $mXml.Package.Identity.Version
    $exeBytes = [System.IO.File]::ReadAllBytes((Join-Path $pkgVerifyDir "radium.exe"))
    $peOffset = [BitConverter]::ToInt32($exeBytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($exeBytes, $peOffset + 4)
    $expectedMachine = if ($pkgArch -eq "arm64") { [uint16]0xAA64 } else { [uint16]0x8664 }
    if ($machine -ne $expectedMachine) {
        throw "Bundled package $($pkg.Name) PE machine mismatch: manifest='$pkgArch', PE=0x$($machine.ToString('X4'))"
    }
    $foundArchitectures += $pkgArch.ToLower()
    Write-Host "[+] Verified bundled MSIX: $($pkg.Name) | Identity=$pkgId | Version=$pkgVer | Arch=$pkgArch (PE=0x$($machine.ToString('X4')))" -ForegroundColor Green
}

foreach ($reqArch in $RequireArchitectures) {
    if ($foundArchitectures -notcontains $reqArch.ToLower()) {
        throw "Bundle validation failed: required architecture '$reqArch' is missing from $bundlePath (found: $($foundArchitectures -join ', '))"
    }
}

Remove-Item -Recurse -Force $stageBundleDir
Remove-Item -Recurse -Force $verifyBundleDir

$hash = (Get-FileHash -Path $bundlePath -Algorithm SHA256).Hash
Write-Host "[+] Verified MSIX bundle: $bundlePath ($((Get-Item $bundlePath).Length) bytes, SHA256: $hash)" -ForegroundColor Green
