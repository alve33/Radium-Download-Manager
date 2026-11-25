# Radium Legacy Name Cleanup & Audit Report

**Date:** 2026-11-25  
**Scope:** Repository-wide audit and cleanup of legacy `OpenIDM` and `Flux` references in **Radium Download Manager** (`radium.exe`, `radium-nmh.exe`).

---

## 1. Summary of Legacy References Discovered

Across the active repository (excluding `.git`, `build/`, `node_modules/`, `dist/`, `test-results/`, and `third_party/`), recursive case-insensitive searches for `OpenIDM`, `OpenIdm`, `openidm`, `OPENIDM`, `Open IDM`, `Flux`, `FLUX`, `flux`, `FluxDownloadManager`, and `Flux Download Manager` identified legacy references across:

- **91 C++ source and test files** (`src/` and `tests/`): primarily legacy `namespace openidm`, `OPENIDM_LOG_*` macros, `WM_OPENIDM_*` window messages, file header comments (`// OpenIDM — ...` / `// OpenIDM / Flux — ...`), and `CreateFluxAppIcon()` / `fluxType` literals.
- **4 build & CI files**: `CMakeLists.txt` (`FLUX_VERSION_*` macros and legacy `flux*` / `openidm*` custom targets), `scripts/generate_config.py` (`src/common/version.hpp` `FLUX_VERSION_*` aliases), `.github/workflows/windows-release.yml`, `.github/workflows/windows-compatibility.yml` (`FLUX_VERSION` env var), and `.github/workflow-templates/private-runner-benchmark.yml.template`.
- **3 browser extension files**: `extension/catch-script/catch.js`, `extension/js/content-script.js`, and `extension/js/background.js` (dual `flux*` / `openidm*` DOM attributes, message actions, `flux_*` session storage mirroring, and legacy comments).
- **11 test and utility scripts**: `scripts/verify-ffmpeg-capabilities.ps1`, `scripts/verify-release-security.ps1`, `scripts/report-package-sizes.ps1`, `tests/test_extension_detection.js`, `tests/browser/test_extension_runtime.js`, `tests/browser/test_native_messaging_positive.js`, `tests/test_repair_pipeline_e2e.py`, `tests/verify_speed_display_dialog.py`, `tests/test_dist_portable.py`, `tests/test_core_operations.py`, and `tests/run_all_integration_tests.py`.
- **8 active documentation files**: `docs/FEATURE_MATRIX.md`, `docs/UI_DESIGN_SYSTEM.md`, `docs/UPSTREAM_INTEGRATION_MAP.md`, `docs/WINDOWS_BUILD_AND_PACKAGING.md`, `docs/DOWNLOAD_ENGINE_DESIGN.md`, `docs/STREAM_DOWNLOAD_PIPELINE.md`, `docs/ERROR_HANDLING_AND_RECOVERY.md`, and `docs/PRODUCT_REQUIREMENTS.md`.

---

## 2. Outdated References Corrected vs. Preserved

- **Active C++ / Extension / Build / CI (`src/`, `extension/`, `.github/`, `CMakeLists.txt`, `config/`):**
  - Eliminated all outdated branding, legacy CMake target aliases (`flux`, `flux_core`, `openidm`, etc.), `FLUX_VERSION_*` macro aliases, `namespace openidm`, `OPENIDM_LOG_*`, `WM_OPENIDM_*`, `CreateFluxAppIcon`, and `flux_*` DOM/storage writes.
  - Only **51 lines** in `src/`, `extension/`, `.github/`, `CMakeLists.txt`, and `config/` retain legacy strings, all of which are intentional backward-compatibility mechanisms (detailed in Section 5).
- **Total files modified in this audit:** **115+ files** across `src/`, `tests/`, `extension/`, `scripts/`, `.github/`, `CMakeLists.txt`, and `docs/`.

---

## 3. Files Modified and Why

