# Radium Download Manager: Complete Gap Audit Findings

**Date:** November 7, 2026  
**Status:** Verified by Static Source Code Audit and Platform Test Execution  

---

## 1. Overview & Issue Taxonomy

This document compiles all verified technical, architectural, usability, and security findings identified during the product audit of Radium Download Manager.

Each finding is classified by:
- **Severity:** Critical, High, Medium, or Low
- **Category:** UI/UX, Download Engine, BitTorrent, Extension, Security, Windows Integration, Packaging, or Documentation
- **Location:** Source file and line numbers
- **Estimated Complexity:** Small (< 1 day), Medium (2–4 days), or Large (> 1 week)

---

## 2. Table of Findings

| ID | Category | Title | Severity | Location | Complexity |
| :--- | :--- | :--- | :---: | :--- | :---: |
| **AUD-01** | UI / a11y | Main window download list completely lacks MSAA / UI Automation (`WM_GETOBJECT`) | **Critical** | `src/ui/win32_main_window.cpp:10126` | Large |
| **AUD-02** | UI / Perf | GDI fonts created and destroyed on every single frame in `PaintMainWindow` | **High** | `src/ui/win32_main_window.cpp:7953-7968` | Small |
| **AUD-03** | Torrent | BitTorrent piece picker uses sequential selection instead of Rarest-First | **High** | `src/engine_torrent/piece_manager.hpp:159-189` | Medium |
| **AUD-04** | Torrent | BitTorrent engine lacks Endgame Mode, causing transfers to stall at 99% | **High** | `src/engine_torrent/piece_manager.hpp:145-156` | Medium |
| **AUD-05** | Torrent | Unconditional unchoking of all connected peers (no Tit-for-Tat anti-leech) | **High** | `src/engine_torrent/torrent_engine.cpp:544, 655` | Medium |
| **AUD-06** | Crawler | Crawler injects HTTP `Range` header into HTML web page requests, causing HTTP 416 | **High** | `src/crawler/site_grabber.cpp:218-223` | Small |
| **AUD-07** | Archive | ZIP Preview fails or corrupts on archives >4 GB due to missing ZIP64 support | **High** | `src/archive/zip_preview.cpp:17-47, 105-128` | Medium |
| **AUD-08** | Windows CLI | `main.cpp` ignores positional arguments, breaking Windows `.torrent` double-click | **High** | `src/app/main.cpp:91-128` | Small |
| **AUD-09** | Extension | Firefox MV3 extension missing `catch.js` page injection | **High** | `extension/manifest.firefox.json:49-60` | Medium |
| **AUD-10** | Security | Command line argument construction in worker lacks quote/backslash escaping | **High** | `src/worker_stream/nm3u8dl_re_worker.cpp:45-55` | Medium |
| **AUD-11** | Security | Unsanitized replacement template in custom antivirus scanner execution | **High** | `src/security/antivirus_scanner.cpp:69` | Small |
| **AUD-12** | Engine | Worker 0 terminates permanently after first segment, leaking one concurrency slot | **Medium** | `src/brisk_engine/http_download_engine.cpp:1152` | Small |
| **AUD-13** | Engine | Missing MD5 hash verification despite requirements and documentation | **Medium** | `src/common/crypto_utils.hpp:16` | Small |
| **AUD-14** | FTP | FTP client is strictly single-stream and rejects FTPS | **Medium** | `src/engine_ftp/ftp_client.cpp:215, 337` | Large |
| **AUD-15** | UI / UX | Speed Limiter and Scheduler use crude single-line text prompt dialogs | **Medium** | `src/ui/win32_main_window.cpp:9472, 9493` | Medium |
| **AUD-16** | UI / UX | ZIP Preview uses plain Win32 `MessageBoxW` instead of structured tree dialog | **Medium** | `src/ui/win32_main_window.cpp:9573` | Medium |
| **AUD-17** | Settings | Missing Settings Export/Import facility in Settings Center | **Medium** | `src/storage/import_export.cpp:1` | Medium |
| **AUD-18** | Security | Database stores custom HLS decryption keys in plaintext in SQLite | **Medium** | `src/storage/sqlite_store.cpp:94` | Medium |
| **AUD-19** | Windows | Lacks Win32 SEH unhandled exception filter and minidump crash handler | **Medium** | `src/app/main.cpp:1` | Small |
| **AUD-20** | Packaging | No automatic background update patching or official WinGet package | **Medium** | `src/ui/win32_main_window.cpp:3462` | Medium |
| **AUD-21** | UI / a11y | Keyboard navigation lacks `Tab`/`Shift+Tab` focus rings and `VK_APPS` context menu | **Medium** | `src/ui/win32_main_window.cpp:9971-10009` | Medium |
| **AUD-22** | UI / i18n | Over 320 wide string literals hardcoded in UI with no localization table | **Medium** | `src/ui/win32_main_window.cpp` | Large |
| **AUD-23** | Branding | Extensive legacy project names ("Flux" and "OpenIDM") in docs and headers | **Medium** | `docs/`, `src/storage/import_export.cpp` | Small |
| **AUD-24** | Extension | Service worker dormancy can cause race condition on cold webRequest | **Medium** | `extension/js/background.js:115-302` | Small |
| **AUD-25** | UI / Design | Low contrast ratio on muted text tokens in extension (`#64748B` on `#242528`) | **Low** | `extension/css/popup.css:21` | Small |

