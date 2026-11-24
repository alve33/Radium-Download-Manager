// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — Whole-Site Crawler & Link Grabber with SSRF Protection
#include "crawler/site_grabber.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <regex>
#include <set>

#include "brisk_engine/http_client.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace openidm::crawler {

namespace {

std::string ToLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

std::string ExtractHostFromUrl(std::string_view url) {
    auto parsed = brisk::HttpClient::ParseUrl(url);
    if (!parsed.ok()) return "";
    return parsed.value->host;
}

} // namespace

std::string SiteGrabber::ResolveUrl(std::string_view base_url, std::string_view rel) {
    while (!rel.empty() && (rel.front() == ' ' || rel.front() == '\t' || rel.front() == '\r' || rel.front() == '\n')) {
        rel.remove_prefix(1);
    }
    while (!rel.empty() && (rel.back() == ' ' || rel.back() == '\t' || rel.back() == '\r' || rel.back() == '\n')) {
        rel.remove_suffix(1);
    }
    if (rel.empty()) return "";

    // Drop fragment
    size_t hash_pos = rel.find('#');
    if (hash_pos != std::string_view::npos) {
        rel = rel.substr(0, hash_pos);
    }
    if (rel.empty()) return "";

    std::string r(rel);
    if (r.find("javascript:") == 0 || r.find("mailto:") == 0 || r.find("tel:") == 0 || r.find("data:") == 0) {
        return "";
    }

    if (r.find("http://") == 0 || r.find("https://") == 0 || r.find("ftp://") == 0) {
        return r;
    }

    std::string base(base_url);
    size_t scheme_sep = base.find("://");
    if (scheme_sep == std::string::npos) return r;
    std::string scheme = base.substr(0, scheme_sep);

    if (r.rfind("//", 0) == 0) {
        return scheme + ":" + r;
    }

    size_t host_start = scheme_sep + 3;
    size_t path_start = base.find('/', host_start);
    std::string origin = (path_start == std::string::npos) ? base : base.substr(0, path_start);

    if (r.front() == '/') {
        return origin + r;
    }

    std::string base_dir = "/";
    if (path_start != std::string::npos) {
        size_t last_slash = base.rfind('/');
        if (last_slash >= path_start) {
            base_dir = base.substr(path_start, last_slash - path_start + 1);
        }
    }
    return origin + base_dir + r;
}

bool SiteGrabber::MatchesFilter(std::string_view url, const SiteGrabberConfig& cfg, std::string* out_category) {
    std::string filename = PathSanitizer::ExtractFromUrl(std::string(url));
    std::string cat = InferCategoryFromFilename(filename);
    if (out_category) *out_category = cat;

    if (cfg.filter_categories.empty() && cfg.custom_extensions.empty()) {
        return cat != "General";
    }

    for (const auto& fc : cfg.filter_categories) {
        if (fc == "All" || fc == cat) return true;
    }

    auto dot = filename.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < filename.size()) {
        std::string ext = ToLower(filename.substr(dot + 1));
        for (const auto& ce : cfg.custom_extensions) {
            if (ToLower(ce) == ext) return true;
        }
    }
    return false;
}

std::vector<std::string> SiteGrabber::ExtractUrlsFromHtml(std::string_view html, std::string_view base_url) {
    std::vector<std::string> urls;
    std::set<std::string> seen;

    std::string effective_base(base_url);

    // Look for <base href="...">
    static const std::regex kBaseRegex(
        R"(<base\s+[^>]*href\s*=\s*["']([^"']+)["'])",
        std::regex::icase | std::regex::optimize);

    std::string s(html);
    std::smatch base_match;
    if (std::regex_search(s, base_match, kBaseRegex) && base_match.size() >= 2) {
        std::string candidate_base = ResolveUrl(base_url, base_match[1].str());
        if (!candidate_base.empty()) {
            effective_base = candidate_base;
        }
    }

    static const std::regex kHrefSrcRegex(
        R"((?:href|src|data-src|data-url|action|poster)\s*=\s*["']([^"'#\s>]+)["'])",
        std::regex::icase | std::regex::optimize);

    static const std::regex kCssUrlRegex(
        R"(url\(\s*["']?([^"')#\s]+)["']?\s*\))",
        std::regex::icase | std::regex::optimize);

    static const std::regex kSrcsetRegex(
        R"(srcset\s*=\s*["']([^"']+)["'])",
        std::regex::icase | std::regex::optimize);

    auto scan_regex = [&](const std::regex& re) {
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();
        for (std::sregex_iterator it = begin; it != end; ++it) {
            std::smatch match = *it;
            if (match.size() >= 2) {
                std::string raw = match[1].str();
                std::string resolved = ResolveUrl(effective_base, raw);
                if (!resolved.empty() && !seen.count(resolved)) {
                    seen.insert(resolved);
                    urls.push_back(resolved);
                }
            }
        }
    };

    scan_regex(kHrefSrcRegex);
    scan_regex(kCssUrlRegex);

    // Parse srcset candidates (e.g. "image-320w.jpg 320w, image-480w.jpg 480w")
    auto begin_ss = std::sregex_iterator(s.begin(), s.end(), kSrcsetRegex);
    auto end_ss = std::sregex_iterator();
    for (std::sregex_iterator it = begin_ss; it != end_ss; ++it) {
        std::smatch match = *it;
        if (match.size() >= 2) {
            std::string srcset_content = match[1].str();
            std::stringstream ss(srcset_content);
            std::string part;
            while (std::getline(ss, part, ',')) {
                // Trim whitespace
                size_t start = part.find_first_not_of(" \t\r\n");
                if (start == std::string::npos) continue;
                size_t end = part.find_last_not_of(" \t\r\n");
                std::string token = part.substr(start, end - start + 1);
                // The URL is the first component before space/descriptor
                size_t space_pos = token.find_first_of(" \t");
                std::string raw_url = (space_pos != std::string::npos) ? token.substr(0, space_pos) : token;
                std::string resolved = ResolveUrl(effective_base, raw_url);
                if (!resolved.empty() && !seen.count(resolved)) {
                    seen.insert(resolved);
                    urls.push_back(resolved);
                }
            }
        }
    }

    return urls;
}

