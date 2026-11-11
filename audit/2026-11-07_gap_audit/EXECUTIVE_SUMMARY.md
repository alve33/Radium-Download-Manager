# Radium Download Manager: Professional Product Gap Audit
# EXECUTIVE SUMMARY

**Date:** November 7, 2026  
**Product:** Radium Download Manager (Desktop C++20/Win32 Application + Chromium/Firefox MV3 Extension)  
**Evaluation Scope:** Complete Technical, User Experience, Performance, Security, Reliability, Competitive Standing, and Commercial Production Readiness Audit.  
**Auditor:** Senior Software Engineering & Product Audit Team  

---

## 1. Overall Product Verdict

Radium Download Manager is an ambitious, high-performance, open-source download manager engineered with a native **C++20 / Win32 / I/O Completion Port (IOCP)** segmented HTTP download engine, dynamic connection scaling with work-stealing range bisection, out-of-process stream extraction (`N_m3u8DL-RE` / `FFmpeg 7.1`), an in-tree BitTorrent engine, and an ACL-restricted Native Messaging IPC architecture.

On technical metrics, Radium outperforms established competitors like Internet Download Manager (IDM), Free Download Manager (FDM), and Motrix in raw footprint:
- **Peak RAM:** 18–22 MB (vs IDM's 50–120 MB and Motrix's 150–350 MB Electron footprint).
- **Startup Latency:** ~53 ms cold launch.
- **Pre-allocation:** NTFS `FSCTL_SET_SPARSE` instantaneous multi-gigabyte disk allocation (<1 ms vs 1500 ms cluster-zeroing).
- **Core Binary Size:** ~3.07 MB standalone PE32+ executable.

However, evaluated against the standards of a **mature, commercially released Windows product (such as IDM, qBittorrent, or FDM)**, Radium is currently at **Late Alpha / Early Beta** maturity. While the core HTTP engine and stream worker pipelines are robust, the product suffers from substantial gaps in **Accessibility, Screen Reader Support, BitTorrent Swarm Algorithms, Settings Lifecycle, Native Windows UX, and Commercial Trust Infrastructure**.

---

## 2. The Biggest Critical Weaknesses

### 1. Total Screen Reader & Accessibility Blackout (Critical / P0)
- **Finding:** The main download queue view is a custom-drawn Win32 canvas (`Radium_MainWindowClass` in `src/ui/win32_main_window.cpp`) that completely lacks `WM_GETOBJECT` handling. It does not implement Microsoft UI Automation (`IUIAutomation` / `IRawElementProviderSimple`) or MSAA (`IAccessible`).
- **Impact:** Screen readers (Windows Narrator, NVDA, JAWS) read Radium as a blank window. Visually impaired users cannot determine download progress, select tasks, or navigate categories.

### 2. GDI Handle Churn & Paint-Cycle Overhead (High / P1)
- **Finding:** In `src/ui/win32_main_window.cpp:7953–7968`, four Segoe UI fonts (`hFontBody`, `hFontBold`, `hFontSmall`, `hFontSmallBold`) are created via `CreateFontW` and deleted via `DeleteObject` **on every single WM_PAINT call**.
- **Impact:** During high-speed multi-connection downloads updating every 100–250 ms, or rapid mouse hovers across rows, Windows allocates and destroys thousands of GDI font objects, causing unnecessary CPU cycles and micro-stuttering.

### 3. BitTorrent Engine Missing Real-World Swarm Algorithms (High / P1)
- **Finding:** The BitTorrent engine (`src/engine_torrent/`):
  - **Lacks Rarest-First Piece Selection (`piece_manager.hpp:159-189`):** Selects missing pieces in strictly sequential order (`0..N`), harming swarm distribution and stalling multi-peer downloads.
  - **Lacks Endgame Mode (`piece_manager.hpp:145-156`):** Does not duplicate requests for the final pending blocks; transfers stall at 99% if the final peer is slow or drops connection.
  - **Lacks Anti-Leech Choking (`torrent_engine.cpp:544, 655`):** Unconditionally unchokes all connected peers on handshake without implementing Tit-for-Tat rate sorting.
  - **Lacks Web Seeding (`BEP 17 / BEP 19`):** Cannot use HTTP mirrors.
  - **Lacks UPnP / NAT-PMP:** Inbound listener on port 6881 cannot accept incoming peer connections behind standard NAT routers.

### 4. Flawed Crawler Range Requests on HTML Pages (High / P1)
- **Finding:** In `src/crawler/site_grabber.cpp:218–223`, the crawler injects `Range: bytes=0-4194304` when downloading HTML pages.
- **Impact:** Dynamic application servers (PHP, Node.js) and CDNs (Cloudflare) reject byte-range requests on dynamic HTML, returning `HTTP 416 Range Not Satisfiable` and causing site crawling to fail.

### 5. ZIP Preview Incompatible with ZIP64 Archives >4 GB (High / P1)
- **Finding:** In `src/archive/zip_preview.cpp:17–47`, EOCD and Central Directory records are strictly parsed using standard 32-bit integer structures without checking for the Zip64 EOCD Locator (`0x07064b50`).
- **Impact:** Any archive >4 GB or containing >65,535 files triggers `ERR_ZIP_CORRUPT` and cannot be previewed.

