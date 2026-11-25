# Radium Download Manager 2.0.0 (Stable Release)

**Radium Download Manager** is a lightweight, open-source native C++20 and Win32 download accelerator and browser media downloader for **Windows 10 and Windows 11**. It combines an in-process segmented HTTP/HTTPS engine (`WinHTTP` + IOCP), an out-of-process HLS/DASH stream pipeline (`N_m3u8DL-RE` + `FFmpeg`), a built-in BitTorrent v1 engine, and a Manifest V3 browser extension over ACL-protected Windows Named Pipes.

Version **2.0.0** is an official **Stable Release** upgrading from `1.0.0` with major reliability fixes, multi-site video extraction, guided browser extension setup, Windows 10/11 theme integration, and Store-ready MSIX packaging.

---

## What's New Since Version 1.0.0

### Major New Features
- **Guided First-Launch Browser Extension Setup & In-App Updater:** New installations present a one-time setup dialog to install the Manifest V3 browser extension (automatically skipped for existing v1.0.0 users with saved tasks). You can also download or update `radium-extension.zip` directly from **Settings → Browser Integration**.
- **Modular Multi-Site Video Extraction (`VideoExtractionManager`):** Added dedicated extractors for YouTube, Bilibili (`__playinfo__` DASH video+audio pairing and multipart episodes), Facebook (`fbcdn.net` byte-range stripping), and `yt-dlp` fallback, with live non-blocking quality probing in the Add Download dialog.
- **Add Torrent File Selector & Creator:** Opening a `.torrent` file or browser `.torrent` offer now launches a dedicated file-tree selector (with bounded 16 MiB off-UI-thread metainfo fetching), alongside `.torrent` creation (`TorrentCreator`) and BEP 9 magnet metadata exchange.
- **Site Grabber & ZIP Archive Previewer:** Added a recursive breadth-first site grabber with SSRF protections and a pre-extraction ZIP previewer with Zip Slip and compression-bomb guards.
- **MSIX & Multi-Arch Bundle Packages:** Official GitHub Actions builds now produce `Radium-x64-2.0.0.0.msix`, `Radium-ARM64-2.0.0.0.msix`, and `Radium-2.0.0.0.msixbundle`.

### Download Engine Improvements
- **Speculative Background Download Continuity:** Browser-intercepted downloads begin transferring immediately in the background while waiting for your confirmation in the New Download dialog, eliminating startup lag while cleanly cancelling and cleaning up temporary files if you cancel or encounter a file conflict.
- **File Conflict Resolution & Ghost Task Fixes:** Added an interactive **File Already Exists** prompt with remembered **Overwrite / Rename / Skip** preferences and atomic file commits. Fixed edge cases where repeated downloads of the same URL or startup task restoration could trigger false conflict dialogs or leave silent paused tasks.
- **Segmented HTTP & FTP Stability:** Fixed a dangling `std::string_view` in the `Content-Range` parser and a recursive mutex deadlock in `SegmentScheduler::WaitForWork`. Preempted workers now exit cleanly, the TTFB preemption floor is raised to 1000 ms with an in-flight resplit guard, and FTP `EPSV`/`PASV`/NAT bypass and `REST 350` resume truncation are hardened.

### Video & BitTorrent Improvements
- **Stream Quality Integrity:** Enforced `ffprobe` media container verification, honest progress reporting without artificial clamps, and intermediate `.part` preservation on stream retries.
- **Accurate BitTorrent Speeds:** Added a payload-delta `TorrentRateMeter` that measures verified payload throughput rather than raw socket framing, resets cleanly when idle, and tracks verified uploads.

### Browser Extension & UI Improvements
- **Browser Extension:** Improved repeat-attempt download correlation, per-tab SPA/fragment URL tracking, anchor-bound `blob:`/`data:` artifact capture (such as AI web exports), and a compact closed Shadow DOM video overlay with quality selection and fullscreen sync.
- **Windows 10 / 11 Light & Dark Themes:** Added automatic Windows System Theme detection (`System` / `Light` / `Dark`) with a persisted toolbar toggle, dark-mode input styling fixes, a more compact New Download dialog (520×480 DPI-scaled), synchronous row hover redraws, and state-appropriate task context menus.

