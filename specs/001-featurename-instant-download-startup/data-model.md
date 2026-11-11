# Data Model & Storage Contracts: Sub-100ms Instant Download Startup

## 1. Candidate Entity Extension
In `extension/js/function.js` & `catch-script/catch.js`:
```typescript
interface StreamCandidateQuality {
  id: string; // e.g. "1080p_mp4", "360p_mp4", "144p_mp4"
  label: string;
  resolution: string; // e.g. "1080p", "144p"
  width: number;
  height: number;
  fps: number;
  itag: number;
  bandwidth: number;
  size: number;
  sizeEstimated: boolean;
  ext: "mp4" | "webm" | "m4a";
  mime: string;
  videoUrl: string; // Direct deciphered GoogleVideo URL with n-sig applied
  pairedAudioUrl: string; // Direct deciphered GoogleVideo audio URL with n-sig applied
  requiresAudioMux: boolean;
  requiresCipherExtraction: boolean;
  variantUrl: string;
}
```

## 2. In-Memory Session Handshake Contract
In `extension/catch-script/catch.js`:
- Listen for user quality selection / overlay click.
- Synchronously resolve deciphered URL from active HTML5 MSE session or player API (`movie_player.getVideoData()`, `movie_player.getAvailableQualityData()`, or hook player decipher functions).
- Transmit directly to Radium core via Named Pipe `stream.add`:
```json
{
  "streamType": "YOUTUBE",
  "sourceType": "YOUTUBE_EXTRACTION",
  "videoUrl": "https://rrX---sn-xxx.googlevideo.com/videoplayback?...",
  "pairedAudioUrl": "https://rrX---sn-xxx.googlevideo.com/videoplayback?...",
  "autoStart": true,
  "clientClickTime": 1793688799000
}
```

## 3. C++ Fast-Path Execution State Machine
In `src/engine_stream/nm3u8dl_re_adapter.cpp`:
- `TaskState::kQueued` (0 ms)
- `TaskState::kDownloading` (< 5 ms) -> invokes `brisk::HttpClient::PerformGet` using WinHTTP IOCP.
- `First data bytes received` (15-25 ms) -> received directly from Google CDN over HTTPS.
- `TaskState::kMuxing` -> FFmpeg remuxing video and audio streams.
- `TaskState::kCompleted` -> File committed.
