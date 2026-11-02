# Executive Summary — Radium Download Manager Complete Codebase Audit

**Audit Date:** 2026-10-30  
**Repository:** `https://github.com/sayedalve/Radium-Download-Manager.git` (Local Workspace: `c:\Users\sayed\Downloads\openidm`)  
**Audited Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)  
**Audit Scope:** Full-repository architectural, C++20 correctness, memory/resource safety, concurrency, protocol engine (HTTP/HTTPS, HLS/DASH/RTSP, BitTorrent, FTP, Crawler), Win32 GUI, IPC/NMH, Manifest V3 Browser Extension, Security, Performance, Test Suite, Build/Packaging, and Documentation audit.

---

## 1. Overall Health Assessment & Release Readiness

Radium Download Manager has a strong native Windows 11 architectural foundation:
- **Zero-dependency Win32/Direct2D/WinHTTP core** with a fast startup time (`64.65 ms` cold start), compact binary footprint (`radium.exe` at `2.83 MB`), and low memory overhead (`10.68 MB` idle RSS).
- **Comprehensive multi-protocol reach** spanning segmented HTTP/HTTPS (`Brisk`-style dynamic range splitting), HLS/DASH/MSS stream extraction (`N_m3u8DL-RE` + `FFmpeg` inside a Windows Job Object), BitTorrent (BEP 3/9/10/15/23 + DHT), FTP/FTPS, and HTML site grabbing.
- **All baseline automated test suites currently pass** (`77/77` in `radium_unit_tests.exe`, `4/4` in `radium_core_tests.exe`, `18/18` in `test_extension_detection.js`, `8/8` in `run_all_integration_tests.py`, and `10/10` in `test_core_operations.py`), and all GitHub Actions CI workflows on `c5f68f4` are green.

However, deep code inspection across all C++20 translation units, browser extension scripts, and build/test pipelines uncovered **61 verified findings** (`49` functional/concurrency/correctness bugs and `12` security vulnerabilities), plus **22 dead-code/artifact bloat items** and **12 duplicate subsystem implementations**.

### Release Readiness Verdict: **CONDITIONAL HOLD (Fix P0 Blockers Before Next Public Release)**
While v1.0.0 operates well under nominal happy-path conditions, **12 Critical (`P0`) findings** affect security boundaries, download data integrity, thread termination safety, and Firefox browser extension startup. These `P0` issues should be remediated before cutting v1.0.1.

---

## 2. Summary of Findings by Severity & Category

| Category | Critical (`P0`) | High (`P0`/`P1`) | Medium (`P1`/`P2`) | Low / Info (`P2`/`P3`) | Total |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Core Controller, Concurrency & Storage (`BUG-001`–`BUG-014`)** | 2 | 8 | 4 | 0 | **14** |
| **HTTP, Stream, FTP & Crawler Engines (`BUG-015`–`BUG-025`)** | 1 | 8 | 2 | 0 | **11** |
| **BitTorrent, Archive & Antivirus (`BUG-026`–`BUG-036`)** | 4 | 7 | 0 | 0 | **11** |
| **Win32 GUI, IPC, NMH & Browser Extension (`BUG-037`–`BUG-049`)** | 3 | 6 | 4 | 0 | **13** |
| **Security Vulnerabilities (`SEC-001`–`SEC-012`)** | 4 | 7 | 1 | 0 | **12** |
| **Total Verified Defects & Vulnerabilities** | **14** | **36** | **11** | **0** | **61** |

*(In addition, see [DEAD_CODE_REPORT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/DEAD_CODE_REPORT.md) for 22 dead-code/bloat items, [DUPLICATE_CODE_REPORT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/DUPLICATE_CODE_REPORT.md) for 12 duplicate implementations, [PERFORMANCE_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/PERFORMANCE_AUDIT.md) for 12 performance bottlenecks, and [BUILD_AND_DEPENDENCY_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/BUILD_AND_DEPENDENCY_AUDIT.md) for 14 build/CI/packaging/documentation issues.)*

---

## 3. Top Systemic Risks Requiring Immediate Attention