### Security, Reliability & Hygiene
- **Post-Download Windows Defender Scan:** Optional/strict-mode `MpCmdRun` scanning that blocks launching flagged downloads, plus automatic redaction of cookies, tokens, and `Authorization` headers in logs, UI details, and exports.
- **Clean Internal Architecture:** Completed internal `openidm`/`flux` → `radium` namespace cleanup while keeping all v1.0.0 / legacy data migration and compatibility paths intact.

---

## Available Windows Downloads (v2.0.0)

| Asset | Architecture | Format | Notes |
| :--- | :---: | :---: | :--- |
| `Radium-Setup-x64-2.0.0.exe` | x64 | Inno Setup Installer | **Recommended for most Windows 10 / 11 users** (includes stream workers) |
| `Radium-Setup-x64-Core-2.0.0.exe` | x64 | Inno Setup Installer | Lightweight installer without bundled FFmpeg / stream workers |
| `Radium-v2.0.0-Windows-x64-Full.zip` | x64 | Portable ZIP | Extract and run; includes stream workers |
| `Radium-v2.0.0-Windows-x64-Core.zip` | x64 | Portable ZIP | Core portable binaries only |
| `Radium-Setup-ARM64-2.0.0.exe` | ARM64 | Inno Setup Installer | Native ARM64 installer for Windows 10 / 11 on ARM |
| `Radium-Setup-ARM64-Core-2.0.0.exe` | ARM64 | Inno Setup Installer | Lightweight native ARM64 installer |
| `Radium-v2.0.0-Windows-ARM64-Full.zip` | ARM64 | Portable ZIP | Native ARM64 portable package with stream workers |
| `Radium-v2.0.0-Windows-ARM64-Core.zip` | ARM64 | Portable ZIP | Native ARM64 core portable binaries |
| `Radium-x64-2.0.0.0.msix` | x64 | Unsigned MSIX | Store-submission package (**unsigned** — see MSIX note below) |
| `Radium-ARM64-2.0.0.0.msix` | ARM64 | Unsigned MSIX | Store-submission package (**unsigned** — see MSIX note below) |
| `Radium-2.0.0.0.msixbundle` | x64 + ARM64 | Unsigned MSIX Bundle | Combined Store-submission bundle (**unsigned** — see MSIX note below) |
| `radium-extension.zip` | Cross-browser | Extension ZIP | Manifest V3 extension for Chrome, Edge, Brave, and Firefox |
| `SHA256SUMS.txt` | All | Checksums | SHA-256 digests for all release assets |

---

## Installation & Upgrade Instructions

### Upgrading from Version 1.0.0
- **Standard Installer / Portable Users:** Run `Radium-Setup-x64-2.0.0.exe` (or `ARM64`) over your existing installation. Your download history, queue, scheduler, speed limits, and settings in `%LOCALAPPDATA%\Radium Download Manager\state.db` are preserved automatically, and existing downloaded files in your Downloads folder are never touched.
- **Portable Users:** Extract the new ZIP archive and copy your existing `data/` folder (if upgrading an existing portable directory) to keep your history and settings.
- **MSIX Data Isolation Note:** Windows MSIX packages run with `%LOCALAPPDATA%` virtualization (`%LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalCache\Local\Radium Download Manager`), whereas the traditional Inno Setup installer stores state directly in `%LOCALAPPDATA%\Radium Download Manager`. Both share your Windows `%USERPROFILE%\Downloads` folder.

### MSIX Signing & Installation Notice
- The `.msix` and `.msixbundle` assets attached to this GitHub Release are **unsigned** Store-submission packages (`Publisher=CN=6FC19DA1-6E47-4244-879F-EB1F5DC0E7DA`).
- **Standard Windows users cannot install an unsigned `.msix` directly by double-clicking**; Windows requires a trusted Authenticode signature or Microsoft Store distribution.
- For normal desktop installation outside the Microsoft Store, please use **`Radium-Setup-x64-2.0.0.exe`** (or the ARM64 / Portable packages).
