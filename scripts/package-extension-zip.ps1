# SPDX-License-Identifier: Apache-2.0
# Packages the Radium browser extension (extension/) into a clean, verifiable
# release ZIP asset (dist/radium-extension.zip) for direct GitHub Release download.

[CmdletBinding()]
param(
    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..")).Path
$ExtDir = Join-Path $RepoRoot "extension"

if (-not (Test-Path $ExtDir)) {
    throw "Extension directory not found: $ExtDir"
}

if (-not $OutputPath) {
    $DistDir = Join-Path $RepoRoot "dist"
    New-Item -ItemType Directory -Force -Path $DistDir | Out-Null
    $OutputPath = Join-Path $DistDir "radium-extension.zip"
} else {
    $ParentDir = Split-Path -Parent $OutputPath
    if ($ParentDir) {
        New-Item -ItemType Directory -Force -Path $ParentDir | Out-Null
    }
}

$RequiredFiles = @(
    "manifest.json",
    "manifest.firefox.json",
    "popup.html",
    "catch-script/catch.js",
    "css/popup.css",
    "css/tokens.css",
    "icons/icon-16.png",
    "icons/icon-32.png",
    "icons/icon-48.png",
    "icons/icon-128.png",
    "js/background.js",
    "js/categories.generated.js",
    "js/content-script.js",
    "js/function.js",
    "js/init.js",
    "js/popup.js"
)

foreach ($rel in $RequiredFiles) {
    $full = Join-Path $ExtDir $rel
    if (-not (Test-Path $full -PathType Leaf)) {
        throw "Required extension file missing: $full"
    }
}

$manifest = Get-Content (Join-Path $ExtDir "manifest.json") -Raw | ConvertFrom-Json
if ($manifest.manifest_version -ne 3) {
    throw "manifest.json must declare manifest_version 3"
}

if (Test-Path $OutputPath) {
    Remove-Item -Force $OutputPath
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$zip = [System.IO.Compression.ZipFile]::Open($OutputPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $files = Get-ChildItem -Path $ExtDir -Recurse -File | Where-Object {
        $_.FullName -notmatch '(\\|/)node_modules(\\|/)' -and
        $_.FullName -notmatch '(\\|/)\.git(\\|/)' -and
        $_.Extension -ne '.bak'
    }
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($ExtDir.Length).TrimStart('\', '/').Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip,
            $f.FullName,
            $rel,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally {
    $zip.Dispose()
}

$hash = (Get-FileHash -Path $OutputPath -Algorithm SHA256).Hash
$size = (Get-Item $OutputPath).Length
Write-Host "Packaged radium-extension.zip v$($manifest.version): $OutputPath ($size bytes, SHA256=$hash)"
