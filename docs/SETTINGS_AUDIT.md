# Settings Lifecycle Audit

**Reviewed:** 2026-11-01  
**Scope:** Visible controls in the Win32 Settings Center. Values are loaded from and saved to the SQLite `settings` table as the `app_settings` JSON object by `DownloadController`.

## Runtime settings

| Setting | UI update and persistence | Runtime consumer | Verification / state |
| :--- | :--- | :--- | :--- |
| Start Minimized to System Tray | Draft toggle → Apply → `startMinimized` | Startup window visibility in Win32 app | Existing startup path; native startup behavior needs interactive verification. |
| Minimize to System Tray on Close | Draft toggle → Apply → `closeToTray` | Main window close handler; active queue/tray lifetime | Existing behavior; native close interaction needs interactive verification. |
| Reduce UI Animations | Draft toggle → Apply → `reducedMotion` | `DesktopView` animation state | Existing C++ Settings Center regression coverage. |
| Maximum Simultaneous Downloads | Draft dropdown → `maxConcurrentTasks` | Scheduler atomic concurrency limit | Existing scheduler/controller tests; runtime value is applied immediately. |
| Start New Downloads Immediately | Draft toggle → Apply → `autoStartDownloads` | Desktop Add Download and IPC headless default | Existing add-task paths; browser GUI offers still wait for confirmation. |
| Existing File Conflict Resolution | Draft dropdown → `conflictPolicy` | Download commit and browser blob commit | Existing conflict-resolution tests. |
| Max Automatic Retries | Draft dropdown → `maxAutoRetries` | Scheduler transient-error retry loop | Existing scheduler behavior; regression coverage is in C++ suite. |
| Base Retry Interval | Draft dropdown → `retryIntervalSec` | Scheduler exponential backoff | Existing scheduler behavior; regression coverage is in C++ suite. |
| Queue Post-Completion Action | Draft dropdown → `queueCompletionAction` | Scheduler invokes sleep/shutdown after queue drains | Requires a Windows host and user privilege to verify the OS action. |
| Default Download Directory | Folder picker → Apply → `defaultDownloadDir` | New tasks for HTTP, FTP, stream, and torrent types | Uses Windows Downloads known folder by default. Legacy automatic `Downloads\Radium` migrates; explicit newer selected paths remain marked. |
| Default Connections per Task | Draft dropdown → `defaultConnectionsPerTask` | New tasks with no per-task override; segmented HTTP engine | Existing HTTP scheduler tests; FTP/torrent connection controls are engine-specific. |
| Global Download Speed Limit | Draft dropdown → `speedLimitBps` | Shared token-bucket limiter for HTTP transfers | Existing global-rate-limit C++ test. |
| Segment Stall Timeout | Draft dropdown → `stallTimeoutMs` | HTTP segmented engine configuration | Existing segment scheduler tests. |
| Minimum Work-Stealing Split Size | Draft dropdown → `minSplitBytes` | HTTP segment split eligibility | Existing scheduler tests. |
| Proxy Connection Mode | Draft dropdown → `proxyMode` | HTTP engine direct/system/manual proxy selection | Manual mode now requires a configured server; invalid/empty manual configuration is rejected. |
| Manual Proxy Server | Action button opens input → `proxyServer` | WinHTTP named proxy for new HTTP tasks | Input and persistence are wired; network behavior depends on the configured proxy and needs a live proxy test. |
| Proxy Bypass List (diagnostic) | Read-only diagnostic from stored settings | HTTP engine bypass argument | Not editable in this Settings UI; kept clearly diagnostic. |
| Configured HTTP Auth User (diagnostic) | Read-only diagnostic from stored settings | HTTP engine default auth context | Not editable in this Settings UI; per-task credentials remain supported. |
| Enable Browser Download & Media Capture | Draft toggle → Apply → `browserCaptureEnabled` | `IpcServer::SetCaptureEnabled` gates browser offers | Existing IPC/browser tests. |
| Minimum Automatic File Capture Size | Draft dropdown → Apply → `minCaptureSizeBytes` | IPC handshake → extension `G.minFileSizeBytes` → size-only interception fallback | Extension regression test covers threshold behavior; explicit links, attachments, and recognized file types remain eligible independently. |
| Local IPC Endpoint | Read-only diagnostic | Reports active IPC endpoint status | Diagnostic only. |
| Show Completion & Error Notifications | Draft toggle → Apply → `showCompletionNotification` | Controller terminal-task callback → UI-thread `PostMessage` → tray balloon | C++ tests cover completion, failure, disabled state, and deduplication. Live Explorer toast display needs a Windows desktop session. |
| Check for Updates | About action starts an async GitHub Releases API query | Stable release selection, SemVer comparison, matching x64/ARM64 installer detection, notes | C++ SemVer tests and Windows build cover local logic; live GitHub API is network-dependent. Automatic installation is intentionally blocked until signing material is configured. |
| Keyboard Shortcuts | Read-only reference | Existing main-window shortcut handler | Reference-only, not customizable. |
| App/version, worker status, platform/license, queue database | Read-only diagnostics | Runtime state and build metadata | Diagnostic-only values, not editable settings. |

## Removed controls

- **File Types** was a static presentation of extension categories and folders. It was removed from the Settings sidebar and rendering. Internal filename/MIME/extension classification remains in the controller and extension.
- **Automatic category subfolders** and **custom category folder** entries were removed. Category labels still classify tasks, but do not rewrite the selected destination. Legacy `categoryFolders` and `categorySubfoldersEnabled` JSON keys are ignored.
- **Remember last folder** is stored as `lastDownloadDir`. It updates only after a confirmed desktop submission and is used to prefill the next applicable dialog. Canceling a dialog does not write it.

## Save and apply lifecycle

`DesktopView::SetDraftSettingByKey` validates and updates draft state. `ActionSaveSettings` validates once more, calls `DownloadController::UpdateSettings`, persists `app_settings`, updates scheduler atomics/HTTP engine configuration, and applies browser capture and animation state. A failed persistence result is returned to the UI rather than reported as saved. `RestoreDefaultSettings` writes factory values through the same controller update path.

