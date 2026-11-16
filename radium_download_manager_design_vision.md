# Radium Download Manager — Complete Browser Extension & Sub-Window Architecture Specification

**Document Role:** Senior Product & Browser Extension Architecture Audit + Exact Bug-Fix Specification  
**Target Components:** Radium Browser Extension (`js/content-script.js`, `catch.js`, `background.js`) & C++20 IPC Sub-Window Router

## Part 1: Root-Cause Analysis of All 4 Extension & Sub-Window Bugs

### Bug 1 — `[Radium Security] SEC-001 BLOCKED` Error & Failed Downloads on Gemini / Modern Web Apps (`image_6bda5a.png` & `image_6bd752.png`)
* **Exact Error in `brave://extensions`:**
  ```text
  [Radium Security] SEC-001 BLOCKED: Unauthenticated radiumDirectBlobDownload attempt without anchor authorization token. [object Object]
  Context: https://gemini.google.com/u/2/app/7a6536b97f576c41?pageId=none
  Stack Trace: js/content-script.js:1075 (g_messageListener)
  ```
* **Why This Happens (`js/content-script.js:1066–1080` vs. `catch.js`):**
  1. On **Google Gemini** (`gemini.google.com`), ChatGPT, Claude, Overleaf, and modern React/Angular web apps, clicking a download button or file chip clicks a `<button>` or `<div>`—**not** a pre-existing `<a href="blob:...">` tag in the DOM.
  2. The website's JavaScript then generates/fetches the file asynchronously, creates a **detached in-memory `<a>` element** (`const a = document.createElement('a'); a.href = URL.createObjectURL(blob); a.download = 'file.zip';`), and calls **`a.click()`** programmatically!
  3. Because that detached `<a>` tag was never directly clicked by the mouse in the DOM, `js/content-script.js` never generated an `authToken` for it in `g_authorizedBlobTokens`.
  4. When `catch.js` (the page-world script hooking `HTMLAnchorElement.prototype.click`) intercepts `a.click()`, it blocks the browser's default download and sends `window.postMessage({ action: "radiumDirectBlobDownload", authToken: undefined, ... })` to `js/content-script.js`.
  5. At **line 1074 of `js/content-script.js`**, the `SEC-001` check sees `!authToken || !g_authorizedBlobTokens.has(authToken)`, logs the `SEC-001 BLOCKED` warning at **line 1075**, and **returns at line 1076—dropping the download completely!**

### Bug 2 — Normal HTTP/HTTPS Downloads Missed on Dynamic / Redirect Links
* Many websites initiate normal HTTP/HTTPS downloads via redirect URLs (such as `googleusercontent.com`, GitHub release assets, Overleaf `/download/project/...`, or `/download?file=...`) that do **not** end in `.zip`/`.pdf`/`.exe` when `chrome.downloads.onCreated` first fires.
* Filtering `onCreated` by URL file extension ignores these downloads completely because the true filename and MIME type only arrive in `chrome.downloads.onDeterminingFilename` (via the HTTP `Content-Disposition` header).

### Bug 3 — Detected Downloads Sometimes Auto-Start Instead of Opening the Sub-Windows
* When a download is detected by the extension, the IPC payload or C++ IPC handler (`OnIpcAddDownload` / `/add` endpoint) sometimes has `autoStart: true` / `silent: true` enabled, or checks for duplicate files only *after* starting the download worker thread.
* Instead, every detected download must follow the strict **3-Step Sub-Window Pipeline**:
  $$\text{Download Detected} \longrightarrow \text{Check If Already Exists} \longrightarrow \begin{cases} \textbf{Exists:} & \text{Open "Download Already Exists" Sub-Window} \\ \textbf{Not Exists:} & \text{Open "New Download" Sub-Window} \end{cases}$$

### Bug 4 — Multiple "Download with Radium" Video Popups on Facebook Reels / Feeds (`image_6be2b4.jpg`)
* Pages like Facebook Reels keep 3 `<video>` tags in the DOM simultaneously (the main active Reel, a blurred ambient background video, and the preloaded next Reel at the bottom edge of the screen).
* Creating one overlay per `<video>` tag spawns 2–3 `Download with Radium` buttons at once instead of a single floating button anchored only to the primary active video.

