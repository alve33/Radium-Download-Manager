# Protocol Download Engine Deep-Dive Audit

**Audit Date:** 2026-10-30  
**Commit:** `c5f68f4c95154e77bb8bfb59993fbd878554598d` (`main`)

---

## 1. HTTP/HTTPS Download Engine (`src/brisk_engine/`)

### 1.1 `BUG-015`: Truncated Single-Stream Downloads Silently Marked `Completed`
- **Location:** `src/brisk_engine/http_download_engine.cpp:650-733, 1350`
- **Root Cause:** When `!range_supported || total_bytes == 0` (Case A: single-stream fallback), `on_headers` is an empty no-op lambda (line 671). The worker streams body chunks until `StreamGet` returns. At line 732:
  ```cpp
  task.total_bytes = written_offset; // Overwrites total_bytes with whatever bytes were read!
  ```
  If the network drops mid-stream (e.g., server resets connection after 10 MB of a 100 MB file), `task.total_bytes` is set to `10 MB` and line 1350 renames `.part` to the final filename, marking the truncated file as 100% complete with zero error indication!
- **Recommended Fix:** Verify that `written_offset == expected_content_length` if `Content-Length` was provided in response headers. If `Content-Length` was missing, require clean HTTP chunked EOF or clean TLS closure before committing.

### 1.2 `BUG-016`: False-Positive SSRF Block on Public Domains Starting with `"fc"` or `"fd"`
- **Location:** `src/brisk_engine/http_client.cpp:204-223, 660`
- **Root Cause:** In `HttpClient::IsLoopbackHost`:
  ```cpp
  if (h.rfind("fc", 0) == 0 || h.rfind("fd", 0) == 0) return true; // Meant for IPv6 ULA (fc00::/7)!
  ```
  Because it checks string prefix without checking if the host is an IPv6 literal containing `:`, this blocks legitimate public domains such as `fdroid.org`, `fcc.gov`, `fc2.com`, and `fcdallas.com`!
- **Recommended Fix:** Only treat `fc` / `fd` as private IPv6 ULA if `h.find(':') != std::string::npos` or `h.front() == '['`.

### 1.3 `BUG-017`: HTTP 401/403/404 on Range Requests Misclassified as `ERR_HTTP_RANGE_PROTOCOL`
- **Location:** `src/brisk_engine/http_download_engine.cpp:868-898`
- **Root Cause:** If a signed download URL expires during a segmented download and the server returns `403 Forbidden` or `401 Unauthorized`, line 893 catches all non-200/206/416/429/5xx codes and treats them as `ERR_HTTP_RANGE_PROTOCOL` with `user_action_resumable = false`, preventing the user from using "Refresh Download Address" to update the link.
- **Recommended Fix:** Call `HttpClient::MapHttpStatusToError(meta.status_code)` so `403` becomes `ERR_HTTP_FORBIDDEN` and `401` becomes `ERR_HTTP_UNAUTHORIZED` (both allowing URL refresh).

---

## 2. Stream Download Pipeline (`src/engine_stream/`, `src/worker_stream/`)

### 2.1 `BUG-021`: Pipe Deadlock in `run_ytdlp_proc` & Missing Job Object
- **Location:** `src/engine_stream/nm3u8dl_re_adapter.cpp:366, 477, 624, 1180, 2937-3014`, `src/worker_stream/nm3u8dl_re_worker.cpp:772, 2059`
- **Root Cause:** In `run_ytdlp_proc`, the child process stdout/stderr pipe is read synchronously via blocking `ReadFile` without an asynchronous reader or PeekNamedPipe loop. If the process emits more than 4 KB of output before exiting, the Windows anonymous pipe buffer fills up, the child process blocks on `WriteFile`, and the parent blocks on `WaitForSingleObject` — a classic Windows pipe deadlock! Furthermore, `ffmpeg.exe` spawned in `nm3u8dl_re_worker.cpp` is not assigned to a Job Object; on timeout, it is orphaned and continues consuming CPU.
- **Recommended Fix:** Always assign all child worker processes to a Job Object configured with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. Read stdout/stderr using overlapped I/O or background reader threads.

### 2.2 `BUG-022`: HLS AES-128 Decryption Omitted on Final Live Segments
- **Location:** `src/worker_stream/nm3u8dl_re_worker.cpp:541-553, 2000-2022`
- **Root Cause:** When `!still_live` is detected on live HLS streams, the worker loop terminates early without processing remaining buffered encrypted segments through the `BCryptDecrypt` AES-128-CBC pipeline, writing raw ciphertext directly to the output container.

---

## 3. BitTorrent Protocol Engine (`src/engine_torrent/`)

### 3.1 `BUG-026`: 32-Bit Integer Overflow Causes Infinite Spin & 100% CPU Deadlock
- **Location:** `src/engine_torrent/torrent_engine.cpp:200-210, 683-693`, `src/engine_torrent/peer_wire_protocol.hpp:274-283`
- **Root Cause:** In `peer_worker`:
  ```cpp
  uint32_t payload_len = ReadBe32(recv_buf.data());
  if (recv_buf.size() < 4 + payload_len) { ... }
  ```
  If a malicious peer sends `payload_len = 0xFFFFFFFC` (or any value $\ge 2^{32} - 4$), `4 + payload_len` wraps to `0..3`. `recv_buf.size() >= 0` is always true, and the parser attempts to slice an invalid frame, spinning in an infinite non-blocking CPU loop that deadlocks `TorrentEngine` shutdown.
- **Recommended Fix:** Validate `payload_len <= kMaxPeerMessageSize (e.g., 2 MiB)` *before* computing `4 + payload_len`.

### 3.2 `BUG-027`: Magnet Metadata Hash Never Verified
- **Location:** `src/engine_torrent/torrent_engine.cpp:264-273`, `src/engine_torrent/torrent_parser.cpp:324-404`
- **Root Cause:** BEP 9 metadata pieces assembled from peers are passed to `ParseMetadataDictionary`. The parser computes the SHA-1 of the incoming bencoded data and overwrites `torrent_info.info_hash_raw` with that value, rather than verifying that `SHA1(metadata) == expected_info_hash` from the magnet link. A peer can serve arbitrary malicious torrent metadata.
- **Recommended Fix:** Reject incoming BEP 9 metadata unless `CryptoUtils::ComputeSha1(metadata_bytes) == expected_info_hash`.

---

## 4. FTP & Site Grabber Engines (`src/engine_ftp/`, `src/crawler/`)

### 4.1 `BUG-024`: Unbounded FTP Response Buffer & Destructive `.part` Truncation
- **Location:** `src/engine_ftp/ftp_client.cpp:141-172, 466-468`
- **Root Cause:** `ReadFtpResponse` appends incoming socket bytes to a `std::string` until a CRLF is found. If a malicious or rogue FTP server streams continuous bytes without CRLF, memory grows without bound until OOM crash. Furthermore, if `REST <offset>` is rejected by the server, line 468 deletes and re-creates `.part` with size 0, destroying previously downloaded data.

### 4.2 `BUG-025`: Site Grabber URL Corruption
- **Location:** `src/crawler/site_grabber.cpp:34-84, 218-250`
- **Root Cause:** In `SiteGrabber::ResolveUrl`, the code searches for the last `/` across the entire string without stripping query parameters (`?query=foo/bar`). If a page URL contains `/` inside a query parameter, relative links are resolved against the wrong base path.