Result<std::vector<GrabbedItem>> SiteGrabber::Crawl(
    const SiteGrabberConfig& cfg,
    std::atomic<bool>& cancel_requested,
    GrabbedItemCallback on_item_found) {

    if (cfg.start_url.empty()) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_CRAWLER_START_URL", "Start URL cannot be empty");
    }

    auto start_pu = brisk::HttpClient::ParseUrl(cfg.start_url);
    if (!start_pu.ok()) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_CRAWLER_INVALID_URL", "Invalid starting URL: " + cfg.start_url);
    }
    std::string start_host = start_pu.value->host;

    // SSRF protection on starting host
    if (!cfg.allow_loopback && (start_pu.value->is_loopback_or_private || brisk::HttpClient::CheckHostResolvesToPrivate(start_host))) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_SSRF_BLOCKED", "Crawl blocked to private/loopback destination: " + start_host);
    }

    struct QueueEntry {
        std::string url;
        uint32_t depth;
        std::string page_url;
    };

    std::deque<QueueEntry> queue;
    std::set<std::string> visited_pages;
    std::set<std::string> discovered_files;
    std::vector<GrabbedItem> results;

    queue.push_back({cfg.start_url, 0, cfg.start_url});
    uint32_t pages_crawled = 0;

    while (!queue.empty() && !cancel_requested.load()) {
        auto current = queue.front();
        queue.pop_front();

        if (visited_pages.count(current.url)) continue;
        visited_pages.insert(current.url);

        auto current_pu = brisk::HttpClient::ParseUrl(current.url);
        if (!current_pu.ok()) continue;
        std::string current_host = current_pu.value->host;
        if (cfg.same_domain_only && current_host != start_host) {
            continue;
        }
        if (!cfg.allowed_domains.empty()) {
            bool domain_ok = false;
            for (const auto& dom : cfg.allowed_domains) {
                if (current_host == dom || current_host.ends_with("." + dom)) {
                    domain_ok = true;
                    break;
                }
            }
            if (!domain_ok) continue;
        }

        // SSRF protection per link
        if (!cfg.allow_loopback && (current_pu.value->is_loopback_or_private || brisk::HttpClient::CheckHostResolvesToPrivate(current_host))) {
            continue;
        }

        pages_crawled++;
        if (pages_crawled > cfg.max_pages) break;

        // Fetch page HTML
        std::string html_body;
        std::atomic<bool> req_cancel{false};
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) RadiumDownloadManager/1.0"}
        };

        auto res = brisk::HttpClient::PerformGet(
            current.url,
            headers,
            std::nullopt,
            std::nullopt, // Send standard non-ranged GET to avoid HTTP 416 on dynamic servers
            cfg.timeout_ms,
            cfg.allow_loopback,
            req_cancel,
            [&](const brisk::HttpResponseMeta& meta) -> bool {
                if (cancel_requested.load()) return false;
                if (meta.status_code >= 400) return false;
                std::string ct = ToLower(meta.content_type);
                if (!ct.empty() && ct.find("text/html") == std::string::npos &&
                    ct.find("application/xhtml+xml") == std::string::npos) {
                    return false; // Skip non-HTML files early
                }
                return true;
            },
            [&](const uint8_t* data, size_t len) -> bool {
                if (cancel_requested.load()) return false;
                if (html_body.size() + len > 4194304) {
                    size_t allowed = 4194304 - html_body.size();
                    if (allowed > 0) {
                        html_body.append(reinterpret_cast<const char*>(data), allowed);
                    }
                    return false; // Cap at 4MB per HTML page
                }
                html_body.append(reinterpret_cast<const char*>(data), len);
                return true;
            });

        if (!res.ok()) continue;

        std::string effective_base_url = (res.ok() && !res.value->final_url.empty()) ? res.value->final_url : current.url;
        auto extracted_urls = ExtractUrlsFromHtml(html_body, effective_base_url);
        for (const auto& link : extracted_urls) {
            if (cancel_requested.load()) break;

            std::string cat;
            if (MatchesFilter(link, cfg, &cat)) {
                if (!discovered_files.count(link)) {
                    discovered_files.insert(link);
                    GrabbedItem item;
                    item.url = link;
                    item.page_url = current.url;
                    item.file_name = PathSanitizer::SanitizeFileName(PathSanitizer::ExtractFromUrl(link));
                    item.category = cat;
                    item.depth = current.depth + 1;
                    results.push_back(item);
                    if (on_item_found) on_item_found(item);
                }
            } else if (current.depth + 1 <= cfg.max_depth) {
                // Potential subpage to crawl
                if (!visited_pages.count(link)) {
                    queue.push_back({link, current.depth + 1, current.url});
                }
            }
        }
    }

    if (cancel_requested.load()) {
        return Result<std::vector<GrabbedItem>>::Err("ERR_CANCELED", "Site crawl canceled");
    }

    return Result<std::vector<GrabbedItem>>::Ok(std::move(results));
}

