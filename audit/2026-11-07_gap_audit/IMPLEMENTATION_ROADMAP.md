# Radium Download Manager: Prioritized Implementation Roadmap

**Date:** November 7, 2026  
**Auditor:** Senior Software Engineering & Product Audit Team  
**Objective:** Transform Radium from Late Alpha / Early Beta into a polished, accessible, reliable, commercial-grade Windows desktop application that can confidently replace Internet Download Manager.

---

## 1. Roadmap Phasing Strategy

```
+-----------------------------------------------------------------------+
| P0: Critical Release Blockers (Safety, Accessibility, Correctness)    |
| - Implement WM_GETOBJECT (MSAA/UIA Screen Reader Accessibility)       |
| - Cache GDI Fonts/Brushes (Eliminate Per-Frame Churn)                 |
| - Accept Positional CLI File Arguments (.torrent Double-Click)         |
| - Fix Crawler HTTP Range Bug on Dynamic HTML                          |
| - Add ZIP64 Archive Parser Support (>4 GB files)                      |
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
| P1: Major Commercial Quality Polish (Completeness & Trust)           |
| - BitTorrent Swarm Algorithms: Rarest-First & Endgame Mode           |
| - Replace String Prompts with Structured Dialogs (Speed & Scheduler)  |
| - Implement Settings Export/Import (JSON Preferences)                 |
| - Worker Command Line Quoting & Antivirus Sanitization                |
| - Fix Firefox MV3 Dynamic catch.js Injection                          |
| - Documentation & Codebase Legacy Name Cleanup ("Flux" -> "Radium")  |
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
| P2: Competitive Enhancements (Feature Parity with IDM/qBittorrent)    |
| - Win32 SEH MiniDump Crash Handler (%LOCALAPPDATA%\Radium\crashes\)   |
| - BitTorrent Tit-for-Tat Choking & UPnP/NAT-PMP Port Forwarding       |
| - Remote HTTP Range ZIP File Preview                                  |
| - Multi-Queue Scheduling & Category Directory Routing                 |
| - 3-Step Native Onboarding Wizard                                     |
| - Package & Publish Extension to Chrome Web Store                     |
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
| P3: Advanced Refinements & Enterprise Readiness                       |
| - Multi-Connection Segmented FTP Downloading via REST                 |
| - UI String Localization Table (i18n)                                 |
| - Direct2D 1.1 Hardware-Accelerated Vector Rendering Migration        |
| - Interactive WinRT Toast Notifications with Inline Action Buttons    |
+-----------------------------------------------------------------------+
```

---

## 2. Priority P0: Critical Release Blockers

*Target: Immediate execution. These issues represent non-negotiable legal, functional, or stability barriers.*

### P0-1: Implement `WM_GETOBJECT` (MSAA / UI Automation) for Main Task List
- **Problem:** Screen readers (Narrator, NVDA) read Radium as a completely blank window (`AUD-01`).
- **Dependencies:** None.
- **Action:**
  1. Handle `WM_GETOBJECT` for `OBJID_CLIENT` in `src/ui/win32_main_window.cpp`.
  2. Implement an `IAccessible` or `IRawElementProviderSimple` provider for the table control.
  3. Expose table rows, column names, progress percentages, and action states to Windows accessibility trees.
- **Expected Benefit:** 100% compliance with Windows desktop accessibility standards; enables visually impaired users to use Radium.

### P0-2: Cache GDI Fonts and Pens in `Win32WindowContext`
- **Problem:** `CreateFontW` and `DeleteObject` are called on every single frame inside `PaintMainWindow` (`AUD-02`).
- **Dependencies:** None.
- **Action:**
  1. Move font and brush handles into `Win32WindowContext`.
  2. Create them once during `WM_CREATE` or first paint.
  3. Recreate them only upon receiving `WM_DPICHANGED` or theme change notifications.
- **Expected Benefit:** Eliminates thousands of GDI allocations/deallocations per second; prevents micro-stutters during high-speed downloads.

