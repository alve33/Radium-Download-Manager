# Tasks: Radium Accessibility 11/10 (A++ Gold Standard)

**Input**: Design documents from `/specs/003-mature-accessibility-a11y/`  
**Prerequisites**: plan.md (required), spec.md (required)  

## Phase 1: Setup & Settings Model

- [X] T016 Add `sound_cues_enabled` to `AppSettings` in src/common/types.hpp
- [X] T017 Expose Sound Cues in Settings Center under "Interface & Accessibility" in src/ui/win32_main_window.cpp

---

## Phase 2: Foundational (Auditory Feedback & Help Dialog)

- [X] T018 Implement `PlayAccessibilitySound(bool success)` with `PlaySoundW` in src/ui/win32_main_window.cpp
- [X] T019 Implement `ShowKeyboardShortcutsHelpWin32` modal dialog and wire `F1` / `?` in src/ui/win32_main_window.cpp

---

## Phase 3: Screen Reader Deep Task Row Announcements (11/10 Standard)

- [X] T020 Implement `FormatTaskAccessibleDescription` and `NotifyWinEvent` announcements for virtual row focus in src/ui/win32_main_window.cpp
- [X] T021 Hook task row selection changes to dispatch accessibility focus events

---

## Phase 4: WCAG 2.1 AAA Contrast Ratio Verification & Tests

- [X] T022 Implement relative luminance and contrast ratio calculation in src/ui/design_system.hpp
- [X] T023 Add unit tests verifying WCAG 2.1 AAA contrast ratios in tests/cpp/unit_tests.cpp
- [X] T024 Run full test suite and confirm 100% pass rate
