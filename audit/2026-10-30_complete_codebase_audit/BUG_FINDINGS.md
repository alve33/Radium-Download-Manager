# Comprehensive Bug & Functional Findings Catalog

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## Catalog Index (`BUG-001` through `BUG-049`)

### 1. Core Controller, State Machine, Concurrency & Storage

#### [BUG-001] Asynchronous `PauseTask` Overwrites Immediate `ResumeTask` / `RefreshTaskUrl`
- **Category:** Core Controller / State Machine Concurrency
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Status:** Verified by static trace
- **Location:** `src/core/download_controller.cpp:1476-1479, 1555-1598, 1627-1671, 2018-2067`
- **Description:** `PauseTask` signals `ctrl->pause_requested = true` and updates in-memory `it->second.state = TaskState::kPaused`, but returns immediately while `RunTaskWorker` is still running in the background. If the user quickly clicks Resume or if `RefreshTaskUrl(task_id)` runs (line 2024 calls `PauseTask` and line 2060 sets `it->second.state = TaskState::kQueued`), when the exiting worker unwinds, line 1570 executes `t.state = local_task.state;` (where `local_task.state` is `kPaused`). This clobbers the new state, leaving the task permanently paused.
- **Recommended Fix:** In `RunTaskWorker` completion/unwind, only overwrite `t.state` if `t.state == TaskState::kDownloading`. Discard worker exit state if the task was already re-queued.

#### [BUG-002] `SqliteStore::UpdateTaskProgressAndSegments` Never Persists Critical Metadata
- **Category:** Storage / Crash Recovery
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Status:** Verified by static trace
- **Location:** `src/storage/sqlite_store.cpp:382-442, 665-726`, `src/core/download_controller.cpp:1347-1375`
- **Description:** Progress updates save `downloaded_bytes` and segment ranges, but omit `part_path`, `file_name`, `etag`, and `last_modified`. After a crash or power cut, `RecoverInterruptedTasks` reads an empty `part_path` and fails to reconcile disk state against segments, corrupting partial resume.
- **Recommended Fix:** Update `part_path`, `file_name`, `etag`, and `last_modified` in the SQL `UPDATE tasks SET ...` query inside `UpdateTaskProgressAndSegments`.

#### [BUG-003] Lock Convoy: `ScheduleLoop` Queries SQLite at 20Hz Under `DownloadController::m_mutex`
- **Category:** Core Controller / Performance & Concurrency
- **Severity:** `HIGH` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Status:** Verified by static trace
- **Location:** `src/core/download_controller.cpp:794-1002, 872, 1689, 1711, 1748`
- **Description:** Every 50ms, `ScheduleLoop` locks `m_mutex` and calls `m_store.ListQueues()`. Any active worker thread flushing to SQLite blocks `ScheduleLoop`, which in turn blocks the UI thread calling `ListTasks()`.
- **Recommended Fix:** Cache queue schedules in memory. Never query SQLite under `m_mutex` in the 20Hz scheduling loop.

#### [BUG-004] `RemoveTask` 3-Second Timeout Race with `Shutdown()` and `ScheduleLoop`
- **Category:** Core Controller / Lifecycle Safety
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Status:** Verified by static trace
- **Location:** `src/core/download_controller.cpp:369-390, 798-816, 1791-1808`
- **Description:** If a worker takes >3s to unwind (e.g. during an AV scan), `RemoveTask` detaches from `m_active` and blocks on `join()` outside `m_mutex`. If `Shutdown()` runs concurrently, it misses the task and deletes `m_store`, causing use-after-free crashes.
- **Recommended Fix:** Retain workers in an internal joining queue that `Shutdown()` explicitly drains.

#### [BUG-005] `CancelTask` During Post-Download Antivirus Scan (`kFinalizing`) Overwrites Completed Download
- **Category:** Core Controller / Data Integrity
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/core/download_controller.cpp:1481-1600, 1673-1724`
- **Description:** The HTTP engine renames `.part` to final before scanning. If `CancelTask` is clicked while scanning in `kFinalizing`, `RunTaskWorker` finishes `kCompleted`, but `CancelTask` subsequently overwrites the task to `kCanceled` and zeros `downloaded_bytes`.
- **Recommended Fix:** Reject cancellation once a task enters `kFinalizing` (or delete the finalized file from disk).

#### [BUG-006] Mirror Failover Overwrites Primary URL and Breaks Resume
- **Category:** Core Controller / Network Failover
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/core/download_controller.cpp:1296-1339`
- **Description:** Failover to mirror `uidx = 1` sets `local_task.url = candidate_url`. On resume, URL deduplication shrinks the list by 1 and shifts indices, skipping the functional mirror.
- **Recommended Fix:** Keep `task.url` immutable; track `current_mirror_url` separately.

