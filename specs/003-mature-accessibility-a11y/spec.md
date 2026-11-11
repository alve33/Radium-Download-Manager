# Feature Specification: Radium Accessibility A++ (11/10 Category Gold Standard)

**Feature Branch**: `003-mature-accessibility-a11y`  
**Created**: 2026-11-07  
**Status**: Approved  
**Objective**: Elevate accessibility from 7.5/10 to **11/10 (A++ Gold Standard)** with comprehensive screen reader row-level accessibility, auditory completion cues, in-app keyboard cheat-sheet dialog, and contrast customization.

## User Scenarios & Testing

### User Story 1 - Screen Reader Virtual Row Navigation & State Announcements (Priority: P1)
As a blind or visually impaired user utilizing Windows Narrator or NVDA, when I arrow up and down through the custom download task list, I want the screen reader to announce each download's filename, status (Downloading, Paused, Completed, Error), transfer speed, progress percentage, and size so that the custom-rendered canvas has parity with a native accessible ListView.

**Independent Test**:
- Trigger focus change via arrow keys (`VK_UP` / `VK_DOWN`). Verify MSAA `NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, row_index + 1)` and `EVENT_OBJECT_NAMECHANGE` are fired.
- Verify screen reader accessibility query helper formats a rich accessible string for any task row: `"{filename}, {status}, {progress}%, {speed}, {downloaded_size} of {total_size}"`.

---

### User Story 2 - Keyboard Shortcuts Cheatsheet & Quick Help Modal (Priority: P1)
As a keyboard-only user, switch-device user, or new user, when I press `F1` or `?`, I want an accessible shortcut cheatsheet modal displaying all application shortcut keys, focus traversal chords, and bypass keys so I never need to hunt through settings or documentation.

**Independent Test**:
- Press `F1` or `Shift+/` (`?`) from the main window.
- Verify the modal pops up displaying keyboard shortcuts cleanly organized by function. Pressing `Escape` or `Enter` closes the modal and restores focus.

---

### User Story 3 - Auditory Cue Alerts for Download Completion & Network Failure (Priority: P2)
As a user with visual impairments or working with Radium in the background, when a long-running download completes or encounters a fatal network stall/error, I want an optional subtle audio chime (`PlaySoundW` with `SND_ALIAS | SND_ASYNC`) so that I am notified immediately without staring at visual indicators.

**Independent Test**:
- Toggle `sound_cues_enabled` setting in `AppSettings`.
- Trigger download completion or fatal error. Verify `PlaySoundW(L"Notification.Default", ...)` or `MessageBeep` is invoked cleanly in background.

---

### User Story 4 - Contrast Ratio Verification & Custom High-Contrast Text Enhancements (Priority: P2)
As a low-vision user, I want custom contrast ratio validation verifying all palettes exceed WCAG 2.1 AAA (7:1 for normal text, 4.5:1 for large text, 3:1 for interactive graphics).

**Independent Test**:
- Execute unit test suite verifying luminance calculation and contrast ratios `(L1 + 0.05) / (L2 + 0.05) >= 7.0` for `text_primary` against `background_main` across High Contrast, Dark, and Light palettes.

---

## Requirements

- **FR-A11Y-007**: Add `sound_cues_enabled` to `AppSettings` in `src/common/types.hpp` with default `true`, allowing users to disable sound cues if desired.
- **FR-A11Y-008**: Link `winmm.lib` in `CMakeLists.txt` for asynchronous OS sound notifications (`PlaySoundW` / `MessageBeep`).
- **FR-A11Y-009**: Implement `ShowKeyboardShortcutsHelpWin32(HWND hwnd_parent, const DesktopView& view)` in `src/ui/win32_main_window.cpp` triggered by `VK_F1` or `?` (`Shift+OEM_2`).
- **FR-A11Y-010**: Enhance `win32_main_window.cpp` with `NotifyTaskRowFocusChanged(HWND hwnd, size_t row_index, const DownloadTask& task)` dispatching `NotifyWinEvent(EVENT_OBJECT_FOCUS, ...)` and `EVENT_OBJECT_NAMECHANGE` with full accessible status speech strings.
- **FR-A11Y-011**: Add luminance and WCAG 2.1 AAA contrast verification tests in `tests/cpp/unit_tests.cpp`.
