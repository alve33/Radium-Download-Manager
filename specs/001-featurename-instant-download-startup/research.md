# Research: Sub-100ms Instant Download Startup

## 1. Root Cause of Current Delay
From `startup_reports.json`:
- Core initialization & named pipe IPC: **1.8 ms**
- Spawning `yt-dlp`: **19 ms**
- Waiting for `yt-dlp` to complete extraction & deciphering: **7,300 ms - 10,600 ms**

Total delay is 100% caused by invoking `yt-dlp.exe` out-of-process.

## 2. Solution: Zero-Latency In-Page Deciphering Hand-off
YouTube's web client already deciphers and streams media chunks via HTML5 MSE (`MediaSource` and `SourceBuffer`).
In `catch.js`:
1. `SourceBuffer.prototype.appendBuffer` and `fetch`/`XMLHttpRequest` receive valid, fully deciphered `videoplayback` URLs.
2. In addition, `document.getElementById('movie_player')` has synchronous methods:
   - `movie_player.getPlayerResponse()`
   - `movie_player.getAvailableQualityData()`
3. In `catch.js`, we hook `SourceBuffer.prototype.appendBuffer` and store the latest deciphered audio and video stream base URLs (stripped of byte range headers) keyed by itag / quality.
4. When the user clicks "Download [quality]":
   - The candidate immediately contains the direct `videoUrl` and `pairedAudioUrl`.
   - `requiresCipherExtraction` is set to `false`.
5. In Radium C++ (`nm3u8dl_re_adapter.cpp`):
   - When `videoUrl` is present and valid, skip `yt-dlp` invocation entirely.
   - Dispatch immediately to direct download / segmented download via WinHTTP IOCP.
   - Network handshake to Google CDN takes ~10-25 ms.
   - Total startup: **< 30 ms**!
