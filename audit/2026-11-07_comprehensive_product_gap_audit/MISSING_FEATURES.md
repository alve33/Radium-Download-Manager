# Radium Download Manager — Missing & Incomplete Features Audit

**Date:** November 7, 2026  
**Auditor:** Senior Product Management & Engineering Audit Team  
**Evaluation Target:** Radium Download Manager (Current Repository State)

---

## 1. Feature Completion Analysis Framework

Features in this audit are evaluated and categorized into four distinct implementation states:
- **Fully Implemented:** Production-ready, verified by automated unit/integration tests and static inspection.
- **Partially Implemented:** Core engine logic exists, but user interface controls, configuration options, or edge-case handling are missing or simulated.
- **Simulated / Stubbed:** Interface exists in UI or documentation, but underlying functionality is hardcoded, mock-tested, or non-functional.
- **Missing:** Essential or expected feature completely absent from the codebase.

---

## 2. Incomplete & Missing Features Inventory

| Feature | Category | Implementation State | Gap Description & Commercial Expectation |
| :--- | :--- | :---: | :--- |
| **Windows UI Automation / MSAA** | Accessibility | **Missing** | Zero accessibility interfaces exposed. Screen readers cannot inspect or interact with the app. |
| **In-App Auto Updater** | Maintenance | **Missing** | Zero automatic update checks. No silent background update or GitHub release prompt. |
| **Torrent Creator** | BitTorrent | **Missing** | Cannot generate `.torrent` files or create custom magnet links from local files. |
| **Torrent Swarm Inspector** | BitTorrent | **Partially Implemented** | Peer engine tracks connections, but UI lacks a "Peers" list and "Trackers" list tab in task properties. |
| **Sequential Torrent Download** | BitTorrent | **Missing** | Cannot prioritize head/tail pieces for video previewing during BitTorrent downloads. |
| **Rich ZIP Archive TreeView** | File Inspection | **Partially Implemented** | Remote archive central directory parsed, but displayed in raw text instead of an interactive file tree. |
| **SOCKS5 / PAC Proxies** | Networking | **Missing** | HTTP/HTTPS proxies supported; SOCKS4/5, Tor routing, and PAC scripts unsupported. |
| **Internationalization (i18n)** | Localization | **Missing** | Hardcoded English UI literals. No Windows Resource Table (`.rc`) or translation catalog. |
| **Portable Mode Flag** | Portability | **Missing** | No automatic redirection to local folders upon detecting `radium.portable`. |
| **Interactive WinRT Toasts** | Windows Integration| **Partially Implemented** | Basic Shell tray balloon tips supported; modern Action Center toasts with action buttons missing. |
| **Column Customization** | UI Polish | **Missing** | Cannot hide/show columns or drag-and-drop to reorder columns in the main download list. |
| **Batch Page Link Grabber** | Extension | **Missing** | Extension detects media and single downloads; lacks context menu "Download All Links on Page". |
| **Visual Speed Limiter Slider** | UI Polish | **Partially Implemented** | Rate limiter engine works; UI relies on a raw text input box rather than sliders/presets. |
| **Visual Queue Scheduler** | UI Polish | **Partially Implemented** | Scheduler engine enforces active/pause windows; UI relies on a text dialog instead of a calendar picker. |
| **Settings Export / Import** | Data Management | **Missing** | Cannot backup or restore configuration settings to a portable JSON file. |

---

## 3. High-Impact Feature Profiles

### 3.1 Torrent Swarm Management & Tracker Editing
- **Current State:** `TorrentEngine` connects to swarms, announces to UDP/HTTP trackers, requests peers from DHT, and exchanges pieces. All swarm coordination is internal.
- **What Is Missing:**
  When right-clicking an active torrent task and selecting "Properties", users expect:
  1. A **Trackers Tab**: Listing all announce URLs, their connection status (`Working`, `Connection Timed Out`, `Updating`), interval, and seeder/leecher counts. Users should be able to click "Add Trackers" to paste public tracker lists.
  2. A **Peers Tab**: Listing connected IP addresses, peer client identifiers (e.g., `qBittorrent 4.6.3`, `Transmission 4.0`), download speed, upload speed, and completed piece percentage.
  3. A **Files Tab**: Allowing users to uncheck individual files or change priority (`Maximum`, `High`, `Normal`, `Do Not Download`) while the torrent is already in progress.

### 3.2 In-App Cryptographic Auto-Updater
- **Current State:** Manual release download via web browser.
- **What Is Missing:**
  Commercial software on Windows relies on background update delivery:
  1. Periodic background query (e.g., every 24 hours or on manual "Help -> Check for Updates" click) to `https://api.github.com/repos/.../releases/latest`.
  2. Cryptographic signature verification of the downloaded update payload against an embedded public key (preventing MITM / DNS spoofing attacks).
  3. Interactive update notification ("Radium v1.1.0 is available. What's new: ... [Update Now] [Later]").
  4. One-click silent execution of Inno Setup with `/VERYSILENT /NORESTART` and restart of `radium.exe`.

### 3.3 Interactive ZIP Archive Tree & Selective File Download
- **Current State:** WinHTTP byte-range requests fetch the ZIP end of central directory record and central directory headers, and output a formatted ASCII text report in a multi-line edit box.
- **What Is Missing:**
  1. Parsing the paths into a hierarchical tree structure (`folder -> subfolder -> file`).
  2. Rendering inside a standard Win32 TreeView control with folder icons.
  3. Allowing the user to select specific files (e.g., check `manual.pdf` inside a 5 GB `.zip` file) and clicking "Download Selected".
  4. The engine downloads only the specific byte offsets corresponding to the selected file entries rather than the full archive.