### 3.1 Security Boundary Bypasses (`SEC-001` through `SEC-007`)
1. **Unauthenticated Webpage `postMessage` to Pre-Prompt File Commit (`SEC-001`):** `extension/js/content-script.js` listens for `window.postMessage({ type: 'radiumDirectBlobDownload' })` without verifying the per-page cryptographic `nonce` generated in `catch.js` or requiring user gesture activation. Combined with `IpcServer::HandleMessage` (`completeChunkedTransfer`), which renames `.part` to the final filename inside the default Downloads folder *before* invoking `m_offer_cb` (the user confirmation dialog), any webpage can silently write arbitrary files to the user's Downloads folder without interaction.
2. **Command-Line Flag Injection in Stream Worker Response File (`SEC-002`):** `Nm3u8dlReAdapter::BuildResponseFileContent` writes `-sv`, `-sa`, and `-ss` stream selector values into `@job.rsp` without quoting, while `ParseResponseFile` in `nm3u8dl_re_worker.cpp` splits unquoted whitespace into independent CLI arguments, allowing crafted stream filter strings to inject flags such as `--ffmpeg-binary-path`.
3. **Antivirus Exit Code Override by Filename Substring (`SEC-003`):** `AntivirusScanner::ScanWithDefender` prints the scanned file path to `MpCmdRun.exe`'s stdout and then overrides Defender's `exit_code == 2` (threat detected) to `Clean` if stdout contains `"found no threats"` or to `Error` (which `DownloadController` allows through!) if stdout contains `"error"`. A malicious file named `update_error.exe` or `found no threats.exe` bypasses Windows Defender scanning.
4. **SSRF Validation Gaps (`SEC-004`, `BUG-016`):** `ValidateManifestSafety` only checks `http://` prefixes (ignoring `https://127.0.0.1` and `https://169.254.169.254`), misses `172.16.0.0/12` and IPv6 `::1`, and is bypassed completely during recursive manifest fetching (`SEC-006`). Conversely, `HttpClient::IsLoopbackHost` blocks every public domain starting with `"fc"` or `"fd"` (`fdroid.org`, `fcc.gov`, `fc2.com`) because it checks `h.rfind("fc", 0) == 0` without checking for an IPv6 colon (`BUG-016`).

### 3.2 Download Integrity & State Machine Races (`BUG-001`, `BUG-002`, `BUG-005`, `BUG-015`)
1. **Truncated Single-Stream HTTP Downloads Marked `Completed` (`BUG-015`):** When downloading from a server that does not support `Accept-Ranges: bytes` (or omits `Content-Length` on `HEAD`), `HttpDownloadEngine` Case A uses a no-op `on_headers` callback and unconditionally sets `task.total_bytes = written_offset` when the stream ends, committing premature network drops as `100% Completed`.
2. **Asynchronous `PauseTask` Clobbers Immediate `ResumeTask` / `RefreshTaskUrl` (`BUG-001`):** `PauseTask` returns while `RunTaskWorker` is still unwinding. When `RefreshTaskUrl` or a rapid user click transitions the task back to `kQueued`, the unwinding worker thread overwrites `t.state` back to `kPaused` at `download_controller.cpp:1570`.
3. **Broken Crash Recovery Metadata (`BUG-002`):** `SqliteStore::UpdateTaskProgressAndSegments` never updates `part_path`, `file_name`, `etag`, or `last_modified` in the `tasks` table. After an unexpected exit, `RecoverInterruptedTasks` sees an empty `part_path` and fails to reset segments even when the `.part` file was truncated or deleted.

### 3.3 BitTorrent Protocol & Concurrency Defects (`BUG-026`–`BUG-035`)
1. **32-Bit Integer Overflow in `4 + payload_len` (`BUG-026`):** A peer sending `payload_len >= 0xFFFFFFFC` wraps `uint32_t(4 + payload_len)` to `0..3` in `torrent_engine.cpp:200, 683` and `peer_wire_protocol.hpp:274`, causing an infinite non-blocking `while (recv_buf.size() >= 0)` loop that pins a CPU core at 100% and hangs `TorrentEngine` shutdown forever (`t.join()`).
2. **Unverified Magnet Metadata Hash (`BUG-027`):** `TorrentEngine::FetchMetadataFromPeer` parses BEP 9 `ut_metadata` dictionaries via `ParseMetadataDictionary`, which overwrites `info_hash_raw` with the SHA-1 of whatever bytes the peer sent without checking that it matches the magnet link's `expected_info_hash`.
3. **Unsynchronized `TaskModel& task` Mutation Across 64 Threads (`BUG-029`):** Up to 64 `peer_worker` threads concurrently read and mutate `task.status_detail` (`std::string`), `task.downloaded_bytes`, and `task.speed_bps` without synchronization.

