# Concurrency & Multithreading Audit Report

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## 1. Thread Inventory & Concurrency Model

Radium Download Manager runs multiple asynchronous threads across distinct subsystems:

| Subsystem | Thread Name / Role | Launch Site | Termination Condition | Mutexes / Sync Primitives | Hazards Identified |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Core Controller** | `ScheduleLoop` (20Hz scheduler) | `DownloadController::StartScheduler()` (`download_controller.cpp:322`) | `m_running == false` (`Shutdown()`) | `m_mutex`, `m_cv`, `m_store.m_mutex` | Lock convoy on SQLite `ListQueues()` at 20Hz (`BUG-003`); races on system suspend (`BUG-007`). |
| **Core Controller** | `RunTaskWorker` (1 worker per downloading task) | `DownloadController::ScheduleLoop()` (`download_controller.cpp:943`) | Task completion, error, cancel, or pause | `m_mutex`, `ctrl->pause_requested`, `ctrl->cancel_requested`, `ctrl->finished` | Async `PauseTask` race clobbers `ResumeTask`/`RefreshTaskUrl` (`BUG-001`); `RemoveTask` 3s join timeout race with `Shutdown()` (`BUG-004`). |
| **HTTP Engine** | Segment Workers (up to 32 per HTTP task) | `HttpDownloadEngine::Execute()` (`http_download_engine.cpp:780`) | Segment completion, error, or `engine_cancel` | `engine_cancel`, `SegmentScheduler::m_mutex`, `active_worker_count` atomic | Rollback of unstarted stolen segments races with worker start (`BUG-019`); data race on `HttpClient::wsa_init` (`SEC-008`). |
| **BitTorrent** | `peer_worker` (up to 64 per torrent task) | `TorrentEngine::Execute()` (`torrent_engine.cpp:881`) | Torrent complete, error, cancel, or choke | `torrent_cancel`, `PieceManager::m_mutex`, `MultiFileStorage` (NO MUTEX!) | Unsynchronized concurrent mutation of `TaskModel& task` (`BUG-029`); 32-bit overflow infinite spin (`BUG-026`). |
| **BitTorrent** | Inbound Listener Thread | `TorrentEngine::Execute()` (`torrent_engine.cpp:860`) | `torrent_cancel == true` | Non-blocking accept on `listener_sock` | Concurrent read/write on `task` model (`BUG-029`). |
| **IPC Server** | Named Pipe Listener & Client Handlers | `IpcServer::Start()` (`ipc_server.cpp:211`) | `m_running == false` (`Stop()`) | `m_mutex`, `m_active_transfers_mutex`, overlapped events | POSIX double-close race & use-after-close on `m_first_pipe_handle` (`BUG-038`); unsynchronized `m_offer_cb`. |
| **Win32 GUI** | Main GUI UI Thread | `Win32MainWindow::Run()` (`win32_main_window.cpp:115`) | `WM_QUIT` | None (UI thread) | UI thread blocked synchronously on file hashing, Defender scanning, and cross-drive copy (`BUG-041`). |

---

## 2. Mutex Hierarchy & Lock Convoy Analysis

### 2.1 The 20Hz `ScheduleLoop` Lock Convoy (`BUG-003`)
In `DownloadController`, synchronization relies heavily on a single recursive/coarse-grained mutex (`DownloadController::m_mutex`) and `SqliteStore::m_mutex`.

```
                    [ DownloadController::m_mutex ]
                                   |
                +------------------+------------------+
                |                                     |
    [ ScheduleLoop (20Hz) ]             [ Worker Progress Callback ]
    Locks m_mutex                       Locks m_store.m_mutex (I/O, DPAPI)
    Calls m_store.ListQueues()          Takes 10-50ms during flush
    Attempts to acquire m_store.m_mutex               |
                |                                     |
                +============== DEADLOCK / CONVOY ====v
                ScheduleLoop blocks holding m_mutex!
                UI thread calling ListTasks() blocks on m_mutex!
                All worker callbacks block on m_mutex!
```

- **Impact:** Any slow disk write or DPAPI encryption in `SqliteStore` stalls `ScheduleLoop`. Because `ScheduleLoop` holds `m_mutex` while calling `m_store.ListQueues()`, the GUI thread freezes, causing micro-stutters and missed frame deadlines.

---

## 3. Data Races & State Machine Concurrency Defects

### 3.1 `BUG-001`: Asynchronous `PauseTask` Overwrites `ResumeTask` / `RefreshTaskUrl`
- **Location:** `src/core/download_controller.cpp:1476-1479, 1555-1598, 1627-1671, 2018-2067`
- **Root Cause:** When `PauseTask(id)` is called, it sets `ctrl->pause_requested = true` and `it->second.state = TaskState::kPaused`, but returns immediately while `RunTaskWorker` is still running in the background. If `ResumeTask(id)` or `RefreshTaskUrl(id)` is called immediately, it transitions `it->second.state` to `TaskState::kQueued`. Seconds later, when the exiting worker unwinds, line 1570 executes:
  ```cpp
  t.state = local_task.state; // local_task.state is TaskState::kPaused!
  ```
  This unconditionally overwrites `t.state` back to `kPaused`, stranding the task forever.

### 3.2 `BUG-029`: Unsynchronized Mutation of `TaskModel& task` Across 64 Torrent Threads
- **Location:** `src/engine_torrent/torrent_engine.cpp:573-593, 733-786, 881-883, 992`
- **Root Cause:** `TorrentEngine::Execute` receives `TaskModel& task`. Up to 64 `peer_worker` threads concurrently mutate:
  ```cpp
  task.status_detail = "Downloading piece " + std::to_string(piece_idx); // std::string reallocation data race!
  task.downloaded_bytes += block_len; // Non-atomic int64_t data race!
  ```
  Simultaneously, the main thread reads `task.status_detail` and `task.downloaded_bytes` for GUI updates. This is undefined behavior under ISO C++20 and causes heap corruption in `std::string`.

### 3.3 `BUG-038`: IPC Server Handle Use-After-Close and Data Race
- **Location:** `src/ipc/ipc_server.cpp:189, 223-271, 315-385, 417-474, 894-896`
- **Root Cause:** `IpcServer::Stop()` closes `m_first_pipe_handle` and client pipe handles while background client threads are blocked in `ReadFile` or `ConnectNamedPipe`. The client thread wakes up, attempts to read from or close the invalid handle, and triggers handle recycling bugs. Furthermore, `m_offer_cb` is modified in `SetDownloadOfferCallback` without acquiring `m_mutex`, while client threads invoke `m_offer_cb` concurrently.

---

## 4. Concurrency Recommendations

1. **Decouple SQLite from `ScheduleLoop`:** Cache queues in memory under a dedicated `m_queues_mutex` and update the cache only on queue create/modify/delete events. Never call `m_store.ListQueues()` inside the 20Hz scheduling loop.
2. **Synchronize Task State Transitions:** In `RunTaskWorker`, only commit state changes if `t.state == TaskState::kDownloading`. If `t.state` has already been transitioned to `kQueued` or `kCanceled` by another thread, discard the worker's exit state.
3. **Atomic Progress Tracking for Torrent Engine:** Pass thread-safe progress counters (`std::atomic<uint64_t>`) to `TorrentEngine` instead of passing `TaskModel& task` directly to worker threads.
