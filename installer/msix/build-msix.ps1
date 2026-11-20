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

    [string]$Version = "",

    [string]$PackageName = "",

    [string]$Publisher = "",

    [string]$PublisherDisplayName = "",

    [string]$BuildDir = "",

    [string]$ToolsDir = "",

    [switch]$StoreSubmission = $false,

    [switch]$SignForTesting = $false,

    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path

# Resolve authoritative version from parameter, RADIUM_VERSION, or root VERSION file,
# and convert SemVer (Major.Minor.Patch[-prerelease]) to 4-part MSIX version (Major.Minor.Patch.0).
if (-not $Version) {
    if ($env:RADIUM_VERSION) {
        $Version = $env:RADIUM_VERSION
    } else {
        $versionFilePath = Join-Path $RepoRoot "VERSION"
        if (Test-Path $versionFilePath) {
            $Version = (Get-Content $versionFilePath -Raw).Trim()
        } else {
            $Version = "1.0.0"
        }
    }
}
if ($Version -match '^v?(\d+)\.(\d+)\.(\d+)\.(\d+)$') {
    $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).$($Matches[4])"
} elseif ($Version -match '^v?(\d+)\.(\d+)\.(\d+)(?:[.-].+)?$') {
    $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
} else {
    throw "Invalid version format '$Version'. Expected SemVer (e.g. 1.0.0) or 4-part MSIX version (e.g. 1.0.0.0)."
}

# Resolve Package Identity (explicit parameter > environment variable > config/radium_config.json > default)
if (-not $PackageName -and $env:MSIX_PACKAGE_NAME) { $PackageName = $env:MSIX_PACKAGE_NAME }
if (-not $Publisher -and $env:MSIX_PUBLISHER) { $Publisher = $env:MSIX_PUBLISHER }
if (-not $PublisherDisplayName -and $env:MSIX_PUBLISHER_DISPLAY_NAME) { $PublisherDisplayName = $env:MSIX_PUBLISHER_DISPLAY_NAME }

