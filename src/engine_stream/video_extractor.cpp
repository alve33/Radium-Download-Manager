#include "engine_stream/video_extractor.hpp"
#include "brisk_engine/http_client.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "engine_stream/nm3u8dl_re_adapter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace openidm::engine_stream {

namespace {

constexpr const char* kDefaultDesktopUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

int64_t CurrentEpochSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

#ifdef _WIN32
std::wstring Utf8ToWideLocal(std::string_view utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), len);
    return out;
}
#endif

std::string ExtractUrlHost(const std::string& url) {
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return "";
    size_t start = scheme_pos + 3;
    size_t end = url.find_first_of("/:?#", start);
    std::string host = (end == std::string::npos) ? url.substr(start) : url.substr(start, end - start);
    return ToLowerAscii(host);
}

std::string ExtractUrlOrigin(const std::string& url) {
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return "";
    size_t start = scheme_pos + 3;
    size_t end = url.find_first_of("/?#", start);
    return (end == std::string::npos) ? url : url.substr(0, end);
}

std::string ExtractUrlPath(const std::string& url) {
    auto scheme_pos = url.find("://");
    if (scheme_pos == std::string::npos) return "";
    size_t start = scheme_pos + 3;
    size_t slash = url.find('/', start);
    if (slash == std::string::npos) return "/";
    size_t q = url.find_first_of("?#", slash);
    return (q == std::string::npos) ? url.substr(slash) : url.substr(slash, q - slash);
}

std::string UnescapeJsonSlashesAndUnicode(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            if (next == '/') {
                out.push_back('/');
                ++i;
                continue;
            }
            if (next == 'u' && i + 5 < raw.size()) {
                std::string hex = raw.substr(i + 2, 4);
                if (hex == "0026") {
                    out.push_back('&');
                    i += 5;
                    continue;
                }
                if (hex == "003d" || hex == "003D") {
                    out.push_back('=');
                    i += 5;
                    continue;
                }
                if (hex == "0025") {
                    out.push_back('%');
                    i += 5;
                    continue;
                }
                if (hex == "002F" || hex == "002f") {
                    out.push_back('/');
                    i += 5;
                    continue;
                }
            }
        }
        out.push_back(raw[i]);
    }
    size_t pos = 0;
    while ((pos = out.find("&amp;", pos)) != std::string::npos) {
        out.replace(pos, 5, "&");
        pos += 1;
    }
    return out;
}

std::string ExtractHtmlTitle(const std::string& html) {
    static const std::regex og_title_re(
        R"(<meta\s+[^>]*property=[\"']og:title[\"'][^>]*content=[\"']([^\"']+)[\"'])",
        std::regex::icase);
    static const std::regex og_title_rev_re(
        R"(<meta\s+[^>]*content=[\"']([^\"']+)[\"'][^>]*property=[\"']og:title[\"'])",
        std::regex::icase);
    static const std::regex title_tag_re(R"(<title[^>]*>([^<]+)</title>)", std::regex::icase);

    std::smatch m;
    if (std::regex_search(html, m, og_title_re) && m.size() > 1) {
        return UnescapeJsonSlashesAndUnicode(m[1].str());
    }
    if (std::regex_search(html, m, og_title_rev_re) && m.size() > 1) {
        return UnescapeJsonSlashesAndUnicode(m[1].str());
    }
    if (std::regex_search(html, m, title_tag_re) && m.size() > 1) {
        std::string t = UnescapeJsonSlashesAndUnicode(m[1].str());
        auto pos = t.find("_哔哩哔哩");
        if (pos != std::string::npos && pos > 0) t = t.substr(0, pos);
        pos = t.find(" - Pornhub.com");
        if (pos != std::string::npos && pos > 0) t = t.substr(0, pos);
        pos = t.find(" - XVIDEOS.COM");
        if (pos != std::string::npos && pos > 0) t = t.substr(0, pos);
        return t;
    }
    return "";
}

std::string ExtractHtmlThumbnail(const std::string& html) {
    static const std::regex og_img_re(
        R"(<meta\s+[^>]*property=[\"']og:image[\"'][^>]*content=[\"']([^\"']+)[\"'])",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(html, m, og_img_re) && m.size() > 1) {
        return UnescapeJsonSlashesAndUnicode(m[1].str());
    }
    return "";
}

std::string BuildQualityLabel(uint32_t height, uint32_t width, double fps, const std::string& fallback_note) {
    uint32_t effective_h = height;
    if (effective_h == 0 && width > 0) {
        if (width >= 3800) effective_h = 2160;
        else if (width >= 2500) effective_h = 1440;
        else if (width >= 1900) effective_h = 1080;
        else if (width >= 1200) effective_h = 720;
        else if (width >= 800) effective_h = 480;
        else if (width >= 600) effective_h = 360;
        else if (width >= 400) effective_h = 240;
    }
    if (effective_h > 0) {
        std::string label = std::to_string(effective_h) + "p";
        if (fps >= 50.0) {
            label += std::to_string(static_cast<int>(fps + 0.5));
        }
        return label;
    }
    if (!fallback_note.empty()) return fallback_note;
    return "Best Available";
}

std::string ComputeAuthFingerprint(const std::map<std::string, std::string>& headers) {
    std::string combined;
    for (const auto& [k, v] : headers) {
        std::string lk = ToLowerAscii(k);
        if (lk == "cookie" || lk == "authorization") {
            combined += lk + ":" + std::to_string(std::hash<std::string>{}(v)) + ";";
        }
    }
    return combined;
}

void ConsolidateAdaptiveFormats(ExtractionMetadata& meta) {
    std::sort(meta.raw_audio_streams.begin(), meta.raw_audio_streams.end(),
              [](const ExtractedFormat& a, const ExtractedFormat& b) {
                  if (a.abr_kbps != b.abr_kbps) return a.abr_kbps > b.abr_kbps;
                  return a.tbr_kbps > b.tbr_kbps;
              });

    const ExtractedFormat* best_audio =
        meta.raw_audio_streams.empty() ? nullptr : &meta.raw_audio_streams.front();
    const ExtractedFormat* best_m4a_audio = nullptr;
    for (const auto& a : meta.raw_audio_streams) {
        if (a.container == "m4a" || a.container == "mp4" ||
            a.acodec.find("mp4a") != std::string::npos) {
            best_m4a_audio = &a;
            break;
        }
    }
    if (!best_m4a_audio) best_m4a_audio = best_audio;

    for (const auto& v : meta.raw_video_streams) {
        if (!v.is_downloadable || v.is_drm_protected || v.video_url.empty()) continue;
        ExtractedFormat merged = v;
        if (!merged.has_audio && best_audio != nullptr) {
            const ExtractedFormat* chosen_audio =
                (merged.container == "mp4" && best_m4a_audio) ? best_m4a_audio : best_audio;
            merged.has_audio = true;
            merged.requires_merge = true;
            merged.audio_url = chosen_audio->video_url.empty() ? chosen_audio->audio_url : chosen_audio->video_url;
            merged.audio_format_id = chosen_audio->format_id;
            merged.audio_ext = chosen_audio->container.empty() ? "m4a" : chosen_audio->container;
            merged.acodec = chosen_audio->acodec;
            merged.abr_kbps = chosen_audio->abr_kbps > 0 ? chosen_audio->abr_kbps : chosen_audio->tbr_kbps;
            if (merged.filesize_bytes > 0 && chosen_audio->filesize_bytes > 0) {
                merged.filesize_bytes += chosen_audio->filesize_bytes;
            }
            if (merged.tbr_kbps > 0 && merged.abr_kbps > 0) {
                merged.tbr_kbps += merged.abr_kbps;
            }
            if (!chosen_audio->format_id.empty() && !merged.format_id.empty()) {
                merged.format_id = merged.format_id + "+" + chosen_audio->format_id;
            }
        }
        meta.formats.push_back(std::move(merged));
    }

    std::sort(meta.formats.begin(), meta.formats.end(),
              [](const ExtractedFormat& a, const ExtractedFormat& b) {
                  if (a.has_video != b.has_video) return a.has_video > b.has_video;
                  if (a.height != b.height) return a.height > b.height;
                  if (static_cast<int>(a.fps) != static_cast<int>(b.fps)) return a.fps > b.fps;
                  bool a_mp4 = (a.container == "mp4");
                  bool b_mp4 = (b.container == "mp4");
                  if (a_mp4 != b_mp4) return a_mp4 > b_mp4;
                  if (a.tbr_kbps != b.tbr_kbps) return a.tbr_kbps > b.tbr_kbps;
                  return a.filesize_bytes > b.filesize_bytes;
              });

    std::vector<ExtractedFormat> deduped;
    std::set<std::string> seen_tiers;
    for (auto& f : meta.formats) {
        if (!f.is_downloadable || f.is_drm_protected) continue;
        std::string tier_key;
        if (f.has_video && f.height > 0) {
            int fps_tier = (f.fps >= 50.0) ? 60 : 30;
            tier_key = std::to_string(f.height) + "p_" + std::to_string(fps_tier);
        } else if (f.has_video) {
            tier_key = "video_" + f.quality_label + "_" + f.format_id;
        } else {
            tier_key = "audio_only";
        }
        if (seen_tiers.insert(tier_key).second) {
            deduped.push_back(std::move(f));
        }
    }

    if (best_audio != nullptr && seen_tiers.find("audio_only") == seen_tiers.end()) {
        ExtractedFormat audio_fmt = *best_audio;
        audio_fmt.has_video = false;
        audio_fmt.has_audio = true;
        audio_fmt.requires_merge = false;
        audio_fmt.quality_label = "Audio Only";
        if (audio_fmt.container.empty() || audio_fmt.container == "mp4") {
            audio_fmt.container = "m4a";
        }
        deduped.push_back(std::move(audio_fmt));
    }

    meta.formats = std::move(deduped);

    int64_t earliest = 0;
    for (const auto& f : meta.formats) {
        int64_t exp = f.expires_at_epoch_s;
        if (exp == 0 && !f.video_url.empty()) {
            exp = VideoExtractionManager::ParseUrlExpirationEpoch(f.video_url);
        }
        if (exp > 0 && (earliest == 0 || exp < earliest)) {
            earliest = exp;
        }
    }
    meta.earliest_expiry_epoch_s = earliest;
}

} // namespace