### 3.4 GUI Responsiveness, Lock Convoys & Browser Extension Bugs (`BUG-003`, `BUG-037`–`BUG-044`)
1. **20Hz SQLite Polling Under `DownloadController::m_mutex` (`BUG-003`):** `ScheduleLoop` calls `m_store.ListQueues()` every 50ms while holding `m_mutex`, serializing the UI thread (`ListTasks()`) behind worker thread disk/DPAPI transactions (`m_store.m_mutex`).
2. **Firefox Extension Startup Crash (`BUG-039`):** `extension/manifest.firefox.json` loads only `["js/background.js"]` in a Firefox MV3 Event Page (`Window` scope), where `typeof importScripts !== "function"`, leaving `G` (`init.js`) and `function.js` undefined and throwing `ReferenceError: G is not defined` on line 14.
3. **Global 60-Second `URL.revokeObjectURL` Suppression (`BUG-042`):** `extension/catch-script/catch.js` monkey-patches `URL.revokeObjectURL` on every webpage to delay blob revocation by 60 seconds and retains up to 480 MB of cloned media buffers per tab for 10 minutes, causing severe memory bloat on media-heavy web apps.

---

## 4. Deliverable Index (`audit/2026-10-30_complete_codebase_audit/`)

1. [EXECUTIVE_SUMMARY.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/EXECUTIVE_SUMMARY.md) — High-level assessment, severity breakdown, systemic risks, and release verdict.
2. [REPOSITORY_MAP.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/REPOSITORY_MAP.md) — Complete directory and module inventory, entry points, interfaces, IPC boundaries, and architecture map.
3. [BUG_FINDINGS.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/BUG_FINDINGS.md) — Detailed evidence-backed report of all 49 functional, concurrency, storage, engine, UI, and extension bugs (`BUG-001`–`BUG-049`).
4. [SECURITY_FINDINGS.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/SECURITY_FINDINGS.md) — Detailed report of all 12 security vulnerabilities (`SEC-001`–`SEC-012`) mapped against `AGENTS.md` §7 and `docs/SECURITY_THREAT_MODEL.md`.
5. [DEAD_CODE_REPORT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/DEAD_CODE_REPORT.md) — Inventory of dead functions, unreachable branches, unused scripts, and 56+ MB of tracked build artifacts.
6. [DUPLICATE_CODE_REPORT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/DUPLICATE_CODE_REPORT.md) — Analysis of 12 duplicated logic and build constructs across the codebase.
7. [CONCURRENCY_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/CONCURRENCY_AUDIT.md) — Thread inventory, lock hierarchy, lock convoy analysis, data races, and lifecycle hazards.
8. [DOWNLOAD_ENGINE_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/DOWNLOAD_ENGINE_AUDIT.md) — Deep technical audit of the HTTP/HTTPS, HLS/DASH, BitTorrent, FTP, and Site Grabber engines.
9. [PERFORMANCE_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/PERFORMANCE_AUDIT.md) — CPU, memory, disk I/O, SQLite, Win32 GDI/Direct2D, and extension memory performance analysis.
10. [TEST_COVERAGE_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/TEST_COVERAGE_AUDIT.md) — Baseline test results, vacuous/self-testing test cases, test fragility, and missing test matrix.
11. [BUILD_AND_DEPENDENCY_AUDIT.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/BUILD_AND_DEPENDENCY_AUDIT.md) — CMake, compiler flags, CI/CD workflows, Inno Setup installer, third-party binary verification, and license/documentation drift.
12. [REFACTORING_ROADMAP.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/REFACTORING_ROADMAP.md) — Top 20 priority fixes and a 6-stage engineering remediation plan.
13. [FINDINGS_INDEX.csv](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/FINDINGS_INDEX.csv) — Machine-readable CSV catalog of all findings.
14. [AUDIT_EXECUTION_LOG.md](file:///c:/Users/sayed/Downloads/openidm/audit/2026-10-30_complete_codebase_audit/AUDIT_EXECUTION_LOG.md) — Chronological execution record of environment checks, test runs, and verification methodology.