#### [BUG-007] Multi-Queue Schedule Inversion & Foreign Key Violation in `MoveTaskToQueue`
- **Category:** Core Controller / Queue Management
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/core/download_controller.cpp:843, 879-896, 2369-2376`
- **Description:** Tasks paused due to system suspend are immediately resumed within 50ms because `in_window` check ignores `PauseReason::kSystemSuspend`. `MoveTaskToQueue` ignores foreign key validation errors.
- **Recommended Fix:** Do not auto-resume tasks marked `kSystemSuspend`. Validate queue ID existence before updating.

#### [BUG-008] Data Race on `m_settings` and `m_cfg`
- **Category:** Core Controller / Concurrency
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/core/download_controller.cpp:695, 707, 718`, `download_controller.hpp:145`
- **Description:** `GetConfig()` returns `const ControllerConfig&` by reference without locking `m_mutex`, while `UpdateSettings` mutates it concurrently.
- **Recommended Fix:** Return by value under lock or use `std::shared_mutex`.

#### [BUG-009] `SqliteStore::Open` Uses ANSI `string()` Instead of UTF-8 / `sqlite3_open16`
- **Category:** Storage / Unicode
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/storage/sqlite_store.cpp:37`
- **Description:** Violates `AGENTS.md` §5.1; fails on user profiles containing non-ASCII characters.
- **Recommended Fix:** Use `sqlite3_open16` or convert path via `Utf8ToWide`.

#### [BUG-010] JSON Export Casts 64-Bit Integers to `double`
- **Category:** Storage / Data Integrity
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/storage/import_export.cpp:142-145`, `src/common/json_mini.hpp:130-135`
- **Description:** Casts file sizes and timestamps $\ge 1\text{ MB}$ to `double`, serializing them as scientific notation (`1.07374e+09`) and losing precision on import.
- **Recommended Fix:** Support explicit `int64_t` in `json_mini::Value`.

#### [BUG-011] `json_mini.hpp` Corrupts UTF-16 Surrogate Pairs & Lacks Recursion Limit
- **Category:** Common / JSON Parser
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/common/json_mini.hpp:198-336`
- **Description:** Parses `\uD83D\uDE00` into invalid CESU-8; recursive nesting without depth check allows stack overflow crashes on untrusted JSON.
- **Recommended Fix:** Combine surrogate pairs into valid UTF-8 and enforce a maximum depth of 64.

#### [BUG-012] `PathSanitizer` Truncates at Colon and Mangles Plus Signs
- **Category:** Common / Sanitization
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/common/path_sanitizer.cpp:35-36, 261-276, 344`
- **Description:** Stripping colons for NTFS ADS protection truncates filenames before file extension. Converts `+` to spaces in URLs (`notepad++.exe` -> `notepad  .exe`).
- **Recommended Fix:** Replace colons with underscores and only decode `+` as space in query strings, not path components.

#### [BUG-013] `DuplicateDetector` Misses `kVerifying` and `kFinalizing` States
- **Category:** Core Controller / Deduplication
- **Severity:** `MEDIUM` | **Priority:** `P2` | **Confidence:** `CONFIRMED`
- **Location:** `src/core/duplicate_detector.hpp:200-277`
- **Description:** Only inspects `kDownloading` and `kQueued`; duplicates submitted while an identical file is being hashed or scanned are permitted to start.
- **Recommended Fix:** Include `kVerifying` and `kFinalizing` in active task filters.

#### [BUG-014] Partial Read Handling in `CryptoUtils::ComputeFileSha256Hex`
- **Category:** Common / Crypto
- **Severity:** `MEDIUM` | **Priority:** `P2` | **Confidence:** `CONFIRMED`
- **Location:** `src/common/crypto_utils.cpp:103-143`
- **Description:** Unchecked BCrypt return codes on `BCryptHashData`; partial reads on network shares can cause hash verification mismatches.
- **Recommended Fix:** Verify all BCrypt `NTSTATUS` return values.

---

### 2. HTTP, Stream, FTP & Crawler Engines

