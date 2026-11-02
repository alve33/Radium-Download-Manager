# Refactoring Roadmap & Remediation Strategy

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## 1. Top 20 Highest-Priority Findings to Fix First

| Rank | ID | Severity | Category | Short Title | Effort | Regression Risk |
| :---: | :--- | :---: | :--- | :--- | :---: | :---: |
| **1** | `SEC-001` | `CRITICAL` | Security / Extension | Zero-click arbitrary file write via unauthenticated `window.postMessage` | 2h | Low |
| **2** | `SEC-002` | `CRITICAL` | Security / Stream | Command-line flag injection via unquoted stream filters in `@job.rsp` | 1h | Low |
| **3** | `SEC-003` | `CRITICAL` | Security / AV | False-negative Windows Defender bypass via filename matching | 30m | Very Low |
| **4** | `SEC-004` | `CRITICAL` | Security / SSRF | SSRF bypasses in `ValidateManifestSafety` (`https://`, IPv6, private IPs) | 2h | Low |
| **5** | `BUG-026` | `CRITICAL` | Torrent | 32-bit unsigned overflow in `4 + payload_len` causing infinite CPU loop & deadlock | 30m | Very Low |
| **6** | `BUG-027` | `CRITICAL` | Torrent | BEP 9 metadata exchange never verifies `SHA1 == expected_info_hash` | 1h | Low |
| **7** | `BUG-039` | `CRITICAL` | Extension / Firefox | Firefox MV3 background script crashes on startup (`G is undefined`) | 30m | Low |
| **8** | `BUG-015` | `CRITICAL` | HTTP Engine | Truncated single-stream HTTP downloads silently committed as `Completed` | 1h | Low |
| **9** | `BUG-001` | `CRITICAL` | Core Controller | Asynchronous `PauseTask` overwrites immediate `ResumeTask` / `RefreshTaskUrl` | 2h | Medium |
| **10** | `BUG-037` | `CRITICAL` | Win32 GUI | Open Torrent file dialog missing `\0` null separators in filter string | 15m | Very Low |
| **11** | `BUG-029` | `CRITICAL` | Torrent / Concurrency | Unsynchronized data race on `TaskModel& task` across 64 peer threads | 2h | Low |
| **12** | `CI-01` | `CRITICAL` | CI/CD | `windows-release.yml` force-pushes 50 MB of binary `.exe`s to `main` | 30m | Low |
| **13** | `PKG-01` | `CRITICAL` | Installer | ARM64 installer packages x64 `N_m3u8DL-RE.exe` and installs into `tools\win-x64` | 30m | Low |
| **14** | `DOC-01` | `CRITICAL` | Legal / License | Root `LICENSE` (Apache-2.0) vs `ADR-0006` / `AGENTS.md` (GPL-3.0) conflict | 1h | Low |
| **15** | `BUG-002` | `CRITICAL` | Storage | `UpdateTaskProgressAndSegments` omits `part_path`/`file_name`/`etag` | 2h | Medium |
| **16** | `SEC-005` | `HIGH` | Security / IPC | Weak `saveDir` validation allows writes outside Downloads & UNC SMB leaks | 1h | Low |
| **17** | `BUG-003` | `HIGH` | Core / Performance | 20Hz `ScheduleLoop` queries SQLite under `m_mutex`, causing UI stutter | 2h | Medium |
| **18** | `BUG-016` | `HIGH` | HTTP Engine | False-positive SSRF block on all public domains starting with `"fc"` or `"fd"` | 15m | Very Low |
| **19** | `BUG-041` | `HIGH` | Win32 GUI | Synchronous SHA-256 and cross-drive copy freezes UI thread | 3h | Medium |
| **20** | `BUG-042` | `HIGH` | Extension | Global 60s `URL.revokeObjectURL` suppression leaks hundreds of MB per tab | 1h | Low |

---

## 2. Six-Stage Remediation Roadmap

```
[ Stage 1: Security & Protocol Integrity ] (P0 Blockers)
   ├── SEC-001 (Extension Blob Auth) & SEC-002 (Filter Flag Injection)
   ├── SEC-003 (Defender Bypass) & SEC-004 (SSRF Fixes)
   ├── BUG-026 (Torrent 32-bit Overflow) & BUG-027 (Magnet SHA-1 Verify)
   └── BUG-039 (Firefox MV3 Crash) & BUG-015 (Truncated HTTP Commit)
                       │
                       ▼
[ Stage 2: Core State Machine & Storage Concurrency ]
   ├── BUG-001 (Pause/Resume/Refresh Race)
   ├── BUG-002 (Crash Recovery Metadata Persistence)
   ├── BUG-003 (Decouple SQLite from 20Hz ScheduleLoop)
   ├── BUG-004 (RemoveTask / Shutdown Join Race)
   └── BUG-005 (Post-Scan Cancelation Integrity)
                       │
                       ▼
[ Stage 3: Protocol Engines Hardening (HTTP, Torrent, Stream, FTP) ]
   ├── BUG-016 (Fix fc*/fd* SSRF Filter) & BUG-017 (HTTP 401/403 Error Mapping)
   ├── BUG-021 (Job Object & Pipe Deadlock in Worker) & BUG-022 (Live AES Decryption)
   ├── BUG-029 (Synchronize TaskModel in Torrent) & BUG-031 (ut_metadata Assembly)
   └── BUG-024 (FTP Unbounded Buffer & Part File Truncation)
                       │
                       ▼
[ Stage 4: Win32 GUI & Browser Extension Polishing ]
   ├── BUG-037 (Fix lpstrFilter in File Dialog)
   ├── BUG-040 (Reentrant Dialog & Dropped Browser Offers)
   ├── BUG-041 (Async UI Task Hashing / Cross-Drive Move)
   ├── BUG-042 (Remove Global revokeObjectURL Delay in catch.js)
   └── BUG-046 / BUG-047 (Fix Layout Math, GDI Leaks & Vector Clones)
                       │
                       ▼
[ Stage 5: Test Suite Integrity & Real Verification ]
   ├── TST-01 (Replace Vacuous Unit Tests with Real Wire Sockets)
   ├── TST-02 (Eliminate POSIX /tmp and Fixed TCP Ports)
   └── Add Automated Regression Tests for SEC-001 through SEC-011
                       │
                       ▼
[ Stage 6: Build, CI/CD, Packaging & Documentation Cleanup ]
   ├── CI-01 (Stop Pushing 50MB Binaries to main) & BLD-01 (Remove Duplicate Targets)
   ├── PKG-01 (Fix ARM64 Packaging) & PKG-02 (Fix Firefox Native Manifest)
   ├── BLD-03 (Static CRT /MT Linkage per ADR-0001)
   └── DOC-01 (Reconcile Apache-2.0 vs GPL-3.0 License Across Repository)
```