## Part 2: Exact Code Fixes Required Across `js/content-script.js`, `catch.js`, `background.js` & C++ IPC

### Fix 1 — Remove the Broken `SEC-001` Block in `js/content-script.js` (Lines 1066–1085) & Allow Programmatic `a.click()` Blob Downloads
In `js/content-script.js` around line 1066 (`else if (ev.data.action === "radiumDirectBlobDownload")`):
1. **Track Recent User Gestures Globally (`g_lastUserGestureTime`):**
   * Attach a capture-phase listener on `window` for `pointerdown`, `mousedown`, `click`, and `keydown` (`Enter`/`Space`) that records `g_lastUserGestureTime = Date.now();`.
2. **Replace the `SEC-001` Hard Block at Lines 1073–1080 of `js/content-script.js`:**
   * Do **NOT** block or `return;` when `authToken` is missing from `g_authorizedBlobTokens`!
   * Instead, accept the `radiumDirectBlobDownload` message whenever:
     * `ev.source === window` AND
     * Either `g_authorizedBlobTokens.has(authToken)` **OR** the page triggered a programmatic `a.click()` / `blob:` download (e.g., within `30,000 ms` of a user click/gesture or with a valid `blob:` / `data:` payload from `catch.js`).
   * Remove the `console.warn("[Radium Security] SEC-001 BLOCKED...")` line completely so Brave/Chrome never logs an extension error in `brave://extensions`.
   * Immediately forward the blob data / base64 / URL and filename (`ev.data.filename || "download"`) to `background.js` (`chrome.runtime.sendMessage`) so Radium receives the file!

### Fix 2 — Catch All Normal HTTP/HTTPS Downloads in `background.js`
1. **Register Both `onDeterminingFilename` and `onCreated` Synchronously at Top Level of `background.js`:**
   * Use `chrome.downloads.onDeterminingFilename.addListener((item, suggest) => { ... })` as the primary interceptor for normal HTTP/HTTPS downloads because it fires **after** redirects (`item.finalUrl`), `Content-Disposition` filename (`item.filename`), `item.mime`, and `item.fileSize` are resolved by the browser!
2. **Do Not Require a File Extension in the Initial URL String:**
   * Intercept **any** `http://` or `https://` download initiated in the browser (including Gemini `googleusercontent.com` downloads, Overleaf, GitHub, Google Drive, and dynamic `/download?id=...` endpoints), excluding only `blob:` (which is handled by `catch.js` / `radiumDirectBlobDownload`) and plain HTML pages saved via `Save Page As` (`item.mime === "text/html"`).
   * Deduplicate by `item.id`, cancel and erase the browser's built-in download (`chrome.downloads.cancel(item.id)` + `chrome.downloads.erase({ id: item.id })`), and send the download metadata with **`autoStart: false`** to the Radium C++ IPC server.

### Fix 3 — Enforce Strict Sub-Window Routing in C++ IPC (`Detect -> Check Exists -> Sub-Window`)
In the C++ application's IPC handler when a download arrives from the extension:
1. **Never Auto-Start:** Force `autoStart = false` (strip any code path that calls `StartDownload()` directly from an extension IPC message).
2. **Restore Main Window if Minimized to Tray:** Call `ShowWindow(hwnd, SW_SHOW); if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd);`.
3. **Check If Download Already Exists:**
   * Check whether the normalized URL (`url` or `finalUrl`) or target `filename` already exists in the download list/database **OR** whether `std::filesystem::exists(saveFolder / filename)` is `true` on disk.
   * **If `alreadyExists == true`:** Open the existing **"Download Already Exists" Sub-Window**.
   * **If `alreadyExists == false`:** Open the existing **"New Download" Sub-Window** pre-filled with the URL, filename, and size.

### Fix 4 — Enforce a Strict Singleton Video Popup (`1 Popup Maximum`) in `js/content-script.js`
* Maintain a single `#radium-singleton-video-btn` element per document (removing any duplicate `.radium-download-btn` elements before updating).
* Filter out `<video>` elements that are smaller than `220×160`, near the top/bottom offscreen edges (`rect.top > window.innerHeight - 140`), or have `filter: blur(...)` / `pointerEvents: none`, and anchor the single popup only to the primary active playing `<video>`.

## Part 3: Copy-Pasteable Master Prompt for Your Coding AI