---

## 3. Detailed Finding Profiles

### AUD-01: Main Window Download List Completely Lacks MSAA / UI Automation (`WM_GETOBJECT`)
- **Severity:** Critical
- **Category:** Accessibility (a11y) & Windows Integration
- **Source Location:** `src/ui/win32_main_window.cpp:10126` (missing `WM_GETOBJECT` message handler)
- **Evidence:** The main window class `Radium_MainWindowClass` renders tasks entirely via custom GDI calls inside `PaintMainWindow`. When Windows Narrator or NVDA inspects the window, no `WM_GETOBJECT` message is processed. Screen readers receive no accessible elements, no rows, no progress values, and no status information.
- **Why this is a problem:** Makes the application completely unusable for visually impaired users and violates standard Windows desktop accessibility requirements.
- **Recommended Solution:** Implement `WM_GETOBJECT` handling for `OBJID_CLIENT`. Provide an `IAccessible` or UI Automation (`IRawElementProviderSimple`) provider that exposes the download list rows as accessible list items with name, value (progress %), state, and role.
- **Complexity:** Large

---

### AUD-02: GDI Fonts Created and Destroyed on Every Single Frame in `PaintMainWindow`
- **Severity:** High
- **Category:** Performance & UI Responsiveness
- **Source Location:** `src/ui/win32_main_window.cpp:7953–7968`, `8631–8635`
- **Evidence:**
  ```cpp
  HFONT hFontBody = CreateFontW(-ScaleForDpi(13, dpi), 0, 0, 0, FW_NORMAL, ...);
  HFONT hFontBold = CreateFontW(-ScaleForDpi(13, dpi), 0, 0, 0, FW_SEMIBOLD, ...);
  HFONT hFontSmall = CreateFontW(-ScaleForDpi(11, dpi), 0, 0, 0, FW_NORMAL, ...);
  HFONT hFontSmallBold = CreateFontW(-ScaleForDpi(11, dpi), 0, 0, 0, FW_SEMIBOLD, ...);
  // ... rendering ...
  DeleteObject(hFontBody); DeleteObject(hFontBold); ...
  ```
- **Why this is a problem:** During high-speed downloads (100–250ms UI timer invalidations) or mouse hovers, thousands of font objects are allocated from the Windows GDI heap and freed per second, generating avoidable CPU churn and micro-stuttering.
- **Recommended Solution:** Cache `HFONT`, `HBRUSH`, and `HPEN` objects in the window context. Recreate them only when receiving `WM_DPICHANGED` or theme change notifications.
- **Complexity:** Small

---

### AUD-03: BitTorrent Piece Picker Uses Sequential Selection Instead of Rarest-First
- **Severity:** High
- **Category:** BitTorrent Engine
- **Source Location:** `src/engine_torrent/piece_manager.hpp:159–189`
- **Evidence:**
  ```cpp
  for (size_t p : m_required_pieces) {
      if (m_pieces[p].is_complete || !peer_bitfield.GetBit(p)) continue;
      // Requests blocks from piece p sequentially
  }
  ```
- **Why this is a problem:** Real-world BitTorrent swarms rely on **Rarest-First** piece picking. Sequential piece selection causes all peers to download the exact same pieces simultaneously, starving the swarm of rare pieces and reducing overall download throughput.
- **Recommended Solution:** Maintain a frequency histogram of pieces across connected peer bitfields (`m_piece_peer_count`). In `GetNextRequestsForPeer`, sort required pieces ascending by peer availability before selecting blocks.
- **Complexity:** Medium