### P0-3: Accept Positional CLI Arguments for `.torrent` and URL Double-Click
- **Problem:** Double-clicking a `.torrent` file associated with Radium passes the file as `argv[1]` without `--download`, causing Radium to ignore it (`AUD-08`).
- **Dependencies:** None.
- **Action:**
  1. In `src/app/main.cpp`, if an argument does not start with `--` and represents an existing file path or a recognized URL scheme (`http://`, `https://`, `magnet:`), treat it as an implicit download/torrent target.
  2. Register `.torrent` file extension and `magnet:` URL protocol associations in `installer/radium.iss` and `AppxManifest.xml.template`.
- **Expected Benefit:** Enables setting Radium as default torrent handler in Windows.

### P0-4: Fix Crawler HTTP Range Bug on Dynamic Web Pages
- **Problem:** Site grabber requests HTML pages with `Range: bytes=0-4194304`, causing HTTP 416 errors on servers that do not accept byte ranges on dynamic HTML (`AUD-06`).
- **Dependencies:** None.
- **Action:**
  1. In `src/crawler/site_grabber.cpp`, omit the Range header when fetching HTML pages.
  2. Truncate response buffers in the streaming callback once 4 MB is reached.
- **Expected Benefit:** Resolves crawling failures across CDNs and dynamic web platforms.

### P0-5: Add ZIP64 Archive Parser Support
- **Problem:** ZIP preview fails with `ERR_ZIP_CORRUPT` on archives >4 GB or >65,535 files (`AUD-07`).
- **Dependencies:** None.
- **Action:**
  1. In `src/archive/zip_preview.cpp`, scan backwards for the Zip64 EOCD Locator (`0x07064b50`).
  2. If present, parse the 64-bit Zip64 EOCD Record (`0x06064b50`) for offsets and counts.
- **Expected Benefit:** Allows previewing multi-gigabyte ISO and backup ZIP archives.

---

## 3. Priority P1: Major Commercial Quality Polish

*Target: Release readiness. Resolves core algorithmic and usability gaps.*

### P1-1: Implement BitTorrent Rarest-First Piece Selection & Endgame Mode
- **Problem:** Sequential piece picking harms swarm health; lack of endgame mode causes transfers to stall at 99% (`AUD-03`, `AUD-04`).
- **Dependencies:** P0 complete.
- **Action:**
  1. In `src/engine_torrent/piece_manager.hpp`, maintain a frequency count of piece availability across connected peer bitfields and sort missing pieces by rarity.
  2. Implement an Endgame threshold (< 20 blocks remaining) where unreceived blocks are requested from all capable peers simultaneously, broadcasting `kCancel` upon receipt.
- **Expected Benefit:** Greatly improves torrent download speeds and guarantees completion on slow peers.

### P1-2: Replace Primitive String Prompts with Structured Dialogs
- **Problem:** Speed Limiter and Scheduler prompt users to type raw strings into basic text edits (`AUD-15`, `AUD-16`).
- **Dependencies:** P0-2 (GDI font caching).
- **Action:**
  1. Replace `ShowSimpleInputDialogWin32` with dedicated dark-themed Win32 dialogs.
  2. Speed Limiter: Provide a numeric slider, presets (1 MB/s, 5 MB/s, Unlimited), and unit toggle.
  3. Scheduler: Provide standard `SysDateTimePick32` time controls with days-of-the-week checkboxes.
  4. ZIP Preview: Replace `MessageBoxW` with a tree/list dialog showing file sizes and extraction buttons.
- **Expected Benefit:** Commercial-grade visual polish matching IDM and FDM.

### P1-3: Implement Settings Export and Import
- **Problem:** Users cannot backup or migrate their configurations (`AUD-17`).
- **Dependencies:** None.
- **Action:**
  1. In `src/storage/import_export.cpp`, implement `ExportSettingsToJson` and `ImportSettingsFromJson` serializing the `app_settings` SQLite key.
  2. Wire export/import buttons into the Settings Center UI.
- **Expected Benefit:** Full configuration portability across PCs.

