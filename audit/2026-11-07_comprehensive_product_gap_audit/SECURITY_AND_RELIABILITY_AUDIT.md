# Radium Download Manager — Security & Reliability Audit

**Date:** November 7, 2026  
**Auditor:** Senior Security & Systems Engineering Audit Team  
**Evaluation Target:** Radium Core Engine, Worker Processes, Native Messaging IPC, Browser Extension & Local Storage

---

## 1. Security Architecture & Threat Model Verification

Radium implements strong, non-negotiable security controls established in `AGENTS.md` and `docs/SECURITY_THREAT_MODEL.md`. An in-depth audit of cryptographic routines, process isolation, and input sanitizers confirmed several exceptional security practices:

1. **Path Traversal Protection (`PathSanitizer`):**
   Filename sanitization strictly eliminates path separators (`/`, `\`), drive letters (`C:`), NTFS alternate data streams (`:`), Windows reserved DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`), trailing dots/spaces, and control characters (`0x00`-`0x1F`). Target paths are resolved using `GetFullPathNameW` and verified against the destination folder boundary.
2. **IPC Security (Named Pipe DACL):**
   The Named Pipe `\\.\pipe\Radium_IPC_<UserSID>` is protected with a strict Discretionary Access Control List (DACL) granting permissions exclusively to the current user's security identifier (`TOKEN_USER`). Local processes running under other user accounts or low-integrity containers cannot connect to the pipe.
3. **No Command-Line Credential Leakage:**
   Worker processes (`N_m3u8DL-RE.exe` and `ffmpeg.exe`) never receive sensitive `Cookie` or `Authorization` headers on their command line. Authentication tokens are written to ACL-restricted temporary files deleted immediately after worker process termination.
4. **Token Redaction in Logs:**
   The `Logger` engine automatically masks sensitive authorization headers, cookies, and URL query parameters containing tokens (`token`, `sig`, `signature`, `key`, `hdnea`, `Policy`, `Key-Pair-Id`) before outputting to disk.
5. **Credential Protection at Rest:**
   Custom HLS decryption keys and site credentials stored in SQLite are protected using Windows Data Protection API (`CryptProtectData` / DPAPI).

---

## 2. Technical Risks, Vulnerabilities & Reliability Gaps

Despite the strong architectural foundation, our technical investigation identified the following areas requiring hardening:

---

### SEC-01: Absence of Cryptographic Integrity & Signature Checks on Out-of-Process Workers
- **Source File:** [`src/common/path_sanitizer.cpp:750-785`](file:///c:/Users/sayed/Downloads/Radium/src/common/path_sanitizer.cpp#L750-L785), [`src/core/download_controller.cpp`](file:///c:/Users/sayed/Downloads/Radium/src/core/download_controller.cpp)
- **Risk Classification:** High
- **Technical Vulnerability:**
  When spawning `N_m3u8DL-RE.exe` and `ffmpeg.exe`, the controller locates the executables via `FindWorkerBinary` searching `%LOCALAPPDATA%\Radium Download Manager\tools`, the executable directory, or `%PATH%`. However, the engine **does not verify Authenticode digital signatures or SHA-256 binary hashes** of the executables before invoking `CreateProcessW` inside the Job Object.
- **Attack Vector:**
  If an unprivileged local script or malware drops a modified binary into a writable user path that resolves earlier in the worker search order, Radium would launch it with the user's desktop privileges.
- **Recommended Hardening:**
  Store hardcoded canonical SHA-256 hashes of approved worker builds in `src/common/central_config.hpp`. Verify the executable hash or validate Microsoft Authenticode signatures (`WinVerifyTrust`) before launching worker processes.

---

### SEC-02: Missing Background Certificate Revocation & Pinned Root Updates
- **Source File:** [`src/brisk_engine/http_client.cpp:820-865`](file:///c:/Users/sayed/Downloads/Radium/src/brisk_engine/http_client.cpp#L820-L865)
- **Risk Classification:** Medium
- **Technical Vulnerability:**
  WinHTTP sessions rely on the Windows OS certificate store. While invalid certificates and hostname mismatches are strictly rejected by default (unless user explicitly flags insecure bypass for private intranet mirrors), WinHTTP does not enforce Online Certificate Status Protocol (OCSP) stapling verification or Certificate Transparency logging checks explicitly.
- **Recommended Hardening:**
  Enable `WINHTTP_ENABLE_SSL_REVOCATION` on WinHTTP request handles to guarantee that compromised intermediate certificates are caught immediately even if cached locally.

---

### REL-01: GDI Handle Lifetime and Resource Exhaustion Under Extreme UI Loads
- **Source File:** [`src/ui/win32_main_window.cpp:1150-1320`](file:///c:/Users/sayed/Downloads/Radium/src/ui/win32_main_window.cpp#L1150-L1320)
- **Risk Classification:** Medium
- **Reliability Assessment:**
  As noted in GAP-02, creating and deleting multiple GDI objects per frame during rapid UI repaints creates potential resource leak vectors if any exception, branch, or early return skips `DeleteObject`. On Windows systems, the GDI handle table has a hard limit of 10,000 handles per process.
- **Recommended Hardening:**
  Migrate all font, brush, and pen allocations to RAII wrappers (`wil::unique_hfont` or `CachedGdiObjects`) instantiated once per DPI change and reused across all subsequent draw calls.

---

### REL-02: SQLite Store Single-Thread Serialization Under Massive Concurrent Segment Commits
- **Source File:** [`src/storage/sqlite_store.cpp:240-310`](file:///c:/Users/sayed/Downloads/Radium/src/storage/sqlite_store.cpp#L240-L310)
- **Risk Classification:** Medium
- **Reliability Assessment:**
  `SqliteStore` wraps database mutations in a single `std::mutex`. When 32 segmented worker threads are downloading at multi-gigabit speeds and each segment completes or checkpoints its range map every few hundred milliseconds, all threads contend for the single SQLite connection lock.
- **Recommended Hardening:**
  Throttle segment checkpoint commits to disk so they occur periodically (e.g., every 500 ms) or batch segment state updates in memory, executing WAL-mode writes asynchronously on a dedicated storage worker thread.

---

### REL-03: Process Exit Handling with Active BitTorrent Swarm Connections
- **Source File:** [`src/engine_torrent/torrent_engine.cpp:920-960`](file:///c:/Users/sayed/Downloads/Radium/src/engine_torrent/torrent_engine.cpp#L920-L960)
- **Risk Classification:** Low
- **Reliability Assessment:**
  When the user initiates a hard application shutdown while actively seeding or downloading multiple large torrents, `torrent_engine` sends graceful `stopped` tracker event announcements over UDP/HTTP to all trackers. If a remote tracker is slow to respond or experiencing packet loss, shutdown can block for up to 2,000 ms per tracker if sockets are not set to non-blocking mode with an immediate cancellation signal.
- **Recommended Hardening:**
  Implement a hard 500 ms timeout on tracker exit announcements and dispatch them asynchronously detached during process teardown.
