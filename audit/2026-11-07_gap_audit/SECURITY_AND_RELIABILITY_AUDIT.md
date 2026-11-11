# Radium Download Manager: Technical Risks, Security & Reliability Audit

**Date:** November 7, 2026  
**Audited Subsystems:**
- Inter-Process Communication & Named Pipe Security (`src/ipc/`, `src/nmh/`, `src/common/ipc_path.hpp`)
- Process Execution & Command Sanitization (`src/worker_stream/`, `src/engine_stream/`, `src/security/`)
- Path Sanitization & Filesystem Safety (`src/common/path_sanitizer.cpp`)
- Cryptography, Credential Storage & Database (`src/common/crypto_utils.cpp`, `src/storage/sqlite_store.cpp`)
- Lifetime, Concurrency & Shutdown Race Conditions (`src/core/download_controller.cpp`, `src/brisk_engine/`)

---

## 1. Security Architecture Evaluation

Radium's security architecture incorporates several modern security principles:
1. **SSRF Guard:** The HTTP client and FTP engine strictly validate target endpoints, rejecting loopback (`127.0.0.1`), link-local (`169.254.0.0/16`), and RFC 1918 private IP ranges unless explicitly opted-in by user settings.
2. **Out-of-Process Worker Isolation:** Video probing and FFmpeg muxing are delegated to worker executables running inside restricted Windows Job Objects with CPU and memory limits.
3. **No Raw Command IPC:** The browser extension communicates strictly via typed JSON-RPC 2.0 messages; arbitrary command strings are never accepted over IPC.

However, detailed code inspection reveals critical security risks and concurrency race conditions that must be addressed before commercial release.

---

## 2. In-Depth Security & Technical Risk Findings

