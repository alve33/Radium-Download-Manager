# Radium Download Manager — Implementation & Remediation Roadmap

**Date:** November 7, 2026  
**Auditor:** Senior Software Engineering & Product Audit Team  
**Scope:** Actionable, prioritized remediation plan to bridge the gap between the current codebase and a commercial-grade Windows 11 release.

---

## 1. Prioritization Framework

Recommendations are organized strictly by commercial release necessity:
- **P0 (Release Blockers):** Non-negotiable technical, accessibility, and stability defects that prevent commercial deployment or violate legal compliance standards.
- **P1 (Commercial Polish):** Major UX and operational gaps that make the software feel unpolished, incomplete, or frustrating to end users.
- **P2 (Competitive Parity):** Important features required to directly compete with market benchmarks (IDM, FDM, qBittorrent).
- **P3 (Advanced Refinements):** Long-term architectural enhancements and power-user optimizations.

---

## 2. Prioritized Action Items

---

### Priority 0: Release Blockers (Immediate Pre-Release Action)

#### P0-1: Implement Windows UI Automation / MSAA Provider (`WM_GETOBJECT`)
- **Target Component:** `src/ui/win32_main_window.cpp`
- **Objective:** Enable screen readers (NVDA, JAWS, Windows Narrator) to interact with the custom GDI download list and control buttons.
- **Implementation Strategy:**
  1. Handle `WM_GETOBJECT` in the main window procedure (`WndProc`).
  2. Implement a lightweight `IRawElementProviderSimple` and `IRawElementProviderFragment` provider for the custom task list.
  3. Expose row items with name, value (completed percentage / speed), and status so assistive tools can announce downloads accurately.
- **Expected Benefit:** Full Section 508 and accessibility compliance; unlocks institutional and commercial adoption.
- **Complexity:** Medium

#### P0-2: Eliminate GDI Font and Pen Allocations in Render Loops
- **Target Component:** `src/ui/win32_main_window.cpp` (Lines ~1150–1320, 4600–4800)
- **Objective:** Eliminate GDI handle churn and kernel-mode allocation overhead during rapid UI repaints.
- **Implementation Strategy:**
  1. Cache all `HFONT` and `HPEN` objects inside `m_dpi_context` during initial window creation and DPI change events (`WM_DPICHANGED`).
  2. Replace all ad-hoc `CreateFontW` / `CreatePen` calls in drawing procedures with `m_dpi_context.GetCachedFont(...)`.
- **Expected Benefit:** Zero GDI handle leaks, smooth 60 FPS animation, and reduced CPU utilization during heavy downloading.
- **Complexity:** Small

#### P0-3: Implement Automatic Portable Mode Detection (`radium.portable`)
- **Target Component:** `src/app/main.cpp:134-145`
- **Objective:** Support zero-configuration USB and portable deployment.
- **Implementation Strategy:**
  1. On startup in `main.cpp`, check `std::filesystem::exists(exe_dir / "radium.portable")`.
  2. If detected, redirect `app_data_dir = exe_dir / "data"`, `temp_root_dir = exe_dir / "temp"`, and `state_db_path = app_data_dir / "state.db"`.
- **Expected Benefit:** Seamless portable operation without leaving traces in `%LOCALAPPDATA%`.
- **Complexity:** Small

---

### Priority 1: Commercial Polish (V1.0 Readiness)

#### P1-1: In-App Cryptographic Auto-Updater
- **Target Component:** `src/core/download_controller.cpp`, `src/ui/win32_main_window.cpp`
- **Objective:** Automated background update detection and one-click patch delivery.
- **Implementation Strategy:**
  1. Add an asynchronous worker that checks `https://api.github.com/repos/radium/releases/latest` on startup or via "Help -> Check for Updates".
  2. Compare version semver against `RADIUM_VERSION_STRING`.
  3. Verify cryptographic SHA-256 hash and prompt user with release notes and one-click installation.
- **Expected Benefit:** Automated fleet updates without requiring manual browser navigation.
- **Complexity:** Medium

#### P1-2: Modernize Scheduler & Bandwidth Limiter Dialogs
- **Target Component:** `src/ui/win32_main_window.cpp:4500-4750`
- **Objective:** Replace raw text input prompts with rich Win32 interactive controls.
- **Implementation Strategy:**
  1. Replace the bandwidth prompt with a custom dialog featuring a TrackBar slider, unit dropdown (KB/s, MB/s), and quick preset buttons (`1 MB/s`, `5 MB/s`, `10 MB/s`, `Unlimited`).
  2. Replace the scheduler prompt with standard Windows Date and Time Picker controls (`DATETIMEPICK_CLASS`) and day-of-week checkboxes.
- **Expected Benefit:** Eliminates input formatting errors and aligns dialog UX with modern Windows 11 aesthetics.
- **Complexity:** Medium

