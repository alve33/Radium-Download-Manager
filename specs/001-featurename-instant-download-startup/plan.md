# Implementation Plan: Instant Download Startup (< 100ms)

**Branch**: `001-featurename-instant-download-startup` | **Date**: 2026-11-03 | **Spec**: [spec.md](spec.md)

## Summary
Eliminate the 7,000–13,000 ms `yt-dlp` invocation delay when downloading YouTube videos by directly capturing and transmitting deciphered `videoplayback` stream URLs from the browser's active HTML5 video session to Radium's native WinHTTP IOCP engine. This achieves real network data streaming in **< 30 ms** from the user's click.

## Technical Context
- **C++ Native Engine**: C++20, MSVC 19.44, WinHTTP IOCP (`src/engine_stream/nm3u8dl_re_adapter.cpp`).
- **Browser Extension**: Manifest V3, TypeScript/JavaScript (`extension/catch-script/catch.js`, `extension/js/function.js`, `extension/js/content-script.js`).
- **IPC Protocol**: Windows Named Pipe JSON protocol (`stream.add`).
- **Performance Budget**: Click to first received byte < 100 ms (target <= 10 ms IPC/dispatch + ~15-20 ms CDN network round-trip).

## Architecture & Implementation Steps

### Phase 1: Browser-Side Stream Interception (`catch.js` & `function.js`)
1. **Hook MediaSource & Fetch/XHR**:
   - YouTube streams video and audio chunks from `https://*.googlevideo.com/videoplayback?...`
   - Store deciphered stream URLs by `itag` and mime type (`video/mp4`, `video/webm`, `audio/mp4`, `audio/webm`).
   - Strip range constraints (`&range=...` / `&rn=...`) to obtain full-stream URLs.
2. **Synchronous In-Page Player Inspection**:
   - On watch page, inspect `document.getElementById('movie_player')`.
   - Call `movie_player.getPlayerResponse()` to get direct streaming formats if unciphered or actively resolved.
3. **Candidate Enrichment**:
   - When user clicks any resolution badge (e.g. `[144p]`, `[720p]`, `[1080p]`), immediately populate `videoUrl` with the matching deciphered stream URL.
   - For audio, attach the highest-bitrate deciphered audio stream URL as `pairedAudioUrl`.
   - Set `requiresCipherExtraction: false`.

### Phase 2: Core Native Engine Fast-Path (`nm3u8dl_re_adapter.cpp`)
1. When `video_url` is provided (matching `googlevideo.com/videoplayback` or direct HTTP/HTTPS stream):
   - Do NOT invoke `yt-dlp.exe`.
   - Immediately transition state to downloading and initiate `brisk::HttpClient::PerformGet` / `download_track`.
2. Record `First data bytes received` event immediately when the first chunk arrives from WinHTTP.

### Phase 3: Verification & Benchmarking
1. Rebuild `radium.exe` Release.
2. Test YouTube video downloads across multiple resolutions.
3. Verify `startup_reports.json` confirms startup time drops from 12,000+ ms down to **< 50 ms**.
