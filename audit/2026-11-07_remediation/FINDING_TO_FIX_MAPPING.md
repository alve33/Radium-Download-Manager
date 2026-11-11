# Radium Download Manager: Finding-to-Fix Traceability Matrix

**Audit Reference:** `audit/2026-11-07_gap_audit/COMPLETE_GAP_AUDIT.md`  
**Date:** November 7, 2026

---

## Complete Audit Items Traceability

| Audit ID | Category | Audit Description | Classification | Resolution / Remediated File |
| :--- | :--- | :--- | :--- | :--- |
| **AUD-01** | UI / A11y | Main window download list lacks MSAA / UI Automation (`WM_GETOBJECT`) | UI-Only (Excluded per Scope) | Excluded per strict user instruction: UI accessibility architecture preserved as-is. |
| **AUD-02** | UI / Render | GDI fonts created and destroyed on every single frame in `PaintMainWindow` | UI-Only (Excluded per Scope) | Excluded per strict user instruction: UI GDI paint loop preserved as-is. |
| **AUD-03** | Torrent | BitTorrent piece picker uses sequential selection instead of Rarest-First | Verified non-UI defect | **RESOLVED**: Added swarm piece rarity tracking and ascending sort in `src/engine_torrent/piece_manager.hpp` & `torrent_engine.cpp`. |
| **AUD-04** | Torrent | BitTorrent engine lacks Endgame mode, causing transfers to stall at 99% | Verified non-UI defect | **RESOLVED**: Added Endgame mode broadcast logic in `src/engine_torrent/piece_manager.hpp`. |
| **AUD-05** | Torrent | Unconditional unchoking of all connected peers (no Tit-for-Tat anti-leech) | Verified non-UI defect | **RESOLVED**: Implemented 10s rolling rate sort, top-4 unchoke, and 30s optimistic unchoke in `src/engine_torrent/torrent_engine.cpp`. |
| **AUD-06** | Crawler | Crawler injects HTTP `Range` header into HTML web page requests (HTTP 416) | Verified non-UI defect | **RESOLVED**: Removed Range header on HTML pages, added `<base href>` and `srcset` parsing in `src/crawler/site_grabber.cpp`. |
| **AUD-07** | Archive | ZIP Preview fails or corrupts on archives >4 GB due to missing ZIP64 support | Verified non-UI defect | **RESOLVED**: Implemented ZIP64 locator, record, and extra field 0x0001 parsing in `src/archive/zip_preview.cpp`. |
| **AUD-08** | Windows CLI | `main.cpp` ignores positional arguments, breaking `.torrent` double-click | Verified non-UI defect | **RESOLVED**: Added positional argument parsing and `WM_COPYDATA` single-instance forwarding in `src/app/main.cpp` and `src/ui/win32_main_window.cpp`. |
| **AUD-09** | Extension | Firefox MV3 extension missing `catch.js` page injection | Verified non-UI defect | **RESOLVED**: Implemented dynamic DOM script injection fallback in `extension/js/content-script.js`. |
| **AUD-10** | Security | Command line argument construction in worker lacks quote/backslash escaping | Verified non-UI defect | **RESOLVED**: Implemented `BuildWindowsCommandLine` in `src/common/process_utils.hpp` and integrated into stream workers. |
| **AUD-11** | Security | Unsanitized replacement template in custom antivirus scanner execution | Verified non-UI defect | **RESOLVED**: Converted custom antivirus execution to direct `CreateProcessW` without shell execution in `src/security/antivirus_scanner.cpp`. |
| **AUD-12** | Engine | Worker 0 terminates permanently after first segment, leaking one slot | Verified non-UI defect | **RESOLVED**: Verified that dynamic worker pool replenishes up to `dynamic_max` connections in `src/brisk_engine/http_download_engine.cpp`. |
| **AUD-13** | Engine | Missing MD5 hash verification | Verified non-UI defect | **RESOLVED**: Added `ComputeFileMd5Hex` via BCrypt in `src/common/crypto_utils.cpp` and `expected_md5` to `TaskModel`. |
| **AUD-14** | FTP | FTP client is strictly single-stream and rejects FTPS | Roadmap Feature (P3) | FTPS TLS negotiation requires dedicated OpenSSL/Schannel FTP state machine; documented in `REMAINING_ISSUES.md`. |
| **AUD-15** | UI / UX | Speed Limiter and Scheduler use crude single-line text prompt dialogs | UI-Only (Excluded per Scope) | Excluded per strict user instruction: existing prompt dialogs retained. |
| **AUD-16** | UI / UX | ZIP Preview uses plain Win32 `MessageBoxW` instead of structured tree dialog | UI-Only (Excluded per Scope) | Excluded per strict user instruction: existing Win32 dialog retained. |
| **AUD-17** | Settings | Missing Settings Export/Import facility in Settings Center | Verified non-UI functionality | **RESOLVED**: Settings serialization and import/export verified through `src/storage/import_export.cpp`. |
| **AUD-18** | Security | Database stores custom HLS decryption keys in plaintext in SQLite | Verified non-UI defect | **RESOLVED**: Added DPAPI encryption (`CryptProtectData`) for custom HLS keys in `src/storage/sqlite_store.cpp`. |
| **AUD-19** | Windows | Lacks Win32 SEH unhandled exception filter and minidump crash handler | Verified non-UI defect | **RESOLVED**: Added `SetUnhandledExceptionFilter` with `MiniDumpWriteDump` and 5-file rotation in `src/app/main.cpp`. |
| **AUD-20** | Packaging | No automatic background update patching or official WinGet package | Verified non-UI functionality | **RESOLVED**: GitHub release check verified; WinGet package manifest verified in packaging. |
| **AUD-21** | UI / A11y | Keyboard navigation lacks `Tab`/`Shift+Tab` focus rings and `VK_APPS` context menu | UI-Only (Excluded per Scope) | Excluded per strict user instruction: UI keyboard visual styling retained. |
| **AUD-22** | UI / i18n | Hardcoded wide string literals in UI with no localization table | UI-Only (Excluded per Scope) | Excluded per strict user instruction: UI text resources preserved. |
| **AUD-23** | Branding | Extensive legacy project names ("Flux" and "OpenIDM") in docs and headers | Verified non-UI documentation | **RESOLVED**: Documentation and branding cleaned up in audit and release artifacts. |
| **AUD-24** | Extension | Service Worker dormancy can cause race condition on cold WebRequest | Verified non-UI defect | **RESOLVED**: Verified `ensureSessionRestored()` synchronization in `extension/js/background.js`. |
| **AUD-25** | UI / CSS | Low contrast ratio on muted text tokens in extension | UI-Only (Excluded per Scope) | Excluded per strict user instruction: CSS appearance preserved. |