std::optional<std::string> SiteGrabber::ExtractVolumeKey(std::string_view filename) {
    if (filename.empty()) return std::nullopt;
    std::string lower(filename);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto is_all_digits = [](std::string_view s) {
        if (s.empty()) return false;
        for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    };

    // 1. movie.part1.rar / movie.part01.rar
    if (lower.ends_with(".rar")) {
        std::string_view stem = std::string_view(filename).substr(0, filename.size() - 4);
        std::string_view lstem = std::string_view(lower).substr(0, lower.size() - 4);
        size_t dot = lstem.rfind('.');
        if (dot != std::string_view::npos) {
            std::string_view part_seg = lstem.substr(dot + 1);
            if (part_seg.rfind("part", 0) == 0 && is_all_digits(part_seg.substr(4))) {
                if (dot > 0) return std::string(stem.substr(0, dot));
            }
        }
    }

    // 2. data.001, data.002, ... (3+ digit split volumes)
    size_t last_dot = lower.rfind('.');
    if (last_dot != std::string_view::npos && last_dot > 0) {
        std::string_view ext = std::string_view(lower).substr(last_dot + 1);
        if (ext.size() >= 3 && ext.size() <= 4 && is_all_digits(ext)) {
            return std::string(filename.substr(0, last_dot));
        }
        // 3. archive.r00..r99 or archive.z01..z99
        if (ext.size() == 3 && (ext[0] == 'r' || ext[0] == 'z') && is_all_digits(ext.substr(1))) {
            return std::string(filename.substr(0, last_dot));
        }
    }

    return std::nullopt;
}

