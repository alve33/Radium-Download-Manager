<#
.SYNOPSIS
    Builds official MSIX packages for Radium Download Manager.
.DESCRIPTION
    Packages Radium Download Manager into installable, Microsoft Store certifiable MSIX packages.
    Supports Windows x64 and ARM64. Handles manifest templating, PRI resource indexing,
    payload staging (extension, manifests, workers, binaries), packaging via MakeAppx,
    and optional test code signing with a temporary local developer certificate.
.PARAMETER Architecture
    Target CPU architecture: 'x64' or 'ARM64'. Defaults to 'x64'.
.PARAMETER Version
    Package version in quad format (Major.Minor.Build.Revision). Defaults to '1.0.0.0'.
.PARAMETER PackageName
    MSIX Package Identity Name. Defaults to 'RadiumDownloadManager'.
.PARAMETER Publisher
    MSIX Publisher Identity string.
    For Microsoft Store submission, use your Partner Center Publisher ID: 'CN=XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX'.
    Defaults to 'CN=TestPublisher' for local test builds.
.PARAMETER PublisherDisplayName
    Publisher display name shown in Windows UI and Store. Defaults to 'Radium Contributors'.
.PARAMETER SignForTesting
    Generates a local self-signed certificate (if needed) and signs the MSIX package for local installation testing.
.PARAMETER OutputDir
    Output directory for generated MSIX files. Defaults to '<RepoRoot>\release'.
#>

