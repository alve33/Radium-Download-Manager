# Radium Download Manager - MSIX Packaging and Microsoft Store Certification Guide

## Overview
This document describes the MSIX packaging architecture, build instructions, browser native messaging integration, testing protocols, and Microsoft Partner Center submission procedures for **Radium Download Manager**.

---

## 1. Why MSIX Solves Microsoft Store Policy 10.2.9 Rejection
* **The Root Problem:** Microsoft Store Certification Policy 10.2.9 ("Security, Package Submissions") requires executable (`.exe`) installers submitted to the Store to be digitally signed with a SHA-256 certificate issued by a Microsoft-trusted Certificate Authority (e.g. DigiCert, Sectigo). Self-signed certificates on `.exe` packages are rejected.
* **The MSIX Solution:** When an MSIX package is submitted to Microsoft Partner Center, Microsoft's Store ingestion service validates the package identity against your registered Publisher ID, strips any temporary signatures, and **signs the final `.msix` with Microsoft's official Windows Store root certificate** upon publication.
* **Cost Advantage:** Developers do not need to buy an expensive third-party EV/OV code signing certificate solely for Store distribution.

---

## 2. Packaging Architecture

The packaging system is housed in `installer/msix/`:
```
installer/msix/
├── AppxManifest.xml.template   # Parameterized Store manifest template
├── priconfig.xml              # MakePri resource configuration index
├── build-msix.ps1             # Automated packaging and signing script
├── Assets/                    # Microsoft Store visual assets
│   ├── StoreLogo.png          (50x50)
│   ├── Square44x44Logo.png    (44x44)
│   ├── Square71x71Logo.png    (71x71)
│   ├── Square150x150Logo.png  (150x150)
│   ├── Square310x310Logo.png  (310x310)
│   ├── Wide310x150Logo.png    (310x150)
│   ├── SplashScreen.png       (620x300)
│   └── unplated target-size variations (24x24, 48x48)
└── README.md                  # This guide
```

### Included Components in MSIX Payload:
1. `radium.exe`: Win32 Native Desktop application (subsystem: Windows GUI).
2. `radium-nmh.exe`: Native Messaging Host (subsystem: Windows CUI/Console).
3. `tools/win-x64/`:
   - `ffmpeg.exe` (media demuxing / transmuxing)
   - `N_m3u8DL-RE.exe` (HLS/DASH segment downloader)
   - `yt-dlp.exe` (video metadata & stream extraction)
4. `extension/`: Manifest V3 unpacked browser extension assets (icons, background scripts, content scripts, popup UI).
5. `manifests/`: Native messaging host JSON manifests.
6. `scripts/`: Native messaging registration utility (`register-native-messaging.ps1`).
7. `resources.pri`: Windows Package Resource Index compiled from visual assets.

---

## 3. Browser Extension Native Messaging Under MSIX

Radium Download Manager communicates with browser extensions via `stdio` Native Messaging. Under MSIX:
1. **Microsoft Edge AppExtension Integration:**
   `AppxManifest.xml` declares:
   ```xml
   <uap3:Extension Category="windows.appExtension">
     <uap3:AppExtension
       Name="com.microsoft.edge.extension"
       Id="com.radium.downloadmanager"
       DisplayName="Radium Download Manager Native Host"
       PublicFolder="manifests">
       <uap3:Properties>
         <NativeMessagingHost>manifests\com.radium.downloadmanager.chromium.json</NativeMessagingHost>
       </uap3:Properties>
     </uap3:AppExtension>
   </uap3:Extension>
   ```
2. **Chrome, Brave, and Firefox Integration:**
   Standard desktop browsers look for native messaging host manifests under `HKCU\Software\[Browser]\NativeMessagingHosts`.
   Radium includes full-trust desktop execution capability (`runFullTrust`), which allows it to access user registry keys if needed. Additionally, `scripts/register-native-messaging.ps1` can be executed to register manifests across Chrome, Brave, and Firefox.
