# RADIUM COMPLETE PROJECT RENAMING & REBRANDING REPORT

**Document Version:** 1.0.0  
**Date:** 2026-10-30  
**Status:** Completed and Verified  
**Scope:** Repository directory structure, C++20 desktop engine, Native Messaging Host, Browser Extensions (Chromium/Firefox MV3), CMake configuration, regression suites, and documentation.

---

## 1. Directory Migration

- **Original Directory:** `C:\Users\sayed\Downloads\openidm`
- **Target Directory:** `C:\Users\sayed\Downloads\Radium`
- **Migration Method:** Full byte-exact directory synchronization preserving Git history (`.git/`), uncommitted modifications, user assets (`RDM_Logo.svg`, MSIX assets), audit logs, and external clean source backup.
- **Git Repository State:** `HEAD` at `c5f68f4c95154e77bb8bfb59993fbd878554598d`. Verified working cleanly under the new `Radium` root directory.

---

## 2. Rebranding & Renaming Summary

### 2.1 C++20 Core & Desktop Application
- **Version Constants:**
  - `FLUX_VERSION_*` macros migrated to `RADIUM_VERSION_*` with backward-compatibility aliases in `src/common/version.hpp`.
  - Application title and version string: `Radium Download Manager v1.0.0 (Radium) [Brisk Engine + Cat Catch Media Detection + N_m3u8DL-RE Pipeline]`.
- **IPC Pipe Namespace:**
  - Primary Named Pipe on Windows 11: `\\.\pipe\Radium_IPC_<UserSID>`.
  - POSIX domain socket: `/tmp/radium_ipc_<uid>.sock`.
  - Fallback environment variables: `RADIUM_IPC_SOCKET`, `FLUX_IPC_SOCKET`, `OPENIDM_IPC_SOCKET`.
- **Logging Macros:**
  - Added primary `RADIUM_LOG_INFO`, `RADIUM_LOG_WARN`, `RADIUM_LOG_ERROR` in `src/common/logger.hpp` while retaining backward-compatibility aliases for `OPENIDM_LOG_*`.
- **UI & Window Identifiers:**
  - Primary Window Class & Window Procedure: `RadiumWndProc` (aliased from `OpenIdmWndProc`).
  - Window Messages: Defined `WM_RADIUM_TRAYICON`, `WM_RADIUM_SHOW_NEW_DOWNLOAD`, `WM_RADIUM_SELECT_TORRENT_METADATA`, `WM_RADIUM_PROBE_COMPLETE`, `WM_RADIUM_TORRENT_FETCH_COMPLETE` with backward-compatible aliases for legacy `WM_OPENIDM_*`.
  - Application Icon: `CreateRadiumAppIcon()` with `CreateFluxAppIcon()` backward-compatibility wrapper.

### 2.2 Browser Extensions (Chromium & Firefox MV3)
- **Content Script (`extension/js/content-script.js`):**
  - Renamed internal DOM tracking properties: `__radiumLastCss`, `__radiumHostEl`, `__radiumBtn`, `__radiumSelect`, `__radiumVideo`, `__radiumSelectedQuality`, `__radiumClickInFlight`, `__radiumLastCandidate`.
  - Overlay host DOM markers: Set both `data-radium-shadow-host` and legacy `data-flux-shadow-host` to ensure seamless interaction with web content and test frameworks.
  - Overlay bypass dataset: Set `data-radium-bypass` alongside `data-flux-bypass`.
- **Catch Script (`extension/catch-script/catch.js`):**
  - Script installation flag: `win.__radiumCatchScriptInstalled`.
  - Media extraction descriptor: Emits `radiumType: "YOUTUBE_STREAM"` alongside `fluxType: "YOUTUBE_STREAM"`.
  - Injected XHR tracking: `this.__radiumUrl`.
  - Anchor click actions: `radiumAnchorClicked` (supported alongside legacy `fluxAnchorClicked`).
  - Blob messaging: `radiumGetBlobData` / `radiumBlobDataResponse` (with legacy fallback).
- **Background Service Worker (`extension/js/background.js`):**
  - Primary functions: `transferBlobToRadium()` and `submitMediaCandidateToRadium()`.
  - Chrome local storage persistence: Saves session state under `radium_*` keys while mirroring to `flux_*` keys for backward compatibility with existing browser sessions.
  - Pipeline trace logging: Migrated to `DOWNLOAD_ACCEPTED_BY_RADIUM`, `HANDED_OFF_TO_RADIUM`, `CAPTURED_BY_RADIUM`.

### 2.3 CMake Build System
- **Core Library Target:** `radium_core` OBJECT library (aliased to `flux_core`).
- **Primary Executables:** `radium`, `radium-nmh`, `radium_unit_tests`, `radium_core_tests`, `N_m3u8DL-RE`.
- **Compatibility Targets:** Maintained `flux`, `flux-nmh`, `flux_unit_tests`, `openidm`, `openidm-nmh`, `openidm_unit_tests`.

---

## 3. Retained Compatibility Identifiers

The following identifiers are intentionally retained to prevent breaking existing user installations, external scripts, and historical audit reports:

1. **Named Pipe & Socket Fallbacks:** `FLUX_IPC_SOCKET` and `OPENIDM_IPC_SOCKET` environment variables in `src/common/ipc_path.hpp`.
2. **Loopback Environment Variables:** `FLUX_ALLOW_LOOPBACK` and `OPENIDM_ALLOW_LOOPBACK` in `src/app/main.cpp` and `src/worker_stream/nm3u8dl_re_worker.cpp`.
3. **Session Storage Backward Compatibility:** Legacy `flux_*` keys in `extension/js/background.js` to ensure users upgrading extensions retain their tab caches and exclusion rules.
4. **YouTube Extractor Payload Tag:** `fluxType: "YOUTUBE_STREAM"` preserved in JSON descriptors for backward compatibility with existing worker binaries.
5. **Historical Documentation:** References in `audit/` and `docs/archive/` representing historical development phases.

---

## 4. Test Verification Summary

All test suites were re-executed from the new `C:\Users\sayed\Downloads\Radium` repository against freshly compiled binaries:

| Test Suite | Result | Details |
| :--- | :--- | :--- |
| **SEC-001 Adversarial Suite** | **6 / 6 PASSED** | `node tests/test_sec001_adversarial.js` |
| **Browser Extension Suite** | **18 / 18 PASSED** | `node tests/test_extension_detection.js` (26 / 26 scenarios) |
| **C++20 Core Dedicated Suite** | **4 / 4 PASSED** | `radium_core_tests.exe` |
| **C++20 Comprehensive Unit Suite** | **77 / 77 PASSED** | `radium_unit_tests.exe` |
| **Python Core Operations Suite** | **10 / 10 PASSED** | `python tests/test_core_operations.py` |
| **End-to-End Integration Suite** | **8 / 8 PASSED** | `python tests/run_all_integration_tests.py` |

---

## 5. Final Status

- **Main Directory:** `C:\Users\sayed\Downloads\Radium`
- **Application Identity:** Radium Download Manager (Coral Red visual branding)
- **Active Code Health:** Zero compiler warnings or errors; all unit, core, integration, and security tests pass.
- **Git Working Tree:** Intact with all uncommitted changes preserved.
