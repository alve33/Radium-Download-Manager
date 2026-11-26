# Radium Download Manager 2.1.0

**Radium Download Manager 2.1.0** is a stable feature and responsiveness update for **Windows 10 and Windows 11**, building on the 2.0.0 architecture with per-download priority scheduling, background video/stream preparation before confirmation, and improved browser player detection.

---

## What's New

- **Per-Download Priority (`High`, `Normal`, `Low`):** Set download priority from the task row's right-click **Priority** submenu. Queued tasks respect priority ordering, active HTTP and BitTorrent transfers weight bandwidth and connection slots accordingly, and priorities persist across restarts in SQLite.
- **Background Video & Stream Preparation:** Browser-captured progressive MP4, HLS (`.m3u8`), DASH (`.mpd`), and YouTube adaptive streams now begin downloading in the background as soon as the **New Download** dialog opens, reducing wait time when you click Start.

## Improvements

- **Instant Quality Switching in New Download:** Switching video resolution or format in the **New Download** dialog automatically cancels the previous background stream preparation and starts preparing the newly selected quality.
- **BitTorrent Scheduling & Storage:** Updated `TorrentEngine`, `PieceManager`, and `MultiFileStorage` for priority-aware bandwidth/slot weighting and cleaner multi-file piece handling.
- **YouTube Player Overlay Detection:** Improved container bounds resolution (`#movie_player`, `.html5-video-player`, `ytd-player`) in the Manifest V3 browser extension for more reliable floating download button placement.

## Bug Fixes

- **Prompt Stream Cancellation:** Switched `ExecuteYouTubeExtraction` stdout/stderr reading to non-blocking `PeekNamedPipe` polling so **Cancel** and **Pause** stop the worker immediately instead of waiting on pipe reads.
- **Speculative Staging Cleanup & Conflict Safety:** Cancelling a background-prepared stream task before confirmation now stops the worker and removes temporary staging files, and destination file conflicts are handled cleanly before commit.
- **IPC Offer Routing:** Incoming `download.offer` messages now dispatch directly into speculative preparation when enabled, with automatic fallback to standard offer queuing.

---

## Downloads

| Asset | Architecture | Format | Notes |
| :--- | :---: | :---: | :--- |
| `Radium-Setup-x64-2.1.0.exe` | x64 | Inno Setup Installer | **Recommended for most Windows 10 / 11 users** (includes stream workers) |
| `Radium-Setup-x64-Core-2.1.0.exe` | x64 | Inno Setup Installer | Lightweight installer without bundled FFmpeg / stream workers |
| `Radium-v2.1.0-Windows-x64-Full.zip` | x64 | Portable ZIP | Extract and run; includes stream workers |
| `Radium-v2.1.0-Windows-x64-Core.zip` | x64 | Portable ZIP | Core portable binaries only |
| `Radium-Setup-ARM64-2.1.0.exe` | ARM64 | Inno Setup Installer | Native ARM64 installer for Windows 10 / 11 on ARM |
| `Radium-Setup-ARM64-Core-2.1.0.exe` | ARM64 | Inno Setup Installer | Lightweight native ARM64 installer |
| `Radium-v2.1.0-Windows-ARM64-Full.zip` | ARM64 | Portable ZIP | Native ARM64 portable package with stream workers |
| `Radium-v2.1.0-Windows-ARM64-Core.zip` | ARM64 | Portable ZIP | Native ARM64 core portable binaries |
| `Radium-x64-2.1.0.0.msix` | x64 | Unsigned MSIX | Store-submission package (**unsigned** — requires Store or trusted cert) |
| `Radium-ARM64-2.1.0.0.msix` | ARM64 | Unsigned MSIX | Store-submission package (**unsigned** — requires Store or trusted cert) |
| `Radium-2.1.0.0.msixbundle` | x64 + ARM64 | Unsigned MSIX Bundle | Combined Store-submission bundle (**unsigned**) |
| `radium-extension.zip` | Cross-browser | Extension ZIP | Manifest V3 extension for Chrome, Edge, Brave, and Firefox |
| `SHA256SUMS.txt` | All | Checksums | SHA-256 digests for all release assets |

---

## Upgrade Information

- **Upgrading from 2.0.0 (Installer):** Run `Radium-Setup-x64-2.1.0.exe` (or `Radium-Setup-ARM64-2.1.0.exe`) over your existing installation. Your download history, queue, priorities, and settings in `%LOCALAPPDATA%\Radium Download Manager\state.db` are preserved automatically.
- **Upgrading from 2.0.0 (Portable):** Extract the new ZIP archive and keep your existing `data/` directory to retain your database and settings.
- **In-App Update Check:** Radium 2.0.0 users can check **Help → Check for Updates** to detect and download 2.1.0 directly from GitHub Releases.
