# Radium Download Manager: Detailed UI & UX Audit

**Date:** November 7, 2026  
**Audited Components:** `src/ui/win32_main_window.cpp` (10,248 lines), `src/ui/design_system.hpp`, `src/ui/startup_log_dialog.cpp`, `extension/popup.html`, `extension/css/popup.css`, `extension/js/popup.js`.

---

## 1. Executive Summary: UI & UX Standing

Radium adopts a native Win32 user interface styled with modern design tokens ("Coral Dark" and "Cosmic Void"). It avoids heavy web runtime wrappers (Electron/Chromium Embedded Framework) while adopting Windows 11 design idioms: dark mode title bars, Mica backdrops, rounded window geometry, and smooth double-buffered client rendering.

However, an engineering audit reveals that the UI is caught between two worlds:
1. **The Custom Canvas:** The main download table is entirely custom-drawn using software GDI `BitBlt` double-buffering. It lacks native Win32 controls, resulting in a **total absence of Windows Accessibility (MSAA / UI Automation)** and screen reader support.
2. **The Fragmented Dialog Paradigm:** Dialogs are inconsistently split between custom Win32 classes (`Radium_NewDownloadDlgClass`), client-area in-canvas overlays (`PaintSettingsCenterOverlayWin32`), primitive single-line string prompts (`ShowSimpleInputDialogWin32`), and standard non-dark Windows `MessageBoxW` dialogs.

---

## 2. In-Depth UI/UX Findings & Deficiencies

### 2.1 Rendering Pipeline & Performance Overhead
- **Location:** `src/ui/win32_main_window.cpp:7953–7968`, `8631–8635`
- **Issue: GDI Font Allocation Churn:** Inside `PaintMainWindow`, four Segoe UI font handles (`hFontBody`, `hFontBold`, `hFontSmall`, `hFontSmallBold`) are created via `CreateFontW` and freed via `DeleteObject` **on every single paint message**.
  - During active downloads updating at 100–250 ms, or rapid cursor movement across rows, the application repeatedly allocates and deallocates GDI handles from the Windows GDI heap.
  - *Recommendation:* Cache these font handles in `Win32WindowContext` and invalidate them only on `WM_DPICHANGED`.
- **Issue: Software GDI Rasterization:** Although documentation references "Direct2D/GDI", the actual rendering is 100% software GDI (`CreateCompatibleBitmap`, `FillRectSolid`, `BitBlt`). GDI lacks subpixel anti-aliasing for curves and fractional scaling, causing visible stair-stepping on rounded borders at 125% and 175% Windows display scaling.

### 2.2 Accessibility & Screen Reader Blackout
- **Location:** `src/ui/win32_main_window.cpp:10126` (missing `WM_GETOBJECT`)
- **Issue: No Screen Reader Node Exposure:**
  - Standard Windows software exposes accessibility trees to Windows Narrator, NVDA, and JAWS via `WM_GETOBJECT` returning an `IAccessible` or `IRawElementProviderSimple` interface.
  - Because Radium draws rows manually on a raw `HWND` canvas without responding to `WM_GETOBJECT`, screen readers identify the main table as a completely blank rectangle.
  - Blind or visually impaired users have no means of querying active downloads, checking percentages, or pausing/resuming tasks.

### 2.3 Keyboard Navigation Deficiencies
- **Location:** `src/ui/win32_main_window.cpp:9971–10009` (`WM_KEYDOWN`)
- **Issue:**
  1. `Tab` and `Shift+Tab` do not cycle focus between the Sidebar Categories, Search Bar, Download Table, and Details Panel.
  2. The Sidebar Categories (`All`, `Downloading`, `Completed`, `Video`, `Torrents`, etc.) can only be selected with a mouse; there are no keyboard shortcuts (e.g. `Ctrl+1..Ctrl+8`) or arrow key selection when focused.
  3. The item context menu cannot be opened using the keyboard `Apps` menu key or `Shift+F10`; it is only triggered via `WM_RBUTTONUP` mouse coordinates (`win32_main_window.cpp:9300`).

### 2.4 Dialog Design & Visual Inconsistencies
1. **Settings Center In-Canvas Overlay (`win32_main_window.cpp:8582`):**
   - The Settings Center is painted directly over the main window canvas rather than opening in a dedicated modal or top-level window.
   - Child controls (such as `hSearchEdit`) can poke through or require complex manual visibility toggling, and closing the overlay requires clicking an in-canvas "Done" button rather than pressing `Escape`.
2. **Primitive Text Prompt Dialogs (`win32_main_window.cpp:9472, 9493`):**
   - The **Speed Limiter** dialog asks the user to manually type an integer in a single-line edit control (`"Enter maximum speed in KB/s"`).
   - The **Scheduler** dialog asks the user to manually type a string formatted as `"HH:MM"`.
   - *Commercial Standard:* IDM and FDM provide sliders, preset speed buttons (512 KB/s, 1 MB/s, 5 MB/s, Unlimited), and native date/time controls.
3. **ZIP Preview & Hash Inspection via `MessageBoxW` (`win32_main_window.cpp:9513, 9573`):**
   - Inspecting a ZIP archive or checking a file's SHA-256 hash opens a standard Windows `MessageBoxW`.
   - The dialog is white and non-dark, jarringly breaking the dark aesthetic.
   - For archives with more than 30 files, the message box text extends beyond the screen boundary, and file paths cannot be copied or selectively extracted.

### 2.5 Browser Extension UI (`popup.html`, `popup.css`, `popup.js`)
- **Sticky Header Missing on Long Stream Lists:** When inspecting media-heavy pages (e.g., streaming sites loading dozens of HLS audio/video playlists), the popup list scrolls vertically, pushing the category tabs and filter search bar out of view.
- **Contrast Ratio Violation:** In `extension/css/popup.css:21`, `--text-muted` is set to `#64748B` against background `#242528`. The resulting contrast ratio is ~3.1:1, failing the WCAG 2.1 AA requirement of 4.5:1 for readable caption text.

---

## 3. UI/UX Scorecard

| Subsystem | Score | Critical Notes |
| :--- | :---: | :--- |
| **Color & Theme Harmony** | 8.0 / 10 | Cohesive dark palette and Mica support; broken by white `MessageBoxW`. |
| **Typography & Spacing** | 7.5 / 10 | Clean Segoe UI ramp; occasional tight padding at 200%+ DPI scaling. |
| **Animation & Feedback** | 8.0 / 10 | Smooth action pulses, hover highlights, and respectful `reducedMotion` toggle. |
| **Dialog Architecture** | 5.5 / 10 | Mixed overlay/modal paradigm; primitive string prompts. |
| **Accessibility (Screen Reader)**| **1.0 / 10** | Missing `WM_GETOBJECT`; completely invisible to screen readers. |
| **Keyboard Navigation** | 5.0 / 10 | Arrow keys work in table; lacks `Tab` traversal and `Shift+F10` menu. |
| **Browser Extension Popup** | 8.5 / 10 | Intuitive media detection list with quality badges and filters. |