#### P1-3: Upgrade ZIP Preview to Interactive TreeView with Selective Download
- **Target Component:** `src/archive/zip_preview.cpp`, `src/ui/win32_main_window.cpp`
- **Objective:** Transform the read-only text preview into an actionable archive browser.
- **Implementation Strategy:**
  1. Parse ZIP central directory paths into a hierarchical tree data model.
  2. Render using a Win32 `SysTreeView32` control with checkboxes and file icons.
  3. Allow users to select specific files and initiate a partial HTTP byte-range download of only the selected entries.
- **Expected Benefit:** Delivers IDM-grade ZIP preview capabilities.
- **Complexity:** Medium

#### P1-4: UI String Externalization & Localization Framework
- **Target Component:** `src/ui/win32_main_window.cpp`, `src/app/radium.rc`
- **Objective:** Enable multi-language translation across all UI elements.
- **Implementation Strategy:**
  1. Extract hardcoded string literals into a structured JSON translation catalog or Windows `.rc` `STRINGTABLE`.
  2. Implement a string lookup helper `Loc(StringId)` that falls back to English if a translation key is missing.
  3. Detect OS language via `GetUserDefaultUILanguage()`.
- **Expected Benefit:** Unlocks non-English global software markets.
- **Complexity:** Large

---

### Priority 2: Competitive Parity (V1.1 Milestones)

#### P2-1: Graphical Torrent Creation Wizard (`.torrent` Generator)
- **Target Component:** `src/engine_torrent/torrent_creator.cpp` (New)
- **Objective:** Allow users to create `.torrent` files and magnet links from local files or directories.
- **Implementation Strategy:**
  1. Implement a `TorrentCreator` class that scans target directories, calculates total size, selects piece size (256 KB to 4 MB), and hashes chunks using SHA-1.
  2. Encode metadata dictionary using existing `bencode.hpp` structures.
  3. Provide an intuitive "Create Torrent" modal dialog in the UI.
- **Expected Benefit:** Full BitTorrent authoring parity with qBittorrent and FDM.
- **Complexity:** Medium

#### P2-2: BitTorrent Swarm Inspector (Peers & Trackers Tabs)
- **Target Component:** `src/engine_torrent/torrent_engine.cpp`, `src/ui/win32_main_window.cpp`
- **Objective:** Expose detailed swarm metrics in task properties.
- **Implementation Strategy:**
  1. Expose `GetTorrentSwarmDetails(task_id)` returning active peer endpoints, client strings, and tracker statuses.
  2. Add "Trackers" and "Peers" tabs to the task properties dialog, enabling tracker URL addition/deletion.
- **Expected Benefit:** Transparency for power users diagnosing slow torrents or dead swarms.
- **Complexity:** Medium

#### P2-3: SOCKS5 & PAC Script Proxy Support
- **Target Component:** `src/brisk_engine/http_client.cpp:798-875`
- **Objective:** Enable routing downloads through SOCKS5, Tor, and corporate PAC proxies.
- **Implementation Strategy:**
  1. Extend `HttpOption` with SOCKS5 handshake negotiation over native Winsock sockets for non-WinHTTP connections.
  2. Configure `WINHTTP_OPTION_PROXY` with SOCKS protocols on Windows 10/11.
- **Expected Benefit:** Unlocks restricted enterprise environments and privacy-conscious users routing via Tor.
- **Complexity:** Medium

#### P2-4: Interactive WinRT Toast Notifications
- **Target Component:** `src/ui/win32_main_window.cpp:3880-3940`
- **Objective:** Replace legacy tray balloons with rich Action Center toasts.
- **Implementation Strategy:**
  1. Implement `ToastNotificationManager` via COM / WinRT XML templates.
  2. Include action buttons: "Open File", "Open Folder", and "Delete".
- **Expected Benefit:** Modern Windows 11 notification experience.
- **Complexity:** Medium

---

### Priority 3: Advanced Refinements (Long-Term Roadmap)

#### P3-1: FTPS (FTP over TLS) Multi-Stream Acceleration
- **Target Component:** `src/engine_ftp/ftp_client.cpp`
- **Objective:** Support authenticated, encrypted FTP transfers (`ftps://`) with segmented byte ranges.
- **Complexity:** Large

#### P3-2: Direct2D 1.1 Vector Rendering Pipeline
- **Target Component:** `src/ui/win32_main_window.cpp`
- **Objective:** Hardware-accelerated Direct2D 1.1 rendering backend with smooth fluid animations and sub-pixel glyph positioning.
- **Complexity:** Large

#### P3-3: Batch Regex URL Pattern Generator
- **Target Component:** `src/ui/win32_main_window.cpp`
- **Objective:** Batch download URL sequencing dialog (e.g., `https://site.com/file[01-99].zip`).
- **Complexity:** Small
