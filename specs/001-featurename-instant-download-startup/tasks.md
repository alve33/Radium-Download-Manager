# Tasks: Sub-100ms Instant Download Startup

**Input**: Design documents from `/specs/001-featurename-instant-download-startup/`
**Prerequisites**: plan.md, spec.md, research.md, data-model.md, quickstart.md

## Organization
Tasks are dependency-ordered and grouped into phases to achieve sub-100ms download startup.

---

## Phase 1: Browser-Side Deciphered Stream Resolution (`catch.js`)

**Purpose**: Snatch active deciphered video and audio playback stream URLs directly in page memory so the extension never falls back to yt-dlp.

- [ ] T001 [P] [US1] Hook `SourceBuffer.prototype.appendBuffer` and `MediaSource` in `extension/catch-script/catch.js` to register deciphered chunk URLs keyed by itag and media type (`video/mp4`, `video/webm`, `audio/mp4`, `audio/webm`).
- [ ] T002 [US1] In `extension/catch-script/catch.js`, query `#movie_player` API synchronously on user download click to resolve active direct streaming URLs.
- [ ] T003 [US1] Strip range headers (`range=`, `rbuf=`, `rn=`) and assign direct playback URLs to `candidate.qualities` entries with `requiresCipherExtraction: false`.

---

## Phase 2: Candidate Deduplication & IPC Propagation (`function.js` & `background.js`)

**Purpose**: Ensure candidate updates preserve direct stream URLs and pass them faithfully to Radium Core via Native Messaging.

- [ ] T004 [P] [US1] In `extension/js/function.js`, ensure `addOrDeduplicateMediaCandidate` merges `videoUrl` and `pairedAudioUrl` across candidate arrivals without overwriting with empty defaults.
- [ ] T005 [US1] In `extension/js/background.js`, in `submitMediaCandidateToRadium`, propagate `effectiveVideoUrl`, `effectiveAudioUrl`, and `autoStart: true` to `stream.add` payload.

---

## Phase 3: Core Engine Direct Stream Fast-Path (`nm3u8dl_re_adapter.cpp`)

**Purpose**: Directly stream from Google CDN via WinHTTP IOCP without ever spawning `yt-dlp.exe` when direct playback URLs are available.

- [ ] T006 [US1] In `src/engine_stream/nm3u8dl_re_adapter.cpp`, ensure `execute_direct_download` is invoked immediately when `video_url` or `qualities` has matching direct URLs.
- [ ] T007 [US1] In `src/engine_stream/nm3u8dl_re_adapter.cpp`, ensure `requires_cipher` defaults to `false` when direct `videoplayback` URLs with valid tokens exist.
- [ ] T008 [US1] Record `First data bytes received` diagnostic event immediately upon first buffer arrival in `download_track`.

---

## Phase 4: Build, Deployment & Verification

**Purpose**: Compile release binary, deploy extension, and verify execution metrics in `startup_reports.json`.

- [ ] T009 Recompile `radium.exe` Release target using MSVC 19.44 / CMake.
- [ ] T010 Copy built binaries to `dist/` and extension to `dist/extension/`.
- [ ] T011 Run unit test suite `radium_unit_tests.exe` to ensure 100% pass rate.
- [ ] T012 Verify with `python print_startup_log.py` that startup latency is strictly `< 100 ms`.
