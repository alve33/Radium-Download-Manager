# Repository Map & Architectural Inventory

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## 1. High-Level System Architecture & Process Boundaries

Radium Download Manager is structured around four runtime processes communicating across two IPC boundaries:

```
+-----------------------------------------------------------------------------------+
| Browser Process (Chromium / Edge / Brave / Firefox)                               |
|  - Content Scripts: extension/catch-script/catch.js (MAIN world)                  |
|                     extension/js/content-script.js (ISOLATED world)               |
|  - Background SW:   extension/js/background.js + init.js + function.js            |
+-----------------------------------------+-----------------------------------------+
                                          |
                                          | Boundary 1: Chrome Native Messaging
                                          | (Length-prefixed 32-bit LE UTF-8 JSON over stdio)
                                          v
+-----------------------------------------------------------------------------------+
| Native Messaging Host: radium-nmh.exe (src/nmh/main.cpp)                          |
|  - Stateless stdio <-> Named Pipe bridge                                          |
|  - Auto-launches radium.exe --background if pipe is unavailable                   |
+-----------------------------------------+-----------------------------------------+
                                          |
                                          | Boundary 2: Windows Named Pipe IPC
                                          | (\\.\pipe\Radium_IPC_<UserSID>, SDDL ACL: SY, BA, OW)
                                          | Line-delimited JSON-RPC 2.0 (v1)
                                          v
+-----------------------------------------------------------------------------------+
| Desktop Application: radium.exe (src/app/main.cpp)                                |
|  +-----------------------+  +--------------------------+  +--------------------+  |
|  | Win32 / Direct2D GUI  |  | IpcServer (Named Pipe)   |  | SqliteStore (WAL)  |  |
|  | (src/ui/*)            |  | (src/ipc/ipc_server.cpp) |  | + DPAPI Encryption |  |
|  +-----------+-----------+  +------------+-------------+  +---------+----------+  |
|              |                           |                          |             |
|              +-------------+-------------+--------------------------+             |
|                            v                                                      |
|  +-----------------------------------------------------------------------------+  |
|  | DownloadController (src/core/download_controller.cpp)                       |  |
|  |  - Task lifecycle state machine, ScheduleLoop, speed limiter, queues        |  |
|  +------+------------------+------------------+-----------------+--------------+  |
|         |                  |                  |                 |                 |
|         v                  v                  v                 v                 |
|  +--------------+  +---------------+  +---------------+  +-------------+          |
|  | HttpDownload |  | Nm3u8dlRe     |  | TorrentEngine |  | FtpClient / |          |
|  | Engine       |  | Adapter       |  | (BEP 3/9/10/  |  | RtspMms /   |          |
|  | (WinHTTP)    |  | (Stream)      |  |  15/23 + DHT) |  | SiteGrabber |          |
|  +--------------+  +-------+-------+  +---------------+  +------+------+          |
+----------------------------|------------------------------------|-----------------+
                             |                                    |
                             | Boundary 3: Windows Job Object     |
                             | (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|
                             |  + @job.rsp response file + pipe)  |
                             v                                    v
+-----------------------------------------------------------------------------------+
| Out-of-Process Stream & Muxing Workers                                            |
|  - N_m3u8DL-RE.exe (src/worker_stream/nm3u8dl_re_worker.cpp or third_party binary)|
|  - ffmpeg.exe (third_party/ffmpeg.exe)                                            |
|  - yt-dlp.exe (optional social video extractor)                                   |
|  - MpCmdRun.exe / Windows Defender (src/security/antivirus_scanner.cpp)           |
+-----------------------------------------------------------------------------------+
```

---

## 2. Directory & Module Inventory

### 2.1 Production C++ Source Directories (`src/`)

