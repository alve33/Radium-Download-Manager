# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Inno Setup Installer Builder Script
# Compiles Full and/or Core Windows installers via Inno Setup 6 (ISCC.exe)
# Enforces strict pre-flight binary existence, architecture, and PE ProductName validation.

[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",

    [string]$Version = "",

    [ValidateSet("Full", "Core", "Both")]
    [string]$Type = "Both",

    [string]$OutputDir = "",

    [string]$BuildDir = "",

    [string]$OutputFilename = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

# 1. Resolve Release Version
if (-not $Version) {
    if ($env:RADIUM_VERSION) {
        $Version = $env:RADIUM_VERSION
    } else {
        $versionFile = Join-Path $RepoRoot "VERSION"
        if (Test-Path $versionFile) {
            $Version = (Get-Content $versionFile -Raw).Trim()
        } else {
            try {
                $gitTag = (git describe --tags --exact-match 2>$null)
                if ($gitTag -match '^v?(\d+\.\d+\.\d+.*)$') {
                    $Version = $Matches[1]
                }
            } catch {}
            if (-not $Version) {
                $Version = "1.0.0"
            }
        }
    }
}
if ($Version -match '^v?(\d+\.\d+\.\d+.*)$') {
    $Version = $Matches[1]
} else {
    throw "Invalid version format '$Version'. Expected semantic version like '1.0.0'."
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $RepoRoot "dist"
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- Inno Setup Installer Builder       " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "  Version      : $Version" -ForegroundColor White
Write-Host "  Installer    : $Type" -ForegroundColor White
Write-Host "  Architecture : $Architecture" -ForegroundColor White
Write-Host "  Output Dir   : $OutputDir" -ForegroundColor White

# 2. Locate ISCC.exe compiler
$isccCandidates = @(
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
    "C:\Program Files\Inno Setup 6\ISCC.exe"
)

$isccPath = $null
foreach ($cand in $isccCandidates) {
    if (Test-Path $cand) {
        $isccPath = $cand
        break
    }
}
if (-not $isccPath) {
    $cmd = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
    if ($cmd) { $isccPath = $cmd.Source }
}

if (-not $isccPath) {
    throw "Inno Setup 6 compiler (ISCC.exe) not found. Please install Inno Setup 6 or ensure it is in PATH."
}
Write-Host "[+] Found ISCC.exe: $isccPath" -ForegroundColor Green

# 3. Locate and Validate Radium Executables
$radiumCandidates = @()
if ($BuildDir) {
    $radiumCandidates += (Join-Path $BuildDir "radium.exe")
    $radiumCandidates += (Join-Path $BuildDir "Release\radium.exe")
}
$radiumCandidates += @(
    (Join-Path $RepoRoot "build\win-$Architecture\Release\radium.exe"),
    (Join-Path $RepoRoot "build\win-x64\Release\radium.exe"),
    (Join-Path $RepoRoot "build\Release\radium.exe"),
    (Join-Path $RepoRoot "dist\$Architecture\radium.exe"),
    (Join-Path $RepoRoot "dist\Radium.exe"),
    (Join-Path $RepoRoot "dist\radium.exe")
)

$foundRadiumExe = $null
foreach ($c in $radiumCandidates) {
    if (Test-Path $c) {
        $foundRadiumExe = (Resolve-Path $c).Path
        break
    }
}

if (-not $foundRadiumExe) {
    throw "Pre-flight validation failed: radium.exe not found in search paths: $($radiumCandidates -join ', ')"
}

# Verify PE ProductName metadata contains "Radium"
$verInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($foundRadiumExe)
if (-not $verInfo.ProductName -or $verInfo.ProductName -notmatch "Radium") {
    throw "Pre-flight validation failed: $foundRadiumExe has invalid ProductName '$($verInfo.ProductName)' (expected containing 'Radium')."
}
Write-Host "[+] Found verified Desktop binary: $foundRadiumExe (ProductName: '$($verInfo.ProductName)')" -ForegroundColor Green

# 4. Compile Installers via Inno Setup
$issScript = Join-Path $RepoRoot "installer\radium.iss"
if (-not (Test-Path $issScript)) {
    throw "Missing Inno Setup script: $issScript"
}

if ($Type -in @("Full", "Both")) {
    Write-Host "`n==> Compiling Full Offline Installer..." -ForegroundColor Cyan
    $fullArgs = @(
        "/DMyAppVersion=$Version",
        "/DTargetArch=$Architecture",
        "/O$OutputDir"
    )
    if ($OutputFilename) {
        $fullArgs += "/DOutputFilename=$OutputFilename"
    }
    $fullArgs += $issScript
    & $isccPath $fullArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compilation failed for Full installer with exit code $LASTEXITCODE"
    }
    Write-Host "[+] Successfully compiled Full Installer!" -ForegroundColor Green

    # Also stage canonical standard naming if not already custom
    if (-not $OutputFilename) {
        $archLabel = if ($Architecture -eq "ARM64") { "arm64" } else { "x64" }
        $canonicalExe = Join-Path $OutputDir "Radium-Download-Manager-v$Version-Windows-$archLabel-Setup.exe"
        $compiledFull = Join-Path $OutputDir "Radium-Setup-$Architecture-$Version.exe"
        if (Test-Path $compiledFull) {
            Copy-Item -Force $compiledFull $canonicalExe
            Write-Host "[+] Staged canonical installer: $canonicalExe" -ForegroundColor Green
        }
    }
}

if ($Type -in @("Core", "Both")) {
    Write-Host "`n==> Compiling Core-Only Installer..." -ForegroundColor Cyan
    $coreArgs = @(
        "/DCoreOnly",
        "/DMyAppVersion=$Version",
        "/DTargetArch=$Architecture",
        "/O$OutputDir",
        $issScript
    )
    & $isccPath $coreArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Inno Setup compilation failed for Core installer with exit code $LASTEXITCODE"
    }
    Write-Host "[+] Successfully compiled Core Installer!" -ForegroundColor Green
}

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "   Installer Compilation Completed Successfully!                 " -ForegroundColor Green
Write-Host "=================================================================" -ForegroundColor Cyan
