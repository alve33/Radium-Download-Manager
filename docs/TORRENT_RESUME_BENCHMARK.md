# TORRENT RESUME BENCHMARK: Radium vs qBittorrent

**Investigation Date:** November 10, 2026  
**Reference Environment:** Windows 11 x86_64, qBittorrent v5.1.4 (libtorrent 1.2.20.0)  
**Target Codebase:** Radium Download Manager (`src/engine_torrent/`, `src/core/download_controller.cpp`)  
**Workload:** 4.5 GB Torrent at 25% Completion (1.125 GB on Disk) and 75% Completion (3.375 GB on Disk)

---

## 1. Resume Latency Comparison: 25% Downloaded

*Five consecutive pause-and-resume cycles on a partially completed torrent (1.125 GB downloaded across ~280 pieces of 4 MiB each):*

| Trial | Radium Pause->Resume Latency (TTFB) | qBittorrent Pause->Resume Latency (TTFB) | Radium Bottleneck Trace |
| :---: | :---: | :---: | :--- |
| **Run 1** | **14.85 s** | **0.24 s** | CheckExistingPieces (3.8s) + Serial Tracker announce (8.0s) + Connect (2.1s) |
| **Run 2** | **12.92 s** | **0.18 s** | CheckExistingPieces (3.6s) + Serial Tracker announce (4.0s) + Connect (4.2s) |
| **Run 3** | **17.20 s** | **0.21 s** | CheckExistingPieces (4.1s) + Serial Tracker announce (8.0s) + Connect (3.9s) |
| **Run 4** | **13.50 s** | **0.19 s** | CheckExistingPieces (3.7s) + Serial Tracker announce (4.0s) + Connect (4.7s) |
| **Run 5** | **15.10 s** | **0.22 s** | CheckExistingPieces (3.9s) + Serial Tracker announce (8.0s) + Connect (2.3s) |
| **Mean** | **14.71 s** | **0.21 s** | **Radium is ~70x slower than qBittorrent on Resume!** |

---

## 2. Resume Latency Scaling vs Download Progress

As the torrent progresses and more data resides on disk, Radium's resume latency degrades monotonically, while qBittorrent remains constant:

| Completed Data on Disk | Radium Disk Hashing (`CheckExistingPieces`) | Radium Tracker + Swarm Re-discovery | Radium Total Resume Latency | qBittorrent Total Resume Latency |
| :---: | :---: | :---: | :---: | :---: |
| **0 MB (0%)** | 0.05 s | 8.2 s – 16.0 s | **8.25 s – 16.05 s** | **0.25 s – 0.50 s** |
| **500 MB (11%)** | 1.80 s | 8.2 s – 16.0 s | **10.00 s – 17.80 s** | **0.18 s – 0.25 s** |
| **1,125 MB (25%)** | 3.82 s | 8.2 s – 16.0 s | **12.02 s – 19.82 s** | **0.18 s – 0.24 s** |
| **3,375 MB (75%)** | 11.45 s | 8.2 s – 16.0 s | **19.65 s – 27.45 s** | **0.19 s – 0.25 s** |
| **4,500 MB (100%)** | 15.30 s | 8.2 s – 16.0 s | **23.50 s – 31.30 s** | **0.20 s – 0.28 s** |

### Why qBittorrent Resume is Instantaneous (< 250 ms)
In qBittorrent / libtorrent:
1. **The session never dies:** When paused, `torrent_handle::pause()` is set. Sockets to seeders may be throttled or closed, but the torrent object remains in memory inside the `session`.
2. **Piece status is already known:** The verified piece bitfield (`have_pieces`) remains in memory. Libtorrent **never reads disk or hashes pieces** on pause/resume.
3. **Peer list is cached:** Discovered peer endpoints are preserved in `torrent::m_peer_list`.
4. **Immediate reconnect:** Upon `resume()`, libtorrent immediately dispatches TCP connect requests to known peers from memory. Payload arrives in under 250 milliseconds.

### Why Radium Resume is Extremely Slow (> 14 seconds)
In Radium:
1. **Destructive Pause:** Pausing terminates `ExecuteTorrentDownload`. Sockets close, threads join, and the `ActiveTorrentSession` is destroyed (`UnregisterSession`).
2. **Brute-Force Disk Re-hashing (`CheckExistingPieces`):** On resume, Radium reads every required piece from disk and computes SHA-1 hashes. At 250 MB/s NVMe read speed, hashing 3.4 GB of data takes ~12 seconds of 100% single-thread CPU before any network packets are even transmitted.
3. **Peer List Discarded:** Radium discards all known peer IPs. It has to contact trackers and DHT all over again from scratch.
4. **Serial Tracker Timeout:** Radium waits sequentially through dead tracker timeouts before discovering who to connect to.
