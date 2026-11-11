# Feature Specification: Mature Torrent Functionality

**Feature Number:** 002  
**Feature Name:** mature-torrent-functionality  
**Status:** In Progress / Planning  
**Target:** Radium Download Manager (v1.0.0+)

---

## 1. Problem Statement & Motivation

Radium has implemented a high-performance BitTorrent coordinator (`src/engine_torrent/torrent_engine.cpp`) supporting BEP 9 (Metadata Exchange), BEP 15 (UDP Trackers), BEP 5 (DHT Node Discovery), multi-file storage mapping, choking/unchoking algorithms, and seeding controls.

However, against mature clients (qBittorrent, FDM) and commercial expectations, Radium's torrent experience has three critical deficiencies:
1. **No Torrent Creation:** Users cannot create `.torrent` files or generate magnet links from local files/folders.
2. **Missing Swarm & Tracker Visibility:** Users cannot view or edit announce trackers (add backup trackers, see tier errors) and cannot inspect connected peers (IP, client software, speed, downloaded %).
3. **No Sequential Downloading / Stream Priority:** Users cannot prioritize initial and trailing pieces for media previewing while downloading.

---

## 2. Requirements & User Stories

### User Story 1: Torrent Creation Wizard
- **As a** content creator or power user,
- **I want to** create a `.torrent` file and custom magnet link from an existing local file or directory,
- **So that** I can share files via BitTorrent swarms and seed them directly in Radium.
- **Acceptance Criteria:**
  - `TorrentCreator::CreateTorrent(...)` scans target files/directories.
  - Automatically calculates piece length (auto-selection: 256 KiB to 4 MiB based on total payload size) or accepts custom piece size.
  - Computes SHA-1 piece hashes across the contiguous byte stream.
  - Generates valid Bencoded metainfo dictionary containing `announce`, `announce-list` (tiers), `info` dictionary (`piece length`, `pieces`, `name`, `files` or `length`), and optional `comment`/`created by`.
  - Supports generating a canonical `magnet:?xt=urn:btih:<hex>&dn=<name>&tr=<tracker>` link.
  - Unit tests verify piece hashing, bencoding, and round-trip re-parsing with `TorrentParser`.

### User Story 2: Swarm & Tracker Management
- **As a** BitTorrent user with a stalled or slow download,
- **I want to** inspect tracker statuses and add new tracker URLs to an active torrent task,
- **So that** Radium can discover additional peers from healthy public or private trackers.
- **Acceptance Criteria:**
  - `TorrentEngine` tracks per-tracker status: URL, Tier, Status (`Online`, `Updating`, `Offline / Error`), Seeds, Peers, Interval.
  - API to dynamically inject tracker URLs into a running task (`AddTrackersToTask`).
  - `TorrentEngine` tracks active peer endpoints: IP address, port, client handshake identifier, download speed, upload speed, piece completion percentage.
  - Exposes `TorrentSwarmDetails` struct for UI consumption.

### User Story 3: Sequential & Stream Piece Scheduling
- **As a** media consumer downloading a video torrent,
- **I want to** enable sequential piece downloading (prioritizing the first 5% and last 5% of the file, then downloading sequentially),
- **So that** I can preview the video in VLC or an external player before the entire multi-gigabyte file finishes downloading.
- **Acceptance Criteria:**
  - `PieceManager` supports a `SequentialMode` flag.
  - In sequential mode, pieces are requested in ascending index order, with head (first N pieces) and tail (last N pieces) given top priority.
  - Verified by deterministic unit tests asserting piece selection ordering.

---

## 3. Scope & Non-Goals

- **In Scope:**
  - `src/engine_torrent/torrent_creator.hpp` & `.cpp`: Standalone, reusable, high-speed piece hasher and `.torrent` generator.
  - `src/engine_torrent/piece_manager.hpp`: Sequential & head/tail priority piece selection algorithm.
  - `src/engine_torrent/torrent_engine.hpp` & `.cpp`: Dynamic tracker addition, swarm detail extraction (`TorrentSwarmDetails`), and integration.
  - Unit tests in `tests/cpp/unit_tests.cpp` covering torrent creation, round-trip parsing, sequential scheduling, and tracker expansion.
- **Out of Scope (Preserving UI Isolation):**
  - Not altering custom GDI appearance without explicit directive; backend structures and CLI/Controller interfaces will be fully implemented and exposed for UI wiring.