| Area | Files Modified | Reason |
| :--- | :--- | :--- |
| **Build & Config** | `CMakeLists.txt`, `scripts/generate_config.py`, `src/common/version.hpp` | Removed `FLUX_VERSION_*` macros and legacy `flux*` / `openidm*` CMake targets. |
| **CI Workflows** | `.github/workflows/windows-release.yml`, `.github/workflows/windows-compatibility.yml`, `.github/workflow-templates/private-runner-benchmark.yml.template` | Removed `FLUX_VERSION` env exports and updated benchmark template branding to Radium. |
| **C++ Core & UI** | 80 files in `src/` (including `src/common/types.hpp`, `src/common/logger.hpp`, `src/common/ipc_path.hpp`, `src/ui/win32_main_window.cpp`, `src/ipc/ipc_server.hpp/.cpp`, `src/core/download_controller.cpp`, `src/engine_stream/nm3u8dl_re_adapter.cpp`, `src/worker_stream/nm3u8dl_re_worker.cpp`, `src/brisk_engine/http_client.cpp`, `src/brisk_engine/http_download_engine.cpp`) | Renamed `namespace openidm` → `namespace radium`, `OPENIDM_LOG_*` → `RADIUM_LOG_*`, `WM_OPENIDM_*` → `WM_RADIUM_*`, removed duplicate `CreateFluxAppIcon`, and updated comments/generators to Radium Download Manager. |
| **Browser Extension** | `extension/catch-script/catch.js`, `extension/js/content-script.js`, `extension/js/background.js` | Replaced `data-flux-*` / `data-openidm-*` DOM attributes, `flux*` internal postMessage actions, `fluxType`, and `flux_*` session writes with `radium*` equivalents. |
| **Scripts & Tests** | `scripts/verify-ffmpeg-capabilities.ps1`, `scripts/verify-release-security.ps1`, `scripts/report-package-sizes.ps1`, `tests/cpp/unit_tests.cpp`, `tests/unit/core_unit_tests.cpp`, `tests/test_ipc_path.cpp`, `tests/test_extension_detection.js`, `tests/browser/test_extension_runtime.js`, `tests/browser/test_native_messaging_positive.js`, `tests/test_repair_pipeline_e2e.py`, `tests/verify_speed_display_dialog.py`, `tests/test_dist_portable.py`, `tests/test_core_operations.py`, `tests/run_all_integration_tests.py`, and C++ test/fuzz headers | Updated test assertions, binary lookups, log/temp names, and namespaces to Radium while keeping explicit migration/fallback tests intact. |
| **Documentation** | `docs/FEATURE_MATRIX.md`, `docs/UI_DESIGN_SYSTEM.md`, `docs/UPSTREAM_INTEGRATION_MAP.md`, `docs/WINDOWS_BUILD_AND_PACKAGING.md`, `docs/DOWNLOAD_ENGINE_DESIGN.md`, `docs/STREAM_DOWNLOAD_PIPELINE.md`, `docs/ERROR_HANDLING_AND_RECOVERY.md`, `docs/PRODUCT_REQUIREMENTS.md`, `docs/INDEX.md` | Updated active specifications to Radium identifiers and fixed duplicate `AppMutex` entry in `WINDOWS_BUILD_AND_PACKAGING.md`. |

---

## 4. Old Identifiers Renamed

- `namespace openidm` / `openidm::*` → `namespace radium` / `radium::*`
- `OPENIDM_LOG_DEBUG` / `INFO` / `WARN` / `ERROR` → `RADIUM_LOG_DEBUG` / `INFO` / `WARN` / `ERROR`
- `WM_OPENIDM_*` (`TRAYICON`, `SHOW_NEW_DOWNLOAD`, `SELECT_TORRENT_METADATA`, `PROBE_COMPLETE`, `TORRENT_FETCH_COMPLETE`, `SHOW_CONFLICT`, `TASK_NOTIFICATION`, `UPDATE_CHECK_COMPLETE`, `CHECK_FIRST_LAUNCH_ONBOARDING`, `CONFLICT_COMMIT_COMPLETE`) → `WM_RADIUM_*`
- `FLUX_VERSION_MAJOR` / `MINOR` / `PATCH` / `STRING` / `FLUX_PRODUCT_NAME` / `FLUX_FULL_VERSION_STRING` → removed (canonical `RADIUM_VERSION_*` used exclusively)
- CMake targets `flux`, `flux_core`, `flux-nmh`, `flux_unit_tests`, `flux_core_tests`, `openidm`, `openidm-nmh`, `openidm_unit_tests`, `openidm_core_tests` → removed (canonical `radium*` targets used exclusively)
- `CreateFluxAppIcon()` → removed duplicate (canonical `CreateRadiumAppIcon()` used)
- Extension DOM/message identifiers: `data-flux-shadow-host` / `data-openidm-shadow-host` → `data-radium-shadow-host`, `fluxOverlayAttached` → `radiumOverlayAttached`, `__FluxCatchTestHook` → `__RadiumCatchTestHook`, `fluxScanPageMedia` / `fluxGetBlobData` / `fluxBlobDataResponse` / `fluxDiscoverBlobSource` / `fluxAnchorClicked` / `fluxFetchBlobData` → `radium*` equivalents, `submitMediaCandidateToFlux` / `submitMediaCandidateToOpenIDM` → `submitMediaCandidateToRadium`, `transferBlobToFlux` → `transferBlobToRadium`, `fluxType` → `radiumType` (with read-only fallback in `nm3u8dl_re_adapter.cpp` / `nm3u8dl_re_worker.cpp`).

