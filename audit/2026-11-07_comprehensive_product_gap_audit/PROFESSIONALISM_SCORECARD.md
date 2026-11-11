# Radium Download Manager — Professionalism Scorecard

**Date:** November 7, 2026  
**Auditor:** Senior Software Engineering & Product Audit Team  
**Evaluation Target:** Radium Download Manager (Current Repository State)  
**Scoring Standard:** Graded on a 0 to 10 scale against mature commercial Windows software (IDM, FDM, qBittorrent, Chrome).

---

## 1. Scorecard Summary

| Category | Score (0 - 10) | Rating | Primary Justification |
| :--- | :---: | :---: | :--- |
| **Visual Polish** | **8.0 / 10** | Strong | Modern dark/coral theme, crisp high-DPI scaling, and fluent layout; deducted for raw text prompt dialogs. |
| **User Experience (UX)** | **7.0 / 10** | Adequate | Intuitive main workflow; deducted for text-based scheduler/speed dialogs and static ZIP preview. |
| **Download Reliability** | **9.2 / 10** | Commercial Grade | RFC 9110 compliance, dynamic work-stealing, stepped 429 backoff, atomic commits, SHA-256 checks. |
| **Torrent Functionality** | **6.5 / 10** | Incomplete | Robust downloading/seeding via BEP 9/15/5; deducted for missing torrent creator and peer/tracker inspector. |
| **Browser Integration** | **9.0 / 10** | Commercial Grade | MV3 Chrome/Edge/Brave/Firefox support, deep media detection, closed Shadow DOM, native named pipe RPC. |
| **Performance** | **9.5 / 10** | Industry Leading | Sub-35 ms startup, 12.7 MB idle RSS, >480 Mbps throughput. Greatly outperforms Qt and Electron apps. |
| **Security & Privacy** | **8.8 / 10** | Strong | Strict path traversal sanitization, DPAPI at rest, DACL-protected named pipe, token masking in logs. |
| **Stability** | **8.9 / 10** | High | Structured exception handling, IOCP workers, atomic commits, zero unhandled crash vectors in tests. |
| **Windows Integration** | **8.2 / 10** | Good | ITaskbarList3 progress, tray icon, dark mode sync, high-DPI support; deducted for missing WinRT toasts. |
| **Installation & Updates** | **6.5 / 10** | Incomplete | Clean Inno Setup and MSIX definitions; deducted for total lack of in-app automatic update delivery. |
| **Documentation & Brand** | **8.8 / 10** | Comprehensive | Exhaustive architectural specs, ADRs, and clean branding; deducted for lack of user video guides/portal. |
| **Feature Completeness** | **7.5 / 10** | Moderate | Complete core HTTP/stream engine; deducted for missing SOCKS5, site credential manager, torrent creator. |
| **Accessibility (A11y)** | **2.0 / 10** | Unacceptable | Complete omission of `WM_GETOBJECT`, MSAA, and UI Automation provider interfaces. |
| **Overall Production Readiness**| **7.5 / 10** | Near Release | World-class native engine requiring UI dialog refinement, accessibility, and auto-updater to reach v1.0. |

---

## 2. Detailed Category Rationales

### 1. Visual Polish: 8.0 / 10
- **Strengths:** Custom double-buffered GDI rendering eliminates flickering. Clean typography, consistent coral design tokens (`tokens.css`, `design_system.hpp`), modern status badges, and precise high-DPI scaling (100% to 200%).
- **Weaknesses:** Secondary modal dialogs (Scheduler, Bandwidth Limiter) revert to standard Win32 prompt dialogs that lack the coral theme. GDI font and pen allocations occur inside rendering loops.

### 2. User Experience: 7.0 / 10
- **Strengths:** Simple drag-and-drop torrent importing, one-click browser download interception, unobtrusive video overlay button, clean task list filtering by categories.
- **Weaknesses:** Non-interactive ZIP preview (plain text). Scheduler and speed limiter lack visual sliders and presets. Column headers cannot be reordered or customized. Error messages in the status column are clipped without tooltip expansion.

### 3. Download Reliability: 9.2 / 10
- **Strengths:** Exceptional adherence to RFC 9110 byte-range satisfaction invariants. Stepped adaptive connection reduction (32 -> 16 -> 8 -> 4 -> 2 -> 1) on HTTP 429/503. Work-stealing segment preservation prevents re-downloading bytes. Atomic `.part` file commits prevent partial file corruption.
- **Weaknesses:** FTPS (FTP over TLS) unsupported. Extremely high-throughput concurrent segment commits serialize on a single SQLite mutex.