### P1-4: Worker Command-Line Escaping & Antivirus Sanitization
- **Problem:** Arguments ending with backslashes cause parameter swallowing; custom antivirus uses naive string replacement (`AUD-10`, `AUD-11`).
- **Dependencies:** None.
- **Action:**
  1. Implement `ArgvToCommandLine` with proper backslash doubling in `src/worker_stream/nm3u8dl_re_worker.cpp`.
  2. Parse custom antivirus arguments into typed argument vectors in `src/security/antivirus_scanner.cpp`.
- **Expected Benefit:** Hardens process execution against parameter corruption.

### P1-5: Firefox MV3 Dynamic `catch.js` Injection
- **Problem:** Firefox manifest lacks `catch.js` page injection due to `"world": "MAIN"` compatibility (`AUD-09`).
- **Dependencies:** None.
- **Action:**
  1. In `extension/js/content-script.js`, inject `catch-script/catch.js` via a DOM `<script>` element or use `browser.scripting.executeScript`.
- **Expected Benefit:** Restores full video sniffing and player hooking on Mozilla Firefox.

### P1-6: Documentation & Codebase Legacy Name Cleanup
- **Problem:** Over 40 references to "Flux" and "OpenIDM" persist in active documentation and banners (`AUD-23`).
- **Dependencies:** None.
- **Action:**
  1. Update `docs/UI_DESIGN_SYSTEM.md`, `docs/FEATURE_MATRIX.md`, `docs/TEST_MATRIX.md`, and source banners to "Radium".
- **Expected Benefit:** Consistent, professional brand identity across all deliverables.

---

## 4. Priority P2: Competitive Enhancements

*Target: Feature parity with market leaders.*

### P2-1: Win32 SEH MiniDump Crash Handler
- **Action:** Call `SetUnhandledExceptionFilter` in `main.cpp` to write crash dumps (`MiniDumpWriteDump`) to `%LOCALAPPDATA%\Radium\crashes\` (`AUD-19`).
- **Benefit:** Eliminates silent crash disappearance; gives users actionable crash reports.

### P2-2: BitTorrent Tit-for-Tat Choking & UPnP Port Forwarding
- **Action:** Implement BEP 3 choking/unchoking algorithms and UPnP-IGD port mapping for port 6881 (`AUD-05`).
- **Benefit:** Protects upload bandwidth and allows incoming connections behind NAT routers.

### P2-3: Remote HTTP Range ZIP File Preview
- **Action:** Fetch the last 64 KiB of remote ZIP files via HTTP Range to preview archive contents before downloading.
- **Benefit:** Matches one of IDM's most famous proprietary features.

### P2-4: Multi-Queue Scheduling & Category Folder Routing
- **Action:** Restore category folder routing in Settings Center and support multiple named download queues.
- **Benefit:** Highly requested by power users organizing massive download libraries.

### P2-5: First-Run Onboarding Wizard & Extension Publishing
- **Action:**
  1. Add a 3-step native wizard on initial launch.
  2. Package and publish the extension to the Chrome Web Store and Mozilla Add-on Store.
- **Benefit:** Eliminates manual developer mode extension loading; dramatically improves user acquisition.

---

## 5. Priority P3: Advanced Refinements & Enterprise Readiness

*Target: Long-term architectural perfection.*

### P3-1: Multi-Connection Segmented FTP Downloading
- **Action:** Integrate `SegmentScheduler` into `FtpClient` using concurrent `REST` commands (`AUD-14`).
- **Benefit:** Maximizes throughput on high-latency FTP servers.

### P3-2: UI Localization (i18n)
- **Action:** Extract over 320 wide string literals into a string resource table or JSON localization catalog (`AUD-22`).
- **Benefit:** Unlocks international markets (German, Chinese, Spanish, Japanese, etc.).

### P3-3: Direct2D 1.1 Hardware-Accelerated Vector Rendering
- **Action:** Migrate client-area rendering from GDI software bitmaps to Direct2D / DirectWrite.
- **Benefit:** Delivers GPU-accelerated subpixel anti-aliasing on high-DPI displays.

### P3-4: Interactive WinRT Toast Notifications
- **Action:** Upgrade balloon notifications to WinRT `ToastNotificationManager` with inline "Open File" and "Show in Folder" action buttons.
- **Benefit:** Modern Windows 11 Action Center integration.