---

## 5. Backward-Compatibility Mechanisms Implemented & Preserved

All upgrade and compatibility paths required by existing `OpenIDM` and `Flux` installations remain intact:

1. **AppData & State Migration (`src/common/path_sanitizer.cpp`):** Automatically detects and migrates legacy `%LOCALAPPDATA%\Flux Download Manager`, `%LOCALAPPDATA%\OpenIDM`, `FLUX_DATA_DIR`, and `OPENIDM_DATA_DIR` into `%LOCALAPPDATA%\Radium Download Manager`.
2. **POSIX Key Derivation Seed (`src/common/crypto_utils.cpp`):** `OpenIDM_DPAPI_Equivalent_UserKey_v1_UID_` preserved verbatim per `AGENTS.md` §10.4 so encrypted credentials remain decryptable.
3. **IPC & Environment Overrides (`src/common/ipc_path.hpp`, `src/app/main.cpp`, `src/brisk_engine/http_client.cpp`, `src/worker_stream/nm3u8dl_re_worker.cpp`):** Evaluates `RADIUM_IPC_SOCKET` > `FLUX_IPC_SOCKET` > `OPENIDM_IPC_SOCKET`, `RADIUM_ALLOW_LOOPBACK` > `FLUX_ALLOW_LOOPBACK` > `OPENIDM_ALLOW_LOOPBACK`, and `RADIUM_HOST_RESOLVER_MAP` > `FLUX_HOST_RESOLVER_MAP`.
4. **Native Messaging Host Aliases (`config/radium_config.json`, `installer/radium.iss`, `installer/manifests/`, `scripts/register-native-messaging.ps1`, `extension/js/init.js`, `src/core/extension_installer.cpp`):** Registers `com.radium.downloadmanager` as primary while retaining `com.flux.downloadmanager` and `org.openidm.native` compatibility manifests (pointing to `radium-nmh.exe`) and `flux-extension@fluxdownloadmanager.org` in Firefox `allowed_extensions`.
5. **Single-Instance Upgrade Mutex (`installer/radium.iss`):** `AppMutex=Local\Radium_SingleInstance,Local\Flux_SingleInstance,Local\OpenIDM_SingleInstance`.
6. **Extension Session Migration (`extension/js/background.js`):** `restoreSessionState()` still reads legacy `flux_*` keys from `chrome.storage.local` to migrate them into `radium_*` keys, while `persistSessionState()` now writes only `radium_*` keys.
7. **Legacy Portable Binary Cleanup (`scripts/build-windows-msvc.ps1`):** Continues removing stale `Flux.exe` / `flux.exe` / `openidm.exe` from `dist/`.

---

## 6. Historical References Intentionally Retained

- `CHANGELOG.md`, `docs/archive/`, `docs/adr/`, `docs/SOURCE_MAPPING.md`, `docs/GLOSSARY.md`, `docs/RELEASE_AND_UPDATE_PROCESS.md`, `docs/ROADMAP.md`, `docs/TECHNICAL_RISK_REGISTER.md`, `docs/LICENSE_REVIEW.md`, `docs/TEST_MATRIX.md`, `docs/TEST_STRATEGY.md`, `docs/PERFORMANCE_AND_SIZE_BUDGET.md`, `docs/RADIUM_BRANDING_SPECIFICATION.md`, `docs/DATA_MODEL_AND_STORAGE.md`, `docs/KNOWN_LIMITATIONS.md`, `docs/NATIVE_MESSAGING_AND_IPC.md`, `THIRD_PARTY_NOTICES.md`, `README.md`: historical rebrand notes, ADR rationale, and compatibility documentation.
- `benchmarks/` and `scripts/run_real_internet_benchmark.py` / `scripts/run-competitor-benchmarks.py`: historical benchmark reports and logs from prior Flux benchmark runs.
- `idm audit/`: reverse-engineering analysis artifacts.
- `redesign/`: archived UI redesign snapshot directory (not part of the CMake build).