std::string ExtractionErrorCategoryToString(ExtractionErrorCategory category) {
    switch (category) {
        case ExtractionErrorCategory::kNone: return "NONE";
        case ExtractionErrorCategory::kUnsupportedUrl: return "UNSUPPORTED_URL";
        case ExtractionErrorCategory::kNoVideoFound: return "NO_VIDEO_FOUND";
        case ExtractionErrorCategory::kLoginRequired: return "LOGIN_REQUIRED";
        case ExtractionErrorCategory::kPrivateOrRestricted: return "PRIVATE_OR_RESTRICTED";
        case ExtractionErrorCategory::kAgeRestricted: return "AGE_RESTRICTED";
        case ExtractionErrorCategory::kGeoRestricted: return "GEO_RESTRICTED";
        case ExtractionErrorCategory::kDrmProtected: return "DRM_PROTECTED";
        case ExtractionErrorCategory::kUrlExpired: return "URL_EXPIRED";
        case ExtractionErrorCategory::kExtractorParseFailure: return "EXTRACTOR_PARSE_FAILURE";
        case ExtractionErrorCategory::kNetworkFailure: return "NETWORK_FAILURE";
        case ExtractionErrorCategory::kMergeFailure: return "MERGE_FAILURE";
    }
    return "UNKNOWN";
}

std::string ExtractionErrorCategoryToCode(ExtractionErrorCategory category) {
    switch (category) {
        case ExtractionErrorCategory::kNone: return "";
        case ExtractionErrorCategory::kUnsupportedUrl: return "ERR_EXTRACTOR_UNSUPPORTED_URL";
        case ExtractionErrorCategory::kNoVideoFound: return "ERR_EXTRACTOR_NO_VIDEO_FOUND";
        case ExtractionErrorCategory::kLoginRequired: return "ERR_EXTRACTOR_LOGIN_REQUIRED";
        case ExtractionErrorCategory::kPrivateOrRestricted: return "ERR_EXTRACTOR_PRIVATE_VIDEO";
        case ExtractionErrorCategory::kAgeRestricted: return "ERR_EXTRACTOR_AGE_RESTRICTED";
        case ExtractionErrorCategory::kGeoRestricted: return "ERR_EXTRACTOR_GEO_RESTRICTED";
        case ExtractionErrorCategory::kDrmProtected: return "ERR_STREAM_DRM_PROTECTED";
        case ExtractionErrorCategory::kUrlExpired: return "ERR_STREAM_URL_EXPIRED";
        case ExtractionErrorCategory::kExtractorParseFailure: return "ERR_EXTRACTOR_PARSE_FAILURE";
        case ExtractionErrorCategory::kNetworkFailure: return "ERR_EXTRACTOR_NETWORK_FAILURE";
        case ExtractionErrorCategory::kMergeFailure: return "ERR_FFMPEG_MERGE_FAILED";
    }
    return "ERR_EXTRACTOR_FAILED";
}

std::string ExtractionDiagnosticReport::ToRedactedJson() const {
    json::Value j = json::Object{};
    j["input_url"] = Logger::RedactSensitiveData(input_url);
    j["normalized_url"] = Logger::RedactSensitiveData(normalized_url);
    j["detected_platform"] = detected_platform;
    j["selected_extractor"] = selected_extractor;
    j["discovered_format_count"] = static_cast<int64_t>(discovered_format_count);
    j["selected_format_id"] = selected_format_id;
    j["stream_type"] = stream_type;
    json::Array h_arr;
    for (const auto& h : header_names_used) {
        h_arr.push_back(h);
    }
    j["header_names_used"] = std::move(h_arr);
    j["auth_session_used"] = auth_session_used;
    j["ffmpeg_merge_required"] = ffmpeg_merge_required;
    j["failure_category"] = ExtractionErrorCategoryToString(failure_category);
    j["failure_code"] = failure_code;
    j["failure_message"] = Logger::RedactSensitiveData(failure_message);
    j["extraction_duration_ms"] = extraction_duration_ms;
    return j.dump();
}

// ============================================================================
// VideoExtractionManager Static Classification & Helpers
// ============================================================================

std::string VideoExtractionManager::IdentifyPlatform(const std::string& url) {
    if (IsDirectMediaOrManifestUrl(url)) {
        return "direct_media";
    }

    std::string host = ExtractUrlHost(url);
    std::string path = ToLowerAscii(ExtractUrlPath(url));
    if (host.empty()) return "unknown";

    if (host == "youtu.be" || host.ends_with(".youtube.com") || host == "youtube.com" ||
        host.ends_with(".googlevideo.com")) {
        return "youtube";
    }
    if (host == "b23.tv" || host == "bilibili.com" || host.ends_with(".bilibili.com") ||
        host.ends_with(".bilivideo.com") || host.ends_with(".bilivideo.cn") ||
        host.ends_with(".hdslb.com")) {
        return "bilibili";
    }
    if (host == "fb.watch" || host == "facebook.com" || host.ends_with(".facebook.com") ||
        host.ends_with(".fbcdn.net")) {
        return "facebook";
    }
    if (host == "vimeo.com" || host.ends_with(".vimeo.com")) {
        return "vimeo";
    }
    if (host == "dailymotion.com" || host.ends_with(".dailymotion.com") || host == "dai.ly") {
        return "dailymotion";
    }
    if (host == "twitch.tv" || host.ends_with(".twitch.tv")) {
        return "twitch";
    }
    if (host == "reddit.com" || host.ends_with(".reddit.com") || host == "v.redd.it") {
        return "reddit";
    }
    if (host == "x.com" || host.ends_with(".x.com") ||
        host == "twitter.com" || host.ends_with(".twitter.com") ||
        host.ends_with(".twimg.com")) {
        return "twitter";
    }
    if (host == "instagram.com" || host.ends_with(".instagram.com")) {
        return "instagram";
    }
    if (host == "tiktok.com" || host.ends_with(".tiktok.com")) {
        return "tiktok";
    }
    if (host == "streamable.com" || host.ends_with(".streamable.com")) {
        return "streamable";
    }
    if (path.find("/videos/watch/") != std::string::npos ||
        path.find("/w/") == 0) {
        return "peertube";
    }

    if (IsDirectMediaOrManifestUrl(url)) {
        return "direct_media";
    }

    std::string reg_dom = ExtractRegistrableDomain(host);
    if (!reg_dom.empty() && host.find('.') != std::string::npos) {
        // Ensure host is not an IPv4 address (e.g. 127.0.0.1)
        bool is_ipv4 = true;
        for (char ch : host) {
            if (!((ch >= '0' && ch <= '9') || ch == '.')) {
                is_ipv4 = false;
                break;
            }
        }
        if (!is_ipv4 && host != "localhost") {
            size_t dot_pos = reg_dom.find('.');
            if (dot_pos != std::string::npos && dot_pos > 0) {
                return reg_dom.substr(0, dot_pos);
            }
            return reg_dom;
        }
    }

    return "generic";
}

bool VideoExtractionManager::IsDirectMediaOrManifestUrl(const std::string& url) {
    std::string path = ToLowerAscii(ExtractUrlPath(url));
    static const std::vector<std::string> kExts = {
        ".m3u8", ".mpd", ".mp4", ".webm", ".mkv", ".mov", ".flv", ".m4a", ".mp3", ".ts", ".m4s", ".aac", ".ogg"
    };
    for (const auto& ext : kExts) {
        if (path.ends_with(ext)) return true;
    }
    return false;
}

bool VideoExtractionManager::IsSupportedVideoSiteUrl(const std::string& url) {
    if (IsDirectMediaOrManifestUrl(url)) {
        return false;
    }
    if (PathSanitizer::IsWebpageWatchUrl(url)) {
        return true;
    }
    std::string platform = IdentifyPlatform(url);
    return platform != "unknown" && platform != "generic" && platform != "direct_media";
}

std::string VideoExtractionManager::StripCdnRangeTrapParams(const std::string& url) {
    if (url.empty()) return url;
    std::string lower = ToLowerAscii(url);
    bool is_googlevideo = lower.find("googlevideo.com/videoplayback") != std::string::npos;
    bool is_fbcdn = lower.find("fbcdn.net/") != std::string::npos ||
                    (lower.find("bytestart=") != std::string::npos && lower.find("byteend=") != std::string::npos);

    if (!is_googlevideo && !is_fbcdn) {
        return url;
    }

    auto qpos = url.find('?');
    if (qpos == std::string::npos) return url;

    std::string base = url.substr(0, qpos);
    std::string query_and_frag = url.substr(qpos + 1);
    std::string frag;
    auto fpos = query_and_frag.find('#');
    if (fpos != std::string::npos) {
        frag = query_and_frag.substr(fpos);
        query_and_frag = query_and_frag.substr(0, fpos);
    }

    std::vector<std::string> kept_params;
    std::stringstream ss(query_and_frag);
    std::string item;
    while (std::getline(ss, item, '&')) {
        if (item.empty()) continue;
        auto eq = item.find('=');
        std::string key = ToLowerAscii(eq == std::string::npos ? item : item.substr(0, eq));
        if (is_googlevideo && (key == "range" || key == "rn" || key == "rbuf" || key == "ump" || key == "srfvp")) {
            continue;
        }
        if (is_fbcdn && (key == "bytestart" || key == "byteend")) {
            continue;
        }
        kept_params.push_back(item);
    }

    std::string rebuilt = base;
    if (!kept_params.empty()) {
        rebuilt += "?";
        for (size_t i = 0; i < kept_params.size(); ++i) {
            if (i > 0) rebuilt += "&";
            rebuilt += kept_params[i];
        }
    }
    rebuilt += frag;
    return rebuilt;
}

std::string VideoExtractionManager::NormalizeExtractorUrl(const std::string& url) {
    std::string cleaned = StripCdnRangeTrapParams(url);
    std::string host = ExtractUrlHost(cleaned);
    if (host == "m.facebook.com" || host == "touch.facebook.com" || host == "mbasic.facebook.com") {
        auto pos = cleaned.find(host);
        if (pos != std::string::npos) {
            cleaned.replace(pos, host.size(), "www.facebook.com");
        }
    }
    return cleaned;
}

int64_t VideoExtractionManager::ParseUrlExpirationEpoch(const std::string& url) {
    if (url.empty()) return 0;

    static const std::regex dec_exp_re(
        R"((?:[?&;/]|%26|%3F)(?:expire|expires|exp|e|deadline|validto)(?:=|%3D|/)(1[7-9]\d{8}|2\d{9})(?:[&;/#]|$))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(url, m, dec_exp_re) && m.size() > 1) {
        try {
            return std::stoll(m[1].str());
        } catch (...) {}
    }

    static const std::regex hex_oe_re(R"((?:[?&])oe=([0-9a-fA-F]{8})(?:[&#]|$))");
    if (std::regex_search(url, m, hex_oe_re) && m.size() > 1) {
        try {
            return std::stoll(m[1].str(), nullptr, 16);
        } catch (...) {}
    }

    static const std::regex hdnea_exp_re(R"(exp=(1[7-9]\d{8}|2\d{9}))", std::regex::icase);
    if (std::regex_search(url, m, hdnea_exp_re) && m.size() > 1) {
        try {
            return std::stoll(m[1].str());
        } catch (...) {}
    }

    return 0;
}

