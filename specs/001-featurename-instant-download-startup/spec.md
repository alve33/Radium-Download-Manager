# Feature Specification: Sub-100ms Instant Download Startup

**Feature Branch**: `001-featurename-instant-download-startup`
**Created**: 2026-11-03
**Status**: Active
**Goal**: Reduce the download startup delay from click to first transferred byte down to < 100 ms (targeting <= 10 ms).

## Problem Statement
When a user clicks "Download [quality]" on a YouTube video, the application previously took 7,000 ms to 13,000 ms before transferring the first byte. The logs showed:
- `Download task created`: 0.0 ms
- `Scheduler dispatches task`: 1.8 ms to 23 ms (sub-25ms!)
- `yt-dlp process creation starts`: ~19 ms
- `First data bytes received`: 7,400 ms to 13,500 ms (waiting for `yt-dlp.exe` to spawn, run Python, extract webpage, decipher signatures, and select formats).

## Requirements
1. **Target**: From clicking Download to the first data bytes received must be < 100 ms (preferred <= 10 ms).
2. **Direct Playback Stream Hand-off**: The extension running in the browser page has direct, instantaneous access to deciphered playback URLs (`googlevideo.com/videoplayback?...`) via:
   - Captured active media chunk requests in `catch.js` (tracked from `fetch`/`XHR`).
   - In-page player instance `#movie_player.getVideoData()`, `movie_player.getAvailableQualityData()`, or MSE `SourceBuffer.prototype.appendBuffer`.
3. **Bypass `yt-dlp` completely** whenever direct deciphered stream URLs are present.
4. **Immediate IOCP WinHTTP transfer**: The native C++ engine dispatches directly to `brisk::HttpClient` / `SegmentWorker`, starting data transfer in ~10-25 ms over the network.
5. **No regressions**: Video badge overlay, Mover button, Shadow DOM isolation, and download reliability must remain intact.