### 2.1 Worker Process Command Line Construction Lacks Escape Handling
- **Location:** `src/worker_stream/nm3u8dl_re_worker.cpp:45–55`
- **Severity: High**
- **Vulnerability Mechanism:**
  ```cpp
  cmd_stream << L"\"" << wide_arg << L"\" ";
  ```
  The worker converts arguments into a single string by wrapping them in double quotes. However, on Windows, if an argument ends with a trailing backslash (for example, a user-selected output directory like `C:\Downloads\`), the command string becomes:
  `... --output-dir "C:\Downloads\" --output-name "video" ...`
  In standard Win32 argument parsing (`CommandLineToArgvW`), the sequence `\"` is interpreted as an escaped double quote rather than an argument boundary. As a result, the next argument (`--output-name`) is swallowed into the path, causing argument parsing failure or potential injection if arbitrary input is provided.
- **Recommended Fix:** Implement standard Windows command-line quoting rules (`ArgvToCommandLine` algorithm): count consecutive backslashes preceding a quote and double them before appending the closing quote.

### 2.2 Unsanitized Replacement Template in Custom Antivirus Scanner
- **Location:** `src/security/antivirus_scanner.cpp:69`
- **Severity: High**
- **Vulnerability Mechanism:**
  ```cpp
  // Scans file using user-defined executable and args template
  std::string cmd = executable_path + " " + args_template;
  // Replaces %1 with file path
  size_t pos = cmd.find("%1");
  if (pos != std::string::npos) cmd.replace(pos, 2, "\"" + target_file + "\"");
  ```
  The code performs naive string replacement of `%1` inside a user-configured template string. If an attacker manages to write a malformed argument string into settings, or if the template contains unquoted parameters, arbitrary command execution could be achieved.
- **Recommended Fix:** Avoid passing string templates through command line concatenations. Parse template tokens into an argument vector and spawn the process via `CreateProcessW` directly without invoking shell wrappers.

### 2.3 Plaintext Decryption Key Storage in SQLite Database
- **Location:** `src/storage/sqlite_store.cpp:94`
- **Severity: Medium**
- **Vulnerability Mechanism:** The `stream_options_json` column stores `custom_hls_key_hex` (the AES-128 decryption key for protected streams) as unencrypted JSON text.
- **Risk:** Any non-elevated process running on the same local user account can read `%LOCALAPPDATA%\Radium\state.db` and extract keys or cached authorization headers.
- **Recommended Fix:** Protect sensitive credential fields (HLS keys, Basic Auth passwords, cookies) using Windows DPAPI (`CryptProtectData`) before serializing to SQLite.

### 2.4 Incomplete Path Traversal Prevention on Extended Device Names
- **Location:** `src/common/path_sanitizer.cpp:222–284`
- **Severity: Medium**
- **Vulnerability Mechanism:**
  - `IsReservedDosName` checks standard names (`CON`, `PRN`, `AUX`, `NUL`, `COM1..COM9`, `LPT1..LPT9`).
  - However, Windows also treats device names with long extensions (e.g. `NUL.verylongextension`) as reserved device handles.
  - The sanitizer extracts stems, but if the extension exceeds 16 characters, it passes the full filename to the check, which may allow certain DOS device variants to slip through.
  - Furthermore, `CommitPartFile` does not canonicalize the final destination path via `GetFullPathNameW` to confirm that the file does not traverse out of the target directory via directory junctions.
- **Recommended Fix:** Always pass the stem of the filename to `IsReservedDosName` regardless of extension length, and verify the canonical path of the target directory using `GetFullPathNameW` prior to renaming `.part` files.

### 2.5 Named Pipe Client Impersonation Risk in IPC
- **Location:** `src/ipc/ipc_server.cpp:115–140`
- **Severity: Medium**
- **Mechanism:** When the Native Messaging Host connects to the desktop named pipe, `CreateFileW` is called with `OPEN_EXISTING`.
- **Risk:** By default, named pipe clients grant `SECURITY_IMPERSONATION` rights to the server. If a malicious server impersonates a connecting client, it could execute actions in the client's context. While Radium creates the server and connects from NMH, the client connection in `radium-nmh.exe` should explicitly specify `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION` in the flags to prevent unauthorized impersonation.

---

## 3. Reliability, Concurrency & Shutdown Race Conditions

### 3.1 Shutdown Race Condition in `DownloadController::Shutdown()`
- **Location:** `src/core/download_controller.cpp:330–365`
- **Severity: High**
- **Issue:**
  ```cpp
  void DownloadController::Shutdown() {
      m_stopping.store(true);
      // Iterates m_active tasks and requests pause
      for (auto& [id, task] : m_active_tasks) {
          task->RequestPause();
      }
      // Joins worker threads
  }
  ```
  When a download completes naturally while `Shutdown()` is iterating `m_active_tasks`, the worker thread calls `OnTaskCompleted()`, which attempts to acquire `m_tasks_mutex` and erase itself from `m_active_tasks`. If `Shutdown()` holds or interleaves locks on `m_tasks_mutex`, a deadlock or iterator invalidation crash occurs on application exit.
- **Recommended Fix:** Snapshot the task pointers into a local vector under the lock, release the lock, and then request pauses and join worker threads.

### 3.2 Lack of SEH Unhandled Exception Filter (Crash MiniDump)
- **Location:** `src/app/main.cpp:1`
- **Severity: Medium**
- **Issue:** The desktop application lacks a top-level Win32 Structured Exception Handler (`SetUnhandledExceptionFilter`). If a hardware exception occurs (access violation in WinHTTP or GDI heap corruption), Windows terminates the process immediately with zero telemetry, leaving the user with an unexplained disappear-on-launch symptom.
- **Recommended Fix:** Implement `SetUnhandledExceptionFilter` using `MiniDumpWriteDump` to write a lightweight, privacy-redacted `.dmp` file into `%LOCALAPPDATA%\Radium\crashes\` so users can attach crash dumps when reporting bugs.

---

## 4. Reliability & Security Scorecard

| Domain | Score | Evaluation Summary |
| :--- | :---: | :--- |
| **Network & SSRF Safety** | 9.0 / 10 | Strict private IP validation and host pinning. |
| **Worker Isolation** | 8.5 / 10 | Windows Job Object process isolation; argument quoting needs hardening. |
| **Path Sanitization** | 8.0 / 10 | Robust DOS name and ADS filtering; junction canonicalization needs check. |
| **Credential Storage** | 6.5 / 10 | SQLite database stores custom HLS decryption keys in plaintext (needs DPAPI). |
| **Concurrency & Thread Safety** | 7.5 / 10 | Well-designed atomic states; shutdown lock interleaving requires snapshotting. |
| **Crash Observability** | 4.0 / 10 | Ring buffer log exists, but no unhandled exception filter or minidump output. |
