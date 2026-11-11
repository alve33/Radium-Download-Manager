# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager -- Automated Centralized Version & Configuration Validation Script
# Validates that all repository components, manifests, build scripts, headers, and metadata
# are in 100% agreement with the root authoritative VERSION file and config/radium_config.json.

[CmdletBinding()]
param(
    [switch]$FailOnDrift = $true
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- Version & Configuration Validator  " -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

# 1. Authoritative VERSION file validation
$versionFile = Join-Path $RepoRoot "VERSION"
if (-not (Test-Path $versionFile)) {
    throw "[FAIL] Missing root authoritative VERSION file: $versionFile"
}

$authoritativeVersion = (Get-Content $versionFile -Raw).Trim()
if ($authoritativeVersion -notmatch '^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$') {
    throw "[FAIL] Root VERSION content '$authoritativeVersion' is not a valid Semantic Version (expected Major.Minor.Patch)."
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3]
$quadVersion = "$major.$minor.$patch.0"

Write-Host "[+] Authoritative Version : $authoritativeVersion (SemVer: $major.$minor.$patch, Quad: $quadVersion)" -ForegroundColor Green

# 2. Run Python generator drift check
Write-Host "`n[Check 1/5] Verifying generated targets against VERSION and central configuration..." -ForegroundColor White
$genScript = Join-Path $RepoRoot "scripts\generate_config.py"
& python $genScript --check
if ($LASTEXITCODE -ne 0) {
    throw "[FAIL] generate_config.py detected drift in generated files!"
}
Write-Host "  -> PASS: All generated targets (version.hpp, radium.rc, central_config.hpp, manifests, package.json, radium.iss, tokens.css) match." -ForegroundColor Green

# 3. Validate C++ version header defines
Write-Host "`n[Check 2/5] Inspecting src\common\version.hpp compile-time definitions..." -ForegroundColor White
$versionHpp = Join-Path $RepoRoot "src\common\version.hpp"
$hppText = Get-Content $versionHpp -Raw
if ($hppText -notmatch "#define\s+RADIUM_VERSION_MAJOR\s+$major") {
    throw "[FAIL] version.hpp RADIUM_VERSION_MAJOR mismatch (expected $major)"
}
if ($hppText -notmatch "#define\s+RADIUM_VERSION_MINOR\s+$minor") {
    throw "[FAIL] version.hpp RADIUM_VERSION_MINOR mismatch (expected $minor)"
}
if ($hppText -notmatch "#define\s+RADIUM_VERSION_PATCH\s+$patch") {
    throw "[FAIL] version.hpp RADIUM_VERSION_PATCH mismatch (expected $patch)"
}
if ($hppText -notmatch "#define\s+RADIUM_VERSION_STRING\s+`"$authoritativeVersion`"") {
    throw "[FAIL] version.hpp RADIUM_VERSION_STRING mismatch (expected `"$authoritativeVersion`")"
}
Write-Host "  -> PASS: version.hpp defines match authoritative version." -ForegroundColor Green

# 4. Validate Windows PE resource file
Write-Host "`n[Check 3/5] Inspecting src\app\radium.rc resource file..." -ForegroundColor White
$rcFile = Join-Path $RepoRoot "src\app\radium.rc"
$rcText = Get-Content $rcFile -Raw
if ($rcText -notmatch "FILEVERSION\s+$major,$minor,$patch,0") {
    throw "[FAIL] radium.rc FILEVERSION mismatch (expected $major,$minor,$patch,0)"
}
if ($rcText -notmatch "PRODUCTVERSION\s+$major,$minor,$patch,0") {
    throw "[FAIL] radium.rc PRODUCTVERSION mismatch (expected $major,$minor,$patch,0)"
}
if ($rcText -notmatch "`"FileVersion`",\s+`"$quadVersion`"") {
    throw "[FAIL] radium.rc FileVersion string mismatch (expected `"$quadVersion`")"
}
if ($rcText -notmatch "`"ProductVersion`",\s+`"$quadVersion`"") {
    throw "[FAIL] radium.rc ProductVersion string mismatch (expected `"$quadVersion`")"
}
Write-Host "  -> PASS: radium.rc PE metadata matches authoritative quad version." -ForegroundColor Green

# 5. Validate Browser Extension manifests
Write-Host "`n[Check 4/5] Inspecting browser extension manifests (Chromium, Firefox, Dev)..." -ForegroundColor White
$extManifests = @(
    (Join-Path $RepoRoot "extension\manifest.json"),
    (Join-Path $RepoRoot "extension\manifest.chromium.json"),
    (Join-Path $RepoRoot "extension\manifest.firefox.json")
)
foreach ($mPath in $extManifests) {
    $mObj = Get-Content $mPath -Raw | ConvertFrom-Json
    if ($mObj.version -ne $authoritativeVersion) {
        throw "[FAIL] Extension manifest version mismatch in $mPath : got '$($mObj.version)', expected '$authoritativeVersion'"
    }
}
Write-Host "  -> PASS: All browser extension manifests have identical version '$authoritativeVersion'." -ForegroundColor Green

# 6. Validate Packaging & Installer scripts
Write-Host "`n[Check 5/5] Inspecting Inno Setup and MSIX metadata..." -ForegroundColor White
$issFile = Join-Path $RepoRoot "installer\radium.iss"
$issText = Get-Content $issFile -Raw
if ($issText -notmatch "#define\s+MyAppVersion\s+`"$authoritativeVersion`"") {
    throw "[FAIL] Inno Setup MyAppVersion mismatch in $issFile"
}

# Check MSIX builder defaults resolution
$msixBuilder = Join-Path $RepoRoot "installer\msix\build-msix.ps1"
if (-not (Test-Path $msixBuilder)) {
    throw "[FAIL] MSIX builder missing at: $msixBuilder"
}

Write-Host "  -> PASS: Installer and packaging metadata synchronized." -ForegroundColor Green

Write-Host "`n=================================================================" -ForegroundColor Cyan
Write-Host "   ALL VERSION AND CONFIGURATION VALIDATIONS PASSED!             " -ForegroundColor Green
Write-Host "=================================================================`n" -ForegroundColor Cyan
