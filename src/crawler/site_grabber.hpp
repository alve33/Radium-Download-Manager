// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Whole-Site Crawler & Link Grabber with SSRF Protection
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace openidm::crawler {

struct SiteGrabberConfig {
    std::string start_url;
    uint32_t max_depth{2};          // 1 to 5
    uint32_t max_pages{100};        // Max web pages to visit
    bool same_domain_only{true};
    std::vector<std::string> allowed_domains;
    std::vector<std::string> filter_categories; // "Images", "Documents", "Video", "Audio", "Compressed", "All"
    std::vector<std::string> custom_extensions;
    std::string destination_folder;
    bool allow_loopback{false};     // Strict SSRF guard
    int timeout_ms{5000};
};

struct GrabbedItem {
    std::string url;
    std::string page_url;
    std::string file_name;
    std::string category;
    uint64_t estimated_size{0};
    uint32_t depth{0};
};

struct GrabbedGroup {
    std::string group_name;
    bool is_split_volume{false};
    std::vector<GrabbedItem> items;
};

using GrabbedItemCallback = std::function<void(const GrabbedItem&)>;

class SiteGrabber {
public:
    SiteGrabber() = default;
    ~SiteGrabber() = default;

    // Crawls website up to max_depth and returns discovered downloadable files
    static Result<std::vector<GrabbedItem>> Crawl(
        const SiteGrabberConfig& cfg,
        std::atomic<bool>& cancel_requested,
        GrabbedItemCallback on_item_found = nullptr);

    // Extracts split-archive volume base key (e.g. "movie.part1.rar" -> "movie", "data.001" -> "data")
    static std::optional<std::string> ExtractVolumeKey(std::string_view filename);

    // Groups grabbed items into split-volume packages, per-page groups, or "Various files"
    static std::vector<GrabbedGroup> GroupGrabbedItems(const std::vector<GrabbedItem>& items);

    // Expands GD3-style batch URL templates: [01-10:2], [a-z], {mp4,mkv}
    static std::vector<std::string> ExpandBatchUrlTemplate(std::string_view tmpl, size_t max_urls = 5000);

    // Resolves a relative or protocol-relative URL against a base URL
    static std::string ResolveUrl(std::string_view base_url, std::string_view relative_url);

    // Checks if a URL matches the configured categories or extensions
    static bool MatchesFilter(std::string_view url, const SiteGrabberConfig& cfg, std::string* out_category = nullptr);

    // Extracts link and media URLs from an HTML document buffer
    static std::vector<std::string> ExtractUrlsFromHtml(std::string_view html, std::string_view base_url);
};

} // namespace openidm::crawler
