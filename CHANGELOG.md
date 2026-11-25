# Changelog

All notable changes to **Radium Download Manager** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.0.0] - 2026-11-25

### Major New Features
- **First-Launch Browser Extension Setup & In-App Updater (`src/core/extension_installer.cpp`, `src/ui/extension_setup_dialog.cpp`):** Added a guided first-launch onboarding flow for installing the Manifest V3 browser extension (automatically skipped for existing installations with persisted tasks), plus Settings controls to download or update `radium-extension.zip` from GitHub Releases with atomic staging and rollback.
- **Modular Multi-Site Video Extraction (`src/engine_stream/video_extractor.cpp`, `extension/catch-script/catch.js`):** Added `VideoExtractionManager` (`IVideoExtractor`) supporting YouTube, Bilibili (`__playinfo__` DASH video+audio pairing and multipart episode resolution), Facebook (`fbcdn.net` `bytestart`/`byteend` stripping), and `yt-dlp` fallback, with non-blocking quality probing in the Add Download dialog.
- **Add Torrent File Selector & Magnet Metadata Exchange (`src/ui/win32_main_window.cpp`, `src/engine_torrent/`):** Added a dedicated Add Torrent dialog for `.torrent` files and browser `.torrent` offers (bounded 16 MiB off-UI-thread metainfo fetch), selective multi-file downloading, `.torrent` file creation (`TorrentCreator`), and BEP 9 magnet metadata exchange.
- **Recursive Site Grabber, ZIP Preview & Remote Control (`src/crawler/`, `src/archive/`, `src/remote/`):** Added breadth-first site grabbing with SSRF protection, pre-extraction ZIP archive previewing with Zip Slip and bomb guards, and MCP / remote control RPC support.
- **Store-Ready MSIX & Multi-Arch Bundle Packaging (`installer/msix/`):** Added GitHub Actions packaging for `Radium-x64-2.0.0.0.msix`, `Radium-ARM64-2.0.0.0.msix`, and `Radium-2.0.0.0.msixbundle` (unsigned Store-submission packages).

### Download Engine Improvements
- **Speculative Background Download & Confirmation Continuity:** Eligible browser-intercepted downloads begin transferring in the background while awaiting user confirmation, reducing dispatch latency while cleanly cancelling and cleaning up staging if cancelled or conflicted.
- **File Conflict Resolution & Ghost Task Prevention (`src/core/download_controller.cpp`, `src/core/duplicate_detector.hpp`):** Added an interactive "File Already Exists" dialog with remembered Overwrite / Rename / Skip policies, atomic file replacement, and fixes preventing duplicate dialogs or silent paused ghost tasks on repeated URLs or startup restoration.
- **Segmented HTTP & FTP Reliability:** Fixed a dangling `std::string_view` in the `Content-Range` parser, resolved a recursive mutex deadlock in `SegmentScheduler::WaitForWork`, ensured preempted workers exit cleanly, raised the TTFB preemption floor to 1000 ms, added an in-flight resplit guard, and improved FTP `EPSV`/`PASV`/NAT bypass and `REST 350` resume truncation handling.

### Video & BitTorrent Improvements
- **Stream Quality Integrity & Verification:** Enforced `ffprobe` container verification, honest progress reporting without artificial clamps, and intermediate `.part` preservation on stream retry.
- **Payload-Delta Torrent Rate Meter (`src/engine_torrent/torrent_rate_meter.hpp`):** Replaced raw socket counters with monotonic payload-delta rate reporting, idle reset, verified upload accounting, and sequential piece mode.

### Browser Extension & UI Improvements
- **Browser Extension (`extension/`):** Improved repeat-attempt download correlation, per-tab SPA/fragment navigation URL reconciliation, anchor-bound `blob:`/`data:` artifact capture, and compact closed Shadow DOM video overlay with quality selector and fullscreen sync. Published `radium-extension.zip` directly with releases.
- **Windows 10 / 11 Theme & Layout Polish (`src/ui/`):** Added Windows System Theme detection (System / Light / Dark) with a persisted toolbar toggle, fixed dark-mode input field styling, made the New Download dialog more compact (520×480 DPI-scaled), added synchronous row hover redraws, and updated all platform labels to Windows 10 / 11.

### Security, Reliability & Hygiene
- **Repository-Wide Namespace & Hygiene Cleanup:** Completed internal `openidm`/`flux` → `radium` namespace and macro cleanup (`namespace radium`, `RADIUM_LOG_*`, `WM_RADIUM_*`) while preserving all legacy AppData, Native Messaging, and IPC migration paths. Removed committed binaries and obsolete audit dumps and added `SECURITY.md`, `CODE_OF_CONDUCT.md`, and CI hygiene checks.
- **Post-Download Antivirus Scan & Secret Redaction:** Integrated Windows Defender (`MpCmdRun`) scanning with threat execution blocking and credential scrubbing in logs, UI details, and JSON/CSV exports.

## [1.0.0] - 2026-11-10

### Added
- Native C++20 / Win32 desktop download manager (`radium.exe`) with adaptive segmented HTTP/HTTPS engine (`WinHTTP` + IOCP).
- Out-of-process HLS/DASH/MSS stream pipeline via `N_m3u8DL-RE` and `FFmpeg` in isolated Windows Job Objects.
- Built-in BitTorrent v1 engine with metadata exchange (BEP 9), tracker announces, piece verification, and selective file downloads.
- Manifest V3 browser extension for Chromium and Firefox communicating via `radium-nmh.exe` and per-user ACL-restricted Windows Named Pipes.
- Windows x64 and ARM64 installers and portable packages.
