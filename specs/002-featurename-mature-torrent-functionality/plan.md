# Implementation Plan: Mature Torrent Functionality

**Branch:** `002-featurename-mature-torrent-functionality`  
**Feature Spec:** `specs/002-featurename-mature-torrent-functionality/spec.md`  
**Status:** In Progress

---

## 1. Technical Context

- **Current State:**
  - `src/engine_torrent/bencode.hpp`: Complete, verified Bencode serializer and parser.
  - `src/engine_torrent/torrent_parser.hpp` / `.cpp`: Parses `.torrent` files into `TorrentInfo`.
  - `src/engine_torrent/piece_manager.hpp`: In-memory and disk piece state tracker with block requests.
  - `src/engine_torrent/torrent_engine.cpp`: Torrent coordinator running tracker announcements and peer swarms.
- **Architectural Constraints (AGENTS.md):**
  - ISO C++20 standard (`/std:c++20`).
  - Strict RAII OS wrappers; zero uncaught exceptions across module boundaries.
  - No new external dependencies (use standard library, Win32 BCrypt / CryptoAPI or existing SHA-1 routines).
  - Deterministic test verification: All 77 existing test suites must continue to pass; add new tests for torrent creation, sequential piece scheduling, and swarm inspection.

---

## 2. Design & Architecture

### Phase 1: Torrent Creator (`src/engine_torrent/torrent_creator.hpp` & `.cpp`)
- `TorrentCreatorOptions`:
  - `piece_length`: default 0 (auto-select: 256KB if <512MB, 512KB if <1GB, 1MB if <4GB, 2MB if <8GB, 4MB if >=8GB).
  - `trackers`: list of announce URLs.
  - `comment`, `created_by`, `is_private`.
- `TorrentCreator::CreateFromFile(const std::filesystem::path& file, const TorrentCreatorOptions& opts)` -> `Result<std::vector<uint8_t>>`
- `TorrentCreator::CreateFromDirectory(const std::filesystem::path& dir, const TorrentCreatorOptions& opts)` -> `Result<std::vector<uint8_t>>`
- `TorrentCreator::GenerateMagnetLink(...)` -> `std::string`
- High-performance SHA-1 hashing via Windows BCrypt / CryptoAPI (`BCryptCreateHash` / `BCryptHashData`).

### Phase 2: Sequential & Stream Piece Selection (`src/engine_torrent/piece_manager.hpp`)
- Add `m_sequential_mode` and `m_prioritize_head_tail` flags to `PieceManager`.
- Modify `PickNextBlockToRequest(...)` / `PickNextPieceToRequest(...)`:
  - If sequential mode enabled:
    - Step 1: Prioritize first 5 pieces (container headers / moov atom).
    - Step 2: Prioritize last 5 pieces (trailer metadata).
    - Step 3: Prioritize available pieces in ascending piece index order.
  - Retain existing rarest-first / random-first scheduling when sequential mode is off.

### Phase 3: Swarm & Tracker Diagnostics (`src/engine_torrent/torrent_engine.hpp` & `.cpp`)
- `TrackerEndpointInfo`: `url`, `tier`, `status`, `seeders`, `leechers`, `last_error`.
- `PeerDiagnosticsInfo`: `ip`, `port`, `client_name`, `download_bps`, `upload_bps`, `progress_ratio`.
- `TorrentSwarmDetails`: `task_id`, `trackers`, `peers`, `total_seeds`, `total_leechers`.
- Method `AddTrackersToTask(task_id, const std::vector<std::string>& new_trackers)` -> `Result<void>`.
- Method `GetSwarmDetails(task_id)` -> `Result<TorrentSwarmDetails>`.

### Phase 4: Integration with Core Controller & Test Verification
- Expose methods on `DownloadController`:
  - `CreateTorrentFile(source_path, dest_torrent_path, options)`
  - `AddTrackersToTask(task_id, trackers)`
  - `GetTorrentSwarmDetails(task_id)`
  - `SetTorrentSequentialMode(task_id, bool enable)`
- Comprehensive C++ regression test suite covering all new methods and edge cases.