[CmdletBinding()]
param(
    [ValidateSet("x64", "ARM64")]
    [string]$Architecture = "x64",

    [string]$Version = "1.0.0.0",

    [string]$PackageName = "RadiumDownloadManager",

    [string]$Publisher = "CN=TestPublisher",

    [string]$PublisherDisplayName = "Radium Contributors",

    [switch]$SignForTesting = $false,

    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path

# Resolve authoritative version from root VERSION file if not explicitly provided or defaulted
if (-not $Version -or $Version -eq "1.0.0.0") {
    $versionFilePath = Join-Path $RepoRoot "VERSION"
    if (Test-Path $versionFilePath) {
        $rawVer = (Get-Content $versionFilePath -Raw).Trim()
        if ($rawVer -match '^(\d+)\.(\d+)\.(\d+)') {
            $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
        }
    }
}

$configFilePath = Join-Path $RepoRoot "config\radium_config.json"
if (Test-Path $configFilePath) {
    try {
        $cfgJson = Get-Content $configFilePath -Raw | ConvertFrom-Json
        if ($cfgJson.product.msix_package_name -and ($PackageName -eq "RadiumDownloadManager")) {
            $PackageName = $cfgJson.product.msix_package_name
        }
        if ($cfgJson.product.publisher_display_name -and ($PublisherDisplayName -eq "Radium Contributors")) {
            $PublisherDisplayName = $cfgJson.product.publisher_display_name
        }
    } catch {}
}

if (-not $OutputDir) {
    $OutputDir = Join-Path $RepoRoot "release"
}
if (-not (Test-Path $OutputDir)) {
    New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "   Radium Download Manager -- MSIX Package Builder ($Architecture)" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host "Package Identity Name : $PackageName" -ForegroundColor Gray
Write-Host "Package Version       : $Version" -ForegroundColor Gray
Write-Host "Publisher             : $Publisher" -ForegroundColor Gray
Write-Host "Architecture          : $Architecture" -ForegroundColor Gray

# 1. Locate Windows 10/11 SDK tools
$sdkRoots = @(
    "C:\Program Files (x86)\Windows Kits\10\bin",
    "C:\Program Files\Windows Kits\10\bin"
)

$makeappxExe = $null
$makepriExe = $null
$signtoolExe = $null

foreach ($root in $sdkRoots) {
    if (Test-Path $root) {
        $versions = Get-ChildItem -Path $root -Directory | Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } | Sort-Object Name -Descending
        foreach ($v in $versions) {
            $m = Join-Path $v.FullName "x64\makeappx.exe"
            $p = Join-Path $v.FullName "x64\makepri.exe"
            $s = Join-Path $v.FullName "x64\signtool.exe"
            if ((Test-Path $m) -and (Test-Path $p) -and (Test-Path $s)) {
                $makeappxExe = $m
                $makepriExe = $p
                $signtoolExe = $s
                break
            }
        }
    }
    if ($makeappxExe) { break }
}

if (-not $makeappxExe) {
    throw "Windows 10/11 SDK tools (makeappx.exe, makepri.exe, signtool.exe) were not found in standard paths."
}
Write-Host "[+] Found Windows SDK tools: $(Split-Path -Parent $makeappxExe)" -ForegroundColor Green

# 2. Locate built binaries
$radiumExe = $null
$radiumNmhExe = $null

$searchPaths = @(
    (Join-Path $RepoRoot "build\win-$Architecture\Release"),
    (Join-Path $RepoRoot "build\win-$Architecture"),
    (Join-Path $RepoRoot "dist\$Architecture"),
    (Join-Path $RepoRoot "dist"),
    (Join-Path $RepoRoot "build\Release"),
    (Join-Path $RepoRoot "build")
)

foreach ($sp in $searchPaths) {
    $candMain = Join-Path $sp "radium.exe"
    $candNmh = Join-Path $sp "radium-nmh.exe"
    if ((Test-Path $candMain) -and (Test-Path $candNmh)) {
        $radiumExe = (Resolve-Path $candMain).Path
        $radiumNmhExe = (Resolve-Path $candNmh).Path
        break
    }
}

if (-not $radiumExe -or -not $radiumNmhExe) {
    throw "Required binaries (radium.exe and radium-nmh.exe) for $Architecture were not found.`nSearched:`n - $($searchPaths -join "`n - ")"
}
Write-Host "[+] Found application executable: $radiumExe" -ForegroundColor Green
Write-Host "[+] Found native host executable : $radiumNmhExe" -ForegroundColor Green

# 3. Create staging directory
$stageDir = Join-Path $env:TEMP "radium_msix_stage_$Architecture"
if (Test-Path $stageDir) {
    Remove-Item -Recurse -Force $stageDir
}
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stageDir "Assets") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stageDir "manifests") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stageDir "extension") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stageDir "scripts") -Force | Out-Null

# 4. Copy payload
Copy-Item $radiumExe (Join-Path $stageDir "radium.exe")
Copy-Item $radiumNmhExe (Join-Path $stageDir "radium-nmh.exe")
Copy-Item (Join-Path $RepoRoot "README.md") $stageDir
Copy-Item (Join-Path $RepoRoot "LICENSE") $stageDir
Copy-Item (Join-Path $RepoRoot "THIRD_PARTY_NOTICES.md") $stageDir

# Copy Visual Assets
$assetsSource = Join-Path $RepoRoot "installer\msix\Assets"
if (Test-Path $assetsSource) {
    Copy-Item (Join-Path $assetsSource "*") (Join-Path $stageDir "Assets") -Recurse
} else {
    throw "MSIX visual assets directory missing: $assetsSource"
}

# Copy Manifests & scripts
Copy-Item (Join-Path $RepoRoot "installer\manifests\*") (Join-Path $stageDir "manifests")
Copy-Item (Join-Path $RepoRoot "scripts\register-native-messaging.ps1") (Join-Path $stageDir "scripts")

# Copy Browser Extension (excluding dev files)
$extSrc = Join-Path $RepoRoot "extension"
$extDst = Join-Path $stageDir "extension"
Get-ChildItem -Path $extSrc -Recurse | Where-Object {
    $_.FullName -notmatch '\\node_modules|\\tests|\\scratch|\.ts$|\.map$|\.git'
} | ForEach-Object {
    $rel = $_.FullName.Substring($extSrc.Length).TrimStart('\', '/')
    $targetPath = Join-Path $extDst $rel
    if ($_.PSIsContainer) {
        if (-not (Test-Path $targetPath)) { New-Item -ItemType Directory -Path $targetPath -Force | Out-Null }
    } else {
        $parent = Split-Path -Parent $targetPath
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        Copy-Item $_.FullName $targetPath -Force
    }
}

# Copy stream workers if present (clean minimal set only)
$toolsSrc = Join-Path $RepoRoot "tools\win-x64"
if (Test-Path $toolsSrc) {
    $toolsDst = Join-Path $stageDir "tools\win-x64"
    New-Item -ItemType Directory -Path $toolsDst -Force | Out-Null
    foreach ($w in @("ffmpeg.exe", "N_m3u8DL-RE.exe", "yt-dlp.exe")) {
        $workerPath = Join-Path $toolsSrc $w
        if (Test-Path $workerPath) {
            Copy-Item $workerPath (Join-Path $toolsDst $w) -Force
        }
    }
    Write-Host "[+] Staged bundled stream workers from: $toolsSrc" -ForegroundColor Green
}

# 5. Generate AppxManifest.xml
$manifestTemplatePath = Join-Path $RepoRoot "installer\msix\AppxManifest.xml.template"
if (-not (Test-Path $manifestTemplatePath)) {
    throw "AppxManifest template missing at: $manifestTemplatePath"
}

$manifestContent = Get-Content -Path $manifestTemplatePath -Raw
$manifestContent = $manifestContent.Replace('${PACKAGE_NAME}', $PackageName)
$manifestContent = $manifestContent.Replace('${PACKAGE_PUBLISHER}', $Publisher)
$manifestContent = $manifestContent.Replace('${PACKAGE_VERSION}', $Version)
$manifestArch = switch ($Architecture) {
    "ARM64" { "arm64" }
    "x64"   { "x64" }
    Default { $Architecture.ToLower() }
}
$manifestContent = $manifestContent.Replace('${PACKAGE_ARCH}', $manifestArch)
$manifestContent = $manifestContent.Replace('${PUBLISHER_DISPLAY_NAME}', $PublisherDisplayName)

$manifestOut = Join-Path $stageDir "AppxManifest.xml"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($manifestOut, $manifestContent, $utf8NoBom)
Write-Host "[+] Generated AppxManifest.xml ($Architecture, v$Version)" -ForegroundColor Green

# 6. Build PRI (Package Resource Index)
$priconfigPath = Join-Path $RepoRoot "installer\msix\priconfig.xml"
if (-not (Test-Path $priconfigPath)) {
    & $makepriExe createconfig /cf $priconfigPath /dq en-US /o /pv 10.0.0 | Out-Null
}

$priOutput = Join-Path $stageDir "resources.pri"
& $makepriExe new /pr $stageDir /cf $priconfigPath /of $priOutput /o | Out-Null
if (Test-Path $priOutput) {
    Write-Host "[+] Generated Package Resource Index: $priOutput" -ForegroundColor Green
}

# 7. Pack with MakeAppx
$outMsixName = "Radium-$Architecture-$Version.msix"
$outMsixPath = Join-Path $OutputDir $outMsixName

if (Test-Path $outMsixPath) {
    Remove-Item -Force $outMsixPath
}

Write-Host "==> Packaging into MSIX: $outMsixPath..." -ForegroundColor Yellow
$packArgs = @("pack", "/d", $stageDir, "/p", $outMsixPath, "/o")
& $makeappxExe $packArgs
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx failed with exit code $LASTEXITCODE"
}
Write-Host "[+] Successfully created MSIX package: $outMsixPath" -ForegroundColor Green

# Clean staging directory
Remove-Item -Recurse -Force $stageDir

# 8. Optional test signing for local deployment validation
if ($SignForTesting) {
    Write-Host "`n==> Signing package for local testing..." -ForegroundColor Yellow
    $certSubject = $Publisher
    $cert = Get-ChildItem -Path Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $certSubject } | Select-Object -First 1

    if (-not $cert) {
        Write-Host "Creating self-signed code signing certificate with Subject: $certSubject..." -ForegroundColor Cyan
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject $certSubject `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -HashAlgorithm "SHA256"
        
        # Export and install into Trusted People so Windows allows test installation
        $tempCertPath = Join-Path $env:TEMP "RadiumTestCert.cer"
        Export-Certificate -Cert $cert -FilePath $tempCertPath -Force | Out-Null
        Import-Certificate -FilePath $tempCertPath -CertStoreLocation "Cert:\CurrentUser\TrustedPeople" | Out-Null
        Remove-Item $tempCertPath -Force
        Write-Host "[+] Installed test certificate to CurrentUser\TrustedPeople for seamless local testing." -ForegroundColor Green
    }

    & $signtoolExe sign /fd SHA256 /sha1 $cert.Thumbprint /v $outMsixPath
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Test signing failed with exit code $LASTEXITCODE"
    } else {
        Write-Host "[+] Successfully signed test MSIX package." -ForegroundColor Green
    }
}

# 9. Compute SHA-256
$hash = (Get-FileHash -Path $outMsixPath -Algorithm SHA256).Hash
Write-Host "`n=== Package Summary ===" -ForegroundColor Cyan
Write-Host "File   : $outMsixPath" -ForegroundColor White
Write-Host "Size   : $((Get-Item $outMsixPath).Length) bytes" -ForegroundColor White
Write-Host "SHA256 : $hash" -ForegroundColor White

return @{
    Path = $outMsixPath
    Size = (Get-Item $outMsixPath).Length
    SHA256 = $hash
    Architecture = $Architecture
    Version = $Version
}