#### [BUG-015] Truncated Single-Stream Downloads Silently Marked `Completed`
- **Category:** HTTP Engine / Data Integrity
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/brisk_engine/http_download_engine.cpp:650-733, 1350`
- **Description:** Line 732 unconditionally overwrites `task.total_bytes = written_offset`, committing premature network drops as complete files.
- **Recommended Fix:** Validate against expected content length and verify EOF closure before renaming `.part`.

#### [BUG-016] False-Positive SSRF Block on Public Domains Starting with `"fc"` or `"fd"`
- **Category:** HTTP Engine / Network
- **Severity:** `HIGH` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/brisk_engine/http_client.cpp:204-223, 660`
- **Description:** Blocks domains like `fdroid.org` and `fcc.gov` because it checks string prefix without verifying IPv6 colon syntax.
- **Recommended Fix:** Require `:` or `[` before matching IPv6 ULA prefixes.

#### [BUG-017] Segment Worker Misclassifies HTTP 401/403/404 as `ERR_HTTP_RANGE_PROTOCOL`
- **Category:** HTTP Engine / Error Handling
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/brisk_engine/http_download_engine.cpp:868-898`
- **Description:** Expired signed URLs hitting 401/403 are treated as non-resumable protocol errors, blocking user URL refresh.
- **Recommended Fix:** Map status codes via `HttpClient::MapHttpStatusToError`.

#### [BUG-018] Weak ETag Sent in `If-Range` Header
- **Category:** HTTP Engine / Protocol
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/brisk_engine/http_download_engine.cpp:463, 493-497`
- **Description:** Checks `probe_meta.etag` instead of `task.etag`, transmitting weak ETags (`W/"..."`) that RFC 7233 prohibits in `If-Range`.
- **Recommended Fix:** Validate that `task.etag` is a strong validator (`!starts_with("W/")`) before setting `If-Range`.

#### [BUG-019] Segment Scheduler Steal Race Causes Fatal Protocol Error on 200 OK
- **Category:** HTTP Engine / Dynamic Range Splitting
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/brisk_engine/http_download_engine.cpp:743, 885-891, 1082-1090`
- **Description:** If a stolen segment's worker receives HTTP 200 instead of 206, rolling back the stolen segment races with the primary worker's write pointer.
- **Recommended Fix:** Synchronize segment ownership transitions under `SegmentScheduler::m_mutex`.

#### [BUG-020] Unclamped Body Delivery Beyond `Content-Length` & 5000ms Minimum Timeout Clamp
- **Category:** HTTP Engine / Resource Limits
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/brisk_engine/http_client.cpp:300-311, 811-815, 980-1003`
- **Description:** Reads past advertised `Content-Length` if server keeps sending; 5000ms minimum timeout prevents fast failover on dead mirrors.
- **Recommended Fix:** Clamp reads to remaining expected segment bytes; allow timeouts down to 1000ms.

#### [BUG-021] Pipe Deadlock in `run_ytdlp_proc` & Missing Job Object
- **Category:** Stream Engine / Process Management
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_stream/nm3u8dl_re_adapter.cpp:366, 477, 2937-3014`, `src/worker_stream/nm3u8dl_re_worker.cpp:772, 2059`
- **Description:** Synchronous `ReadFile` on child stdout deadlock if buffer fills > 4 KB; spawned `ffmpeg.exe` instances are not in a Job Object.
- **Recommended Fix:** Use asynchronous pipe readers and bind all child processes to a Job Object with `KILL_ON_JOB_CLOSE`.

#### [BUG-022] HLS AES-128 Decryption Omitted on Final Live Segments
- **Category:** Stream Engine / Decryption
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/worker_stream/nm3u8dl_re_worker.cpp:541-553, 2000-2022`
- **Description:** Loop exits on `!still_live` before decrypting remaining live segments, corrupting the end of the file.
- **Recommended Fix:** Drain and decrypt all buffered segments before closing the stream.