bool VideoExtractionManager::IsStreamUrlExpired(const std::string& url, int64_t now_epoch_s, int64_t safety_margin_s) {
    int64_t exp = ParseUrlExpirationEpoch(url);
    if (exp <= 0) return false;
    if (now_epoch_s <= 0) {
        now_epoch_s = CurrentEpochSeconds();
    }
    return (now_epoch_s + safety_margin_s) >= exp;
}

std::string VideoExtractionManager::ExtractRegistrableDomain(const std::string& host) {
    std::string clean = ToLowerAscii(host);
    while (!clean.empty() && clean.front() == '.') clean.erase(clean.begin());
    if (clean.empty()) return "";

    std::vector<std::string> parts;
    std::stringstream ss(clean);
    std::string label;
    while (std::getline(ss, label, '.')) {
        if (!label.empty()) parts.push_back(label);
    }
    if (parts.size() <= 2) return clean;

    const std::string& last = parts.back();
    const std::string& second_last = parts[parts.size() - 2];
    if (last.size() == 2 &&
        (second_last == "co" || second_last == "com" || second_last == "net" ||
         second_last == "org" || second_last == "gov" || second_last == "edu") &&
        parts.size() >= 3) {
        return parts[parts.size() - 3] + "." + second_last + "." + last;
    }

    return second_last + "." + last;
}

std::map<std::string, std::string> VideoExtractionManager::BuildSiteHeaders(
    const std::string& url,
    const std::string& page_url,
    const std::map<std::string, std::string>& existing_headers) {
    std::map<std::string, std::string> result = existing_headers;

    auto has_header_ci = [&](const std::string& name) -> bool {
        std::string target = ToLowerAscii(name);
        for (const auto& [k, v] : result) {
            if (ToLowerAscii(k) == target && !v.empty()) return true;
        }
        return false;
    };

    std::string effective_site_url = !page_url.empty() ? page_url : url;
    std::string platform = IdentifyPlatform(effective_site_url);
    if (platform == "generic" || platform == "direct_media") {
        platform = IdentifyPlatform(url);
    }

    if (!has_header_ci("User-Agent")) {
        result["User-Agent"] = kDefaultDesktopUserAgent;
    }

    if (!has_header_ci("Referer")) {
        if (platform == "bilibili") {
            result["Referer"] = !page_url.empty() ? page_url : "https://www.bilibili.com/";
        } else if (platform == "facebook") {
            result["Referer"] = "https://www.facebook.com/";
        } else if (platform == "youtube") {
            result["Referer"] = "https://www.youtube.com/";
        } else if (!page_url.empty()) {
            result["Referer"] = page_url;
        } else {
            std::string origin = ExtractUrlOrigin(url);
            if (!origin.empty()) {
                result["Referer"] = origin + "/";
            }
        }
    }

    if (!has_header_ci("Origin")) {
        if (platform == "bilibili") {
            result["Origin"] = "https://www.bilibili.com";
        } else if (platform == "facebook") {
            result["Origin"] = "https://www.facebook.com";
        } else if (!page_url.empty()) {
            std::string origin = ExtractUrlOrigin(page_url);
            if (!origin.empty()) result["Origin"] = origin;
        }
    }

    return result;
}

ExtractionErrorCategory VideoExtractionManager::ClassifyError(
    const std::string& raw_error,
    int http_status) {
    std::string lower = ToLowerAscii(raw_error);

    if (lower.find("drm") != std::string::npos ||
        lower.find("widevine") != std::string::npos ||
        lower.find("playready") != std::string::npos ||
        lower.find("fairplay") != std::string::npos ||
        lower.find("sample-aes-ctr") != std::string::npos ||
        lower.find("cenc:pssh") != std::string::npos) {
        return ExtractionErrorCategory::kDrmProtected;
    }

    if (lower.find("login required") != std::string::npos ||
        lower.find("log in to continue") != std::string::npos ||
        lower.find("you must log in") != std::string::npos ||
        lower.find("authentication required") != std::string::npos ||
        lower.find("sign in to confirm") != std::string::npos ||
        lower.find("cookies-from-browser") != std::string::npos ||
        lower.find("registered users") != std::string::npos ||
        (lower.find("vip") != std::string::npos && lower.find("login") != std::string::npos)) {
        return ExtractionErrorCategory::kLoginRequired;
    }

    if (lower.find("private video") != std::string::npos ||
        lower.find("video is private") != std::string::npos ||
        lower.find("content isn't available") != std::string::npos ||
        lower.find("this content isn't available right now") != std::string::npos ||
        lower.find("members-only") != std::string::npos ||
        lower.find("premium only") != std::string::npos) {
        return ExtractionErrorCategory::kPrivateOrRestricted;
    }

    if (lower.find("age-restricted") != std::string::npos ||
        lower.find("confirm your age") != std::string::npos ||
        lower.find("inappropriate for some users") != std::string::npos) {
        return ExtractionErrorCategory::kAgeRestricted;
    }

    if (lower.find("not available in your country") != std::string::npos ||
        lower.find("geo-restricted") != std::string::npos ||
        (lower.find("region") != std::string::npos && lower.find("restrict") != std::string::npos) ||
        lower.find("仅限港澳台") != std::string::npos ||
        lower.find("仅限中国大陆") != std::string::npos) {
        return ExtractionErrorCategory::kGeoRestricted;
    }

    if (http_status == 410 || lower.find("http error 410") != std::string::npos ||
        lower.find("url expired") != std::string::npos ||
        lower.find("link has expired") != std::string::npos) {
        return ExtractionErrorCategory::kUrlExpired;
    }

    if (lower.find("unsupported url") != std::string::npos) {
        return ExtractionErrorCategory::kUnsupportedUrl;
    }

    if (lower.find("no video formats found") != std::string::npos ||
        lower.find("no video found") != std::string::npos ||
        lower.find("unable to extract video") != std::string::npos) {
        return ExtractionErrorCategory::kNoVideoFound;
    }

    if (http_status == 401 || http_status == 403) {
        return ExtractionErrorCategory::kLoginRequired;
    }

    if (lower.find("json") != std::string::npos || lower.find("parse") != std::string::npos) {
        return ExtractionErrorCategory::kExtractorParseFailure;
    }

    return ExtractionErrorCategory::kNetworkFailure;
}

// ============================================================================
// Native Bilibili Parser (window.__playinfo__ / player/playurl JSON + __INITIAL_STATE__)
// ============================================================================