---

### AUD-04: BitTorrent Engine Lacks Endgame Mode, Causing Transfers to Stall at 99%
- **Severity:** High
- **Category:** BitTorrent Engine
- **Source Location:** `src/engine_torrent/piece_manager.hpp:145–156`
- **Evidence:** Blocks are flagged `inc.blocks_requested[b] = true` on the first request and never re-requested from another peer while pending.
- **Why this is a problem:** When only a few blocks remain at the end of a download, if the peer assigned to those blocks has high latency, chokes, or drops connection, the download stalls indefinitely until a 4000 ms socket timeout expires.
- **Recommended Solution:** Implement an Endgame threshold (e.g., fewer than 20 blocks remaining). In Endgame mode, request all unreceived blocks from all capable peers simultaneously, broadcasting `kCancel` messages as soon as a block arrives.
- **Complexity:** Medium

---

### AUD-05: Unconditional Unchoking of All Connected Peers (No Tit-for-Tat Anti-Leech)
- **Severity:** High
- **Category:** BitTorrent Engine
- **Source Location:** `src/engine_torrent/torrent_engine.cpp:544, 655`
- **Evidence:**
  ```cpp
  std::string unchoke_msg = PeerWireProtocol::CreateUnchoke();
  send(peer.socket, unchoke_msg.data(), ...);
  ```
- **Why this is a problem:** Radium immediately unchokes every connected peer. It has no 10-second choke evaluation loop, no 20-second rolling download speed calculation, and no optimistic unchoke rotation. Leechers can consume upload bandwidth without contributing back to Radium.
- **Recommended Solution:** Implement standard BEP 3 choking: rank peers by download rate, unchoke the top 4 fastest uploaders, choke the rest, and rotate 1 optimistic unchoke every 30 seconds.
- **Complexity:** Medium

---

### AUD-06: Crawler Injects HTTP `Range` Header into HTML Web Page Requests
- **Severity:** High
- **Category:** Crawler / Site Grabber
- **Source Location:** `src/crawler/site_grabber.cpp:218–223`
- **Evidence:**
  ```cpp
  auto res = brisk::HttpClient::PerformGet(
      current.url, headers, 0, 4194304, // Requests byte range 0-4MB on HTML
  ```
- **Why this is a problem:** Dynamic HTML endpoints (PHP, Node.js, Python) and CDNs (Cloudflare) frequently reject `Range` headers on non-static content, responding with `HTTP 416 Range Not Satisfiable`. This causes the crawler to abort crawling legitimate sites.
- **Recommended Solution:** Issue standard non-ranged `GET` requests for HTML pages, and cap response buffer size in the streaming callback once 4 MB is reached.
- **Complexity:** Small

---

### AUD-07: ZIP Preview Fails or Corrupts on Archives >4 GB Due to Missing ZIP64 Support
- **Severity:** High
- **Category:** Archive Inspection
- **Source Location:** `src/archive/zip_preview.cpp:17–47, 105–128`
- **Evidence:** `ZipEOCD` and `ZipCDHeader` use standard 32-bit unsigned integer offsets and sizes. If an archive exceeds 4 GB or 65,535 entries, standard ZIP specification sets `0xFFFF` / `0xFFFFFFFF` and writes a Zip64 EOCD Locator (`0x07064b50`). Radium ignores this locator, treats the 0xFFFFFFFF values as invalid, and returns `ERR_ZIP_CORRUPT`.
- **Why this is a problem:** Users downloading large archives (ISO images, large game mods, data sets) cannot preview them.
- **Recommended Solution:** Search backwards for the Zip64 EOCD Locator signature (`0x07064b50`) before reading standard EOCD. If present, parse the 64-bit Zip64 EOCD Record (`0x06064b50`) for offsets and counts.
- **Complexity:** Medium

---

### AUD-08: `main.cpp` Ignores Positional Arguments, Breaking Windows `.torrent` Double-Click
- **Severity:** High
- **Category:** Windows Shell Integration
- **Source Location:** `src/app/main.cpp:91–128`
- **Evidence:** `main.cpp` parses arguments using an explicit flag loop:
  ```cpp
  if (arg == L"--download" && i + 1 < argc) { ... }
  else if (arg == L"--stream" && i + 1 < argc) { ... }
  ```
