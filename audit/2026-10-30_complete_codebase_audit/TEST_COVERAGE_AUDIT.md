# Test Suite & Verification Quality Audit Report

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## 1. Test Suite Inventory & Baseline Test Results

All test suites were executed on the local Windows 11 host during Phase Zero:

| Test Suite / Binary | Engine / Harness | Suites Run | Status | Execution Time | Key Defects Identified |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `build/win-x64/Release/radium_unit_tests.exe` | C++ Custom Harness | 77 / 77 | **PASSED** | 8.42s | Vacuous / fake tests in 13 suites (`TST-01`); POSIX `/tmp` paths and fixed TCP ports (`TST-02`). |
| `build/win-x64/Release/radium_core_tests.exe` | CTest Executable | 4 / 4 | **PASSED** | 0.12s | 100% duplicate of unit tests; omitted from CI (`BLD-02`). |
| `tests/test_extension_detection.js` | Node.js | 18 / 18 | **PASSED** | 1.85s | Tests detection rules but does not exercise Firefox MV3 event page bootstrap (`BUG-039`). |
| `python tests/run_all_integration_tests.py` | Python 3 Harness | 8 / 8 | **PASSED** | 12.10s | Passes, but relies on mock loopback server; skips live tracker tests. |
| `python tests/test_core_operations.py` | Python 3 Harness | 10 / 10 | **PASSED** | 4.65s | Tests happy path; does not exercise rapid pause/resume race (`BUG-001`). |

---

## 2. In-Depth Analysis of Fake / Vacuous Test Cases (`TST-01`)

The audit revealed that multiple tests in `tests/cpp/unit_tests.cpp` pass with 100% success while **never actually invoking the production C++ code they claim to verify**:

### 2.1 Fake Live BitTorrent Mock Peer Tests
- **Location:** `tests/cpp/unit_tests.cpp:5240-5281, 5306-5356, 5458-5524`
- **Root Cause:**
  - `TestMagnetMetadataExchangeBEP9Live` spins up `MockMagnetMetadataPeerServer` on port `16883`, logs `"[+] Connecting to peer and exchanging BEP 10/9 metadata..."`, but **never creates a socket connection**. Instead, it parses a local string buffer in memory and stops the server!
  - `TestMultiPeerTransferAndSeedingControls` and `TestBitTorrentEngineTransferAndIntegrity` do the same: they start `MockPeerServer` instances on ports `16881` and `16885`, but never connect a client socket; they manually feed bytes into `PieceManager::ProcessBlock` in a local loop.
- **Risk:** `TorrentEngine` wire-protocol bugs (such as `BUG-026` 32-bit overflow and `BUG-027` unverified metadata hash) went completely undetected despite existing "live" test names.

### 2.2 Self-Testing Inline Lambdas
- **Location:** `tests/cpp/unit_tests.cpp:2750-2791, 5607-5756, 5850-5993`
- **Mechanism:** Tests such as `TestHttpRangeSatisfactionValidation` and `TestHttpRangeCorrectnessAnd416Validation` define local lambdas (e.g. `validate_range_response`) inside the test function itself and test the lambda against hardcoded tables. **Not a single line of `HttpClient` or `HttpDownloadEngine` is called!**

---

## 3. Test Flakiness, Collisions & System Hazards (`TST-02`)

1. **Fixed TCP Ports:** `tests/cpp/unit_tests.cpp` binds ports `16881` through `16886`. Running concurrent builds (`ctest -j2`) fails with `WSAEADDRINUSE`.
2. **POSIX Paths on Windows:** Fifteen test suites create directories in `/tmp/openidm_...`, causing files to be written to `C:\tmp\...` instead of `%TEMP%`.
3. **Destructive Process Termination:** `tests/browser/test_native_messaging_positive.js:222-227` calls `taskkill /F /IM radium.exe`, which terminates any active user instance running on the host.

---

## 4. Remediation Recommendations

1. **Rewrite Mock Peer Tests to Use Real Sockets:** Connect `TorrentEngine` to ephemeral port `0` assigned by the OS and verify peer handshakes over the wire.
2. **Eliminate Mock-Self Lambdas:** Replace inline lambdas with calls to `HttpClient::ParseContentRange`, `HttpDownloadEngine::Execute`, and `PathSanitizer::MigrateLegacyAppDataIfNeeded`.
3. **Use `std::filesystem::temp_directory_path()` Everywhere:** Replace all `/tmp/` hardcoded strings.
