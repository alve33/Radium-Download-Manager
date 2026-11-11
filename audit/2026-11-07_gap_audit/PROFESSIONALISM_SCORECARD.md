# Radium Download Manager: Professionalism Scorecard

**Date:** November 7, 2026  
**Auditor:** Senior Software Engineering & Product Audit Team  
**Evaluation Target:** Radium Download Manager (Desktop C++20 + WebExtension)  

---

## 1. Scorecard Summary

| Category | Score (0–10) | Benchmark Comparison | Primary Driver of Score |
| :--- | :---: | :---: | :--- |
| **Visual Polish** | **7.5 / 10** | Above IDM, Below Motrix | Sleek dark theme & Mica backdrop; degraded by plain `MessageBoxW` dialogs. |
| **User Experience (UX)** | **6.5 / 10** | Below FDM, Above JD2 | Clean dashboard layout, but primitive string prompts for speed limit and scheduler. |
| **Download Reliability** | **8.5 / 10** | Matches IDM | Outstanding segmented HTTP engine with work-stealing, sparse files, and 429/503 backoff. |
| **Torrent Functionality** | **4.5 / 10** | Far Below qBittorrent | Working baseline (BEP 3/9/15), but lacks Rarest-First, Endgame mode, Choking, and UPnP. |
| **Browser Integration** | **7.5 / 10** | Matches FDM, Below IDM | Solid Native Messaging & MV3 persistence; missing Web Store release and Firefox main hook. |
| **Performance** | **9.0 / 10** | Beats All Competitors | Industry-leading 18–22 MB peak RSS, ~53 ms cold launch, <1 ms sparse allocation. |
| **Security & Privacy** | **8.0 / 10** | Above IDM / JD2 | Strict SSRF defense, ACL pipe security, and isolated workers. Custom AV quoting needs fix. |
| **Stability & Error Handling**| **8.0 / 10** | Matches IDM | Strong network drop recovery and stall watchdogs; lacks SEH minidump crash handler. |
| **Windows Integration** | **7.0 / 10** | Below IDM, Above Motrix | Per-Monitor v2 DPI, Taskbar progress, Tray icon; broken positional CLI file associations. |
| **Installation & Updates** | **6.5 / 10** | Matches FDM | Robust Inno Setup & MSIX scripts; lacks silent auto-updates and WinGet publishing. |
| **Documentation & Branding** | **6.5 / 10** | Above IDM / JD2 | Exhaustive architecture docs; marred by remnants of legacy names ("Flux", "OpenIDM"). |
| **Feature Completeness** | **6.0 / 10** | Below IDM / FDM | Solid HTTP/Stream pipeline; incomplete Torrent, FTP (single-stream), and Crawler features. |
| **Accessibility (a11y)** | **1.0 / 10** | Far Below All Rivals | Total screen reader blackout; custom GDI canvas does not implement `WM_GETOBJECT`. |
| **Overall Production Readiness**| **5.5 / 10** | Late Alpha / Early Beta | High technical potential, but requires accessibility, UI dialog polish, and torrent algorithms. |

---

## 2. In-Depth Justification Behind Scores

### Visual Polish: 7.5 / 10
- **Strengths:** High-contrast "Coral Dark" color palette, smooth geometry, Windows 11 DWM rounded corners, and Mica backdrop support.
- **Deductions:** White, non-dark standard `MessageBoxW` dialogs for ZIP preview and SHA-256 inspection break visual harmony. Software GDI lacks anti-aliased geometry curves at fractional scaling (125%, 150%).

### User Experience: 6.5 / 10
- **Strengths:** Clean main window layout with distinct category sidebars and contextual action buttons.
- **Deductions:** Setting speed limits and scheduler requires typing manual strings into primitive single-line prompt boxes (`"Enter maximum speed in KB/s"`). No visual calendar pickers, sliders, or onboarding wizard.

