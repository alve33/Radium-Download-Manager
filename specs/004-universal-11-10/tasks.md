# Tasks: Universal 11/10 Polish and Production Quality Elevation

**Input**: Design documents from `specs/004-universal-11-10/` (`spec.md`, `plan.md`)  
**Status**: Implemented & Verified (11/10 Standard)

---

## Phase 1: Update Manager & Cryptographic Integrity Verification

- [x] T025 [P1] Create header `src/core/update_manager.hpp` defining `UpdateInfo`, `SemVer`, `UpdateCheckResult`, and `UpdateManager`.
- [x] T026 [P1] Implement `src/core/update_manager.cpp` with SemVer parsing/comparison, JSON update manifest parsing, and BCrypt SHA-256 package hash verification.
- [x] T027 [P1] Add `update_manager.cpp` to `CMakeLists.txt` (`RADIUM_CORE_SOURCES`).

---

## Phase 2: True Portable Mode & Settings Backup/Restore

- [x] T028 [P1] Implement portable mode detection helper in `src/common/path_sanitizer.hpp` and `.cpp` (`IsPortableMode`).
- [x] T029 [P2] Implement `src/common/settings_serializer.hpp` for JSON export, import validation, and factory reset.
- [x] T030 [P2] Support self-contained JSON configuration backup and restore.

---

## Phase 3: Verification & Regression Testing

- [x] T031 Add unit test suite 79 in `tests/cpp/unit_tests.cpp` covering:
  - SemVer parsing, ordering, and update eligibility detection.
  - SHA-256 package verification (valid vs tampered).
  - Portable mode directory resolution with and without marker files.
  - Settings JSON serialization, deserialization, and schema validation.
- [x] T032 Run MSVC build pipeline and confirm all 79 unit test suites, 5 core test suites, and 10 end-to-end fixtures pass with 0 errors.
