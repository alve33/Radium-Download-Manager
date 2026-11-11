# Radium Download Manager: Missing & Incomplete Features Audit

**Date:** November 7, 2026  
**Auditor:** Senior Software Engineering & Product Audit Team  

---

## 1. Overview

This document categorizes all features that are **missing, partially implemented, simulated, or incomplete** in Radium Download Manager when evaluated against the requirements of a production-grade commercial download manager.

---

## 2. Feature Gap Inventory by Subsystem

### 2.1 BitTorrent Engine (`src/engine_torrent/`)

| Feature | Status | Description & Impact |
| :--- | :---: | :--- |
| **Rarest-First Piece Selection** | **MISSING** | Pieces are requested sequentially (`0..N`) via `piece_manager.hpp:159`. Harms swarm distribution and causes duplicate block downloads. |
| **Endgame Mode** | **MISSING** | `piece_manager.hpp:145` does not re-request pending blocks from multiple peers. Downloads stall at 99% if the final peer is slow. |
| **Bandwidth Choking (Tit-for-Tat)** | **MISSING** | `torrent_engine.cpp:544` unchokes all connected peers unconditionally. Leechers can saturate upload bandwidth without giving back. |
| **Web Seeding (BEP 17 / BEP 19)** | **MISSING** | Ignores `url-list` / `httpseeds` in `.torrent` files. Cannot use HTTP mirrors to accelerate or recover abandoned torrents. |
| **Tracker Scrape (BEP 48 / BEP 15)**| **MISSING** | `tracker_client.hpp` lacks Scrape action. Cannot inspect seeder/leecher counts without sending a full announce. |
| **PEX (Peer Exchange, BEP 11)** | **MISSING** | Lacks `ut_pex` extended messaging. Slower peer discovery in trackerless swarms. |
| **Torrent File Creation** | **MISSING** | No functionality to hash local files and generate `.torrent` metainfo. |
| **UPnP / NAT-PMP Port Forwarding** | **MISSING** | Port 6881 is bound locally, but router port mapping is not negotiated. Incoming peer connections fail behind NAT. |

---

### 2.2 HTTP/HTTPS Download Engine & Controller

| Feature | Status | Description & Impact |
| :--- | :---: | :--- |
| **MD5 Hash Verification** | **MISSING** | Requirements specify SHA-256 and MD5, but only SHA-256 is implemented in `crypto_utils.cpp`. Legacy mirrors providing MD5 cannot be verified. |
| **Category Destination Folder Routing** | **REMOVED / MISSING** | Download category routing was removed from Settings Center; all downloads default to `%USERPROFILE%\Downloads`. Cannot route ISOs to `D:\ISOs` or Videos to `D:\Videos`. |
| **Multi-Queue Scheduling** | **PARTIAL** | Only a single queue schedule window is supported. Cannot define independent named queues (e.g. "Work Queue", "Nightly Queue"). |
| **Worker 0 Work-Stealing Re-entry** | **INCOMPLETE** | Worker 0 finishes its initial range and terminates permanently instead of re-entering the work-stealing loop, causing thread churn. |

---

### 2.3 Site Grabber & Crawler (`src/crawler/`)

| Feature | Status | Description & Impact |
| :--- | :---: | :--- |
| **Clean HTML Requesting** | **BROKEN** | Injects `Range: bytes=0-4194304` into HTML page requests (`site_grabber.cpp:218`), causing HTTP 416 errors on dynamic servers and CDNs. |
| **Modern HTML5 / SPA Parsing** | **MISSING** | Uses simple regex for `href/src`; misses `<img srcset>`, `<picture>`, and cannot crawl JavaScript Single Page Applications (SPAs). |
| **`robots.txt` Compliance** | **MISSING** | Does not check `/robots.txt`; crawls immediately without politeness delays. |
| **Pre-crawl File Size Estimation** | **MISSING** | Discovered items always display `size = 0` because no `HEAD` probe or content-length check is performed. |

---

### 2.4 Archive ZIP Previewer (`src/archive/`)

| Feature | Status | Description & Impact |
| :--- | :---: | :--- |
| **ZIP64 Format Support** | **MISSING** | Uses 32-bit integer structures (`zip_preview.cpp:17`). Fails with `ERR_ZIP_CORRUPT` on archives >4 GB or >65k entries. |
| **Remote HTTP Range ZIP Inspection** | **MISSING** | Can only preview local files on disk. Cannot inspect or selectively extract files from a remote ZIP over HTTP before downloading. |
| **CP437 vs UTF-8 Encoding Detection**| **MISSING** | Ignores ZIP bit flag 11; corrupts accented or CJK characters created on Windows OEM code pages. |

---

### 2.5 FTP Engine (`src/engine_ftp/`)

| Feature | Status | Description & Impact |
| :--- | :---: | :--- |
| **Segmented Multi-Connection FTP** | **MISSING** | Downloads files as a single sequential TCP stream. Cannot saturate broadband connections on high-latency FTP servers. |
| **FTPS (FTP over TLS)** | **DISABLED** | Explicitly rejects `ftps://` with `ERR_FTPS_UNSUPPORTED`. |
| **Directory Recursion (`MLSD`/`LIST`)**| **MISSING** | Cannot browse or download remote FTP folder structures. |

---

### 2.6 Windows Integration, Settings & Packaging

| Feature | Status | Description & Impact |
| :--- | :---: | :--- |
| **Screen Reader / MSAA / UIA** | **MISSING** | Main window does not implement `WM_GETOBJECT`; screen readers see a blank window. |
| **Positional CLI Argument Handling** | **MISSING** | `main.cpp` ignores `argv[1]` if not preceded by `--download`. Windows `.torrent` double-click does nothing. |
| **Settings Export / Import** | **MISSING** | User settings cannot be exported or restored to/from file. |
| **Interactive Toast Notifications** | **MISSING** | Uses classic balloon `NIF_INFO` routed to Action Center; lacks WinRT inline action buttons ("Open File", "Open Folder"). |
| **Win32 SEH MiniDump Crash Handler** | **MISSING** | Crashes exit silently without generating a `.dmp` diagnostic artifact. |
| **Automated WinGet Publishing** | **MISSING** | Not published to Windows Package Manager (`winget`). |
| **First-Run Onboarding Wizard** | **MISSING** | Direct launch into dashboard; does not guide users through extension installation or directory setup. |
| **Internationalization (i18n)** | **MISSING** | Over 320 wide string literals are hardcoded in English. |