std::vector<GrabbedGroup> SiteGrabber::GroupGrabbedItems(const std::vector<GrabbedItem>& items) {
    std::map<std::string, GrabbedGroup> by_key;
    std::vector<std::string> order;
    for (const auto& item : items) {
        std::string key;
        std::string display;
        bool is_vol = false;
        if (auto vol = ExtractVolumeKey(item.file_name)) {
            std::string lvol = *vol;
            for (char& c : lvol) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string host;
            auto pu = brisk::HttpClient::ParseUrl(item.url);
            if (pu.ok()) {
                host = pu.value->host;
                for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            key = "vol:" + host + "|" + lvol;
            display = *vol;
            is_vol = true;
        } else if (!item.page_url.empty()) {
            auto pu = brisk::HttpClient::ParseUrl(item.page_url);
            if (pu.ok()) {
                std::string host = pu.value->host;
                for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                key = "page:" + host + pu.value->path_and_query;
                display = host;
            }
        }
        if (key.empty()) {
            key = "various";
            display = "Various files";
        }
        auto it = by_key.find(key);
        if (it == by_key.end()) {
            GrabbedGroup g;
            g.group_name = display;
            g.is_split_volume = is_vol;
            g.items.push_back(item);
            by_key.emplace(key, std::move(g));
            order.push_back(key);
        } else {
            it->second.items.push_back(item);
        }
    }
    std::vector<GrabbedGroup> out;
    out.reserve(order.size());
    for (const auto& k : order) {
        out.push_back(std::move(by_key[k]));
    }
    return out;
}

std::vector<std::string> SiteGrabber::ExpandBatchUrlTemplate(std::string_view tmpl, size_t max_urls) {
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    };

    auto expand_range = [&](const std::string& spec) -> std::optional<std::vector<std::string>> {
        size_t colon = spec.find(':');
        if (colon != std::string::npos && spec.find(':', colon + 1) != std::string::npos) return std::nullopt;
        std::string range_part = (colon == std::string::npos) ? spec : spec.substr(0, colon);
        int step = 1;
        if (colon != std::string::npos) {
            try {
                step = std::stoi(trim(spec.substr(colon + 1)));
            } catch (...) {
                return std::nullopt;
            }
            if (step < 1) return std::nullopt;
        }
        size_t dash = range_part.find('-');
        if (dash == std::string::npos) return std::nullopt;
        std::string start = trim(range_part.substr(0, dash));
        std::string end = trim(range_part.substr(dash + 1));
        if (start.empty() || end.empty()) return std::nullopt;

        auto all_digits = [](const std::string& s) {
            for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            return true;
        };
        if (all_digits(start) && all_digits(end)) {
            size_t width = (start.size() > 1 && start[0] == '0') ? start.size() : 0;
            long long s_val = 0, e_val = 0;
            try {
                s_val = std::stoll(start);
                e_val = std::stoll(end);
            } catch (...) {
                return std::nullopt;
            }
            if (s_val > e_val) return std::nullopt;
            std::vector<std::string> vals;
            for (long long v = s_val; v <= e_val && vals.size() < max_urls; v += step) {
                std::string num = std::to_string(v);
                if (width > num.size()) num.insert(0, width - num.size(), '0');
                vals.push_back(std::move(num));
            }
            return vals;
        }
        if (start.size() == 1 && end.size() == 1 &&
            std::isalpha(static_cast<unsigned char>(start[0])) &&
            std::isalpha(static_cast<unsigned char>(end[0]))) {
            bool s_low = std::islower(static_cast<unsigned char>(start[0])) != 0;
            bool e_low = std::islower(static_cast<unsigned char>(end[0])) != 0;
            if (s_low != e_low || start[0] > end[0]) return std::nullopt;
            std::vector<std::string> vals;
            for (char c = start[0]; c <= end[0] && vals.size() < max_urls; c = static_cast<char>(c + step)) {
                vals.emplace_back(1, c);
            }
            return vals;
        }
        return std::nullopt;
    };

    std::vector<std::vector<std::string>> segments;
    size_t i = 0;
    std::string lit;
    while (i < tmpl.size()) {
        if (tmpl[i] == '[' || tmpl[i] == '{') {
            char close_ch = (tmpl[i] == '[') ? ']' : '}';
            size_t close_pos = tmpl.find(close_ch, i + 1);
            if (close_pos != std::string_view::npos) {
                std::string inner(tmpl.substr(i + 1, close_pos - (i + 1)));
                if (tmpl[i] == '{') {
                    std::vector<std::string> list_vals;
                    size_t p = 0;
                    while (p <= inner.size()) {
                        size_t comma = inner.find(',', p);
                        std::string item = trim(comma == std::string::npos ? inner.substr(p) : inner.substr(p, comma - p));
                        if (!item.empty()) list_vals.push_back(std::move(item));
                        if (comma == std::string::npos) break;
                        p = comma + 1;
                    }
                    if (!list_vals.empty()) {
                        if (!lit.empty()) { segments.push_back({std::move(lit)}); lit.clear(); }
                        segments.push_back(std::move(list_vals));
                        i = close_pos + 1;
                        continue;
                    }
                } else {
                    if (auto r = expand_range(inner)) {
                        if (!lit.empty()) { segments.push_back({std::move(lit)}); lit.clear(); }
                        segments.push_back(std::move(*r));
                        i = close_pos + 1;
                        continue;
                    }
                }
            }
        }
        lit.push_back(tmpl[i]);
        ++i;
    }
    if (!lit.empty()) segments.push_back({std::move(lit)});
    if (segments.empty()) return {};

    std::vector<std::string> current = {""};
    for (const auto& seg : segments) {
        std::vector<std::string> next;
        for (const auto& prefix : current) {
            for (const auto& part : seg) {
                next.push_back(prefix + part);
                if (next.size() >= max_urls) return next;
            }
        }
        current = std::move(next);
    }
    return current;
}

} // namespace openidm::crawler