3. **IPC and Named Pipes:**
   MSIX containerization with `runFullTrust` does not isolate Win32 named pipes (`\\.\pipe\OpenIDM_IPC_*`) or single-instance mutexes (`Local\Radium_SingleInstance`), ensuring instant communication between `radium-nmh.exe` and `radium.exe`.
4. **Data Persistence:**
   Radium stores user configuration, task state, and SQLite databases in `%LOCALAPPDATA%\Radium Download Manager\state.db` and downloads into `%USERPROFILE%\Downloads\Radium`. These paths remain fully accessible and persist across package updates and clean uninstalls.

---

## 4. How to Build the MSIX Packages

### Prerequisites
- Windows 10/11 (version 1809 / build 17763 or later).
- Windows 10/11 SDK installed (`makeappx.exe`, `makepri.exe`, `signtool.exe`).

### Build Instructions

#### A. For Microsoft Store Submission (Production)
Run in PowerShell:
```powershell
# 1. Obtain your Partner Center Publisher ID (e.g., CN=12345678-ABCD-EF01-2345-6789ABCDEF01)
# 2. Build x64 package
powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 `
    -Architecture x64 `
    -Version "1.0.0.0" `
    -PackageName "933298ce-82b5-494e-b832-571f35234ecf" `
    -Publisher "CN=YOUR-PARTNER-CENTER-PUBLISHER-ID" `
    -PublisherDisplayName "Radium Contributors"

# 3. Build ARM64 package
powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 `
    -Architecture ARM64 `
    -Version "1.0.0.0" `
    -PackageName "933298ce-82b5-494e-b832-571f35234ecf" `
    -Publisher "CN=YOUR-PARTNER-CENTER-PUBLISHER-ID" `
    -PublisherDisplayName "Radium Contributors"
```
The output files will be created in `release\`:
- `release/Radium-x64-1.0.0.0.msix`
- `release/Radium-ARM64-1.0.0.0.msix`

#### B. For Local Test Installation (Self-Signed)
To test installing the `.msix` directly on your machine without Store signing:
```powershell
powershell -ExecutionPolicy Bypass -File installer\msix\build-msix.ps1 `
    -Architecture x64 `
    -Version "1.0.0.0" `
    -SignForTesting
```
This automatically:
- Creates a local test code signing certificate in `Cert:\CurrentUser\My`.
- Installs the certificate into `Cert:\CurrentUser\TrustedPeople`.
- Signs the MSIX with `signtool.exe`.
- Enables double-click or `Add-AppxPackage` installation on the local system.

---

## 5. Microsoft Partner Center Submission Checklist

1. **Log in to Microsoft Partner Center:**
   Navigate to [Partner Center Dashboard](https://partner.microsoft.com/dashboard/apps-and-games/overview).
2. **Open Existing Product:**
   Select product: **Radium Download Manager** (Product ID: `933298ce-82b5-494e-b832-571f35234ecf`).
3. **Verify Package Identity & Publisher:**
   Go to **Product management** > **Product Identity**.
   Copy the exact values:
   - **Package/Identity Name**
   - **Publisher ID** (e.g., `CN=...`)
4. **Re-build Packages with Exact Identity:**
   Use the exact Publisher ID from Step 3 when running `build-msix.ps1`.
5. **Start a New Submission:**
   - Go to **Packages**.
   - Drag and drop `Radium-x64-1.0.0.0.msix` and `Radium-ARM64-1.0.0.0.msix`.
   - Partner Center will analyze the packages, inspect `AppxManifest.xml`, and verify the capabilities (`runFullTrust`).
6. **Certification Notes:**
   In the "Notes for certification" box, state:
   > *"Radium Download Manager is a high-performance open-source download accelerator. The package includes Win32 binaries and bundled open-source media utilities (FFmpeg, N_m3u8DL-RE, yt-dlp) for stream downloading. All user state is stored in LocalAppData."*
7. **Submit to the Store:**
   Click **Submit to the Store**. Microsoft's automated certification will sign the MSIX package with the official Microsoft Store certificate and publish it.