Result<ExtractionMetadata> VideoExtractionManager::ParseBilibiliPlayInfoJson(
    const std::string& html_or_json,
    const std::string& page_url) {
    ExtractionMetadata meta;
    meta.input_url = page_url;
    meta.webpage_url = page_url;
    meta.platform_id = "bilibili";
    meta.extractor_name = "BilibiliExtractor";
    meta.extracted_at_epoch_s = CurrentEpochSeconds();
    meta.required_headers = BuildSiteHeaders(page_url, "", {});

    static const std::regex bvid_re(R"((BV[0-9A-Za-z]{10}|av\d+|ep\d+|ss\d+))", std::regex::icase);
    std::smatch id_match;
    if (std::regex_search(page_url, id_match, bvid_re) && id_match.size() > 1) {
        meta.video_id = id_match[1].str();
    }

    static const std::regex page_param_re(R"([?&]p=(\d+))");
    std::smatch p_match;
    if (std::regex_search(page_url, p_match, page_param_re) && p_match.size() > 1) {
        meta.current_page = std::max(1, std::atoi(p_match[1].str().c_str()));
    }

    auto extract_assigned_json = [](const std::string& text, const std::string& marker) -> std::string {
        auto pos = text.find(marker);
        if (pos == std::string::npos) return {};
        auto brace_start = text.find('{', pos + marker.size());
        if (brace_start == std::string::npos) return {};
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (size_t i = brace_start; i < text.size(); ++i) {
            char c = text[i];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    return text.substr(brace_start, i - brace_start + 1);
                }
            }
        }
        return {};
    };

    const std::string initial_state_raw = extract_assigned_json(html_or_json, "window.__INITIAL_STATE__");
    if (!initial_state_raw.empty()) {
        try {
            auto state_json = json::Parse(initial_state_raw);
            if (state_json.is_object()) {
                if (state_json["bvid"].is_string() && !state_json["bvid"].as_string().empty()) {
                    meta.video_id = state_json["bvid"].as_string();
                }
                if (state_json["videoData"].is_object()) {
                    const auto& vd = state_json["videoData"];
                    if (vd["title"].is_string()) {
                        meta.title = vd["title"].as_string();
                    }
                    if (vd["pic"].is_string()) {
                        meta.thumbnail_url = vd["pic"].as_string();
                    }
                    if (vd["duration"].is_number()) {
                        meta.duration_seconds = vd["duration"].as_double();
                    }
                    if (vd["owner"].is_object() && vd["owner"]["name"].is_string()) {
                        meta.uploader = vd["owner"]["name"].as_string();
                    }
                    if (vd["pages"].is_array()) {
                        std::string base_page_url = page_url;
                        auto qpos = base_page_url.find('?');
                        if (qpos != std::string::npos) base_page_url = base_page_url.substr(0, qpos);
                        const auto& pages_arr = vd["pages"].as_array();
                        for (const auto& p : pages_arr) {
                            MultipartEpisode ep;
                            ep.page_number = static_cast<int>(p["page"].as_int64(1));
                            if (p["cid"].is_number()) {
                                ep.part_id = std::to_string(p["cid"].as_int64(0));
                            }
                            ep.title = p["part"].as_string();
                            ep.duration_seconds = p["duration"].as_double(0.0);
                            ep.url = base_page_url + "?p=" + std::to_string(ep.page_number);
                            meta.episodes.push_back(ep);
                            if (ep.page_number == meta.current_page && !ep.title.empty() &&
                                pages_arr.size() > 1 && !meta.title.empty()) {
                                meta.title += " - P" + std::to_string(ep.page_number) + " " + ep.title;
                            }
                        }
                    }
                }
            }
        } catch (...) {}
    }

    if (meta.title.empty()) {
        meta.title = ExtractHtmlTitle(html_or_json);
    }
    if (meta.thumbnail_url.empty()) {
        meta.thumbnail_url = ExtractHtmlThumbnail(html_or_json);
    }

    std::string json_payload = extract_assigned_json(html_or_json, "window.__playinfo__");
    if (json_payload.empty()) {
        json_payload = html_or_json;
    }

    json::Value root;
    try {
        root = json::Parse(json_payload);
    } catch (...) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_PARSE_FAILURE",
            "Could not locate valid Bilibili playinfo JSON in response.");
    }
    if (!root.is_object()) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_PARSE_FAILURE",
            "Could not locate valid Bilibili playinfo JSON object.");
    }

    const json::Value* data_node = &root;
    if (root["data"].is_object()) {
        data_node = &root["data"];
    } else if (root["result"].is_object()) {
        data_node = &root["result"];
    }

    std::map<int, std::string> qn_desc_map = {
        {127, "8K"},      {126, "Dolby Vision"}, {125, "HDR"},   {120, "4K"},
        {116, "1080p60"}, {112, "1080p+"},       {80, "1080p"},  {74, "720p60"},
        {64, "720p"},     {32, "480p"},          {16, "360p"}
    };
    std::map<int, uint32_t> qn_height_map = {
        {127, 4320}, {126, 2160}, {125, 2160}, {120, 2160},
        {116, 1080}, {112, 1080}, {80, 1080},  {74, 720},
        {64, 720},   {32, 480},   {16, 360}
    };

    if ((*data_node)["timelength"].is_number()) {
        double tl_ms = (*data_node)["timelength"].as_double();
        if (meta.duration_seconds <= 0.0 && tl_ms > 0.0) {
            meta.duration_seconds = tl_ms / 1000.0;
        }
    }

    // 1. Parse DASH streams (`data.dash.video` and `data.dash.audio`)
    if ((*data_node)["dash"].is_object()) {
        const auto& dash = (*data_node)["dash"];
        double duration_s = dash["duration"].as_double(meta.duration_seconds);
        if (duration_s > 0.0 && meta.duration_seconds <= 0.0) {
            meta.duration_seconds = duration_s;
        }

        auto parse_audio_array = [&](const json::Value& arr_val) {
            if (!arr_val.is_array()) return;
            for (const auto& a : arr_val.as_array()) {
                if (!a.is_object()) continue;
                std::string url = a["baseUrl"].as_string(a["base_url"].as_string());
                if (url.empty()) continue;
                ExtractedFormat af;
                int id = static_cast<int>(a["id"].as_int64(30280));
                af.format_id = "audio_" + std::to_string(id);
                af.video_url = url;
                af.audio_url = url;
                af.has_video = false;
                af.has_audio = true;
                af.requires_merge = false;
                af.container = "m4a";
                af.acodec = a["codecs"].as_string("mp4a.40.2");
                uint64_t bw = static_cast<uint64_t>(a["bandwidth"].as_int64(0));
                af.abr_kbps = bw / 1000;
                af.tbr_kbps = af.abr_kbps;
                if (duration_s > 0.0 && bw > 0) {
                    af.filesize_bytes = static_cast<uint64_t>((bw / 8.0) * duration_s);
                }
                af.quality_label = "Audio Only";
                af.expires_at_epoch_s = ParseUrlExpirationEpoch(url);
                af.required_headers = meta.required_headers;
                meta.raw_audio_streams.push_back(std::move(af));
            }
        };

        if (dash.contains("audio")) {
            parse_audio_array(dash["audio"]);
        }
        if (dash["dolby"].is_object() && dash["dolby"].contains("audio")) {
            parse_audio_array(dash["dolby"]["audio"]);
        }

        if (dash["video"].is_array()) {
            for (const auto& v : dash["video"].as_array()) {
                if (!v.is_object()) continue;
                std::string url = v["baseUrl"].as_string(v["base_url"].as_string());
                if (url.empty()) continue;

                ExtractedFormat vf;
                int qn = static_cast<int>(v["id"].as_int64(0));
                vf.width = static_cast<uint32_t>(v["width"].as_int64(0));
                vf.height = static_cast<uint32_t>(v["height"].as_int64(0));
                if (vf.height == 0 && qn_height_map.count(qn)) {
                    vf.height = qn_height_map[qn];
                }
                std::string fr_str = v["frameRate"].as_string(v["frame_rate"].as_string());
                if (!fr_str.empty()) {
                    vf.fps = std::atof(fr_str.c_str());
                } else if (qn == 116 || qn == 74) {
                    vf.fps = 60.0;
                } else {
                    vf.fps = 30.0;
                }

                vf.vcodec = v["codecs"].as_string("avc1");
                vf.acodec = "none";
                vf.container = "mp4";
                vf.has_video = true;
                vf.has_audio = false;
                vf.requires_merge = true;
                vf.video_url = url;
                uint64_t bw = static_cast<uint64_t>(v["bandwidth"].as_int64(0));
                vf.vbr_kbps = bw / 1000;
                vf.tbr_kbps = vf.vbr_kbps;
                if (duration_s > 0.0 && bw > 0) {
                    vf.filesize_bytes = static_cast<uint64_t>((bw / 8.0) * duration_s);
                }
                vf.format_id = "dash_" + std::to_string(qn) + "_" + vf.vcodec.substr(0, std::min<size_t>(4, vf.vcodec.size()));
                vf.quality_label = BuildQualityLabel(vf.height, vf.width, vf.fps,
                                                     qn_desc_map.count(qn) ? qn_desc_map[qn] : "");
                vf.expires_at_epoch_s = ParseUrlExpirationEpoch(url);
                vf.required_headers = meta.required_headers;
                meta.raw_video_streams.push_back(std::move(vf));
            }
        }
    }

    // 2. Parse legacy/progressive `durl` streams if present
    if ((*data_node)["durl"].is_array()) {
        int qn = static_cast<int>((*data_node)["quality"].as_int64(64));
        for (const auto& d : (*data_node)["durl"].as_array()) {
            if (!d.is_object()) continue;
            std::string url = d["url"].as_string();
            if (url.empty()) continue;

            ExtractedFormat df;
            df.format_id = "durl_" + std::to_string(qn);
            df.height = qn_height_map.count(qn) ? qn_height_map[qn] : 720;
            df.fps = 30.0;
            df.quality_label = BuildQualityLabel(df.height, 0, df.fps, "");
            df.container = (url.find(".flv") != std::string::npos) ? "flv" : "mp4";
            df.vcodec = "avc1";
            df.acodec = "mp4a";
            df.has_video = true;
            df.has_audio = true;
            df.requires_merge = false;
            df.filesize_bytes = static_cast<uint64_t>(d["size"].as_int64(0));
            df.video_url = url;
            df.expires_at_epoch_s = ParseUrlExpirationEpoch(url);
            df.required_headers = meta.required_headers;
            meta.formats.push_back(std::move(df));
        }
    }

    ConsolidateAdaptiveFormats(meta);

    if (meta.formats.empty()) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_NO_VIDEO_FOUND",
            "No playable DASH or progressive video streams found in Bilibili playinfo.");
    }

    if (meta.title.empty()) {
        meta.title = !meta.video_id.empty() ? ("Bilibili_" + meta.video_id) : "Bilibili_Video";
    }

    meta.diagnostics.input_url = page_url;
    meta.diagnostics.normalized_url = page_url;
    meta.diagnostics.detected_platform = "bilibili";
    meta.diagnostics.selected_extractor = "BilibiliExtractor";
    meta.diagnostics.discovered_format_count = meta.formats.size();
    meta.diagnostics.stream_type = !meta.raw_video_streams.empty() ? "DASH" : "DIRECT_MP4";
    meta.diagnostics.ffmpeg_merge_required = !meta.raw_video_streams.empty();

    return Result<ExtractionMetadata>::Ok(std::move(meta));
}

// ============================================================================
// Native Facebook Video Parser
// ============================================================================