if ($StoreSubmission) {
    if ($SignForTesting) {
        throw "Store submission packages must NOT be signed with a local test certificate (-SignForTesting cannot be combined with -StoreSubmission)."
    }
    if ([string]::IsNullOrWhiteSpace($PackageName)) {
        throw "Store submission requires MSIX_PACKAGE_NAME (-PackageName) from Microsoft Partner Center. Placeholder/config defaults are forbidden."
    }
    if ([string]::IsNullOrWhiteSpace($Publisher)) {
        throw "Store submission requires MSIX_PUBLISHER (-Publisher) from Microsoft Partner Center. Placeholder/config defaults are forbidden."
    }
    if ([string]::IsNullOrWhiteSpace($PublisherDisplayName)) {
        throw "Store submission requires MSIX_PUBLISHER_DISPLAY_NAME (-PublisherDisplayName) from Microsoft Partner Center. Placeholder/config defaults are forbidden."
    }
    if ($Publisher -match 'TestPublisher') {
        throw "Store submission rejected: Publisher '$Publisher' is a development placeholder."
    }
    if ($Publisher -match '2\.25\.311718361087948018008814957342246049000') {
        throw "Store submission rejected: Publisher '$Publisher' uses the unsigned development OID."
    }
    if ($Publisher -notmatch '^CN=.+') {
        throw "Store submission rejected: Publisher '$Publisher' must start with 'CN=' and match Partner Center."
    }
} else {
    $configFilePath = Join-Path $RepoRoot "config\radium_config.json"
    if (Test-Path $configFilePath) {
        try {
            $cfgJson = Get-Content $configFilePath -Raw | ConvertFrom-Json
            if (-not $PackageName -and $cfgJson.product.msix_package_name) {
                $PackageName = $cfgJson.product.msix_package_name
            }
            if (-not $Publisher -and $cfgJson.product.msix_publisher) {
                $Publisher = $cfgJson.product.msix_publisher
            }
            if (-not $PublisherDisplayName -and $cfgJson.product.publisher_display_name) {
                $PublisherDisplayName = $cfgJson.product.publisher_display_name
            }
        } catch {}
    }

    if (-not $PackageName) { $PackageName = "RadiumDownloadManager" }
    if (-not $Publisher) { $Publisher = "CN=TestPublisher" }
    if (-not $PublisherDisplayName) { $PublisherDisplayName = "Radium Contributors" }

    if ($Publisher -eq "CN=TestPublisher") {
        Write-Warning "MSIX Publisher is set to '$Publisher' (development placeholder). Set MSIX_PUBLISHER / -Publisher to your Microsoft Partner Center Publisher ID before Store submission."
    }
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

# 2. Locate built binaries & validate PE32+ architecture
$radiumExe = $null
$radiumNmhExe = $null

$searchPaths = @()
if ($BuildDir) {
    $resolvedBuildDir = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }
    $searchPaths += @(
        $resolvedBuildDir,
        (Join-Path $resolvedBuildDir "Release")
    )
}
$searchPaths += @(
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

$expectedMachine = if ($Architecture -eq "ARM64") { [uint16]0xAA64 } else { [uint16]0x8664 }
foreach ($bin in @($radiumExe, $radiumNmhExe)) {
    $bytes = [System.IO.File]::ReadAllBytes($bin)
    if ($bytes.Length -lt 256 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Invalid PE header (missing MZ) in $bin"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $machine = [BitConverter]::ToUInt16($bytes, $peOffset + 4)
    if ($machine -ne $expectedMachine) {
        throw "PE architecture mismatch for ${bin}: expected 0x$($expectedMachine.ToString('X4')) ($Architecture), got 0x$($machine.ToString('X4'))"
    }
}

Write-Host "[+] Found application executable: $radiumExe ($Architecture)" -ForegroundColor Green
Write-Host "[+] Found native host executable : $radiumNmhExe ($Architecture)" -ForegroundColor Green

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

# Copy offline desktop translation catalog (required by src/ui/localization.cpp)
$desktopCatalogSrc = Join-Path $RepoRoot "translations\desktop.json"
if (Test-Path $desktopCatalogSrc) {
    $translationsDst = Join-Path $stageDir "translations"
    New-Item -ItemType Directory -Path $translationsDst -Force | Out-Null
    Copy-Item $desktopCatalogSrc (Join-Path $translationsDst "desktop.json") -Force
    Write-Host "[+] Staged translations\desktop.json" -ForegroundColor Green
} else {
    throw "Required translation catalog missing: $desktopCatalogSrc"
}

# Copy Visual Assets
$assetsSource = Join-Path $RepoRoot "installer\msix\Assets"
if (Test-Path $assetsSource) {
    Copy-Item (Join-Path $assetsSource "*") (Join-Path $stageDir "Assets") -Recurse
} else {
    throw "MSIX visual assets directory missing: $assetsSource"
}

# Copy Manifests (rewriting host path to package-relative ..\radium-nmh.exe) & scripts
$manifestsSrc = Join-Path $RepoRoot "installer\manifests"
$manifestsDst = Join-Path $stageDir "manifests"
$utf8NoBomManifest = New-Object System.Text.UTF8Encoding($false)
Get-ChildItem -Path $manifestsSrc -Filter "*.json" | ForEach-Object {
    $mObj = Get-Content $_.FullName -Raw | ConvertFrom-Json
    $mObj.path = "..\radium-nmh.exe"
    $jsonOut = $mObj | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText((Join-Path $manifestsDst $_.Name), $jsonOut, $utf8NoBomManifest)
}
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
$archToolsSubdir = if ($Architecture -eq "ARM64") { "win-arm64" } else { "win-x64" }
$toolsSrc = $null
if ($ToolsDir) {
    $resolvedTools = if ([System.IO.Path]::IsPathRooted($ToolsDir)) { $ToolsDir } else { Join-Path $RepoRoot $ToolsDir }
    if (Test-Path $resolvedTools) { $toolsSrc = $resolvedTools }
}
if (-not $toolsSrc) {
    $archToolsPath = Join-Path $RepoRoot "tools\$archToolsSubdir"
    $fallbackToolsPath = Join-Path $RepoRoot "tools\win-x64"
    if (Test-Path $archToolsPath) {
        $toolsSrc = $archToolsPath
    } elseif (Test-Path $fallbackToolsPath) {
        $toolsSrc = $fallbackToolsPath
    }
}
if ($toolsSrc -and (Test-Path $toolsSrc)) {
    $toolsDst = Join-Path $stageDir "tools\$archToolsSubdir"
    New-Item -ItemType Directory -Path $toolsDst -Force | Out-Null
    foreach ($w in @("ffmpeg.exe", "N_m3u8DL-RE.exe", "yt-dlp.exe")) {
        $workerPath = Join-Path $toolsSrc $w
        if (Test-Path $workerPath) {
            Copy-Item $workerPath (Join-Path $toolsDst $w) -Force
        }
    }
    Write-Host "[+] Staged bundled stream workers from: $toolsSrc -> tools\$archToolsSubdir" -ForegroundColor Green
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

# Validate AppxManifest.xml structure and referenced assets
[xml]$manifestXml = Get-Content -Path $manifestOut -Raw
if ($manifestXml.Package.Identity.Name -ne $PackageName) {
    throw "AppxManifest Identity Name mismatch: $($manifestXml.Package.Identity.Name)"
}
if ($manifestXml.Package.Identity.Publisher -ne $Publisher) {
    throw "AppxManifest Identity Publisher mismatch: $($manifestXml.Package.Identity.Publisher)"
}
if ($manifestXml.Package.Identity.Version -ne $Version) {
    throw "AppxManifest Identity Version mismatch: $($manifestXml.Package.Identity.Version)"
}
if ($manifestXml.Package.Identity.ProcessorArchitecture -ne $manifestArch) {
    throw "AppxManifest ProcessorArchitecture mismatch: $($manifestXml.Package.Identity.ProcessorArchitecture)"
}
$requiredAssets = @(
    "Assets\StoreLogo.png",
    "Assets\Square44x44Logo.png",
    "Assets\Square71x71Logo.png",
    "Assets\Square150x150Logo.png",
    "Assets\Square310x310Logo.png",
    "Assets\Wide310x150Logo.png",
    "Assets\SplashScreen.png"
)
foreach ($assetRel in $requiredAssets) {
    $assetFull = Join-Path $stageDir $assetRel
    if (-not (Test-Path $assetFull)) {
        throw "AppxManifest referenced asset missing from staging dir: $assetRel"
    }
}
Write-Host "[+] Generated & validated AppxManifest.xml ($manifestArch, v$Version)" -ForegroundColor Green

# 6. Build PRI (Package Resource Index)
$priconfigPath = Join-Path $RepoRoot "installer\msix\priconfig.xml"
if (-not (Test-Path $priconfigPath)) {
    & $makepriExe createconfig /cf $priconfigPath /dq en-US /o /pv 10.0.0 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed with exit code $LASTEXITCODE" }
}

$priOutput = Join-Path $stageDir "resources.pri"
& $makepriExe new /pr $stageDir /cf $priconfigPath /of $priOutput /o | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $priOutput)) {
    throw "makepri new failed with exit code $LASTEXITCODE"
}
Write-Host "[+] Generated Package Resource Index: $priOutput" -ForegroundColor Green

# 7. Pack with MakeAppx
$outMsixName = "Radium-$Architecture-$Version.msix"
$outMsixPath = Join-Path $OutputDir $outMsixName

if (Test-Path $outMsixPath) {
    Remove-Item -Force $outMsixPath
}

Write-Host "==> Packaging into MSIX: $outMsixPath..." -ForegroundColor Yellow
$packArgs = @("pack", "/d", $stageDir, "/p", $outMsixPath, "/o")
& $makeappxExe $packArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $outMsixPath)) {
    throw "MakeAppx failed with exit code $LASTEXITCODE"
}
Write-Host "[+] Successfully created MSIX package: $outMsixPath" -ForegroundColor Green

