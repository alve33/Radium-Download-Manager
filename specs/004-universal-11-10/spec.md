# Feature Specification: Universal 11/10 Polish and Production Quality Elevation

**Feature Branch**: `004-universal-11-10`  
**Created**: 2026-11-07  
**Status**: Approved  
**Objective**: Elevate Radium across all product dimensions from commercial viability (7.5/10) to benchmark-setting **11/10 (A++ Gold Standard)** by remediating every identified gap from the product gap audit:
1. **Automated In-App Update Delivery & Integrity Verification**: Background update check engine with semver comparison, manifest parsing, and SHA-256 binary validation.
2. **True Portable Mode Flag**: Detection of `radium.portable` or `portable.dat` in application directory redirecting all SQLite databases, worker tools, settings, and logs into a self-contained local `data/` folder.
3. **Settings Backup, Restore, and Reset**: Clean JSON export/import of user configuration with schema migration, credential preservation, and factory-reset capabilities.
4. **Comprehensive Accessibility (A11y)**: Screen reader announcements (`WM_GETOBJECT`, MSAA / UI Automation virtual rows), keyboard shortcuts help modal (`F1`/`?`), and auditory cues for download completion and failures.
5. **Modern Windows Notification System**: Interactive notification triggers with fallback between shell notification icons and action alerts.

---

## User Scenarios & Verification Requirements

### User Story 1 - In-App Update Engine & Integrity Verification (Priority: P1)
As a user running Radium, I want the application to safely check for updates against official release manifests, compare versions, display release notes, and verify release package SHA-256 hashes before installation so that my installation stays current, secure, and untampered.

**Independent Verification**:
- Add `UpdateManager` with `CheckForUpdatesAsync(current_version, manifest_url)` and `VerifyPackageSha256(file_path, expected_hash)`.
- Unit test version comparison (`1.0.0` < `1.0.1`, `1.0.0-beta` < `1.0.0`).
- Unit test SHA-256 verification against matching and corrupted test payloads.

---

### User Story 2 - Self-Contained Portable Mode (Priority: P1)
As a user running Radium from a USB thumb drive or isolated directory, when a `radium.portable` marker file exists adjacent to `radium.exe`, I want all user databases, temporary files, downloaded worker binaries, and configuration files to reside strictly in `./data/` instead of `%LOCALAPPDATA%`, leaving zero footprint on the host system.

**Independent Verification**:
- Verify `ResolveDataRootDirectory(exe_dir)` inspects `exe_dir / "radium.portable"`.
- If present, returns `exe_dir / "data"`. If absent, defaults to `%LOCALAPPDATA%\Radium Download Manager`.
- Test directory creation and sandboxing in regression tests.

---

### User Story 3 - Settings Export, Import, and Factory Reset (Priority: P2)
As an advanced user managing multiple workstations, I want to export my configured queues, speed limits, download paths, and UI theme to a clean JSON backup file, and import it on another machine with validation and safe rollback on error.

**Independent Verification**:
- `ExportSettingsToJson(const AppSettings& s)` -> valid JSON string.
- `ImportSettingsFromJson(const std::string& json)` -> `Result<AppSettings>`.
- Invalid or corrupted JSON returns descriptive error without corrupting active settings.

---

### User Story 4 - Audio Notifications & Keyboard Shortcuts Cheatsheet (Priority: P2)
As a keyboard-first or visually impaired user, I want auditory alerts on completion/error and an accessible `F1` shortcut cheat-sheet.

**Independent Verification**:
- Add `sound_cues_enabled` to `AppSettings`.
- Verify `ShowKeyboardShortcutsHelpWin32` modal displays organized keybindings.
- Ensure audio cue dispatch executes asynchronously without blocking the UI thread.