> **Copy-Paste Prompt for Coding AI — Fix `SEC-001` Gemini Blob Download Error, Normal HTTP Detection, Single Video Popup & Sub-Window Routing:**
>
> Fix the following 4 issues across the Radium Browser Extension (`js/content-script.js`, `catch.js`, `background.js`) and the C++ IPC handler:
>
> **1. Fix `[Radium Security] SEC-001 BLOCKED` at `js/content-script.js:1075` (Why Gemini & Modern Web App Downloads Fail):**
> * **Root Cause:** On Google Gemini (`gemini.google.com`) and modern SPAs, clicking a download button clicks a `<button>`, and the site's JS asynchronously creates a detached `const a = document.createElement('a')` with a `blob:` URL and calls `a.click()` programmatically. Because the user clicked the `<button>` rather than a pre-existing `<a href="blob:...">` in the DOM, `g_authorizedBlobTokens` has no `authToken` for that anchor, causing `js/content-script.js:1075` to block the download with `[Radium Security] SEC-001 BLOCKED: Unauthenticated radiumDirectBlobDownload attempt without anchor authorization token`.
> * **Exact Fix in `js/content-script.js` & `catch.js`:**
>   1. Track recent user interaction in `js/content-script.js` via a capture-phase listener on `window` (`pointerdown`, `mousedown`, `click`, `keydown`) that updates `g_lastUserGestureTime = Date.now()`.
>   2. At `js/content-script.js:1066–1085` (`else if (ev.data.action === "radiumDirectBlobDownload")`), **remove the `SEC-001` block and `console.warn` that returns early when `authToken` is missing!** Accept `radiumDirectBlobDownload` whenever `ev.source === window` (whether `authToken` is in `g_authorizedBlobTokens` OR `a.click()` was called programmatically by the page) and forward the blob payload and filename to `background.js` -> Radium IPC server so downloads from Gemini and all modern web apps work without errors.
>
> **2. Fix Normal HTTP/HTTPS Download Detection in `background.js`:**
> * Register both `chrome.downloads.onDeterminingFilename` and `chrome.downloads.onCreated` synchronously at the top level of `background.js`.
> * Do NOT filter out downloads in `onCreated` just because `item.url` lacks a file extension (sites like Gemini/Google, Overleaf, GitHub, and CDNs use redirect or query URLs whose real filename and MIME type only appear in `onDeterminingFilename` via `Content-Disposition`).
> * In `onDeterminingFilename(item, suggest)`, intercept all `http://` and `https://` file downloads (excluding `blob:` and `text/html` web pages), deduplicate by `item.id`, cancel and erase the browser's built-in download (`chrome.downloads.cancel(item.id)` + `chrome.downloads.erase({ id: item.id })`), and send `{ url: item.finalUrl || item.url, originalUrl: item.url, filename, fileSize: item.fileSize, mime: item.mime, referrer: item.referrer, autoStart: false }` to the Radium C++ IPC server.
>
> **3. Enforce Strict C++ Sub-Window Flow (`Detect -> Check Exists -> Sub-Window`, Never Auto-Start):**
> * In the C++ IPC receiver, never auto-start an intercepted download (`autoStart` must always be `false`).
> * When a download is detected:
>   1. Restore/foreground the main window if minimized to tray (`ShowWindow(hwnd, SW_SHOW); SetForegroundWindow(hwnd);`).
>   2. Check if the download already exists (same normalized URL or target filename in the queue/DB, OR `std::filesystem::exists(saveFolder / filename)` on disk).
>   3. **If it ALREADY EXISTS:** Trigger the existing **"Download Already Exists" Sub-Window**.
>   4. **If it DOES NOT EXIST:** Trigger the existing **"New Download" Sub-Window** (and wait for the user to confirm before starting).
>
> **4. Show Strictly ONE "Download with Radium" Video Popup (`js/content-script.js`):**
> * Enforce a single global DOM overlay (`#radium-singleton-video-btn`), removing any duplicate `.radium-download-btn` nodes.
> * Filter out blurred background videos (`filter.includes('blur')`), tiny videos (`< 220×160`), and offscreen preloaded Reels (`rect.top > window.innerHeight - 140`), attaching the single button only to the primary active playing `<video>`.