# Validate generated MSIX via MakeAppx unpack round-trip
$verifyDir = Join-Path $env:TEMP "radium_msix_verify_$Architecture"
if (Test-Path $verifyDir) {
    Remove-Item -Recurse -Force $verifyDir
}
& $makeappxExe unpack /p $outMsixPath /d $verifyDir /o | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx unpack validation failed for $outMsixPath (exit code $LASTEXITCODE)"
}
foreach ($reqFile in @("AppxManifest.xml", "radium.exe", "radium-nmh.exe", "translations\desktop.json", "resources.pri", "Assets\StoreLogo.png")) {
    $unpackedPath = Join-Path $verifyDir $reqFile
    if (-not (Test-Path $unpackedPath) -or (Get-Item $unpackedPath).Length -le 0) {
        throw "MSIX validation failed: missing or empty payload file '$reqFile' inside $outMsixPath"
    }
}
if ($StoreSubmission) {
    $sigFile = Join-Path $verifyDir "AppxSignature.p7x"
    if (Test-Path $sigFile) {
        throw "Store submission validation failed: AppxSignature.p7x found inside $outMsixPath (Store packages must be unsigned)."
    }
    $unpackedManifestText = Get-Content (Join-Path $verifyDir "AppxManifest.xml") -Raw
    if ($unpackedManifestText -match 'TestPublisher' -or $unpackedManifestText -match '2\.25\.311718361087948018008814957342246049000') {
        throw "Store submission validation failed: placeholder publisher or dev OID found in unpacked AppxManifest.xml."
    }
    [xml]$unpackedXml = $unpackedManifestText
    if ($unpackedXml.Package.Properties.PublisherDisplayName -ne $PublisherDisplayName) {
        throw "Store submission validation failed: PublisherDisplayName mismatch."
    }
}
Remove-Item -Recurse -Force $verifyDir
Write-Host "[+] Verified MSIX payload integrity via MakeAppx unpack." -ForegroundColor Green

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