#### [BUG-023] Broken UTF-8 to `std::wstring` Conversion (`std::wstring(s.begin(), s.end())`)
- **Category:** Stream Engine / Unicode
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/worker_stream/nm3u8dl_re_worker.cpp:2052`, `src/engine_stream/rtsp_mms_adapter.cpp:124`
- **Description:** Zero-extends UTF-8 bytes as Latin-1 code units, mangling non-ASCII file paths and failing process creation.
- **Recommended Fix:** Replace with `Utf8ToWide`.

#### [BUG-024] Unbounded FTP Response Buffer & Destructive `.part` Truncation
- **Category:** FTP Engine / Protocol
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_ftp/ftp_client.cpp:141-172, 466-468`
- **Description:** Reading until CRLF without length limit permits memory exhaustion; `.part` is truncated to 0 if server rejects `REST`.
- **Recommended Fix:** Enforce a 64 KB response limit and preserve existing `.part` files on resume failure.

#### [BUG-025] Site Grabber Corrupts Base URLs Containing Query Strings
- **Category:** Crawler / URL Resolution
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/crawler/site_grabber.cpp:34-84, 218-250`
- **Description:** Finding the last `/` across URLs with query strings corrupts base paths; discards HTML pages `> 4 MiB` when aborted.
- **Recommended Fix:** Strip query parameters before path extraction and use standard URL resolution.

---

### 3. BitTorrent Protocol Engine & Archive Previewer

#### [BUG-026] 32-Bit Unsigned Integer Overflow Causes Infinite CPU Loop & Deadlock
- **Category:** BitTorrent / Memory & Concurrency
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:200-210, 683-693`, `peer_wire_protocol.hpp:274-283`
- **Description:** If `payload_len >= 0xFFFFFFFC`, `4 + payload_len` wraps to `0..3`. `recv_buf.size() >= 0` stays true forever, locking the worker thread in an infinite 100% CPU spin and hanging `TorrentEngine` shutdown.
- **Recommended Fix:** Check `payload_len <= kMaxPeerMessageSize (2 MiB)` before computing `4 + payload_len`.

#### [BUG-027] Magnet BEP 9 Metadata Exchange Never Verifies `SHA1 == expected_info_hash`
- **Category:** BitTorrent / Data Integrity
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:264-273`, `src/engine_torrent/torrent_parser.cpp:324-404`
- **Description:** Overwrites `torrent_info.info_hash_raw` with the SHA-1 of incoming bytes rather than asserting equality with the magnet link's target hash.
- **Recommended Fix:** Verify that `SHA1(metadata_bytes) == expected_info_hash`; reject mismatches immediately.

#### [BUG-028] Unvalidated `begin` Alignment & Block Length in `PieceManager::ProcessBlock`
- **Category:** BitTorrent / Protocol Validation
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/piece_manager.hpp:195-272`
- **Description:** Unchecked `begin` offsets and unsolicited block writes cause out-of-bounds piece buffer corruption and premature completion flags.
- **Recommended Fix:** Assert that `begin` is a multiple of 16 KB, within piece bounds, and matches an active in-flight request.

#### [BUG-029] Unsynchronized `TaskModel& task` Mutation Across 64 Peer Worker Threads
- **Category:** BitTorrent / Concurrency
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:573-593, 733-786, 881-883`
- **Description:** Up to 64 threads concurrently write `task.status_detail` (`std::string`) and `task.downloaded_bytes` without synchronization, causing heap corruption.
- **Recommended Fix:** Pass atomic progress counters (`std::atomic<uint64_t>`) instead of raw `TaskModel&`.

#### [BUG-030] Naive `"4:info"` Substring Search in `TorrentParser` Computes Wrong `info_hash`
- **Category:** BitTorrent / Bencode Parsing
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_parser.cpp:206-225`
- **Description:** `data.find("4:info")` matches inside earlier strings (e.g. comment or announce list), hashing the wrong slice of bytes.
- **Recommended Fix:** Use proper recursive-descent bencode tokenization to locate the `info` dictionary.

#### [BUG-031] BEP 9 `ut_metadata` Assembly Has 32-Bit Multiplication Overflow
- **Category:** BitTorrent / Metadata Exchange
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:226-274`
- **Description:** `piece_idx * 16384` overflow on large piece indices; accepts partial metadata when only the final piece arrives.
- **Recommended Fix:** Use 64-bit offsets and track received piece bitmasks explicitly.

#### [BUG-032] `MultiFileStorage` Lacks Mutex Synchronization & Ignores EOF Short Reads
- **Category:** BitTorrent / Storage
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/multi_file_storage.hpp:118-124, 174-187`
- **Description:** Concurrent file reads and writes across worker threads execute without a mutex; ignores short reads, reusing stale stack buffers.
- **Recommended Fix:** Protect `MultiFileStorage` with a mutex and verify read return sizes.

