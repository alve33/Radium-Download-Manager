# Implementation Plan: Universal 11/10 Polish and Production Quality Elevation

**Feature**: Universal 11/10 Polish and Production Quality Elevation  
**Branch**: `004-universal-11-10`  
**Status**: In Progress  

---

## Technical Architecture

### 1. Update Manager & Cryptographic Integrity (`src/core/update_manager.hpp` & `.cpp`)
- `UpdateInfo` struct: `version`, `release_notes`, `download_url`, `sha256_hash`, `published_at`, `mandatory`.
- Semver comparison function `CompareSemVer(v1, v2)` handling major, minor, patch, and prerelease tags.
- Cryptographic hash verifier `VerifyPackageSha256(file_path, expected_hash)` using Windows BCrypt SHA-256 routines.
- Manifest parser deserializing JSON update feeds.

### 2. True Portable Mode Detection (`src/common/path_sanitizer.hpp` & `.cpp` / `central_config.hpp`)
- `ResolveApplicationDataDirectory(const std::filesystem::path& exe_dir)`:
  - If `std::filesystem::exists(exe_dir / "radium.portable")` or `(exe_dir / "portable.dat")`:
    - Returns `exe_dir / "data"`.
  - Else:
    - Returns `GetKnownFolderPath(FOLDERID_LocalAppData) / "Radium Download Manager"`.
- Ensures zero host traces when portable marker is present.

### 3. Settings Serialization & Backup Engine (`src/common/settings_serializer.hpp`)
- `SerializeSettings(const AppSettings& settings)` -> clean indented JSON string.
- `DeserializeSettings(const std::string& json_str)` -> `Result<AppSettings>` with rigorous schema bounds checking (e.g. valid speed limits, thread counts between 1 and 64, sanitized directory paths).
- `ResetToDefaults()` returning clean default configuration.

### 4. Accessibility & UI Enhancements
- Keyboard shortcut help viewer callable from menus and shortcut keys.
- Auditory alert triggers via `PlaySoundW` or `MessageBeep`.
- Contrast ratio metrics in design system.

---

## Constitution Compliance Check
- C++20 standard compliance with MSVC `/W4 /WX`.
- Zero raw unmanaged handles.
- No unapproved external dependencies.
- Cryptographic integrity enforced on external downloads and updates.