Result<ExtractionMetadata> VideoExtractionManager::ParseFacebookVideoHtml(
    const std::string& html,
    const std::string& page_url) {
    ExtractionMetadata meta;
    meta.input_url = page_url;
    meta.webpage_url = NormalizeExtractorUrl(page_url);
    meta.platform_id = "facebook";
    meta.extractor_name = "FacebookExtractor";
    meta.extracted_at_epoch_s = CurrentEpochSeconds();
    meta.required_headers = BuildSiteHeaders(meta.webpage_url, meta.webpage_url, {});

    static const std::regex fb_id_re(R"((?:videos/|reel/|v=|/v/|/r/)(\d{8,22}))");
    std::smatch id_m;
    if (std::regex_search(page_url, id_m, fb_id_re) && id_m.size() > 1) {
        meta.video_id = id_m[1].str();
    }

    meta.title = ExtractHtmlTitle(html);
    meta.thumbnail_url = ExtractHtmlThumbnail(html);

    auto add_fb_progressive = [&](const std::string& raw_url, uint32_t height, const std::string& label, const std::string& id) {
        std::string clean_url = StripCdnRangeTrapParams(UnescapeJsonSlashesAndUnicode(raw_url));
        if (!clean_url.starts_with("http://") && !clean_url.starts_with("https://")) return;
        for (const auto& existing : meta.formats) {
            if (existing.video_url == clean_url) return;
        }
        ExtractedFormat fmt;
        fmt.format_id = id;
        fmt.height = height;
        fmt.width = (height == 1080) ? 1920 : (height == 720 ? 1280 : 640);
        fmt.fps = 30.0;
        fmt.quality_label = label;
        fmt.container = "mp4";
        fmt.vcodec = "avc1";
        fmt.acodec = "mp4a";
        fmt.has_video = true;
        fmt.has_audio = true;
        fmt.requires_merge = false;
        fmt.video_url = clean_url;
        fmt.expires_at_epoch_s = ParseUrlExpirationEpoch(clean_url);
        fmt.required_headers = meta.required_headers;
        meta.formats.push_back(std::move(fmt));
    };

    static const std::vector<std::tuple<std::regex, uint32_t, std::string, std::string>> kFbPatterns = {
        {std::regex(R"re("browser_native_hd_url"\s*:\s*"(https?:[^"]+)")re"), 720, "720p", "fb_hd_native"},
        {std::regex(R"re("playable_url_quality_hd"\s*:\s*"(https?:[^"]+)")re"), 720, "720p", "fb_hd"},
        {std::regex(R"re("hd_src_no_ratelimit"\s*:\s*"(https?:[^"]+)")re"), 720, "720p", "fb_hd_noratelimit"},
        {std::regex(R"re("hd_src"\s*:\s*"(https?:[^"]+)")re"), 720, "720p", "fb_hd_src"},
        {std::regex(R"re("browser_native_sd_url"\s*:\s*"(https?:[^"]+)")re"), 360, "360p", "fb_sd_native"},
        {std::regex(R"re("playable_url"\s*:\s*"(https?:[^"]+)")re"), 360, "360p", "fb_sd"},
        {std::regex(R"re("sd_src_no_ratelimit"\s*:\s*"(https?:[^"]+)")re"), 360, "360p", "fb_sd_noratelimit"},
        {std::regex(R"re("sd_src"\s*:\s*"(https?:[^"]+)")re"), 360, "360p", "fb_sd_src"}
    };

    for (const auto& [re, height, label, id] : kFbPatterns) {
        std::sregex_iterator it(html.begin(), html.end(), re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            if (it->size() > 1) {
                add_fb_progressive((*it)[1].str(), height, label, id);
            }
        }
    }

    static const std::regex fb_rep_re(
        R"re(\{[^{}]*"base_url"\s*:\s*"(https?:[^"]+)"[^{}]*\})re");
    {
        std::sregex_iterator it(html.begin(), html.end(), fb_rep_re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string obj_str = (*it)[0].str();
            std::string base_url = StripCdnRangeTrapParams(UnescapeJsonSlashesAndUnicode((*it)[1].str()));
            if (base_url.empty()) continue;

            uint32_t h = 0;
            uint32_t w = 0;
            uint64_t bw = 0;
            std::string mime;
            std::string codecs;
            std::smatch m;
            static const std::regex h_re(R"re("height"\s*:\s*(\d+))re");
            static const std::regex w_re(R"re("width"\s*:\s*(\d+))re");
            static const std::regex bw_re(R"re("bandwidth"\s*:\s*(\d+))re");
            static const std::regex mime_re(R"re("mime_type"\s*:\s*"([^"]+)")re");
            static const std::regex codecs_re(R"re("codecs"\s*:\s*"([^"]+)")re");
            if (std::regex_search(obj_str, m, h_re)) h = static_cast<uint32_t>(std::stoul(m[1].str()));
            if (std::regex_search(obj_str, m, w_re)) w = static_cast<uint32_t>(std::stoul(m[1].str()));
            if (std::regex_search(obj_str, m, bw_re)) bw = std::stoull(m[1].str());
            if (std::regex_search(obj_str, m, mime_re)) mime = m[1].str();
            if (std::regex_search(obj_str, m, codecs_re)) codecs = m[1].str();

            bool is_audio = (mime.find("audio") != std::string::npos) ||
                            (h == 0 && codecs.find("mp4a") != std::string::npos);
            ExtractedFormat rep;
            rep.video_url = base_url;
            rep.expires_at_epoch_s = ParseUrlExpirationEpoch(base_url);
            rep.required_headers = meta.required_headers;
            if (is_audio) {
                rep.format_id = "fb_dash_audio_" + std::to_string(bw / 1000);
                rep.has_video = false;
                rep.has_audio = true;
                rep.container = "m4a";
                rep.acodec = codecs.empty() ? "mp4a.40.2" : codecs;
                rep.abr_kbps = bw / 1000;
                rep.tbr_kbps = rep.abr_kbps;
                rep.quality_label = "Audio Only";
                meta.raw_audio_streams.push_back(std::move(rep));
            } else if (h > 0) {
                rep.format_id = "fb_dash_" + std::to_string(h) + "p";
                rep.width = w;
                rep.height = h;
                rep.fps = 30.0;
                rep.has_video = true;
                rep.has_audio = false;
                rep.requires_merge = true;
                rep.container = "mp4";
                rep.vcodec = codecs.empty() ? "avc1" : codecs;
                rep.vbr_kbps = bw / 1000;
                rep.tbr_kbps = rep.vbr_kbps;
                rep.quality_label = BuildQualityLabel(h, w, 30.0, "");
                meta.raw_video_streams.push_back(std::move(rep));
            }
        }
    }

    ConsolidateAdaptiveFormats(meta);

    if (meta.formats.empty()) {
        std::string lower_html = ToLowerAscii(html);
        if (lower_html.find("login_form") != std::string::npos ||
            lower_html.find("you must log in to continue") != std::string::npos ||
            lower_html.find("log in to facebook") != std::string::npos) {
            return Result<ExtractionMetadata>::Err(
                "ERR_EXTRACTOR_LOGIN_REQUIRED",
                "Facebook requires an authenticated browser session to view or download this video.");
        }
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_NO_VIDEO_FOUND",
            "Could not extract playable video streams from Facebook page.");
    }

    if (meta.title.empty()) {
        meta.title = !meta.video_id.empty() ? ("Facebook_Video_" + meta.video_id) : "Facebook_Video";
    }

    meta.diagnostics.input_url = page_url;
    meta.diagnostics.normalized_url = meta.webpage_url;
    meta.diagnostics.detected_platform = "facebook";
    meta.diagnostics.selected_extractor = "FacebookExtractor";
    meta.diagnostics.discovered_format_count = meta.formats.size();
    meta.diagnostics.stream_type = !meta.raw_video_streams.empty() ? "DASH" : "DIRECT_MP4";
    meta.diagnostics.ffmpeg_merge_required = !meta.raw_video_streams.empty();

    return Result<ExtractionMetadata>::Ok(std::move(meta));
}

// ============================================================================
// Native HTML5 & Standard Web Player Parser
// ============================================================================

Result<ExtractionMetadata> VideoExtractionManager::ParseHtml5PlayerPage(
    const std::string& html,
    const std::string& page_url) {
    ExtractionMetadata meta;
    meta.input_url = page_url;
    meta.webpage_url = page_url;
    meta.platform_id = IdentifyPlatform(page_url);
    meta.extractor_name = "GenericWebVideoExtractor";
    meta.extracted_at_epoch_s = CurrentEpochSeconds();
    meta.required_headers = BuildSiteHeaders(page_url, page_url, {});

    meta.title = ExtractHtmlTitle(html);
    meta.thumbnail_url = ExtractHtmlThumbnail(html);

    auto add_discovered_stream = [&](const std::string& raw_url, uint32_t height, const std::string& format_hint) {
        std::string url = StripCdnRangeTrapParams(UnescapeJsonSlashesAndUnicode(raw_url));
        if (!url.starts_with("http://") && !url.starts_with("https://")) return;
        std::string lower_u = ToLowerAscii(url);
        if (lower_u.find(".jpg") != std::string::npos || lower_u.find(".webp") != std::string::npos ||
            lower_u.find(".png") != std::string::npos || lower_u.find(".vtt") != std::string::npos ||
            lower_u.find(".srt") != std::string::npos || lower_u.find(" thumb") != std::string::npos) {
            return;
        }
        if (lower_u.find("/media/mp4?") != std::string::npos || lower_u.find("/media/hls?") != std::string::npos) {
            return;
        }

        for (auto& existing : meta.formats) {
            if (existing.video_url == url) {
                if (existing.height == 0 && height > 0) {
                    existing.height = height;
                    existing.width = (height == 2160) ? 3840 : (height == 1440 ? 2560 : (height == 1080 ? 1920 : (height == 720 ? 1280 : (height == 480 ? 854 : (height == 360 ? 640 : 0)))));
                    existing.quality_label = std::to_string(height) + "p";
                    existing.format_id = (existing.protocol == "m3u8_native" ? "hls_" : "mp4_") + std::to_string(height) + "p";
                }
                return;
            }
        }

        bool is_hls = (lower_u.find(".m3u8") != std::string::npos) || (ToLowerAscii(format_hint) == "hls");
        bool is_dash = (lower_u.find(".mpd") != std::string::npos) || (ToLowerAscii(format_hint) == "dash");

        ExtractedFormat fmt;
        fmt.height = height;
        fmt.width = (height == 2160) ? 3840 : (height == 1440 ? 2560 : (height == 1080 ? 1920 : (height == 720 ? 1280 : (height == 480 ? 854 : (height == 360 ? 640 : 0)))));
        fmt.fps = 30.0;
        fmt.quality_label = height > 0 ? (std::to_string(height) + "p") : (is_hls ? "HLS Stream" : "Best Available");
        fmt.container = (lower_u.find(".webm") != std::string::npos) ? "webm" : "mp4";
        fmt.vcodec = (fmt.container == "webm") ? "vp9" : "avc1";
        fmt.acodec = (fmt.container == "webm") ? "opus" : "mp4a";
        fmt.has_video = true;
        fmt.has_audio = true;
        fmt.requires_merge = false;
        fmt.protocol = is_hls ? "m3u8_native" : (is_dash ? "http_dash_segments" : "https");
        fmt.format_id = (is_hls ? "hls_" : "mp4_") + (height > 0 ? std::to_string(height) + "p" : "auto");
        fmt.video_url = url;
        fmt.expires_at_epoch_s = ParseUrlExpirationEpoch(url);
        fmt.required_headers = meta.required_headers;
        meta.formats.push_back(std::move(fmt));
    };

    static const std::regex media_def_re(
        R"re(\{[^{}]*"videoUrl"\s*:\s*"(https?:[^"]+)"[^{}]*\})re");
    {
        std::sregex_iterator it(html.begin(), html.end(), media_def_re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string block = (*it)[0].str();
            std::string vurl = (*it)[1].str();
            uint32_t h = 0;
            std::string fmt_type;
            std::smatch m;
            static const std::regex q_str_re(R"re("quality"\s*:\s*"(\d{3,4})p?")re");
            static const std::regex q_num_re(R"re("quality"\s*:\s*(\d{3,4}))re");
            static const std::regex fmt_re(R"re("format"\s*:\s*"([^"]+)")re");
            if (std::regex_search(block, m, q_str_re)) h = static_cast<uint32_t>(std::stoul(m[1].str()));
            else if (std::regex_search(block, m, q_num_re)) h = static_cast<uint32_t>(std::stoul(m[1].str()));
            if (std::regex_search(block, m, fmt_re)) fmt_type = m[1].str();
            add_discovered_stream(vurl, h, fmt_type);
        }
    }

    static const std::regex xv_title_re(R"re(html5player\.setVideoTitle\s*\(\s*['"]([^'"]+)['"]\s*\))re");
    std::smatch xv_m;
    if (meta.title.empty() && std::regex_search(html, xv_m, xv_title_re) && xv_m.size() > 1) {
        meta.title = UnescapeJsonSlashesAndUnicode(xv_m[1].str());
    }
    static const std::regex xv_hls_re(R"re(html5player\.setVideoHLS\s*\(\s*['"](https?:[^'"]+)['"]\s*\))re");
    if (std::regex_search(html, xv_m, xv_hls_re) && xv_m.size() > 1) {
        add_discovered_stream(xv_m[1].str(), 1080, "hls");
    }
    static const std::regex xv_high_re(R"re(html5player\.setVideoUrlHigh\s*\(\s*['"](https?:[^'"]+)['"]\s*\))re");
    if (std::regex_search(html, xv_m, xv_high_re) && xv_m.size() > 1) {
        add_discovered_stream(xv_m[1].str(), 720, "mp4");
    }
    static const std::regex xv_low_re(R"re(html5player\.setVideoUrlLow\s*\(\s*['"](https?:[^'"]+)['"]\s*\))re");
    if (std::regex_search(html, xv_m, xv_low_re) && xv_m.size() > 1) {
        add_discovered_stream(xv_m[1].str(), 360, "mp4");
    }

    static const std::regex quality_kv_re(
        R"re(["']?(?:quality_)?(2160|1440|1080|720|480|360|240)p?["']?\s*:\s*\[?\s*["'](https?:[^"']+)["'])re",
        std::regex::icase);
    {
        std::sregex_iterator it(html.begin(), html.end(), quality_kv_re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            uint32_t h = static_cast<uint32_t>(std::stoul((*it)[1].str()));
            add_discovered_stream((*it)[2].str(), h, "");
        }
    }

    static const std::regex source_tag_re(R"(<source\s+[^>]*src=[\"'](https?:[^\"']+)[\"'][^>]*>)", std::regex::icase);
    {
        std::sregex_iterator it(html.begin(), html.end(), source_tag_re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            std::string tag = (*it)[0].str();
            std::string src = (*it)[1].str();
            uint32_t h = 0;
            std::smatch qm;
            static const std::regex res_attr_re(R"((?:res|label|title|data-quality)=[\"']?(\d{3,4})p?[\"']?)", std::regex::icase);
            if (std::regex_search(tag, qm, res_attr_re)) {
                h = static_cast<uint32_t>(std::stoul(qm[1].str()));
            }
            add_discovered_stream(src, h, "");
        }
    }

    static const std::regex manifest_url_re(
        R"([\"'](https?://[^\"'\s<>\\]+?\.(?:m3u8|mpd)(?:\?[^\"'\s<>\\]*)?)[\"'])",
        std::regex::icase);
    {
        std::sregex_iterator it(html.begin(), html.end(), manifest_url_re);
        std::sregex_iterator end;
        for (; it != end; ++it) {
            add_discovered_stream((*it)[1].str(), 0, "hls");
        }
    }

    if (meta.formats.empty()) {
        static const std::regex video_tag_src_re(R"(<video\s+[^>]*src=[\"'](https?:[^\"']+)[\"'][^>]*>)", std::regex::icase);
        {
            std::sregex_iterator it(html.begin(), html.end(), video_tag_src_re);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                add_discovered_stream((*it)[1].str(), 0, "");
            }
        }

        static const std::regex common_player_var_re(
            R"re(["'](?:video_url|videoUrl|video_src|file|src|stream_url|get_file)["']\s*:\s*["'](https?:[^"']+)["'])re",
            std::regex::icase);
        {
            std::sregex_iterator it(html.begin(), html.end(), common_player_var_re);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                add_discovered_stream((*it)[1].str(), 0, "");
            }
        }

        static const std::regex direct_mp4_url_re(
            R"([\"'](https?://[^\"'\s<>\\]+?\.(?:mp4|webm)(?:\?[^\"'\s<>\\]*)?)[\"'])",
            std::regex::icase);
        {
            std::sregex_iterator it(html.begin(), html.end(), direct_mp4_url_re);
            std::sregex_iterator end;
            for (; it != end; ++it) {
                add_discovered_stream((*it)[1].str(), 0, "");
            }
        }
    }

    ConsolidateAdaptiveFormats(meta);

    if (meta.formats.empty()) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_NO_VIDEO_FOUND",
            "No playable video streams found in page HTML.");
    }

    if (meta.title.empty()) {
        meta.title = "Extracted_Video";
    }

    meta.diagnostics.input_url = page_url;
    meta.diagnostics.normalized_url = page_url;
    meta.diagnostics.detected_platform = meta.platform_id;
    meta.diagnostics.selected_extractor = "GenericWebVideoExtractor";
    meta.diagnostics.discovered_format_count = meta.formats.size();
    meta.diagnostics.stream_type = meta.formats.front().protocol == "m3u8_native" ? "HLS" : "DIRECT_MP4";

    return Result<ExtractionMetadata>::Ok(std::move(meta));
}

// ============================================================================
// yt-dlp -J JSON Parser
// ============================================================================

Result<ExtractionMetadata> VideoExtractionManager::ParseYtDlpJson(
    const std::string& json_str,
    const std::string& fallback_url) {
    std::string clean_json = json_str;
    auto first_brace = clean_json.find('{');
    auto last_brace = clean_json.rfind('}');
    if (first_brace != std::string::npos && last_brace != std::string::npos && last_brace > first_brace) {
        clean_json = clean_json.substr(first_brace, last_brace - first_brace + 1);
    }

    json::Value j;
    try {
        j = json::Parse(clean_json);
    } catch (...) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_PARSE_FAILURE",
            "Failed to parse extractor JSON metadata.");
    }
    if (!j.is_object()) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_PARSE_FAILURE",
            "Extractor JSON root is not an object.");
    }

    const json::Value* video_node = &j;
    ExtractionMetadata meta;
    if (j["entries"].is_array() && !j["entries"].as_array().empty()) {
        int idx = 1;
        for (const auto& entry : j["entries"].as_array()) {
            if (!entry.is_object()) continue;
            MultipartEpisode ep;
            ep.page_number = idx++;
            ep.part_id = entry["id"].as_string();
            ep.title = entry["title"].as_string();
            ep.duration_seconds = entry["duration"].as_double(0.0);
            ep.url = entry["webpage_url"].as_string(entry["url"].as_string());
            meta.episodes.push_back(ep);
        }
        if (j["entries"].as_array()[0].is_object()) {
            video_node = &j["entries"].as_array()[0];
        }
    }

    meta.input_url = fallback_url;
    meta.webpage_url = (*video_node)["webpage_url"].as_string(fallback_url);
    meta.video_id = (*video_node)["id"].as_string();
    meta.title = (*video_node)["title"].as_string();
    meta.uploader = (*video_node)["uploader"].as_string((*video_node)["channel"].as_string());
    meta.duration_seconds = (*video_node)["duration"].as_double(0.0);
    meta.thumbnail_url = (*video_node)["thumbnail"].as_string();
    meta.is_live = (*video_node)["is_live"].as_bool(false);
    meta.extracted_at_epoch_s = CurrentEpochSeconds();

    std::string drm_field = ToLowerAscii((*video_node)["drm"].as_string());
    if (!drm_field.empty() || (*video_node)["has_drm"].as_bool(false)) {
        meta.is_drm_protected = true;
    }
    std::string extractor_key = ToLowerAscii((*video_node)["extractor_key"].as_string((*video_node)["extractor"].as_string()));
    meta.platform_id = IdentifyPlatform(meta.webpage_url);
    if (meta.platform_id == "generic" && !extractor_key.empty()) {
        meta.platform_id = extractor_key;
    }
    meta.extractor_name = extractor_key.empty() ? "YtDlpExtractor" : ("YtDlp:" + extractor_key);
    meta.required_headers = BuildSiteHeaders(meta.webpage_url, meta.webpage_url, {});

    if ((*video_node)["http_headers"].is_object()) {
        for (const auto& [hk, hv] : (*video_node)["http_headers"].as_object()) {
            if (hv.is_string()) {
                meta.required_headers[hk] = hv.as_string();
            }
        }
    }

    if ((*video_node)["subtitles"].is_object()) {
        for (const auto& [lang, sub_arr] : (*video_node)["subtitles"].as_object()) {
            if (sub_arr.is_array() && !sub_arr.as_array().empty() && sub_arr.as_array()[0].is_object()) {
                const auto& s0 = sub_arr.as_array()[0];
                SubtitleTrack sub;
                sub.language_code = lang;
                sub.language_name = s0["name"].as_string(lang);
                sub.url = s0["url"].as_string();
                sub.ext = s0["ext"].as_string("vtt");
                sub.is_auto_generated = false;
                if (!sub.url.empty()) meta.subtitles.push_back(std::move(sub));
            }
        }
    }

    if ((*video_node)["formats"].is_array()) {
        for (const auto& f : (*video_node)["formats"].as_array()) {
            if (!f.is_object()) continue;
            std::string url = f["url"].as_string();
            std::string manifest_url = f["manifest_url"].as_string();
            if (url.empty() && manifest_url.empty()) continue;

            std::string vcodec = f["vcodec"].as_string("none");
            std::string acodec = f["acodec"].as_string("none");
            std::string ext = f["ext"].as_string("mp4");
            std::string format_note = f["format_note"].as_string();
            std::string format_id = f["format_id"].as_string();

            if (ext == "mhtml" || format_note == "storyboard" ||
                (vcodec == "none" && acodec == "none" && ext != "mp4" && ext != "webm" && ext != "m3u8")) {
                continue;
            }

            ExtractedFormat ef;
            ef.format_id = format_id;
            ef.width = static_cast<uint32_t>(f["width"].as_int64(0));
            ef.height = static_cast<uint32_t>(f["height"].as_int64(0));
            ef.fps = f["fps"].as_double(30.0);
            ef.vcodec = vcodec;
            ef.acodec = acodec;
            ef.container = ext;
            ef.protocol = f["protocol"].as_string("https");
            ef.video_url = StripCdnRangeTrapParams(url.empty() ? manifest_url : url);

            if (f["filesize"].is_number() && f["filesize"].as_int64(0) > 0) {
                ef.filesize_bytes = static_cast<uint64_t>(f["filesize"].as_int64(0));
            } else if (f["filesize_approx"].is_number() && f["filesize_approx"].as_int64(0) > 0) {
                ef.filesize_bytes = static_cast<uint64_t>(f["filesize_approx"].as_int64(0));
            }
            if (f["tbr"].is_number()) {
                ef.tbr_kbps = static_cast<uint64_t>(f["tbr"].as_double(0.0));
            }
            if (f["vbr"].is_number()) {
                ef.vbr_kbps = static_cast<uint64_t>(f["vbr"].as_double(0.0));
            }
            if (f["abr"].is_number()) {
                ef.abr_kbps = static_cast<uint64_t>(f["abr"].as_double(0.0));
            }
            if (ef.filesize_bytes == 0 && ef.tbr_kbps > 0 && meta.duration_seconds > 0.0) {
                ef.filesize_bytes = static_cast<uint64_t>((ef.tbr_kbps * 1000.0 / 8.0) * meta.duration_seconds);
            }

            if (f["has_drm"].as_bool(false)) {
                ef.is_drm_protected = true;
                ef.is_downloadable = false;
                meta.is_drm_protected = true;
            }

            bool has_v = (vcodec != "none") && (ef.height > 0 || ef.width > 0 || !vcodec.empty());
            bool has_a = (acodec != "none");
            if (vcodec == "none" && acodec == "none" && (ext == "mp4" || ext == "webm" || ef.protocol.find("m3u8") != std::string::npos)) {
                has_v = true;
                has_a = true;
            } else if (ef.height > 0 && vcodec == "none") {
                has_v = true;
            }

            ef.has_video = has_v;
            ef.has_audio = has_a;
            ef.requires_merge = (has_v && !has_a);
            ef.quality_label = has_v ? BuildQualityLabel(ef.height, ef.width, ef.fps, format_note) : "Audio Only";
            ef.expires_at_epoch_s = ParseUrlExpirationEpoch(ef.video_url);
            ef.required_headers = meta.required_headers;

            if (f["http_headers"].is_object()) {
                for (const auto& [hk, hv] : f["http_headers"].as_object()) {
                    if (hv.is_string()) {
                        ef.required_headers[hk] = hv.as_string();
                    }
                }
            }

            if (has_v && has_a) {
                meta.formats.push_back(std::move(ef));
            } else if (has_v && !has_a) {
                meta.raw_video_streams.push_back(std::move(ef));
            } else if (!has_v && has_a) {
                meta.raw_audio_streams.push_back(std::move(ef));
            }
        }
    } else if ((*video_node)["url"].is_string() && !(*video_node)["url"].as_string().empty()) {
        ExtractedFormat ef;
        ef.format_id = (*video_node)["format_id"].as_string("default");
        ef.width = static_cast<uint32_t>((*video_node)["width"].as_int64(0));
        ef.height = static_cast<uint32_t>((*video_node)["height"].as_int64(0));
        ef.fps = (*video_node)["fps"].as_double(30.0);
        ef.vcodec = (*video_node)["vcodec"].as_string("avc1");
        ef.acodec = (*video_node)["acodec"].as_string("mp4a");
        ef.container = (*video_node)["ext"].as_string("mp4");
        ef.has_video = true;
        ef.has_audio = true;
        ef.requires_merge = false;
        ef.video_url = StripCdnRangeTrapParams((*video_node)["url"].as_string());
        ef.quality_label = BuildQualityLabel(ef.height, ef.width, ef.fps, "");
        ef.expires_at_epoch_s = ParseUrlExpirationEpoch(ef.video_url);
        ef.required_headers = meta.required_headers;
        meta.formats.push_back(std::move(ef));
    }

    ConsolidateAdaptiveFormats(meta);

    if (meta.is_drm_protected && meta.formats.empty()) {
        return Result<ExtractionMetadata>::Err(
            "ERR_STREAM_DRM_PROTECTED",
            "This stream is protected by DRM (Widevine/PlayReady/FairPlay) and cannot be downloaded.");
    }

    if (meta.formats.empty()) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_NO_VIDEO_FOUND",
            "No downloadable video or audio formats were found for this URL.");
    }

    meta.diagnostics.input_url = fallback_url;
    meta.diagnostics.normalized_url = meta.webpage_url;
    meta.diagnostics.detected_platform = meta.platform_id;
    meta.diagnostics.selected_extractor = meta.extractor_name;
    meta.diagnostics.discovered_format_count = meta.formats.size();
    meta.diagnostics.stream_type = !meta.raw_video_streams.empty() ? "DASH" : "DIRECT_MP4";
    meta.diagnostics.ffmpeg_merge_required = meta.formats.front().requires_merge;

    return Result<ExtractionMetadata>::Ok(std::move(meta));
}

// ============================================================================
// Concrete Modular Extractors
// ============================================================================

namespace {

Result<ExtractionMetadata> RunYtDlpMetadataProbe(const ExtractionRequest& req) {
    std::error_code ec;
    std::filesystem::path ytdlp_bin = PathSanitizer::FindWorkerBinary("yt-dlp", PathSanitizer::GetExecutableDir());
    if (ytdlp_bin.empty() || !std::filesystem::exists(ytdlp_bin, ec)) {
        return Result<ExtractionMetadata>::Err(
            "ERR_YTDLP_NOT_FOUND",
            "yt-dlp.exe extractor engine was not found in tools/ directory.");
    }

    std::string normalized_url = VideoExtractionManager::NormalizeExtractorUrl(req.url);
    auto site_headers = VideoExtractionManager::BuildSiteHeaders(normalized_url, req.page_url, req.headers);

    std::filesystem::path temp_cookie_path;
    auto cleanup_cookie = [&]() {
        if (!temp_cookie_path.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(temp_cookie_path, rm_ec);
        }
    };

    for (const auto& [k, v] : site_headers) {
        if (ToLowerAscii(k) == "cookie" && !v.empty()) {
            auto tmp_dir = PathSanitizer::GetDefaultAppDataDir() / "tmp" / "extract_probe";
            std::filesystem::create_directories(tmp_dir, ec);
            std::string fname = "cookies_" + std::to_string(CurrentEpochSeconds()) + "_" +
                                std::to_string(reinterpret_cast<uintptr_t>(&req)) + ".txt";
            auto ck_res = stream::Nm3u8dlReAdapter::WriteNetscapeCookieFile(
                tmp_dir, fname, v, !req.page_url.empty() ? req.page_url : normalized_url);
            if (ck_res.ok() && ck_res.value) {
                temp_cookie_path = *ck_res.value;
            }
            break;
        }
    }

#ifdef _WIN32
    std::vector<std::wstring> args;
    args.push_back(L"--dump-single-json");
    args.push_back(L"--no-warnings");
    args.push_back(L"--no-playlist");
    args.push_back(L"--socket-timeout");
    args.push_back(L"15");

    if (req.episode_page > 0) {
        args.push_back(L"--playlist-items");
        args.push_back(std::to_wstring(req.episode_page));
    }

    for (const auto& [k, v] : site_headers) {
        if (v.empty()) continue;
        std::string lk = ToLowerAscii(k);
        if (lk == "cookie") continue;
        if (lk == "user-agent") {
            args.push_back(L"--user-agent");
            args.push_back(Utf8ToWideLocal(v));
        } else if (lk == "referer") {
            args.push_back(L"--referer");
            args.push_back(Utf8ToWideLocal(v));
        } else {
            args.push_back(L"--add-header");
            args.push_back(Utf8ToWideLocal(k + ":" + v));
        }
    }

    if (!temp_cookie_path.empty() && std::filesystem::exists(temp_cookie_path, ec)) {
        args.push_back(L"--cookies");
        args.push_back(temp_cookie_path.wstring());
    }

    std::filesystem::path node_bin = PathSanitizer::FindWorkerBinary("node", PathSanitizer::GetExecutableDir());
    if (std::filesystem::exists(node_bin, ec)) {
        args.push_back(L"--js-runtimes");
        args.push_back(L"node:" + node_bin.wstring());
    }

    args.push_back(Utf8ToWideLocal(normalized_url));

    std::wstring cmd_line = L"\"" + ytdlp_bin.wstring() + L"\"";
    for (const auto& a : args) {
        cmd_line += L" \"";
        for (wchar_t c : a) {
            if (c == L'"') cmd_line += L"\\\"";
            else cmd_line += c;
        }
        cmd_line += L"\"";
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE stdout_r = nullptr, stdout_w = nullptr;
    if (!CreatePipe(&stdout_r, &stdout_w, &sa, 0)) {
        cleanup_cookie();
        return Result<ExtractionMetadata>::Err("ERR_PIPE_CREATE", "Failed to create pipe for yt-dlp probe.");
    }
    SetHandleInformation(stdout_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = stdout_w;
    si.hStdError = stdout_w;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmd(cmd_line.begin(), cmd_line.end());
    mutable_cmd.push_back(L'\0');

    BOOL created = CreateProcessW(
        nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(stdout_w);

    if (!created) {
        CloseHandle(stdout_r);
        cleanup_cookie();
        return Result<ExtractionMetadata>::Err("ERR_PROCESS_SPAWN", "Failed to launch yt-dlp process.");
    }

    std::string output;
    char buf[4096];
    DWORD bytes_read = 0;
    while (ReadFile(stdout_r, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buf, bytes_read);
    }
    CloseHandle(stdout_r);

    WaitForSingleObject(pi.hProcess, req.timeout_ms > 0 ? req.timeout_ms : 25000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    cleanup_cookie();

    if (exit_code == 0 && !output.empty()) {
        auto parsed = VideoExtractionManager::ParseYtDlpJson(output, normalized_url);
        if (parsed.ok()) {
            return parsed;
        }
    }

    auto err_cat = VideoExtractionManager::ClassifyError(output);
    return Result<ExtractionMetadata>::Err(
        ExtractionErrorCategoryToCode(err_cat),
        Logger::RedactSensitiveData(output.empty() ? "yt-dlp metadata probe failed" : output.substr(0, 400)));
#else
    cleanup_cookie();
    return Result<ExtractionMetadata>::Err("ERR_UNSUPPORTED_OS", "Process execution requires Windows host.");
#endif
}

Result<std::string> FetchWebpageHtml(const std::string& url, const std::map<std::string, std::string>& headers) {
    std::string body;
    std::atomic<bool> cancel{false};
    int status_code = 0;
    auto res = brisk::HttpClient::PerformGet(
        url,
        headers,
        std::nullopt,
        std::nullopt,
        10000,
        true,
        cancel,
        [&](const brisk::HttpResponseMeta& meta) {
            status_code = meta.status_code;
            return true;
        },
        [&](const uint8_t* data, size_t len) {
            if (body.size() + len > 4 * 1024 * 1024) return false;
            body.append(reinterpret_cast<const char*>(data), len);
            return true;
        });
    if (!res.ok()) {
        return Result<std::string>::Err(res.error->code, res.error->message);
    }
    if (status_code >= 400) {
        auto cat = VideoExtractionManager::ClassifyError(body, status_code);
        return Result<std::string>::Err(
            ExtractionErrorCategoryToCode(cat),
            "HTTP status " + std::to_string(status_code));
    }
    return Result<std::string>::Ok(std::move(body));
}

class BilibiliExtractor final : public IVideoExtractor {
public:
    std::string Name() const override { return "BilibiliExtractor"; }
    std::string PlatformId(const std::string&) const override { return "bilibili"; }

    bool SupportsUrl(const std::string& url) const override {
        return VideoExtractionManager::IdentifyPlatform(url) == "bilibili";
    }

    std::map<std::string, std::string> GetDefaultHeaders(const std::string& url) const override {
        return VideoExtractionManager::BuildSiteHeaders(url, url, {});
    }

    Result<ExtractionMetadata> Extract(const ExtractionRequest& request) override {
        auto headers = VideoExtractionManager::BuildSiteHeaders(request.url, request.page_url, request.headers);

        auto html_res = FetchWebpageHtml(request.url, headers);
        if (html_res.ok() && html_res.value && !html_res.value->empty()) {
            auto native_res = VideoExtractionManager::ParseBilibiliPlayInfoJson(*html_res.value, request.url);
            if (native_res.ok() && native_res.value && !native_res.value->formats.empty()) {
                return native_res;
            }
        }

        return RunYtDlpMetadataProbe(request);
    }
};

class FacebookExtractor final : public IVideoExtractor {
public:
    std::string Name() const override { return "FacebookExtractor"; }
    std::string PlatformId(const std::string&) const override { return "facebook"; }

    bool SupportsUrl(const std::string& url) const override {
        return VideoExtractionManager::IdentifyPlatform(url) == "facebook";
    }

    std::map<std::string, std::string> GetDefaultHeaders(const std::string& url) const override {
        return VideoExtractionManager::BuildSiteHeaders(url, url, {});
    }

    Result<ExtractionMetadata> Extract(const ExtractionRequest& request) override {
        std::string norm_url = VideoExtractionManager::NormalizeExtractorUrl(request.url);
        std::string host = ExtractUrlHost(norm_url);
        if (host.ends_with(".fbcdn.net")) {
            ExtractionMetadata meta;
            meta.input_url = request.url;
            meta.webpage_url = !request.page_url.empty() ? request.page_url : norm_url;
            meta.platform_id = "facebook";
            meta.extractor_name = "FacebookExtractor";
            meta.title = "Facebook_Video";
            meta.extracted_at_epoch_s = CurrentEpochSeconds();
            meta.required_headers = VideoExtractionManager::BuildSiteHeaders(norm_url, request.page_url, request.headers);

            ExtractedFormat fmt;
            fmt.format_id = "fbcdn_direct";
            fmt.quality_label = "Best Available";
            fmt.container = "mp4";
            fmt.vcodec = "avc1";
            fmt.acodec = "mp4a";
            fmt.has_video = true;
            fmt.has_audio = true;
            fmt.requires_merge = false;
            fmt.video_url = norm_url;
            fmt.expires_at_epoch_s = VideoExtractionManager::ParseUrlExpirationEpoch(norm_url);
            fmt.required_headers = meta.required_headers;
            meta.formats.push_back(std::move(fmt));
            meta.earliest_expiry_epoch_s = meta.formats.front().expires_at_epoch_s;
            return Result<ExtractionMetadata>::Ok(std::move(meta));
        }

        auto headers = VideoExtractionManager::BuildSiteHeaders(norm_url, request.page_url, request.headers);
        headers["Accept"] = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8";
        headers["Sec-Fetch-Mode"] = "navigate";

        auto html_res = FetchWebpageHtml(norm_url, headers);
        if (html_res.ok() && html_res.value && !html_res.value->empty()) {
            auto native_res = VideoExtractionManager::ParseFacebookVideoHtml(*html_res.value, norm_url);
            if (native_res.ok() && native_res.value && !native_res.value->formats.empty()) {
                return native_res;
            }
        }

        return RunYtDlpMetadataProbe(request);
    }
};

class GenericWebVideoExtractor final : public IVideoExtractor {
public:
    std::string Name() const override { return "GenericWebVideoExtractor"; }

    std::string PlatformId(const std::string& url) const override {
        return VideoExtractionManager::IdentifyPlatform(url);
    }

    bool SupportsUrl(const std::string& url) const override {
        return url.starts_with("http://") || url.starts_with("https://");
    }

    std::map<std::string, std::string> GetDefaultHeaders(const std::string& url) const override {
        return VideoExtractionManager::BuildSiteHeaders(url, url, {});
    }

    Result<ExtractionMetadata> Extract(const ExtractionRequest& request) override {
        // 1. First probe via yt-dlp metadata probe if available
        auto ytdlp_res = RunYtDlpMetadataProbe(request);
        if (ytdlp_res.ok() && ytdlp_res.value && !ytdlp_res.value->formats.empty()) {
            return ytdlp_res;
        }

        // 2. Fetch page HTML and run native HTML5 player extraction
        auto headers = VideoExtractionManager::BuildSiteHeaders(request.url, request.page_url, request.headers);
        auto html_res = FetchWebpageHtml(request.url, headers);
        if (html_res.ok() && html_res.value && !html_res.value->empty()) {
            auto native_res = VideoExtractionManager::ParseHtml5PlayerPage(*html_res.value, request.url);
            if (native_res.ok() && native_res.value && !native_res.value->formats.empty()) {
                return native_res;
            }
        }

        return ytdlp_res;
    }
};

} // namespace

// ============================================================================
// VideoExtractionManager Implementation
// ============================================================================

VideoExtractionManager& VideoExtractionManager::Instance() {
    static VideoExtractionManager instance;
    return instance;
}

VideoExtractionManager::VideoExtractionManager() {
    m_extractors.push_back(std::make_unique<BilibiliExtractor>());
    m_extractors.push_back(std::make_unique<FacebookExtractor>());
    m_extractors.push_back(std::make_unique<GenericWebVideoExtractor>());
}

Result<ExtractionMetadata> VideoExtractionManager::ExtractMetadata(const ExtractionRequest& request) {
    auto start_tp = std::chrono::steady_clock::now();

    if (request.url.empty() || (!request.url.starts_with("http://") && !request.url.starts_with("https://"))) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_UNSUPPORTED_URL",
            "Only http:// and https:// URLs are supported for video extraction.");
    }

    std::string norm_url = NormalizeExtractorUrl(request.url);
    std::string auth_fp = ComputeAuthFingerprint(request.headers);
    int64_t now_s = CurrentEpochSeconds();

    if (!request.bypass_cache) {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_cache.find(norm_url);
        if (it != m_cache.end()) {
            const auto& entry = it->second;
            bool auth_matches = (entry.auth_fingerprint == auth_fp);
            bool ttl_valid = (now_s - entry.cached_at_epoch_s) < 300;
            bool streams_valid = (entry.metadata.earliest_expiry_epoch_s == 0) ||
                                 ((now_s + 90) < entry.metadata.earliest_expiry_epoch_s);
            if (auth_matches && ttl_valid && streams_valid) {
                return Result<ExtractionMetadata>::Ok(entry.metadata);
            }
            m_cache.erase(it);
        }
    }

    IVideoExtractor* selected = nullptr;
    for (const auto& ext : m_extractors) {
        if (ext->SupportsUrl(norm_url)) {
            selected = ext.get();
            break;
        }
    }

    if (!selected) {
        return Result<ExtractionMetadata>::Err(
            "ERR_EXTRACTOR_UNSUPPORTED_URL",
            "No registered video extractor supports URL: " + Logger::RedactSensitiveData(norm_url));
    }

    ExtractionRequest normalized_req = request;
    normalized_req.url = norm_url;
    auto res = selected->Extract(normalized_req);

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start_tp)
                          .count();

    if (!res.ok() || !res.value) {
        ExtractionDiagnosticReport diag;
        diag.input_url = request.url;
        diag.normalized_url = norm_url;
        diag.detected_platform = IdentifyPlatform(norm_url);
        diag.selected_extractor = selected->Name();
        diag.auth_session_used = !auth_fp.empty();
        for (const auto& [k, v] : request.headers) {
            if (!v.empty()) diag.header_names_used.push_back(k);
        }
        diag.failure_category = ClassifyError(res.error ? res.error->message : "");
        diag.failure_code = res.error ? res.error->code : "ERR_EXTRACTOR_FAILED";
        diag.failure_message = res.error ? res.error->message : "Extraction failed";
        diag.extraction_duration_ms = elapsed_ms;
        RADIUM_LOG_WARN("VideoExtractor", "VideoExtraction failed: " + diag.ToRedactedJson());
        return res;
    }

    auto& meta = *res.value;
    meta.diagnostics.input_url = request.url;
    meta.diagnostics.normalized_url = norm_url;
    meta.diagnostics.detected_platform = meta.platform_id;
    meta.diagnostics.selected_extractor = meta.extractor_name;
    meta.diagnostics.discovered_format_count = meta.formats.size();
    meta.diagnostics.auth_session_used = !auth_fp.empty();
    meta.diagnostics.extraction_duration_ms = elapsed_ms;
    meta.diagnostics.header_names_used.clear();
    for (const auto& [k, v] : meta.required_headers) {
        if (!v.empty()) meta.diagnostics.header_names_used.push_back(k);
    }

    RADIUM_LOG_INFO("VideoExtractor", "VideoExtraction succeeded: " + meta.diagnostics.ToRedactedJson());

    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        m_cache[norm_url] = CacheEntry{meta, auth_fp, now_s};
    }

    return res;
}

Result<ExtractedFormat> VideoExtractionManager::SelectBestFormat(
    const ExtractionMetadata& metadata,
    const std::string& preferred_quality_or_id) const {
    if (metadata.formats.empty()) {
        return Result<ExtractedFormat>::Err(
            "ERR_EXTRACTOR_NO_VIDEO_FOUND",
            "No downloadable formats available to select.");
    }

    std::string pref = ToLowerAscii(preferred_quality_or_id);
    if (pref.empty() || pref == "best" || pref == "auto" || pref == "highest") {
        for (const auto& f : metadata.formats) {
            if (f.has_video && f.is_downloadable && !f.is_drm_protected) {
                return Result<ExtractedFormat>::Ok(f);
            }
        }
        return Result<ExtractedFormat>::Ok(metadata.formats.front());
    }

    if (pref.find("audio") != std::string::npos || pref == "m4a" || pref == "mp3") {
        for (const auto& f : metadata.formats) {
            if (!f.has_video && f.has_audio && f.is_downloadable) {
                return Result<ExtractedFormat>::Ok(f);
            }
        }
        if (!metadata.raw_audio_streams.empty()) {
            return Result<ExtractedFormat>::Ok(metadata.raw_audio_streams.front());
        }
    }

    for (const auto& f : metadata.formats) {
        if (!f.is_downloadable || f.is_drm_protected) continue;
        if (ToLowerAscii(f.format_id) == pref || ToLowerAscii(f.quality_label) == pref) {
            return Result<ExtractedFormat>::Ok(f);
        }
    }

    uint32_t target_h = 0;
    if (pref.find("4k") != std::string::npos || pref.find("2160") != std::string::npos) target_h = 2160;
    else if (pref.find("2k") != std::string::npos || pref.find("1440") != std::string::npos) target_h = 1440;
    else {
        static const std::regex h_re(R"((\d{3,4})p?)");
        std::smatch m;
        if (std::regex_search(pref, m, h_re) && m.size() > 1) {
            target_h = static_cast<uint32_t>(std::stoul(m[1].str()));
        }
    }

    if (target_h > 0) {
        for (const auto& f : metadata.formats) {
            if (f.has_video && f.is_downloadable && !f.is_drm_protected && f.height == target_h) {
                return Result<ExtractedFormat>::Ok(f);
            }
        }
        for (const auto& f : metadata.formats) {
            if (f.has_video && f.is_downloadable && !f.is_drm_protected && f.height > 0 && f.height <= target_h) {
                return Result<ExtractedFormat>::Ok(f);
            }
        }
    }

    return Result<ExtractedFormat>::Ok(metadata.formats.front());
}

void VideoExtractionManager::InvalidateCacheForUrl(const std::string& url) {
    std::string norm = NormalizeExtractorUrl(url);
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    m_cache.erase(norm);
    m_cache.erase(url);
}

void VideoExtractionManager::ClearCache() {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    m_cache.clear();
}

} // namespace openidm::engine_stream
