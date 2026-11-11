# Radium Download Manager — UX & User Interface Audit

**Date:** November 7, 2026  
**Auditor:** Senior Product & UI/UX Audit Team  
**Evaluation Target:** Radium Win32 Desktop Application & Manifest V3 Browser Extension

---

## 1. Design System & Visual Polish Overview

Radium utilizes a custom double-buffered GDI rendering architecture defined in `src/ui/design_system.hpp` and `src/ui/win32_main_window.cpp`. The visual aesthetic follows a modern dark/coral theme inspired by Windows 11 Fluent Design principles, featuring 1-pixel borders, rounded control cards, distinct category icon badges, and smooth progress bar fills.

In static appearance and high-DPI scaling (tested across 100%, 125%, 150%, and 200% DPI), the UI presents a sharp, clutter-free layout that compares favorably to IDM's outdated Windows 2000 look.

However, a deep audit of interaction workflows, dialog states, and control behaviors reveals several significant UX shortcomings.

---

## 2. Detailed UX Deficiencies & Gaps

---

### UX-01: Inconsistent Modal Dialog Patterns (Text Prompts vs Rich Custom UI)
- **Location:** [`src/ui/win32_main_window.cpp:4500-4750`](file:///c:/Users/sayed/Downloads/Radium/src/ui/win32_main_window.cpp#L4500-L4750)
- **Problem Description:**
  While the primary window and New Download dialog use custom GDI-rendered card components with coral accent styling, secondary configuration workflows (such as setting speed limits or configuring the queue scheduler) revert to raw Windows prompt dialogs with plain text edit boxes.
- **Why This Breaks UX:**
  Users are suddenly confronted with an abrupt visual and interaction mismatch. Entering a speed limit requires typing a raw number or string like `5000000` rather than adjusting a slider with readable unit badges (`5 MB/s`).
- **Severity:** Medium
- **Recommended Fix:** Build consistent coral-styled Win32 child modal dialogs equipped with TrackBar controls, preset speed pills (`1 MB/s`, `5 MB/s`, `10 MB/s`, `Unlimited`), and intuitive time pickers.

---

### UX-02: Non-Interactive ZIP Archive Preview
- **Location:** [`src/archive/zip_preview.cpp`](file:///c:/Users/sayed/Downloads/Radium/src/archive/zip_preview.cpp), [`src/ui/win32_main_window.cpp`](file:///c:/Users/sayed/Downloads/Radium/src/ui/win32_main_window.cpp)
- **Problem Description:**
  When a user invokes "Preview ZIP Archive" on a remote download task or local file, Radium queries the central directory and prints the files into a single read-only multi-line text edit box (`ES_MULTILINE | ES_READONLY`).
- **Why This Breaks UX:**
  Users cannot collapse or expand subdirectories, click on column headers to sort by file size or date, or check specific files to download only a single file from a 10 GB archive. It functions merely as a "text viewer" rather than an actionable archive inspector.
- **Severity:** Medium
- **Recommended Fix:** Replace the text box with a Win32 TreeView (`WC_TREEVIEWW`) control featuring folder/file glyphs, byte sizes, and checkboxes for partial download range extraction.

---

### UX-03: Lack of List Customization (Column Reordering & Visibility Toggles)
- **Location:** [`src/ui/win32_main_window.cpp:3400-3600`](file:///c:/Users/sayed/Downloads/Radium/src/ui/win32_main_window.cpp#L3400-L3600)
- **Problem Description:**
  The main download list has a hardcoded column structure: `File Name`, `Size`, `Progress`, `Speed`, `Time Left`, `Status`, `Date Added`.
- **Why This Breaks UX:**
  Users with compact screens cannot hide unnecessary columns (such as "Date Added" or "Time Left") to make room for long filenames. Power users cannot add technical columns such as "Connections", "SHA-256", "Average Speed", or "Save Directory". Additionally, columns cannot be dragged to rearrange their order.
- **Severity:** Low
- **Recommended Fix:** Enable `HDS_DRAGDROP` on the Win32 header control, implement a right-click column context menu with checkboxes, and persist column order and visibility in the SQLite database.

---

### UX-04: Video Overlay Button Event Contention & Drag Usability
- **Location:** [`extension/catch-script/catch.js`](file:///c:/Users/sayed/Downloads/Radium/extension/catch-script/catch.js#L120-L180), [`extension/js/content-script.js`](file:///c:/Users/sayed/Downloads/Radium/extension/js/content-script.js)
- **Problem Description:**
  The in-page video download overlay injects a floating pill above HTML5 video elements. While recent commits isolated the drag handle from the download click handler, moving the overlay across complex nested video players (such as custom player controls with high `z-index` overlays) can occasionally cause the overlay to lose pointer capture or get obscured by full-page player wrappers.
- **Why This Breaks UX:**
  Users watching streaming videos in full-window theater modes occasionally see the download button occluded behind custom player overlays.
- **Severity:** Low
- **Recommended Fix:** Enforce a strict `z-index: 2147483647 !important;` on the shadow host and hook the browser fullscreen change event (`fullscreenchange`) to re-anchor the button dynamically to the top-right corner of the fullscreen viewport.

---

### UX-05: Monolingual English Interface (Global Market Barrier)
- **Location:** Entire desktop codebase (`src/ui/win32_main_window.cpp`)
- **Problem Description:**
  All user interface strings, buttons, tooltips, dialogs, and error messages are hardcoded as English string literals.
- **Why This Breaks UX:**
  Over 60% of international download manager users reside in non-English speaking markets. A commercial tool cannot achieve global consumer adoption without localization.
- **Severity:** Medium
- **Recommended Fix:** Externalize all UI text into a structured JSON or `.rc` String Table, enabling crowdsourced community translations for German, Spanish, French, Portuguese, Russian, Simplified Chinese, Japanese, and Arabic.

---

### UX-06: Task Status Detail Readability & Truncation
- **Location:** [`src/ui/win32_main_window.cpp:1800-1920`](file:///c:/Users/sayed/Downloads/Radium/src/ui/win32_main_window.cpp#L1800-L1920)
- **Problem Description:**
  When a download encounters an error (such as HTTP 403 Forbidden, 416 Range Not Satisfiable, or SSL Handshake Failure), the detailed error string is displayed in the "Status" column. Because the status column width is limited, the error description is clipped with an ellipsis (`...`) without a rich tooltip or dedicated error review window.
- **Why This Breaks UX:**
  Users cannot easily read the complete diagnostic message or understand whether the error was due to an expired session token, a disk full condition, or an unreachable host.
- **Severity:** Low
- **Recommended Fix:** Implement a tooltip on hover over failed tasks that displays the full `status_detail` and `error_code`, along with a "View Error Log / Help" action in the context menu.