- **Why this is a problem:** When Windows opens a file associated with an application (or via "Open with..."), it passes the path as `radium.exe "C:\Path\To\File.torrent"` without any flag. Radium ignores this argument and displays an empty window.
- **Recommended Solution:** If `argv[i]` does not start with `--` and represents an existing file path or a recognized URL (`http://`, `https://`, `magnet:`), treat it as an implicit download/torrent target.
- **Complexity:** Small

---

### AUD-09: Firefox MV3 Extension Missing `catch.js` Page Injection
- **Severity:** High
- **Category:** Browser Extension
- **Source Location:** `extension/manifest.firefox.json:49–60`
- **Evidence:** `manifest.chromium.json` declares:
  ```json
  "content_scripts": [{ "matches": ["<all_urls>"], "js": ["catch-script/catch.js"], "world": "MAIN", "run_at": "document_start" }]
  ```
  `manifest.firefox.json` lacks this entry because `"world": "MAIN"` is not supported across older Firefox versions.
- **Why this is a problem:** `catch-script/catch.js` hooks `window.fetch`, `MediaSource`, and `ytInitialPlayerResponse` in the webpage's JavaScript context. On Firefox, these hooks never execute, drastically reducing video and stream detection rates.
- **Recommended Solution:** In Firefox content scripts, inject `catch.js` dynamically via a DOM `<script>` element or use `browser.scripting.executeScript({ target: { tabId }, world: "MAIN" })`.
- **Complexity:** Medium

---

### AUD-10: Command Line Argument Construction in Worker Lacks Quote/Backslash Escaping
- **Severity:** High
- **Category:** Security & Command Execution
- **Source Location:** `src/worker_stream/nm3u8dl_re_worker.cpp:45–55`
- **Evidence:** The worker converts UTF-8 string arguments into a single command line without verifying trailing backslashes or internal quotation marks:
  ```cpp
  cmd_stream << L"\"" << wide_arg << L"\" ";
  ```
