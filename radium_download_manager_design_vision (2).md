# Radium Download Manager — Complete UI/UX Specification, Browser Extension HTTP Capture & Sub-Window Routing Blueprint

**Document Role:** Senior Product Architecture & C++20 / Manifest V3 Extension Bug-Fix Blueprint  
**Target Platform:** Windows Desktop Application (Native Win32 / C++20) + Manifest V3 Browser Extension

## Part 1: Root-Cause Analysis — Extension HTTP Detection & Auto-Start Sub-Window Bypass

### Issue 1: Why the Browser Extension Sometimes Misses Normal HTTP/HTTPS Downloads
1. **Relying Only on `chrome.downloads.onCreated` or URL Extension Matching:**
   * When a user clicks a "Download" button on websites (e.g., Overleaf `/download/project/...`, GitHub releases, Google Drive, CDN dynamic links, PHP/Node endpoints like `/download?id=42`), the initial URL in `chrome.downloads.onCreated` often **has no file extension** (`.pdf`, `.zip`, `.exe` is only revealed later in the HTTP `Content-Disposition` header or after a `302` redirect!).
   * If the extension filters `onCreated` by checking whether `item.url` ends with a known extension, it ignores dynamic HTTP download buttons completely.
2. **Manifest V3 Service Worker Sleep / Async Listener Registration:**
   * If `chrome.downloads.onCreated` / `chrome.downloads.onDeterminingFilename` is registered inside an `async` function or after `await chrome.storage...`, Chrome drops the event when the Service Worker wakes up from idle sleep on a download click.
3. **Native Messaging / Localhost IPC Drop on Service Worker Wake:**
   * If the Native Messaging port or localhost HTTP IPC request fails on the first attempt while the Service Worker wakes up, the extension falls back to letting the browser download the file instead of retrying the Radium IPC server.

### Issue 2: Why Detected Downloads Sometimes Auto-Start Instead of Opening the Sub-Windows
1. **Bypassing the Sub-Window Router via `auto_start` / `silent` Flags:**
   * Either the browser extension sends `"autoStart": true` / `"silent": true` in its IPC JSON payload, OR the C++ IPC handler (`OnIpcAddDownload` / `/add` endpoint) checks an internal setting/flag and calls `StartDownload()` directly—skipping the UI sub-windows completely.
2. **Checking for Duplicates Too Late (Inside the Download Worker Instead of Before the Sub-Window):**
   * Previously (as seen in `image_3c8463.png` where `weaver_gere_4_6_3.pdf` paused with `Status: Destination file already exists`), the app queued/started the download first and only checked if the file existed when the worker thread tried to open the file on disk!
   * Instead, duplicate detection **must happen immediately upon download detection**, BEFORE any download task is created or started, routing strictly to either the **existing "Download Already Exists" Sub-Window** or the **existing "New Download" Sub-Window**.

## Part 2: Exact Engineering Architecture — Strict 3-Stage Detection & Sub-Window Pipeline

Every download intercepted from the browser extension (or pasted/clicked) must follow this **mandatory, zero-exception pipeline**:

$$\text{1. Detect Download} \longrightarrow \text{2. Check If Already Exists} \longrightarrow \begin{cases} \textbf{Exists} \implies \text{Open "Download Already Exists" Sub-Window} \\ \textbf{Not Exists} \implies \text{Open "New Download" Sub-Window} \end{cases}$$

### Stage 1: Fix Browser Extension HTTP/HTTPS Detection (`background.js`)
1. **Synchronous Top-Level Listeners (`onDeterminingFilename` + `onCreated`):**
   * Register `chrome.downloads.onDeterminingFilename` (and `chrome.downloads.onCreated`) **synchronously at line 1 of `background.js`** (never inside an async callback).
   * Why `onDeterminingFilename` is essential: In Chrome/Edge/Brave, `onDeterminingFilename(downloadItem, suggest)` fires **after** HTTP headers, redirects (`finalUrl`), `mime`, `fileSize`, and `Content-Disposition` (`filename`) are resolved, and **before** Chrome pops up its native Save As dialog or writes the final file!
2. **Capture All Normal HTTP/HTTPS Downloads (Not Just Hardcoded URL Extensions):**
   * Intercept any download where `finalUrl` or `url` starts with `http://` or `https://` (excluding `blob:` and `data:` URLs, and excluding `.html`/`.htm` only when `mime === "text/html"`).
   * As soon as `onDeterminingFilename` (or `onCreated` for known direct file URLs) triggers:
     1. Deduplicate by `downloadItem.id` (`handledDownloadIds.add(downloadItem.id)`) so the same click is never sent twice if both `onCreated` and `onDeterminingFilename` fire.
     2. Cancel and erase the browser's built-in download (`chrome.downloads.cancel(downloadItem.id)` and `chrome.downloads.erase({ id: downloadItem.id })`).
     3. Send the full metadata (`url: downloadItem.finalUrl || downloadItem.url`, `originalUrl: downloadItem.url`, `filename: basename(downloadItem.filename)`, `fileSize: downloadItem.fileSize`, `mime: downloadItem.mime`, `referrer: downloadItem.referrer`, `userAgent: navigator.userAgent`, `autoStart: false`) to the Radium C++ IPC server (with a 3-attempt retry if the local socket/pipe is waking up).

