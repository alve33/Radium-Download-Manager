#pragma once

#include "common/types.hpp"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace radium::engine_stream {

enum class ExtractionErrorCategory {
    kNone = 0,
    kUnsupportedUrl,
    kNoVideoFound,
    kLoginRequired,
    kPrivateOrRestricted,
    kAgeRestricted,
    kGeoRestricted,
    kDrmProtected,
    kUrlExpired,
    kExtractorParseFailure,
    kNetworkFailure,
    kMergeFailure
};

std::string ExtractionErrorCategoryToString(ExtractionErrorCategory category);
std::string ExtractionErrorCategoryToCode(ExtractionErrorCategory category);

struct ExtractedFormat {
    std::string format_id;
    std::string quality_label;     // e.g. "1080p60", "1080p", "720p", "360p", "Audio Only"
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    std::string vcodec;            // e.g. "avc1.640028", "hev1", "vp9", "av01", "none"
    std::string acodec;            // e.g. "mp4a.40.2", "opus", "none"
    std::string container;         // e.g. "mp4", "webm", "m4a", "ts"
    uint64_t filesize_bytes = 0;   // Exact or approximate size in bytes (0 if unknown)
    uint64_t tbr_kbps = 0;         // Total bitrate in kbps
    uint64_t vbr_kbps = 0;         // Video bitrate in kbps
    uint64_t abr_kbps = 0;         // Audio bitrate in kbps
    bool has_video = true;
    bool has_audio = true;
    bool requires_merge = false;   // True if separate video + audio streams must be merged via FFmpeg
    bool is_downloadable = true;
    bool is_drm_protected = false;
    std::string protocol;          // "https", "m3u8_native", "http_dash_segments", etc.
    std::string video_url;         // Resolved direct or manifest video stream URL
    std::string audio_url;         // Resolved companion audio stream URL (when requires_merge is true)
    std::string audio_format_id;   // Companion audio format ID
    std::string audio_ext;         // Companion audio extension (e.g. "m4a", "webm")
    int64_t expires_at_epoch_s = 0;// Parsed URL expiration timestamp (0 if non-expiring/unknown)
    std::map<std::string, std::string> required_headers;
};

struct SubtitleTrack {
    std::string language_code;     // e.g. "en", "zh-CN"
    std::string language_name;     // e.g. "English", "Chinese (Simplified)"
    std::string url;
    std::string ext;               // "vtt", "srt", "ass"
    bool is_auto_generated = false;
};

struct MultipartEpisode {
    int page_number = 1;
    std::string part_id;           // e.g. cid on Bilibili
    std::string title;
    double duration_seconds = 0.0;
    std::string url;
};

struct ExtractionDiagnosticReport {
    std::string input_url;
    std::string normalized_url;
    std::string detected_platform; // "bilibili", "facebook", "youtube", "vimeo", "generic", etc.
    std::string selected_extractor;
    size_t discovered_format_count = 0;
    std::string selected_format_id;
    std::string stream_type;       // "DASH", "HLS", "DIRECT_MP4", "DIRECT_WEBM", "MERGED_AV", "UNKNOWN"
    std::vector<std::string> header_names_used; // Names only (values redacted!)
    bool auth_session_used = false;
    bool ffmpeg_merge_required = false;
    ExtractionErrorCategory failure_category = ExtractionErrorCategory::kNone;
    std::string failure_code;
    std::string failure_message;
    int64_t extraction_duration_ms = 0;

    std::string ToRedactedJson() const;
};

struct ExtractionMetadata {
    std::string input_url;
    std::string webpage_url;
    std::string platform_id;       // e.g. "bilibili", "facebook", "youtube", "vimeo"
    std::string extractor_name;    // e.g. "BilibiliExtractor", "FacebookExtractor", "GenericWebVideoExtractor", "YtDlpExtractor"
    std::string video_id;          // e.g. "BV1xx411c7mD", "123456789"
    std::string title;
    std::string uploader;
    double duration_seconds = 0.0;
    std::string thumbnail_url;
    bool is_live = false;
    bool is_drm_protected = false;
    bool requires_auth = false;
    int current_page = 1;
    std::vector<MultipartEpisode> episodes;
    std::vector<ExtractedFormat> formats;          // Consolidated selectable user formats (no fake qualities!)
    std::vector<ExtractedFormat> raw_video_streams;// Raw video-only DASH/adaptive tracks
    std::vector<ExtractedFormat> raw_audio_streams;// Raw audio-only DASH/adaptive tracks
    std::vector<SubtitleTrack> subtitles;
    std::map<std::string, std::string> required_headers;
    int64_t extracted_at_epoch_s = 0;
    int64_t earliest_expiry_epoch_s = 0;
    ExtractionDiagnosticReport diagnostics;
};