| Directory | Files | Responsibility | Main Entry Points & Public Interfaces | Key Data Structures | Dependencies | Associated Tests | Architectural Risks |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `src/app/` | `main.cpp`, `radium.rc`, `app.manifest`, `resource.h` | Process entry point (`main`), CLI flag parsing (`--headless-smoke`, `--background`, `--bench-engine`, `--url`), single-instance mutex (`Global\Radium_SingleInstance_Mutex`), crash handler, DPI awareness, GUI vs headless bootstrap. | `main(int argc, char** argv)`, `radiumUnhandledExceptionFilter` | `ControllerConfig`, `AppSettings` | `core`, `storage`, `ipc`, `ui`, `brisk_engine`, `engine_stream`, `DbgHelp`, `WinMM` | `tests/run_all_integration_tests.py` (`--headless-smoke`) | Missing `SetDefaultDllDirectories` hardening (`SEC-012`); uncaught `std::stoul` (`BUG-048`); ignores `IpcServer::Start()` errors in GUI mode. |
| `src/common/` | `result.hpp`, `error.hpp`, `unicode.hpp`, `json_mini.hpp`, `path_sanitizer.hpp/.cpp`, `crypto_utils.hpp/.cpp`, `logger.hpp/.cpp`, `timed_lock.hpp` | Foundation utilities: `Result<T>` error monad, UTF-8/UTF-16 conversion, custom recursive-descent JSON parser (`json_mini`), path/filename sanitization, Windows DPAPI + BCrypt SHA-256/SHA-1, regex-redacting file logger, mutex timing diagnostics. | `Result<T>`, `Error`, `Utf8ToWide`, `WideToUtf8`, `json::Parse`, `PathSanitizer::SanitizeFilename`, `CryptoUtils::ProtectData`, `Logger::Instance()` | `Error`, `json::Value`, `LogEntry` | Win32 (`Crypt32`, `BCrypt`, `Kernel32`) | `TC-SEC-01`, `TC-SEC-03`, `TC-UNIT-JSON-01`, `TC-CRYPTO-01` | `json_mini::Parse` lacks recursion depth limit & corrupts UTF-16 surrogate pairs (`BUG-011`); `Logger::Log` opens/closes `radium.log` on every call (`PERF-002`). |
| `src/core/` | `task_model.hpp`, `download_controller.hpp/.cpp`, `speed_Throttler.hpp`, `duplicate_detector.hpp`, `system_integration.hpp/.cpp` | Central download orchestrator, task state machine (`kQueued` -> `kProbing` -> `kDownloading` -> `kVerifying` -> `kFinalizing` -> `kCompleted`), 20Hz `ScheduleLoop`, token-bucket bandwidth throttling, duplicate URL detection, Windows power/autostart/toast integration. | `DownloadController::AddTask`, `StartTask`, `PauseTask`, `ResumeTask`, `CancelTask`, `RemoveTask`, `RefreshTaskUrl`, `ListTasks` | `TaskModel`, `SegmentRecord`, `QueueModel`, `AppSettings`, `ActiveWorker` | `storage`, `brisk_engine`, `engine_stream`, `engine_torrent`, `engine_ftp`, `crawler`, `archive`, `security`, `common` | `radium_core_tests.exe`, `tests/test_core_operations.py` | God-class complexity (`2,390` lines); `m_mutex` -> `m_store.m_mutex` lock convoy at 20Hz (`BUG-003`); async `PauseTask` race (`BUG-001`). |
| `src/storage/` | `sqlite_store.hpp/.cpp`, `import_export.hpp/.cpp` | SQLite WAL persistence for `tasks`, `segments`, `queues`, `settings`, and `completed_history`; DPAPI encryption at rest for cookies/headers; crash recovery (`RecoverInterruptedTasks`); JSON/TXT/M3Utask import & export. | `SqliteStore::Open`, `UpsertTask`, `UpdateTaskProgressAndSegments`, `RecoverInterruptedTasks`, `ImportExport::ExportTasksToFile` | `SqliteStore`, SQLite prepared statements | `sqlite3` (`third_party/sqlite`), `common` | `TC-STORE-01..06`, `TC-CRASH-01`, `TC-IMPEXP-01..02` | `UpdateTaskProgressAndSegments` omits `part_path`/`file_name`/`etag` (`BUG-002`); `ExportTasksToString` casts 64-bit ints to `double` (`BUG-010`). |
| `src/brisk_engine/` | `http_client.hpp/.cpp`, `http_download_engine.hpp/.cpp`, `segment_scheduler.hpp/.cpp` | WinHTTP HTTP/1.1 & HTTP/2 client, SSRF DNS guard, multi-connection segmented HTTP downloader, dynamic work-stealing `SegmentScheduler`, `.part` sparse file management. | `HttpClient::Probe`, `HttpClient::StreamGet`, `HttpDownloadEngine::Execute`, `SegmentScheduler::EvaluateSplitOrSteal` | `HttpRequestOptions`, `HttpResponseMetadata`, `SegmentRecord`, `WorkerThroughputSample` | `WinHTTP`, `Ws2_32`, `core`, `common` | `TC-HTTP-01..10`, `TC-SCHED-01..10` | Truncated single-stream downloads marked `Completed` (`BUG-015`); `IsLoopbackHost` blocks `fc*`/`fd*` domains (`BUG-016`). |
| `src/engine_stream/` | `nm3u8dl_re_adapter.hpp/.cpp`, `rtsp_mms_adapter.hpp/.cpp` | Out-of-process stream orchestrator for HLS/DASH/MSS (`N_m3u8DL-RE.exe`), social video URL extraction (built-in YouTube deciphering + `yt-dlp.exe`), DRM detection (`SAMPLE-AES`, `cenc:pssh`), RTSP/MMS via `ffmpeg.exe`, Windows Job Object lifecycle. | `Nm3u8dlReAdapter::Probe`, `Nm3u8dlReAdapter::Execute`, `ValidateManifestSafety`, `RtspMmsAdapter::Execute` | `StreamProbeResult`, `VariantStreamInfo` | `core`, `common`, `brisk_engine`, Win32 Job Objects & Anonymous Pipes | `TC-STREAM-01..10`, `TC-RTSP-01` | `@job.rsp` flag injection (`SEC-002`); SSRF bypasses in `ValidateManifestSafety` (`SEC-004`); blocking pipe deadlock in `run_ytdlp_proc` (`BUG-021`). |
| `src/worker_stream/` | `nm3u8dl_re_worker.cpp` | Built-in C++ replacement executable (`N_m3u8DL-RE.exe`) for HLS/DASH manifest parsing, parallel segment downloading, AES-128-CBC decryption (`BCrypt`), and `ffmpeg.exe` muxing. | `main(int argc, char** argv)`, `ExtractManifestInfo`, `DownloadStream` | `WorkerArgs`, `MediaSegment`, `StreamTrack` | `brisk_engine`, `common`, `BCrypt` | `TC-STREAM-01..10` | Recursive `ExtractManifestInfo` passes `is_manifest_input=true`, allowing remote manifests to read local files (`SEC-006`); final live segments skip AES-128 decryption (`BUG-022`). |
| `src/engine_torrent/` | `torrent_parser.hpp/.cpp`, `torrent_engine.hpp/.cpp`, `peer_wire_protocol.hpp`, `piece_manager.hpp`, `multi_file_storage.hpp`, `tracker_client.hpp`, `dht_client.hpp`, `utp_socket.hpp`, `upnp_nat.hpp`, `web_seed.hpp`, `mse_crypto.hpp` | Full BitTorrent client implementation: `.torrent`/magnet parser, HTTP/UDP tracker client, Mainline DHT, BEP 3/9/10 peer wire protocol, piece/block assembly, multi-file disk storage, WebSeeds (BEP 19), uTP (BEP 29), UPnP NAT-PMP, MSE/PE RC4. | `TorrentParser::ParseTorrentFile`, `ParseMagnetUri`, `TorrentEngine::Execute`, `ProbeTorrent` | `TorrentInfo`, `PieceManager`, `MultiFileStorage`, `PeerWireMessage` | `Ws2_32`, `BCrypt`, `brisk_engine`, `core`, `common` | `TC-TORRENT-01..06` | `4 + payload_len` 32-bit overflow infinite loop (`BUG-026`); unverified BEP 9 metadata hash (`BUG-027`); unvalidated block offset/size (`BUG-028`); 64-thread data race on `TaskModel` (`BUG-029`). |
| `src/engine_ftp/` | `ftp_client.hpp/.cpp` | Native Winsock FTP/FTPS (explicit `AUTH TLS` via SChannel) client supporting `SIZE`, `PASV`/`EPSV`, `REST` resume, and `RETR`. | `FtpClient::Download`, `FtpClient::ProbeSize` | `FtpUrlParts`, `TlsContext` | `Ws2_32`, `Secur32` (SChannel), `core`, `common` | `TC-FTP-01` | Unbounded `ReadFtpResponse` loop (`BUG-024`); unvalidated `PASV` port (`0` or `>65535`); `.part` truncated before `RETR` check. |
| `src/crawler/` | `site_grabber.hpp/.cpp` | HTML link/media crawler (`<a href>`, `<img src>`, `<video>`, `<source>`) with depth-limited BFS traversal and extension filtering. | `SiteGrabber::Crawl`, `SiteGrabber::ResolveUrl`, `ExtractUrlsFromHtml` | `CrawledLink`, `CrawlOptions` | `brisk_engine`, `common` | `TC-CRAWL-01` | Corrupts base URLs with `/` in query string (`BUG-025`); discards pages `> 4 MiB` on `ERR_CONSUMER_ABORTED`; misses IPv6/octal SSRF (`SEC-004`). |
| `src/archive/` | `zip_preview.hpp/.cpp` | Remote and local ZIP central directory inspector (HTTP Range fetch of last 64 KB EOCD) with Zip-Bomb compression ratio detection. | `ZipPreviewer::InspectBuffer`, `InspectLocalFile` | `ZipArchivePreview`, `ZipEntryInfo` | `common` | `TC-ZIP-01` | Silently returns `Ok` on corrupt signatures; lacks ZIP64 support (`0xFFFFFFFF` sizes bypass ratio check) (`BUG-036`). |
| `src/security/` | `antivirus_scanner.hpp/.cpp` | Post-download malware scanner invoking Windows Defender (`MpCmdRun.exe`) or user-configured scanner executable inside a timeout-bounded process. | `AntivirusScanner::ScanFile` | `AntivirusConfig`, `ScanResult` | `common`, Win32 Process API | `TC-AV-01` | Filename substring override of Defender `exit_code == 2` (`SEC-003`); command-line argument injection (`SEC-011`). |
| `src/ipc/` | `ipc_server.hpp/.cpp` | Per-user ACL-protected Windows Named Pipe server (`\\.\pipe\Radium_IPC_<UserSID>`) handling JSON-RPC 2.0 requests from `radium-nmh.exe`. | `IpcServer::Start`, `Stop`, `HandleMessage`, `SetDownloadOfferCallback` | `ChunkedTransferState`, `RateBucket` | `core`, `common`, `Advapi32` | `TC-IPC-01..06` | Handle use-after-close & data races (`BUG-038`); `completeChunkedTransfer` writes final file before user prompt (`SEC-001`); weak `saveDir` validation (`SEC-005`). |
| `src/nmh/` | `main.cpp` | Native Messaging Host executable (`radium-nmh.exe`) bridging browser 32-bit length-prefixed `stdin`/`stdout` messages to the Named Pipe IPC server. | `main(int argc, char** argv)` | None (stateless bridge) | Win32 File/Pipe/Process APIs | `tests/run_all_integration_tests.py` | Naive `ExtractJsonRpcId` string search (`BUG-045`); timeout mismatch with `background.js` (`BUG-043`). |
| `src/ui/` | `win32_main_window.hpp/.cpp`, `design_system.hpp` | Native Win32 + Direct2D/DirectWrite hardware-accelerated dark-mode main window (`9,574` lines) and custom modal dialog procedures. | `Win32MainWindow::Run`, `WndProc`, `RenderD2D`, `ShowNewDownloadModal` | `Win32MainWindow`, `UiAnimationState`, `DialogThemeState` | `d2d1`, `dwrite`, `dwmapi`, `uxtheme`, `comctl32`, `shell32`, `ole32`, `core`, `ipc` | `TC-UI-PERF-01` (headless model benchmark) | Missing `\0` in `OPENFILENAMEW.lpstrFilter` (`BUG-037`); nested modal dialog reentrancy & dropped offers (`BUG-040`); synchronous UI thread hashing/moves (`BUG-041`). |

