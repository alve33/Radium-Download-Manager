# Radium Download Manager: Security Remediation Report

**Date:** November 7, 2026  
**Auditor / Engineer:** Principal Security & Systems Engineer  
**Status:** Remediated & Verified

---

## 1. Process Execution & Command Injection Hardening

### Findings Addressed:
- **AUD-10 (SEC-2.1):** Worker process argument construction lacked quote and backslash escaping, allowing trailing backslashes to swallow quotes in Windows `CommandLineToArgvW`.
- **AUD-11 (SEC-2.2):** Custom antivirus scanner argument replacement used template expansion susceptible to argument manipulation.

### Remediation Details:
1. **Created `src/common/process_utils.hpp`:**
   - Implemented `radium::process::BuildWindowsCommandLine(const std::vector<std::wstring>& args)` strictly following Microsoft Win32 argument encoding rules:
     - Quotes strings containing spaces, tabs, or embedded quotes.
     - Handles backslashes preceding quotes by doubling them ($2N+1$ backslashes).
     - Handles trailing backslashes before the closing quote by doubling them ($2N$ backslashes).
     - Converts empty strings to `""`.
   - Implemented `radium::process::RunProcessCaptureOutput` using `CreateProcessW` directly without invoking `cmd.exe` or shell interpreters, using `CREATE_NO_WINDOW` and clean handle lifetime management.
2. **Hardened Antivirus Scanner (`src/security/antivirus_scanner.cpp`):**
   - Eliminated shell-based expansion. Custom executable paths and target files are passed cleanly to `CreateProcessW` without intermediate string splitting vulnerabilities.

---

## 2. Secrets Protection & At-Rest Encryption

### Findings Addressed:
- **AUD-18 (SEC-2.3):** Custom HLS stream decryption keys (`custom_hls_key_hex`) were persisted in plaintext in SQLite within `stream_options_json`.

### Remediation Details:
1. **Windows DPAPI Integration (`src/storage/sqlite_store.cpp`):**
   - Added automatic DPAPI protection on insert/update:
     ```cpp
     if (!task.stream_options.custom_hls_key_hex.empty()) {
         auto prot_key = CryptoUtils::ProtectSecret(task.stream_options.custom_hls_key_hex);
         if (prot_key.ok()) {
             std::string b64 = CryptoUtils::Base64Encode(...);
             stream_obj["custom_hls_key_hex_dpapi"] = b64;
         }
     }
     ```
   - On read/deserialize, transparently detects `custom_hls_key_hex_dpapi`, decodes Base64, and decrypts via `CryptoUtils::UnprotectSecret` (`CryptUnprotectData` scoped to the current Windows user).
   - Plaintext keys are wiped immediately once tasks reach terminal states (`kCompleted` or `kCanceled`).

---

## 3. Cryptographic Checksum Verification (MD5 & SHA-256)

### Findings Addressed:
- **AUD-13:** Missing MD5 hash verification despite server mirrors and user requirements offering MD5 checksums.

### Remediation Details:
1. **Implemented `CryptoUtils::ComputeFileMd5Hex` (`src/common/crypto_utils.cpp`):**
   - Built on Windows Cryptography Next Generation (`BCRYPT_MD5_ALGORITHM`).
   - Streams file contents in 64 KiB chunks to avoid memory pressure on large multi-gigabyte ISOs.
2. **Task Model & Engine Verification:**
   - Added `expected_md5` to `TaskModel` in `src/common/types.hpp`.
   - Wired MD5 integrity validation into `src/brisk_engine/http_download_engine.cpp` and `src/core/download_controller.cpp`.

---

## 4. Browser Extension & Native Messaging Security

### Findings Addressed:
- **AUD-09 & AUD-24:** Firefox MV3 extension execution and Service Worker dormancy race condition handling.
- **SEC-001 Invariant Enforcement:** Anchor-bound token verification for Blob and object URLs.

### Remediation Details:
1. **Dynamic Page Script Injection (`extension/js/content-script.js`):**
   - Automatically injects `catch.js` into DOM `<script>` tags on browsers where `"world": "MAIN"` is not supported, ensuring media sniffing operates without permissions degradation.
2. **Service Worker State Restoration:**
   - Ensured all native messaging handlers and web requests await `ensureSessionRestored()`.
