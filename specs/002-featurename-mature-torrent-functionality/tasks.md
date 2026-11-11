# Tasks: Mature Torrent Functionality

**Input**: `specs/002-featurename-mature-torrent-functionality/` (`spec.md`, `plan.md`)  
**Status**: Implemented & Verified

---

## Phase 1: Torrent Creation Engine (User Story 1 - P1) 🎯 MVP

**Goal**: Enable creation of `.torrent` files and magnet links from local files or directories with auto piece calculation and SHA-1 hashing.

- [x] T001 [P] [US1] Create header `src/engine_torrent/torrent_creator.hpp` defining `TorrentCreatorOptions`, `TorrentCreatorFileEntry`, and `TorrentCreator`.
- [x] T002 [US1] Implement `src/engine_torrent/torrent_creator.cpp` with multi-file byte-stream slicing, Windows CryptoAPI/BCrypt SHA-1 hashing, and Bencode serialization.
- [x] T003 [US1] Implement `GenerateMagnetLink` method in `src/engine_torrent/torrent_creator.cpp`.
- [x] T004 [US1] Expose `CreateTorrentFile` method in `src/core/download_controller.hpp` and `src/core/download_controller.cpp`.

---

## Phase 2: Sequential & Stream Piece Selection (User Story 2 - P2)

**Goal**: Enable sequential piece downloading and head/tail piece prioritization for media previewing while downloading.

- [x] T005 [US2] Update `src/engine_torrent/piece_manager.hpp` to add `m_sequential_mode` and `m_prioritize_head_tail` configuration.
- [x] T006 [US2] Update `PickNextBlockToRequest` in `src/engine_torrent/piece_manager.hpp` to prioritize head (first 5 pieces), tail (last 5 pieces), and strictly ascending piece sequences when sequential mode is enabled.
- [x] T007 [US2] Expose `SetTorrentSequentialMode(task_id, bool)` in `src/engine_torrent/torrent_engine.hpp` and `src/core/download_controller.hpp`.

---

## Phase 3: Swarm Inspection & Tracker Management (User Story 3 - P2)

**Goal**: Expose tracker health/tier diagnostics, connected peer telemetry, and allow adding new trackers to running tasks.

- [x] T008 [US3] Define `TrackerEndpointInfo`, `PeerDiagnosticsInfo`, and `TorrentSwarmDetails` in `src/engine_torrent/torrent_engine.hpp`.
- [x] T009 [US3] Implement `AddTrackersToTask` and `GetSwarmDetails` in `src/engine_torrent/torrent_engine.cpp` with thread-safe session lock.
- [x] T010 [US3] Expose `AddTrackersToTask` and `GetTorrentSwarmDetails` in `src/core/download_controller.hpp` and `src/core/download_controller.cpp`.

---

## Phase 4: Verification & Regression Testing

**Goal**: Comprehensive unit and integration test verification for all new BitTorrent features.

- [x] T011 Update `CMakeLists.txt` to include `src/engine_torrent/torrent_creator.cpp`.
- [x] T012 Add unit test `TestTorrentCreatorRoundTripAndMagnet` in `tests/cpp/unit_tests.cpp`.
- [x] T013 Add unit test `TestPieceManagerSequentialAndHeadTailPriority` in `tests/cpp/unit_tests.cpp`.
- [x] T014 Add unit test `TestTorrentEngineSwarmDetailsAndDynamicTrackers` in `tests/cpp/unit_tests.cpp`.
- [x] T015 Run full C++ regression test suite (`radium_unit_tests.exe`) and confirm 78/78 suites PASS.
