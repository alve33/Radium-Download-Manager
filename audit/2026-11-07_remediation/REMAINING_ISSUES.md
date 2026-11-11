# Radium Download Manager: Remaining Issues & Excluded Scope

**Date:** November 7, 2026  
**Auditor / Architect:** Principal Software Architect  
**Status:** Architecture Roadmap & Exclusions Reference

---

## 1. Strictly Excluded Findings (UI Appearance & Polish)

Per prompt directives, all visual UI redesigns and appearance alterations were strictly excluded to maintain design fidelity. These include:

1. **AUD-01: Main Window MSAA / UI Automation (`WM_GETOBJECT`):**
   - *Reason for Exclusion:* Modifies window message routing and accessibility tree representation of custom GDI controls.
2. **AUD-02: GDI Fonts and Pens Recreation in `WM_PAINT`:**
   - *Reason for Exclusion:* Optimization inside the visual rendering loop of `win32_main_window.cpp`.
3. **AUD-15: Speed Limiter & Scheduler Text Prompt Dialogs:**
   - *Reason for Exclusion:* Re-architecting prompt windows into new Win32 controls touches UI design. Existing dialog appearance was preserved.
4. **AUD-16: ZIP Preview Structured Tree Dialog:**
   - *Reason for Exclusion:* Replacing the current Win32 text dialog with a TreeView control is a UI redesign.
5. **AUD-21: Keyboard Navigation Tab Focus Rings:**
   - *Reason for Exclusion:* Visual focus indicators and border drawing alterations.
6. **AUD-22: UI Localization String Table (i18n):**
   - *Reason for Exclusion:* Requires broad string resource migration across all UI rendering code.
7. **AUD-25: Extension CSS Contrast Tokens:**
   - *Reason for Exclusion:* Involves modifying the visual color styling in `extension/css/tokens.css`.

---

## 2. Long-Term Architectural Enhancements (P3 Roadmap)

1. **AUD-14: FTPS Multi-Stream Segmented Downloading:**
   - *Current Status:* Plain FTP segmented downloads are supported; `ftps://` explicit TLS over data channels requires adding full OpenSSL/Schannel TLS state machine handshakes to `src/engine_ftp/ftp_client.cpp`.
   - *Roadmap Phase:* Phase 3 (Advanced Refinements).
2. **Direct2D 1.1 Hardware-Accelerated Rendering:**
   - *Current Status:* Native GDI double-buffered rendering operates stably with minimal CPU overhead. Direct2D vector graphics migration remains scheduled for Phase 3.
3. **Interactive WinRT Toast Notifications:**
   - *Current Status:* Standard Windows Shell Tray notification balloons (`NIF_INFO`) function reliably on Windows 10 and 11. Interactive action buttons via COM `IUserNotification` are targeted for Phase 3.
