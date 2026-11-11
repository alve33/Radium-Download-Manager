# Implementation Plan: Elevating Radium Accessibility to 11/10 (A++ Gold Standard)

**Feature**: Mature Accessibility (A11y) to 11/10  
**Branch**: `003-mature-accessibility-a11y`  
**Status**: In Progress  

## Technical Architecture

1. **Screen Reader Accessible Row Announcements**:
   - Helper function `FormatTaskAccessibleDescription(const DownloadTask& task)` in `src/ui/win32_main_window.cpp` producing rich formatted text (e.g. `"ubuntu-24.04.iso, Downloading, 74%, 12.4 MB/s, 1.8 GiB of 2.4 GiB"`).
   - In `HandleKeyShortcut("VK_UP")` and `HandleKeyShortcut("VK_DOWN")`, trigger `NotifyWinEvent(EVENT_OBJECT_FOCUS, hwnd, OBJID_CLIENT, index + 1)` and `NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, hwnd, OBJID_CLIENT, index + 1)`.

2. **Keyboard Help & Shortcut Cheatsheet Dialog**:
   - `ShowKeyboardShortcutsHelpWin32(HWND hwnd_parent, const DesktopView& view)` built using native Win32 dialog proc with high-contrast palette support, Esc/Enter dismissal, and clean tabular layout.
   - Wired to `VK_F1` and `?` keychord in `WM_KEYDOWN`.

3. **Auditory Cue Notifications**:
   - Add `bool sound_cues_enabled{true};` to `AppSettings` in `src/common/types.hpp`.
   - Add sound setting to `GetSettingsCenterView()` under "Interface & Accessibility".
   - Helper `PlayAccessibilitySound(bool success)` in `src/ui/win32_main_window.cpp` calling `PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC)`.

4. **WCAG 2.1 AAA Contrast Ratio Verification**:
   - Implement luminance computation: `CalculateLuminance(const ColorRgba& c)` and `CalculateContrastRatio(const ColorRgba& c1, const ColorRgba& c2)`.
   - Unit tests in `tests/cpp/unit_tests.cpp` asserting contrast ratios exceed WCAG AAA standards.
