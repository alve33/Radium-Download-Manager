# Radium Download Manager: Engine and Protocol Remediation Report

**Date:** November 7, 2026  
**Auditor / Engineer:** Principal Systems & Protocol Engineer  
**Status:** Remediated & Verified

---

## 1. BitTorrent Swarm Protocol Modernization

### 1.1 Rarest-First Piece Selection Algorithm (BEP 3)
- **Problem (AUD-03):** Pieces were picked sequentially from index 0 upward. Rare pieces held by only one peer in the swarm were delayed, risking piece starvation if that peer disconnected.
- **Implementation:**
  - Added swarm piece frequency tracking in `src/engine_torrent/piece_manager.hpp`:
    ```cpp
    std::vector<uint32_t> m_piece_availability;
    void UpdatePeerAvailability(const std::vector<bool>& peer_have, bool connected);
    void OnPeerHave(size_t piece_index);
    ```
  - In `GetNextRequestsForPeer`, candidate incomplete pieces are sorted by rarity ascending (`std::stable_sort` on `m_piece_availability[p]`), picking rarest pieces first across the active swarm.

### 1.2 BitTorrent Endgame Mode
- **Problem (AUD-04):** When a download reached 99%, the final pending blocks were assigned to single slow peers, causing transfers to crawl or stall indefinitely.
- **Implementation:**
  - Added Endgame mode detection in `PieceManager`:
    - Evaluates remaining unfinished pieces against active peer connections.
    - When remaining pieces $\le$ active peer connections, blocks are marked eligible for redundant requests across all connected unchoked peers.
    - When any peer successfully delivers the block, redundant requests to other peers are immediately canceled via `PeerWireProtocol::CreateCancel`.

### 1.3 Tit-for-Tat Choking & Unchoking (BEP 3)
- **Problem (AUD-05):** Radium unchoked every connected peer unconditionally, allowing freeriders and leechers to consume upload bandwidth without reciprocity.
- **Implementation:**
  - Created `PeerSession` state tracking in `src/engine_torrent/torrent_engine.cpp`:
    - Tracks recent downloaded volume per peer in rolling windows.
    - Evaluates regular rechoke cycles every 10 seconds, unchoking the top 4 fastest peers and choking the rest.
    - Evaluates optimistic unchoke cycles every 30 seconds, selecting 1 random choked peer to discover better download rates.
    - Rejects block upload requests (`BtMessageId::kRequest`) when a peer is choked by us.

---

## 2. Crawler & Dynamic HTML Range Bug Fix

### Findings Addressed:
- **AUD-06:** The crawler injected `Range: bytes=0-4194304` into HTML page requests, triggering `HTTP 416 Range Not Satisfiable` on dynamic servers and CDNs (e.g., PHP/Node.js/Cloudflare).

### Implementation:
- Updated `src/crawler/site_grabber.cpp`:
  - Passes `std::nullopt` for `range_start` and `range_end` on web page fetches, issuing clean standard HTTP `GET` requests.
  - Implemented body capping inside the chunk callback to prevent memory exhaustion on oversized HTML payloads.
  - Enhanced HTML parsing to support `<base href="...">` resolution and `<img srcset="...">` responsive candidate discovery.

---

## 3. ZIP64 Archive Preview Support (>4 GB Files)

### Findings Addressed:
- **AUD-07:** The in-memory archive previewer only parsed standard 32-bit ZIP End of Central Directory (EOCD) records, failing or misreporting files on archives exceeding 4 GiB or containing more than 65,535 files.

### Implementation:
- Updated `src/archive/zip_preview.cpp`:
  - Added `Zip64EOCDLocator` (signature `0x07064b50`) and `Zip64EOCDRecord` (signature `0x06064b50`) structure parsing.
  - Added support for extra field tag `0x0001` (ZIP64 Extended Information Extra Field) in Central Directory headers.
  - Reads 64-bit uncompressed size, compressed size, and local header relative offset without unpacking archive bodies.