### Download Reliability: 8.5 / 10
- **Strengths:** Highly reliable segmented HTTP engine. Features dynamic work-stealing, NTFS sparse allocation, early GET request reuse, and robust 429/503 exponential backoff.
- **Deductions:** MD5 verification is missing (only SHA-256 exists). Worker 0 terminates early, leaving a permanent unutilized concurrency slot.

### Torrent Functionality: 4.5 / 10
- **Strengths:** Functional BEP 3 wire protocol, BEP 9 metadata exchange for magnet links, BEP 15 UDP trackers, and SHA-1 piece verification.
- **Deductions:** Completely lacks Rarest-First piece picking (strictly sequential), lacks Endgame Mode (stalls at 99%), unchokes all peers unconditionally (no Tit-for-Tat), and lacks UPnP/NAT-PMP port forwarding.

### Browser Integration: 7.5 / 10
- **Strengths:** Clean MV3 service worker with session state persistence, typed JSON-RPC 2.0 native messaging, and excellent progressive video and HLS/DASH media sniffing.
- **Deductions:** Firefox MV3 manifest lacks `catch.js` page context injection. Extension is not published to the Chrome Web Store, requiring manual developer mode loading.

### Performance: 9.0 / 10
- **Strengths:** Exceptional resource efficiency. Consumes only 18–22 MB peak RSS during multi-gigabyte downloads (compared to 120 MB for IDM and 300 MB for Motrix), boots in ~53 ms, and allocates large files instantly via `FSCTL_SET_SPARSE`.
- **Deductions:** GDI font handles are repeatedly allocated and freed inside `WM_PAINT`, generating avoidable CPU churn during high-frequency UI updates.

### Security & Privacy: 8.0 / 10
- **Strengths:** Strict private IP / loopback SSRF rejection, per-user ACL-restricted named pipe, and zero raw command-line execution over IPC.
- **Deductions:** Worker command-line construction lacks backslash escaping before quotes, and custom antivirus template replacement allows potential parameter manipulation. Custom HLS keys are stored in plaintext in SQLite.

### Stability & Error Handling: 8.0 / 10
- **Strengths:** Excellent network disconnect recovery, automatic hash recalculation on resume, and stall watchdog timers.
- **Deductions:** No `SetUnhandledExceptionFilter` to capture unhandled hardware exceptions and write minidumps.

### Windows Integration: 7.0 / 10
- **Strengths:** Per-Monitor v2 DPI awareness, Windows 11 DWM dark mode, Taskbar progress `ITaskbarList3`, and System Tray icon with Explorer restart recovery.
- **Deductions:** Command-line parser ignores positional arguments, preventing Windows `.torrent` file associations from opening downloads on double-click.

### Installation & Updates: 6.5 / 10
- **Strengths:** Fully functional Inno Setup script with clean uninstallation of Native Messaging registry keys, and multi-architecture MSIX package builder.
- **Deductions:** No silent background update patching, no Ed25519-signed update manifest, and no official WinGet package submission.

### Documentation & Branding: 6.5 / 10
- **Strengths:** Exceptionally detailed architectural documentation and clear Mermaid diagrams.
- **Deductions:** Over 40 active references to legacy names ("Flux" and "OpenIDM") persist in headers and markdown documentation.

### Feature Completeness: 6.0 / 10
- **Strengths:** Complete HTTP, HLS, and DASH download and muxing capabilities.
- **Deductions:** Incomplete BitTorrent engine, single-stream FTP client, and regex-only crawler that injects invalid Range headers on HTML pages.

### Accessibility (a11y): 1.0 / 10
- **Strengths:** Keyboard arrow navigation works within the main table.
- **Deductions:** Complete absence of `WM_GETOBJECT` (MSAA / UI Automation). Screen readers (Narrator, NVDA) read the main download list as completely blank. No `Tab` focus cycling between sidebar, search bar, and table.

### Overall Production Readiness: 5.5 / 10
- **Verdict:** Radium has a world-class segmented download engine, but is currently in a **Late Alpha / Early Beta** state as a commercial desktop product. Addressing accessibility, torrent algorithms, and dialog polish will elevate Radium to a true, trustworthy replacement for IDM.