### 4. Torrent Functionality: 6.5 / 10
- **Strengths:** Implements BEP 9 (Extension for Peers to Send Metadata Files), BEP 15 (UDP Tracker Protocol), BEP 5 (DHT), Choking/Unchoking algorithms, and multi-file storage mapping. Seeding with configurable ratio and time limits.
- **Weaknesses:** No torrent creation wizard. No peer list tab (IPs, client versions, flags). No tracker management tab (cannot view tracker errors or add new trackers on running tasks). No sequential download toggle for stream previewing.

### 5. Browser Integration: 9.0 / 10
- **Strengths:** Pure vanilla TypeScript Manifest V3 extension. Zero page stylesheet pollution via closed Shadow DOM. Deep sniffing hooks for HLS/DASH manifests. Non-blocking fallback to browser download manager if Radium is offline.
- **Weaknesses:** Missing browser context menu entry for "Download All Links on Page". Video overlay button positioning can experience edge-case occlusion on complex fullscreen player wrappers.

### 6. Performance: 9.5 / 10
- **Strengths:** Written in native C++20 with MSVC /O2 optimization. Cold startup in ~32 ms. Idle RAM consumption of only ~12.7 MB. Sustained download throughput of 485+ Mbps. Highly efficient compared to Qt 6 (~65 MB) and Electron (~180 MB).
- **Weaknesses:** GDI font allocation churn during rapid paint iterations.

### 7. Security: 8.8 / 10
- **Strengths:** Bulletproof path traversal sanitization stripping alternate data streams, drive letters, and DOS devices. ACL-protected Named Pipe (`TOKEN_USER` SID check). Credentials protected at rest using Windows DPAPI. Token redaction across all logs.
- **Weaknesses:** Out-of-process worker binaries (`N_m3u8DL-RE.exe` and `ffmpeg.exe`) are not verified against pinned SHA-256 hashes or Authenticode signatures before execution. WinHTTP does not enforce OCSP certificate revocation checks.

### 8. Stability: 8.9 / 10
- **Strengths:** Robust Windows SEH crash logging (`SetUnhandledExceptionFilter`). RAII wrappers on OS handles. Safe crash recovery with disk reconciliation of partial `.part` files. Passes 77 C++ unit and regression suites.
- **Weaknesses:** Process shutdown can experience brief latency if remote torrent UDP trackers are slow to respond to exit announcements.

### 9. Windows Integration: 8.2 / 10
- **Strengths:** Taskbar progress fill (`ITaskbarList3`), system tray minimization with context menu, automatic Windows dark/light theme tracking, Per-Monitor V2 DPI awareness.
- **Weaknesses:** Completion notifications use legacy Shell tray balloon tips rather than rich Windows 11 Action Center WinRT toasts with interactive action buttons.

### 10. Installation & Updates: 6.5 / 10
- **Strengths:** High-quality Inno Setup installer registering clean start menu shortcuts, uninstaller, and browser Native Messaging manifests. Modern MSIX packaging script with valid appx assets.
- **Weaknesses:** Complete absence of an automated background update checker or in-app one-click update pipeline. Users must manually check GitHub releases.

### 11. Documentation & Brand: 8.8 / 10
- **Strengths:** Comprehensive architectural documentation, ADRs, threat models, test matrices, and unified "Radium" brand guidelines.
- **Weaknesses:** Lacks user-facing video tutorials, interactive online help documentation, and a dedicated product website.

### 12. Feature Completeness: 7.5 / 10
- **Strengths:** Full segmented HTTP downloader, HLS/DASH video stream extractor, BitTorrent client, site grabber/crawler, ZIP previewer, scheduler, and speed limiter.
- **Weaknesses:** Missing SOCKS5 proxy support, site credential manager, torrent creator, and batch file renamer.

### 13. Accessibility (A11y): 2.0 / 10
- **Strengths:** High-contrast text colors and clean font rendering.
- **Weaknesses:** **Critical compliance failure.** The custom GDI UI completely omits `WM_GETOBJECT`, MSAA, and UI Automation providers. Screen readers perceive the entire window as an empty, inaccessible box.

### 14. Overall Production Readiness: 7.5 / 10
- **Verdict:** Radium possesses an exceptional, high-speed C++20 engine that matches or outperforms commercial alternatives. However, addressing the accessibility absence, upgrading primitive dialogs, adding torrent swarm controls, and integrating an automatic updater are essential before commercial release.