#### [BUG-033] `kChoke` and `kRejectRequest` Never Release In-Flight Requests
- **Category:** BitTorrent / State Machine
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:696-700, 791-806`
- **Description:** Requests blocked by choke or reject messages remain marked in-flight forever, deadlocking piece completion.
- **Recommended Fix:** Re-queue unfulfilled block requests on choke or reject.

#### [BUG-034] Outbound `peer_worker` Uploads Zero-Filled Boundary File Bytes
- **Category:** BitTorrent / Data Privacy
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:655-659, 729-731`
- **Description:** When serving requests for unselected boundary files, zeroed memory is uploaded to peers.
- **Recommended Fix:** Restrict upload serving strictly to fully verified pieces.

#### [BUG-035] Torrent Monitor Loop Hammers Trackers Every 1 Second
- **Category:** BitTorrent / Tracker Etiquette
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/engine_torrent/torrent_engine.cpp:826-930`
- **Description:** Ignores tracker-specified `interval` headers and re-queries trackers every 1000ms, causing bans for tracker flooding.
- **Recommended Fix:** Honor tracker re-announce intervals (default minimum 300s).

#### [BUG-036] `ZipPreviewer` Silently Returns `Ok` on Corrupt ZIPs & Misses ZIP64
- **Category:** Archive Preview / Parsing
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/archive/zip_preview.cpp:105-229`
- **Description:** Non-ZIP or truncated files return `Ok` with zero entries; lacks ZIP64 support, allowing zip bombs to bypass compression checks.
- **Recommended Fix:** Return `ERR_INVALID_DATA` on corrupt signatures and parse ZIP64 central directory records.

---

### 4. Win32 GUI, IPC, Native Messaging & Browser Extension

#### [BUG-037] Buffer Over-Read in `GetOpenFileNameW` (`IDM_CTX_OPEN_TORRENT`)
- **Category:** Win32 GUI / Memory Safety
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/ui/win32_main_window.cpp:9012`
- **Description:** `OPENFILENAMEW.lpstrFilter` requires double-null termination (`L"Torrent Files\0*.torrent\0All Files\0*.*\0\0"`). The filter string omits embedded `\0` delimiters, causing `GetOpenFileNameW` to read past buffer bounds into heap memory.
- **Recommended Fix:** Construct the filter string with literal null characters: `L"Torrent Files (*.torrent)\0*.torrent\0All Files (*.*)\0*.*\0\0"`.

#### [BUG-038] `IpcServer` Handle Use-After-Close & Double-Close Race
- **Category:** IPC / Handle Safety
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/ipc/ipc_server.cpp:189, 223-271, 315-385, 894-896`
- **Description:** Closing pipe handles while client reader threads are blocked in `ReadFile` causes use-after-free and double-closes on Win32 `HANDLE`s.
- **Recommended Fix:** Use overlapped I/O with cancellation events to gracefully unblock reader threads before closing handles.

#### [BUG-039] Firefox Extension Crashes on Startup (`G is not defined`)
- **Category:** Browser Extension / Firefox MV3
- **Severity:** `CRITICAL` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `extension/manifest.firefox.json:26-29`, `extension/js/background.js:8-17`
- **Description:** Firefox MV3 event pages execute in `Window` scope where `importScripts` is unavailable. Because `manifest.firefox.json` only loads `js/background.js`, `G` and `function.js` are never loaded, crashing immediately with `ReferenceError: G is not defined`.
- **Recommended Fix:** Update `manifest.firefox.json` to load all required scripts:
  ```json
  "background": {
    "scripts": ["js/init.js", "js/function.js", "js/background.js"]
  }
  ```