---

### 2.2 Browser Extension (`extension/`)

| File | World / Context | Responsibility | Key Risks Identified |
| :--- | :--- | :--- | :--- |
| `extension/manifest.json` | Chromium MV3 Manifest | Declares service worker (`js/background.js`), content scripts (`catch.js` in `MAIN`, `content-script.js` in `ISOLATED`), permissions (`webRequest`, `downloads`, `cookies`, `scripting`, `nativeMessaging`, `<all_urls>`). | Overly broad `web_accessible_resources` (`catch-script/catch.js`, `js/init.js`, `js/function.js` exposed to `<all_urls>`). |
| `extension/manifest.firefox.json` | Firefox MV3 Manifest | Firefox-specific manifest with `browser_specific_settings.gecko` and `background.scripts: ["js/background.js"]`. | **Critical (`BUG-039`):** Omits `js/init.js` and `js/function.js` from `background.scripts`, crashing Firefox background startup immediately. |
| `extension/js/init.js` | Background Global State | Initializes global `G` configuration object, regex rules, `RadixTrie`, and storage defaults. | Pollutes global scope; `clearRedundant` runs before async storage hydration completes. |
| `extension/js/function.js` | Shared Helpers | String sanitization (`stringModify`), byte formatting, template replacement. | `stringModify` replaces `:` with `：` (fullwidth colon) instead of stripping NTFS ADS (`:` stream suffix). |
| `extension/js/background.js` | MV3 Service Worker | Native messaging RPC client (`sendNativeRpc`), browser download interception (`chrome.downloads.onCreated`), `webRequest` media sniffer, chunked blob transfer sender, badge/popup state provider. | Race on `ensureSessionRestored` (`BUG-044`); timeout mismatch with `radium-nmh.exe` (`BUG-043`); `radiumDirectBlobDownload` bypasses user prompt (`SEC-001`). |
| `extension/js/content-script.js` | Content Script (`ISOLATED`) | Injects Shadow DOM video overlay pill (`Download Video`), bridges `window.postMessage` from `catch.js` to `chrome.runtime.sendMessage`. | **Critical (`SEC-001`):** Never verifies `ev.data.nonce` on `window.postMessage`; global `__fluxLastCandidate` collision (`BUG-049`). |
| `extension/catch-script/catch.js` | Page Script (`MAIN`) | Monkey-patches `MediaSource.addSourceBuffer`, `URL.createObjectURL`, `URL.revokeObjectURL`, `fetch`, and `XMLHttpRequest` to capture MSE buffers, blobs, and manifests. | **High (`BUG-042`):** Suppresses `URL.revokeObjectURL` globally for 60s and retains up to 480 MB of media buffers per tab for 10 minutes. |
| `extension/js/popup.js`, `popup.html`, `popup.css`, `options.js`, `options.html` | Extension UI | Popup media list, one-click send to Radium, quality selector, page links grabber, and extension options page. | `popup.js:454` hardcodes `.mp4` on page link downloads (`BUG-049`). |