- **Why this is a problem:** On Windows, passing an argument ending in a backslash (e.g. `C:\Downloads\`) followed by a closing quote (`"`) results in `\"`, which Win32 parses as an escaped quote rather than an argument boundary, causing command-line parameter corruption.
- **Recommended Solution:** Use a dedicated Win32 command line escaping helper (`ArgvToCommandLine`) that doubles trailing backslashes before quotes.
- **Complexity:** Medium

---

### AUD-11: Unsanitized Replacement Template in Custom Antivirus Scanner Execution
- **Severity:** High
- **Category:** Security
- **Source Location:** `src/security/antivirus_scanner.cpp:69`
- **Evidence:** The custom antivirus execution replaces `%1` with the file path enclosed in quotes, but does not escape special shell characters in `args_template`.
- **Why this is a problem:** If a malicious configuration is loaded or injected into settings, command parameters could be manipulated.
- **Recommended Solution:** Restrict antivirus invocation to `CreateProcessW` with strongly typed arguments without passing through shell interpreters.
- **Complexity:** Small

---

### AUD-12: Worker 0 Terminates Permanently After First Segment, Leaking One Concurrency Slot
- **Severity:** Medium
- **Category:** Download Engine
- **Source Location:** `src/brisk_engine/http_download_engine.cpp:1152–1172`
- **Evidence:** Worker 0 streams the initial segment directly on the probe socket. Once segment 0 completes, Worker 0 exits permanently. Secondary workers are spawned, but the initial thread is retired rather than reused for work-stealing.
- **Why this is a problem:** Results in avoidable thread churn and temporarily drops active concurrency.
- **Recommended Solution:** Have Worker 0 enter the work-stealing loop after its initial segment finishes.
- **Complexity:** Small

---

### AUD-13: Missing MD5 Hash Verification
- **Severity:** Medium
- **Category:** Download Engine & Integrity
- **Source Location:** `src/common/crypto_utils.hpp:16`, `src/common/types.hpp:220`
- **Evidence:** `TaskModel` only defines `expected_sha256`. MD5 hash calculation is completely absent from `CryptoUtils`.
- **Why this is a problem:** Many legacy mirrors and file distributors provide MD5 checksums rather than SHA-256.
- **Recommended Solution:** Implement `CryptoUtils::ComputeFileMd5Hex` via Windows BCrypt (`BCRYPT_MD5_ALGORITHM`) and add `expected_md5` to `TaskModel`.
- **Complexity:** Small

---

### AUD-14: FTP Client is Strictly Single-Stream and Rejects FTPS
- **Severity:** Medium
- **Category:** FTP Engine
- **Source Location:** `src/engine_ftp/ftp_client.cpp:215, 337`
- **Evidence:**
  ```cpp
  if (url.starts_with("ftps://")) return Error(ERR_FTPS_UNSUPPORTED);
  ```
  `FtpClient::DownloadFile` downloads using a single TCP stream.
- **Why this is a problem:** Modern broadband connections cannot saturate bandwidth on high-latency FTP mirrors without multi-connection segmented downloading via `REST`.
- **Recommended Solution:** Integrate `SegmentScheduler` with multiple FTP data connections, and support explicit TLS (`AUTH TLS`).
- **Complexity:** Large

---

### AUD-15: Speed Limiter and Scheduler Use Crude Single-Line Text Prompt Dialogs
- **Severity:** Medium
- **Category:** UI / UX Design
- **Source Location:** `src/ui/win32_main_window.cpp:9472, 9493`
- **Evidence:** `ShowSimpleInputDialogWin32` prompts the user with a single raw text edit asking for manual strings (e.g., `"Enter maximum speed in KB/s"` or `"24-hour HH:MM format"`).
- **Why this is a problem:** Unprofessional UX compared to IDM and FDM. No input validation, no sliders, no presets, and no visual calendar/time pickers.
- **Recommended Solution:** Replace with dedicated dark-themed dialogs containing numeric sliders, unit dropdowns (KB/s vs MB/s), and standard `SysDateTimePick32` time pickers.
- **Complexity:** Medium

---

### AUD-16: ZIP Preview Uses Plain Win32 `MessageBoxW` Instead of Structured Tree Dialog
- **Severity:** Medium
- **Category:** UI / UX Design
- **Source Location:** `src/ui/win32_main_window.cpp:9573`
- **Evidence:** Formats archive file names into a single newline-delimited string and passes it to `MessageBoxW(hwnd, msg.c_str(), L"ZIP Preview", MB_OK)`.
- **Why this is a problem:** If an archive has 100+ files, `MessageBoxW` overflows the monitor. Text cannot be copied, directory trees cannot be browsed, and individual files cannot be selectively extracted.
- **Recommended Solution:** Implement a dedicated dialog with a `SysTreeView32` or custom list view showing file paths, compressed/uncompressed sizes, and an "Extract Selected" button.
- **Complexity:** Medium

---

### AUD-17: Missing Settings Export/Import Facility in Settings Center
- **Severity:** Medium
- **Category:** Settings & Customization
- **Source Location:** `src/storage/import_export.cpp:1`, `src/ui/win32_main_window.cpp`
- **Evidence:** `ImportExport` provides `ExportTasksToJson` and `ImportTasksFromJson`, but there are no functions to serialize or restore `app_settings` JSON.
- **Why this is a problem:** Users cannot migrate their custom connection counts, proxy configurations, or scheduler preferences across PCs or Windows reinstallations.
- **Recommended Solution:** Add `ExportSettingsToFile` and `ImportSettingsFromFile` to `DownloadController` and expose them in the Settings Center UI.
- **Complexity:** Medium

---

### AUD-18: Database Stores Custom HLS Decryption Keys in Plaintext in SQLite
- **Severity:** Medium
- **Category:** Security & Privacy
- **Source Location:** `src/storage/sqlite_store.cpp:94`
- **Evidence:** The `stream_options_json` column stores `custom_hls_key_hex` unencrypted.
- **Why this is a problem:** If a user downloads private or licensed video streams, keys stored on disk in plaintext can be read by any local process.
- **Recommended Solution:** Encrypt sensitive stream credentials using Windows DPAPI (`CryptProtectData`) before persisting to SQLite.
- **Complexity:** Medium

---

### AUD-19: Lacks Win32 SEH Unhandled Exception Filter and Minidump Crash Handler
- **Severity:** Medium
- **Category:** Stability & Observability
- **Source Location:** `src/app/main.cpp:1`
- **Evidence:** Radium has an in-memory logger ring-buffer, but no `SetUnhandledExceptionFilter`.
- **Why this is a problem:** If a native crash occurs in WinHTTP, Direct2D/GDI, or SQLite, the application terminates silently with zero diagnostics or `.dmp` crash files.
- **Recommended Solution:** Install a structured exception handler writing a mini-dump (`MiniDumpWriteDump`) to `%LOCALAPPDATA%\Radium\crashes\`.
- **Complexity:** Small

---

### AUD-20: No Automatic Background Update Patching or Official WinGet Package
- **Severity:** Medium
- **Category:** Installation & Updates
- **Source Location:** `src/ui/win32_main_window.cpp:3462`
- **Evidence:** As recorded in `win32_main_window.cpp:3462`: *"Automatic installation is disabled: this repository has no published Ed25519-signed update manifest... You can open the official release page and install manually."*
- **Why this is a problem:** Users must manually check GitHub releases and run installers.
- **Recommended Solution:** Automate publishing to `microsoft/winget-pkgs` in CI, and implement an optional WinSparkle update helper.
- **Complexity:** Medium

---

### AUD-21: Keyboard Navigation Lacks `Tab`/`Shift+Tab` Focus Rings and `VK_APPS` Context Menu
- **Severity:** Medium
- **Category:** UI / UX
- **Source Location:** `src/ui/win32_main_window.cpp:9971–10009`
- **Evidence:** `WM_KEYDOWN` handles list navigation (up/down/home/end), but does not support `Tab` cycling between sidebar, search box, and table. Context menu cannot be invoked via the menu key (`VK_APPS`) or `Shift+F10`.
- **Why this is a problem:** Hampers keyboard-only power users and accessibility compliance.
- **Recommended Solution:** Handle `WM_CONTEXTMENU` and implement `Tab` traversal across child window controls.
- **Complexity:** Medium

---

### AUD-22: Over 320 Wide String Literals Hardcoded in UI with No Localization Table
- **Severity:** Medium
- **Category:** Commercial Readiness & Localization
- **Source Location:** `src/ui/win32_main_window.cpp`
- **Evidence:** Hundreds of user-facing strings (`L"Download Complete"`, `L"Resume"`, `L"Pause"`, `L"Settings"`) are hardcoded directly in C++ source code.
- **Why this is a problem:** Prevents localization into non-English languages (German, Chinese, Spanish, Japanese, etc.), limiting global commercial appeal.
- **Recommended Solution:** Extract all user-facing strings into a resource StringTable (`radium.rc`) or a lightweight JSON localization catalog (`i18n.hpp`).
- **Complexity:** Large

---

### AUD-23: Extensive Legacy Project Names ("Flux" and "OpenIDM") in Docs and Headers
- **Severity:** Medium
- **Category:** Branding & Documentation
- **Source Location:** `docs/UI_DESIGN_SYSTEM.md:5`, `docs/FEATURE_MATRIX.md:58`, `src/storage/import_export.cpp:1`
- **Evidence:** Over 40 active references to "Flux" and "OpenIDM" remain in active documentation and source code banners.
- **Why this is a problem:** Confuses users and developers regarding the canonical product identity.
- **Recommended Solution:** Execute a clean documentation refactor standardizing all documentation titles, headers, and descriptions to "Radium".
- **Complexity:** Small

---

### AUD-24: Service Worker Dormancy Can Cause Race Condition on Cold WebRequest
- **Severity:** Medium
- **Category:** Browser Extension
- **Source Location:** `extension/js/background.js:115–302`
- **Evidence:** While state is saved to `chrome.storage.session`, incoming `webRequest.onHeadersReceived` and `runtime.onMessage` listeners do not explicitly await `ensureSessionRestored()` on cold worker start.
- **Why this is a problem:** Rapid subresource downloads occurring immediately as the worker wakes up can evaluate against empty state dictionaries before restore settles.
- **Recommended Solution:** Enqueue incoming events or await `sessionRestorePromise` at the top of listeners.
- **Complexity:** Small

---

### AUD-25: Low Contrast Ratio on Muted Text Tokens in Extension
- **Severity:** Low
- **Category:** Visual Design
- **Source Location:** `extension/css/popup.css:21`
- **Evidence:** `--text-muted: #64748B` on background `--bg-main: #242528` has a contrast ratio of ~3.1:1.
- **Why this is a problem:** Fails WCAG 2.1 AA 4.5:1 minimum contrast requirement for secondary text.
- **Recommended Solution:** Lighten `--text-muted` to `#94A3B8` (4.6:1 contrast ratio).
- **Complexity:** Small