#### [BUG-040] Nested Pseudo-Modal Dialog Reentrancy & Silently Dropped Download Offers
- **Category:** Win32 GUI / Window Management
- **Severity:** `HIGH` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/ui/win32_main_window.cpp:4546-4565, 4769-4781, 4885-4991, 7265-7344`
- **Description:** Invoking modal dialog loops while another modal is open triggers reentrant message pumps. Subsequent incoming download offers from the browser are dropped or stuck in `kQueued` indefinitely.
- **Recommended Fix:** Maintain a FIFO modal queue so subsequent download offers display sequentially after the active dialog closes.

#### [BUG-041] Synchronous UI Thread File Hashing, Antivirus Scanning & Cross-Drive Copy
- **Category:** Win32 GUI / UI Responsiveness
- **Severity:** `HIGH` | **Priority:** `P0` | **Confidence:** `CONFIRMED`
- **Location:** `src/ui/win32_main_window.cpp:4689-4691, 8818-8843, 8925, 8946`
- **Description:** Computing SHA-256 or moving multi-gigabyte files across drives in `IDM_CTX_MOVE_TO` executes synchronously on the GUI thread, freezing the window for 5–30 seconds.
- **Recommended Fix:** Offload file hashing and cross-volume file moves to a background thread pool with a progress dialog.

#### [BUG-042] Global `URL.revokeObjectURL` Suppression Causes Browser Tab OOM
- **Category:** Browser Extension / Memory Leak
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `extension/catch-script/catch.js:789-856`
- **Description:** Delays blob revocation globally for 60 seconds and retains media buffers in memory for 10 minutes, consuming hundreds of megabytes on video sites.
- **Recommended Fix:** Remove global monkey-patching of `revokeObjectURL` or limit retention strictly to detected video manifests.

#### [BUG-043] Timeout Mismatch Between Extension and Native Host
- **Category:** Browser Extension / NMH
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `extension/js/background.js:267-328`, `src/nmh/main.cpp:153-195`
- **Description:** Extension RPC timeout is 5000ms, while `radium-nmh.exe` cold-start retry loop takes up to 11,250ms, causing timeout aborts while the host is still launching.
- **Recommended Fix:** Align extension RPC timeout to 15,000ms.

#### [BUG-044] MV3 Service Worker Session Restore Race
- **Category:** Browser Extension / Lifecycle
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `extension/js/background.js:29-36, 1074, 2102`
- **Description:** `ensureSessionRestored` is not awaited before intercepting `downloads.onCreated`, dropping download rules on browser wake-up.
- **Recommended Fix:** Await session restoration before processing intercepted downloads.

#### [BUG-045] Naive JSON-RPC Substring ID Extraction in NMH
- **Category:** Native Messaging Host / Parsing
- **Severity:** `HIGH` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/nmh/main.cpp:61-84, 238-273`
- **Description:** Substring search for `"id"` matches occurrences inside URLs or headers, generating corrupt RPC error responses.
- **Recommended Fix:** Use `json_mini::Parse` to extract the message `id`.

#### [BUG-046] Win32 Main Window Geometry Desynchronization
- **Category:** Win32 GUI / Layout
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/ui/win32_main_window.cpp:218, 302-305, 1120-1125`
- **Description:** Details pane height calculations diverge (`bar_h = 48` vs `56`), clipping bottom rows behind the status bar.
- **Recommended Fix:** Unify layout constants into shared DPI-scaled layout variables.

#### [BUG-047] Clock Mismatch & Vector Clones in Direct2D Paint Loop
- **Category:** Win32 GUI / Performance
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/ui/design_system.hpp:182-225`, `src/ui/win32_main_window.cpp:7479-8035`
- **Description:** Animation timer compares `steady_clock` against `GetTickCount64`, causing animation jumps; task list is cloned by value on every paint.
- **Recommended Fix:** Unify on `std::chrono::steady_clock` and render from a read-only const reference.

#### [BUG-048] Disconnected Settings Controls & Uncaught CLI Flag Exceptions
- **Category:** Win32 GUI / CLI
- **Severity:** `MEDIUM` | **Priority:** `P1` | **Confidence:** `CONFIRMED`
- **Location:** `src/app/main.cpp:100-101, 189-196`, `src/ui/win32_main_window.cpp:2283-2356, 6607-6745`
- **Description:** Invalid `--connections` argument throws unhandled `std::invalid_argument`; extension install buttons in Settings dialog are no-ops.
- **Recommended Fix:** Wrap CLI parsing in `try/catch` and wire button commands to `register-native-messaging.ps1`.

#### [BUG-049] Doubled File Extensions on Batch Page Links
- **Category:** Browser Extension / IPC
- **Severity:** `MEDIUM` | **Priority:** `P2` | **Confidence:** `CONFIRMED`
- **Location:** `extension/js/popup.js:454`, `src/ipc/ipc_server.cpp:1016-1019`
- **Description:** Appends `.mp4` unconditionally in popup and re-appends file extensions in IPC, generating `video.mp4.mp4` or `file.exe.exe`.
- **Recommended Fix:** Only append file extension if the derived filename does not already end with it.