---

### 2.3 Third-Party Dependencies, Scripts, Installer & CI (`third_party/`, `scripts/`, `installer/`, `.github/`)

| Path | Purpose | Audit Findings |
| :--- | :--- | :--- |
| `third_party/sqlite/sqlite3.c`, `sqlite3.h` | Embedded SQLite 3.46.1 amalgamation (`Public Domain`). | Compiled three separate times across CMake targets (`BUILD-001`) without `SQLITE_DEFAULT_MEMSTATUS=0` or `SQLITE_OMIT_LOAD_EXTENSION=1`. |
| `third_party/ffmpeg.exe` | Prebuilt FFmpeg binary (`47.76 MB`) tracked via Git LFS. | Expected SHA-256 in `scripts/fetch-third-party.ps1:31` is 63 hex chars (corrupted constant, `BUILD-005`). |
| `installer/radium.iss` | Inno Setup 6 installer script. | Never bundles `extension/` directory (`BUILD-007`); passes `/f` to `reg delete` as value name on uninstall (`BUILD-008`). |
| `.github/workflows/windows-release.yml` | Production build, test, and GitHub Release workflow. | Force-commits 50 MB of built `.exe` binaries back to `main` on every push (`BUILD-004`). |
| `.github/workflows/compatibility-lab.yml` | Cloud matrix testing across Windows 11, Windows 10, Server 2022, ARM64, and browsers. | ARM64 cross-compile uses Ninja multi-config path mismatch (`BUILD-006`) and uploads x64 binaries to the ARM64 test runner. |