struct ExtractionRequest {
    std::string url;
    std::string page_url;
    std::string preferred_quality; // e.g. "1080p", "720p", "best", "audio"
    std::string preferred_format_id;
    int episode_page = 0;          // 0 = default/from URL, >= 1 = specific episode/part
    std::map<std::string, std::string> headers; // Cookie, Referer, Origin, User-Agent, Authorization
    bool bypass_cache = false;
    uint32_t timeout_ms = 25000;
};

class IVideoExtractor {
public:
    virtual ~IVideoExtractor() = default;

    // 1. Identify whether this extractor supports the given URL
    virtual std::string Name() const = 0;
    virtual std::string PlatformId(const std::string& url) const = 0;
    virtual bool SupportsUrl(const std::string& url) const = 0;

    // 2. Retrieve default required HTTP headers (Referer, Origin, User-Agent) for this site
    virtual std::map<std::string, std::string> GetDefaultHeaders(const std::string& url) const = 0;

    // 3. Extract video metadata, real available formats, subtitles, thumbnails, and stream URLs
    virtual Result<ExtractionMetadata> Extract(const ExtractionRequest& request) = 0;
};

class VideoExtractionManager {
public:
    static VideoExtractionManager& Instance();

    // URL classification & platform identification
    static std::string IdentifyPlatform(const std::string& url);
    static bool IsSupportedVideoSiteUrl(const std::string& url);
    static bool IsDirectMediaOrManifestUrl(const std::string& url);
    static std::string NormalizeExtractorUrl(const std::string& url);
    static std::string StripCdnRangeTrapParams(const std::string& url);
    static int64_t ParseUrlExpirationEpoch(const std::string& url);
    static bool IsStreamUrlExpired(const std::string& url, int64_t now_epoch_s = 0, int64_t safety_margin_s = 60);

    // Header & Cookie domain helpers
    static std::map<std::string, std::string> BuildSiteHeaders(
        const std::string& url,
        const std::string& page_url,
        const std::map<std::string, std::string>& existing_headers);
    static std::string ExtractRegistrableDomain(const std::string& host);

    // Core extraction entry points
    Result<ExtractionMetadata> ExtractMetadata(const ExtractionRequest& request);
    Result<ExtractedFormat> SelectBestFormat(
        const ExtractionMetadata& metadata,
        const std::string& preferred_quality_or_id) const;

    // Native HTML/JSON parser helpers (used for fast in-process extraction & unit testing)
    static Result<ExtractionMetadata> ParseBilibiliPlayInfoJson(
        const std::string& html_or_json,
        const std::string& page_url);
    static Result<ExtractionMetadata> ParseFacebookVideoHtml(
        const std::string& html,
        const std::string& page_url);
    static Result<ExtractionMetadata> ParseHtml5PlayerPage(
        const std::string& html,
        const std::string& page_url);
    static Result<ExtractionMetadata> ParseYtDlpJson(
        const std::string& json_str,
        const std::string& fallback_url);
    static Result<ExtractionMetadata> ParseHlsMasterPlaylist(
        const std::string& playlist_text,
        const std::string& master_url);

    // MP4 / WebM HTTP-Range container index probe (GD3 features/ffmpeg_pack/container.py port)
    struct ContainerIndexEntry {
        double time_sec = 0.0;
        uint64_t offset = 0;
        uint64_t size = 0;
    };
    struct ContainerIndex {
        std::string format; // "mp4" or "webm"
        double duration_sec = 0.0;
        uint64_t init_end = 0;
        std::vector<ContainerIndexEntry> entries;
    };
    static bool FindContainerIndexRange(
        const std::vector<uint8_t>& head,
        uint64_t file_size,
        uint64_t& out_offset,
        uint64_t& out_length);
    static std::optional<ContainerIndex> ParseContainerIndex(
        const std::vector<uint8_t>& data);