---

## 7. Unrelated References Intentionally Preserved

- **French & Portuguese technical terms (`docs/i18n/README.fr.md`, `docs/i18n/README.pt-BR.md`):** Ordinary words `flux` (French: "stream", e.g., *flux web*, *flux DASH*, *flux vidéo*) and `fluxos` (Portuguese: "streams", e.g., *Processamento de fluxos*).
- **IDM Ghidra disassembly symbols (`idm audit/ghidra/reports/`):** Symbols/strings containing `flux` within third-party IDM binaries.
- **License:** `Apache-2.0` license retained unchanged.

---

## 8. Remaining Legacy References Summary

Final case-sensitive occurrence counts across active code, build, extension, scripts, tests, and docs (`src/`, `extension/`, `config/`, `installer/`, `scripts/`, `tests/`, `docs/`, `.github/`, `CMakeLists.txt`, `README.md`, `THIRD_PARTY_NOTICES.md`):

| Search Term | Final Count | Notes |
| :--- | :---: | :--- |
| `OpenIDM` | 32 | Compatibility migration, `OpenIDM_DPAPI_Equivalent_UserKey_v1_UID_`, ADRs, historical/migration tests |
| `OpenIdm` | 0 | None remaining |
| `openidm` | 95 | `org.openidm.native` manifests/registry, `openidm_data` migration, compatibility tests |
| `OPENIDM` | 40 | `OPENIDM_IPC_SOCKET`, `OPENIDM_ALLOW_LOOPBACK`, `OPENIDM_DATA_DIR`, `OpenIDM_SingleInstance` |
| `Flux` | 125 | `Flux Download Manager` migration, `Local\Flux_SingleInstance`, benchmark scripts, historical docs |
| `FLUX` | 46 | `FLUX_IPC_SOCKET`, `FLUX_ALLOW_LOOPBACK`, `FLUX_DATA_DIR`, `FLUX_HOST_RESOLVER_MAP`, tests |
| `flux` | 345 | `com.flux.downloadmanager` manifests/scripts, `flux_*` storage migration, `fluxType` fallback, French/Portuguese `flux`/`fluxos`, benchmark scripts |

---

## 9. Tests and Build Checks Performed

All checks were executed **after** the final edit on Windows 11 x64 (MSVC 19.51.36252.0 / Windows SDK 10.0.26100.0):

1. **Config & Version Validation (`python scripts/generate_config.py --check`):** **PASSED** (all generated files match `VERSION` `1.0.0`).
2. **MSVC Release Build (`scripts/build-windows-msvc.ps1 -Incremental -SkipWorkerFetch`):**
   - Compiled `radium.exe`, `radium-nmh.exe`, `N_m3u8DL-RE.exe`, `radium_unit_tests.exe`, and `radium_core_tests.exe` with `/W4 /WX` with **0 errors**.
   - Verified PE32+ AMD64 headers and subsystems (`radium.exe` GUI, `radium-nmh.exe` CUI).
   - *Note:* Step `[6/7]` (copying to `dist/Radium.exe`) was skipped because an existing `dist\Radium.exe` instance is currently running on the host.
3. **Core Dedicated Unit Tests (`build\win-x64\Release\radium_core_tests.exe`):** **20 / 20 suites PASSED** (`CORE_EXIT=0`).
4. **C++20 Unit & Regression Tests (`build\win-x64\Release\radium_unit_tests.exe`):** **85 / 85 suites PASSED** (`UNIT_EXIT=0`), including `TestRadiumBrandingAndLegacyDataMigration` and `TestFailureNotificationPersistenceAndMigration`.
5. **Browser Extension Detection & Invariants (`node tests/test_extension_detection.js`):** **19 / 19 suites (28 / 28 edge scenarios) PASSED** (`EXT_EXIT=0`).

---

## 10. Unresolved Items

- Staging the newly built `radium.exe` into `dist\Radium.exe` requires closing the currently running `dist\Radium.exe` process first.
- Live Inno Setup installer (`installer/radium.iss`) compilation and Playwright browser runtime tests (`tests/browser/test_extension_runtime.js`) were not re-executed in this pass.