### 6. Missing Positional CLI Torrent/Magnet Handling (High / P1)
- **Finding:** `src/app/main.cpp:91–128` only parses explicit flags (`--download`, `--stream`). When a user double-clicks a `.torrent` file associated with Radium, Windows passes the path as `argv[1]` without `--download`. Radium ignores it and launches empty.
- **Impact:** Radium cannot function as a default Windows `.torrent` or `magnet:` handler.

### 7. Missing Settings Export/Import Lifecycle (Medium / P2)
- **Finding:** While tasks can be imported/exported to JSON/CSV (`src/storage/import_export.cpp`), there is **no settings export/import facility** for user preferences, proxy configurations, or custom category rules.

### 8. Commercial Trust & Distribution Infrastructure (Medium / P2)
- **Finding:** 
  - **No Authenticode Code Signing:** Binaries are unsigned; Windows SmartScreen triggers untrusted publisher warnings on launch.
  - **Unpublished Browser Extension:** Must be loaded manually via Developer Mode ("Load unpacked").
  - **No Minidump / Crash Reporting:** Crashes in Win32 or WinHTTP exit silently without writing `.dmp` files to disk.
  - **Hardcoded English:** Over 320 wide string literals are hardcoded across `win32_main_window.cpp` with no localization table.

---

## 3. Product Scorecard Summary (0–10)

| Category | Score | Summary Justification |
| :--- | :---: | :--- |
| **Visual Polish** | **7.5 / 10** | Modern "Coral Dark" theme and Mica backdrops; hampered by raw Win32 MsgBoxes and GDI font recreation. |
| **User Experience (UX)** | **6.5 / 10** | Clean main window, but primitive string-prompt dialogs for speed limits/scheduling and no first-run wizard. |
| **Download Reliability** | **8.5 / 10** | Rock-solid HTTP segmented engine with dynamic work-stealing, sparse files, and 429/503 backoff. |
| **Torrent Functionality** | **4.5 / 10** | Working BEP 3/9/10/15/23 baseline, but lacks Rarest-First, Endgame mode, Choking, and UPnP. |
| **Browser Integration** | **7.5 / 10** | Native messaging and MV3 session persistence are strong; lacks Web Store publishing and Firefox main-world hook. |
| **Performance** | **9.0 / 10** | Ultra-lean 18–22 MB peak RSS, ~53 ms startup, instantaneous sparse pre-allocation. |
| **Security & Privacy** | **8.0 / 10** | Strict SSRF pinning, ACL-protected named pipe, and zero-raw-command worker execution. Minor path/logging gaps. |
| **Stability & Error Handling** | **8.0 / 10** | Robust network drop and stall recovery; lacks SEH minidump crash handlers. |
| **Windows Integration** | **7.0 / 10** | Implements PMv2 DPI, DWM dark mode, Taskbar progress; lacks positional CLI file associations. |
| **Installation & Updates** | **6.5 / 10** | Inno Setup and MSIX builders are functional, but lack WinGet automation and silent background updates. |
| **Documentation & Branding** | **6.5 / 10** | Comprehensive architecture docs; marred by dozens of legacy "Flux" and "OpenIDM" remnants. |
| **Feature Completeness** | **6.0 / 10** | Strong HTTP/Stream pipeline; incomplete Torrent, FTP (single-stream), and Crawler features. |
| **Accessibility (a11y)** | **1.0 / 10** | Near-complete screen reader blackout; custom canvas has no MSAA or UI Automation. |
| **Production Readiness** | **5.5 / 10** | Solid technical core, but requires commercial polish, accessibility, and packaging before commercial release. |

---

## 4. Key Takeaways & Recommended Roadmap

1. **Phase 1: Release Blockers (P0 / Critical)**
   - Implement `WM_GETOBJECT` (MSAA/UI Automation) on the main download list view.
   - Cache fonts and GDI objects to eliminate per-frame `CreateFontW`/`DeleteObject` churn.
   - Accept positional arguments in `src/app/main.cpp` and register `.torrent`/`magnet:` associations in `radium.iss`.
   - Fix Site Grabber HTTP Range header injection on HTML documents.
   - Add ZIP64 support to `src/archive/zip_preview.cpp`.

2. **Phase 2: Commercial Parity (P1 / High)**
   - Implement Rarest-First piece selection and Endgame mode in the BitTorrent engine.
   - Replace raw string-prompt dialogs for Speed Limit and Scheduler with structured, dark-themed Win32 dialogs.
   - Add Settings Export/Import (`app_settings` JSON) in the Settings Center.
   - Clean up remaining "Flux" and "OpenIDM" references in documentation and headers.

3. **Phase 3: Ecosystem & Trust (P2 / Medium)**
   - Implement Win32 SEH `MiniDumpWriteDump` for crash reporting in `%LOCALAPPDATA%\Radium\crashes\`.
   - Add a 3-step First-Run Onboarding Wizard.
   - Package and submit the extension to the Chrome Web Store and Mozilla Add-on Store.
   - Submit Radium to `microsoft/winget-pkgs`.