---

## 3. Cross-Cutting Architectural Weaknesses

1. **Monolithic God Files:**
   - `src/ui/win32_main_window.cpp` is **9,574 lines** in a single translation unit containing the main window, Direct2D renderer, tray icon, 14 modal dialog procedures, custom owner-draw controls, and file I/O handlers.
   - `src/engine_stream/nm3u8dl_re_adapter.cpp` is **3,023 lines** combining social video site extraction, YouTube player JS deciphering, manifest probing, DRM inspection, and Windows Job Object process management.
   - `src/core/download_controller.cpp` is **2,390 lines** combining scheduling, thread lifecycle, rate limiting, mirror failover, post-processing, and persistence.
2. **Coarse-Grained Locking Across I/O Boundaries:**
   - `DownloadController::m_mutex` is held across synchronous SQLite queries (`m_store.ListQueues()` at 20Hz in `ScheduleLoop`) and synchronous filesystem deletions (`std::filesystem::remove_all` in `CancelTask` and `RetryTask`), coupling UI responsiveness directly to disk and DPAPI latency (`BUG-003`).
3. **Duplicated Subsystems Between Adapter & Worker:**
   - Because `N_m3u8DL-RE.exe` is compiled from `src/worker_stream/nm3u8dl_re_worker.cpp`, large portions of HLS/DASH parsing, YouTube extraction, URL resolution, and process spawning are copy-pasted between `nm3u8dl_re_adapter.cpp` and `nm3u8dl_re_worker.cpp` with divergent bug fixes (see [DUPLICATE_CODE_REPORT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/DUPLICATE_CODE_REPORT.md)).