    // Error classification from raw extractor / HTTP stderr
    static ExtractionErrorCategory ClassifyError(
        const std::string& raw_error,
        int http_status = 0);

    // Cache management
    void InvalidateCacheForUrl(const std::string& url);
    void ClearCache();

private:
    VideoExtractionManager();
    ~VideoExtractionManager() = default;

    struct CacheEntry {
        ExtractionMetadata metadata;
        std::string auth_fingerprint;
        int64_t cached_at_epoch_s = 0;
    };

    std::vector<std::unique_ptr<IVideoExtractor>> m_extractors;
    mutable std::mutex m_cache_mutex;
    std::map<std::string, CacheEntry> m_cache;
};

// Video part cutting (MDM part.rs port)
struct VideoPartSpec {
    uint32_t from_sec = 0;
    uint32_t to_sec = 0;
    bool exact = false;

    static std::optional<uint32_t> ParseTimeSeconds(std::string_view s);
    static Result<VideoPartSpec> Validate(
        std::string_view from_str,
        std::string_view to_str,
        bool exact,
        std::optional<double> duration_sec = std::nullopt);
    std::string SectionsArg() const;
    std::string FormatTag() const;
    static std::string FormatPartFilename(const std::string& base_name, const VideoPartSpec& part);
};

// gallery-dl image gallery pipeline (MDM gallery.rs port)
struct GalleryItem {
    std::string url;
    std::string relative_path;
};

class GalleryDownloader {
public:
    static std::vector<GalleryItem> ParsePrepareOutput(const std::string& stdout_text);
    static Result<std::vector<GalleryItem>> ExtractGalleryItems(
        const std::string& gallery_url,
        const std::string& cookies_path,
        const std::map<std::string, std::string>& headers,
        std::atomic<bool>& cancel_flag);
    static Result<size_t> DownloadGallery(
        const std::vector<GalleryItem>& items,
        const std::filesystem::path& save_dir,
        const std::map<std::string, std::string>& headers,
        std::atomic<bool>& cancel_flag);
};

// Bilibili Wbi signing, QR login, multi-part URL parsing & Danmaku XML/ASS (GD3 bili_pack port)
struct BiliUrlInfo {
    std::string video_id;
    uint64_t mid = 0;
    uint64_t season_id = 0;
    uint64_t favorite_id = 0;
    std::vector<int> selected_pages;
    bool is_short_url = false;
};

struct BiliQrPollResult {
    int code = -1; // 0 = success, 86101 = unscanned, 86090 = scanned, 86038 = expired
    std::string message;
    std::string refresh_token;
    std::string cookie_header;
};

struct DanmakuItem {
    int64_t id = 0;
    int64_t progress_ms = 0;
    int mode = 1;
    int fontsize = 25;
    uint32_t color = 0xFFFFFF;
    std::string mid_hash;
    std::string content;
    int64_t ctime = 0;
    int weight = 0;
    int pool = 0;
    std::string id_str;
};

class BilibiliHelper {
public:
    static std::string ComputeWbiMixinKey(const std::string& img_key, const std::string& sub_key);
    static std::map<std::string, std::string> SignWbiParams(
        std::map<std::string, std::string> params,
        const std::string& mixin_key,
        int64_t wts_epoch_s = 0);
    static Result<BiliUrlInfo> ParseBilibiliUrl(const std::string& url);
    static std::string BuildQrGenerateUrl();
    static std::string BuildQrPollUrl(const std::string& qrcode_key);
    static Result<BiliQrPollResult> ParseQrPollResponse(const std::string& json_body);
    static std::vector<DanmakuItem> ParseDanmakuProtobuf(const std::vector<uint8_t>& seg_so_bytes);
    static std::string ConvertDanmakuToXml(const std::vector<DanmakuItem>& items, int64_t chat_id = 0);
    static std::string ConvertDanmakuToAss(const std::vector<DanmakuItem>& items, int width = 1920, int height = 1080);
};

} // namespace radium::engine_stream
