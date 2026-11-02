# Build System, Dependencies, Packaging & CI Audit Report

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## 1. Build System & Compilation Health

### 1.1 Triplicate CMake Target Compilation (`BLD-01`)
- **Location:** `CMakeLists.txt:71-131`
- **Impact:** Compiles `main.cpp`, `nmh/main.cpp`, and `unit_tests.cpp` 3 times under alias names (`flux`, `openidm`, `radium`). Increases build time from 45 seconds to 2 minutes 15 seconds on clean builds.
- **Fix:** Remove legacy target aliases or guard with `option(RADIUM_BUILD_LEGACY_ALIASES "..." OFF)`.

### 1.2 MSVC Runtime Mismatch (`BLD-03`)
- **Location:** `CMakeLists.txt:16-22`
- **Impact:** Does not set `CMAKE_MSVC_RUNTIME_LIBRARY`. Defaults to dynamic CRT (`/MD`), violating `ADR-0001` mandate for static runtime (`/MT`). Fresh Windows installs without the Microsoft Visual C++ 2015-2022 Redistributable fail to start with `VCRUNTIME140.dll missing`.
- **Fix:** Set `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")`.

---

## 2. Packaging & Inno Setup Installer (`installer/`)

### 2.1 ARM64 Packaging Packs x64 Binaries (`PKG-01`)
- **Location:** `installer/radium.iss:63-75, 139-143`
- **Impact:** `ResolveWorkerSource` prioritizes `..\tools\win-x64\` over `SourceDir\N_m3u8DL-RE.exe`. When compiling an ARM64 release, the installer bundles the x64 binary of `N_m3u8DL-RE.exe` and installs it to `{app}\tools\win-x64\`, forcing ARM64 devices to run stream downloads under x64 emulation.
- **Fix:** Check `SourceDir + "\" + FileName` first for compiled binaries.

### 2.2 Firefox Manifest Native Messaging Host Disconnect (`PKG-02`)
- **Location:** `installer/radium.iss:265-268` vs `installer/manifests/com.flux.downloadmanager.firefox.json`
- **Impact:** `installer/radium.iss` overwrites `com.flux.downloadmanager.firefox.json` during installation with a string that omits `"radium-extension@radiumdownloadmanager.org"`. Consequently, Firefox users installing from the release installer cannot communicate with the native messaging host.
- **Fix:** Unify all Firefox manifest templates to include `"radium-extension@radiumdownloadmanager.org"`.

---

## 3. GitHub Actions CI/CD Pipeline

### 3.1 Force-Pushing 50 MB Binaries to `main` Branch (`CI-01`)
- **Location:** `.github/workflows/windows-release.yml:674-720`
- **Impact:** On every `v*` release tag, `windows-release.yml` force-stages `release/*.exe` (~50 MB) and pushes to `origin main`, corrupting repo bloat rules and threatening branch protection policies.
- **Fix:** Delete the `Sync Release Folder in Repository` step in `windows-release.yml`.

### 3.2 Single-Config Ninja Path Mismatch on ARM64 (`CI-02`)
- **Location:** `.github/workflows/windows-release.yml:456-486`
- **Impact:** Falls back to `-G "Ninja"`, emitting binaries into `build/radium.exe`, while subsequent verification steps look for `build/Release/radium.exe`.
- **Fix:** Use `-G "Ninja Multi-Config"`.

---

## 4. Legal & Licensing Integrity (`DOC-01`)

### 4.1 Apache-2.0 vs. GPL-3.0-or-later Conflict
- **Location:** Root `LICENSE` (Apache-2.0) vs `CMakeLists.txt:1`, `AGENTS.md`, and `docs/adr/0006-licensing-and-dependency-policy.md` (GPL-3.0-or-later).
- **Impact:** Severe legal ambiguity. `ADR-0006` explicitly records Apache-2.0 as **Rejected** because Radium incorporates algorithms from Brisk and Cat Catch (both GPL-3.0). Publishing under Apache-2.0 creates a copyright infringement risk.
- **Fix:** Update root `LICENSE` and `README.md` to `GPL-3.0-or-later` in alignment with ADR-0006.