### Stage 2: Mandatory Duplicate Check in the C++ IPC & UI Handler
When the Radium C++ application receives a detected download (via Browser Extension IPC, CLI argument, or URL):
1. **Restore & Foreground the Main Window if Minimized to Tray:**
   * If Radium is minimized or hidden in the system tray, call:
     ```cpp
     ShowWindow(hwnd, SW_SHOW);
     if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
     SetForegroundWindow(hwnd);
     ```
     so the user always sees the sub-window pop up!
2. **Normalize Filename & Check If Download Already Exists:**
   * Resolve the candidate `filename` (from the extension's `Content-Disposition` filename, URL path, or pre-flight HEAD probe) and target `saveFolder` (`C:\Users\sayed\Downloads\Flux\...`).
   * Check `bool alreadyExists`:
     * **Condition A (Queue/History Match):** Does an item in the download list already have the same normalized URL (`url` or `finalUrl`) OR the same target `filename` in the active/completed/paused queue?
     * **Condition B (Disk File Match):** Does `std::filesystem::exists(targetFilePath)` return `true` on disk?

### Stage 3: Strict Sub-Window Routing (NEVER Auto-Start!)
* **Force `auto_start = false` for All Detected Downloads:**
  * Remove or disable any code path in the IPC receiver or download manager that automatically starts a newly detected download without showing a sub-window.
* **Branch 1 — If `alreadyExists == true`:**
  * Trigger the existing **"Download Already Exists" Sub-Window** (passing the detected URL, filename, existing file path/status, and options such as Overwrite / Rename / Resume / Cancel).
  * Do **NOT** create or start a download task until the user clicks their choice inside the "Download Already Exists" Sub-Window!
* **Branch 2 — If `alreadyExists == false`:**
  * Trigger the existing **"New Download" Sub-Window** (pre-populated with the detected URL, resolved filename, file size, category/save path, and connection count).
  * Do **NOT** start the download until the user clicks **Download / Start** inside the "New Download" Sub-Window!

## Part 3: Copy-Pasteable Master Prompt for Your Coding AI

> **Copy-Paste Prompt for Coding AI — Fix Browser Extension HTTP Download Detection & Enforce Strict Sub-Window Flow (No Auto-Start):**
>
> Fix the browser extension download detection and the C++ IPC download handling so that every normal HTTP/HTTPS download is reliably captured and **always** opens the appropriate existing Sub-Window instead of auto-starting:
>
> **1. Browser Extension (`background.js` / Service Worker) — Catch All Normal HTTP/HTTPS Downloads:**
> * Register **both** `chrome.downloads.onDeterminingFilename` and `chrome.downloads.onCreated` synchronously at the top level of `background.js` (never inside an `async` callback, so waking Service Workers never miss an event).
> * Do **NOT** ignore downloads just because the initial `downloadItem.url` lacks a file extension (many normal HTTP download buttons use redirect/query URLs like `/download?id=...` or `/project/.../build/...` whose real filename and MIME type only appear in `onDeterminingFilename` via HTTP `Content-Disposition` headers).
> * In `onDeterminingFilename(item, suggest)`:
>   * Intercept any `http://` or `https://` download (excluding `blob:`, `data:`, and plain web pages where `item.mime === "text/html"` and `filename` ends in `.html`/`.htm`).
>   * Track `item.id` in a `Set` (`interceptedIds`) so the same browser download is never sent twice.
>   * Cancel and erase the browser's native download (`chrome.downloads.cancel(item.id)` and `chrome.downloads.erase({ id: item.id })`).
>   * Send `{ url: item.finalUrl || item.url, originalUrl: item.url, filename: extractedFilename, fileSize: item.fileSize, mime: item.mime, referrer: item.referrer, cookies, userAgent, autoStart: false }` to the Radium IPC server (with a 3-attempt retry if the connection is waking up).
>
> **2. C++ IPC Receiver & UI Router — NEVER Auto-Start Detected Downloads:**
> * Remove/disable any logic in the IPC handler (`OnIpcDownload` / `/add` endpoint / extension message handler) that automatically starts a download without user confirmation (`auto_start` must ALWAYS be `false` for intercepted downloads).
> * When an intercepted download arrives from the browser extension:
>   1. Marshal the event to the main UI thread (`PostMessageW(hwnd, WM_APP_DOWNLOAD_DETECTED, ...)`).
>   2. If the main window is minimized or hidden in the system tray, restore and foreground it (`ShowWindow(hwnd, SW_SHOW); if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd);`).
>   3. **Check if the download already exists** (check if the normalized URL or target filename already exists in the download queue/history OR if the destination file `savePath / filename` already exists on disk via `std::filesystem::exists`).
>   4. **Strict Sub-Window Routing (Use the Existing Sub-Windows):**
>      * **If it ALREADY EXISTS:** Open the existing **"Download Already Exists" Sub-Window** (do NOT start the download or open the New Download window).
>      * **If it DOES NOT EXIST:** Open the existing **"New Download" Sub-Window** pre-filled with the URL, filename, file size, and save path (do NOT start the download until the user clicks confirm/download inside the New Download Sub-Window).