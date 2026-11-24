// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Site Interstitial Handlers, Per-Host Identity Presets & Browser Impersonation
#include "brisk_engine/site_handlers.hpp"
#include "brisk_engine/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace openidm::brisk {

namespace {

std::string ToLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && ToLowerStr(a) == ToLowerStr(b);
}

bool UnderDomain(const std::string& host, const std::string& domain) {
    std::string lh = ToLowerStr(host);
    std::string ld = ToLowerStr(domain);
    if (lh == ld) return true;
    if (lh.size() > ld.size() + 1 && lh.ends_with(ld)) {
        return lh[lh.size() - ld.size() - 1] == '.';
    }
    return false;
}

std::string ExtractPathOnly(const std::string& path_and_query) {
    size_t q = path_and_query.find('?');
    size_t f = path_and_query.find('#');
    size_t end = std::min(q, f);
    return (end == std::string::npos) ? path_and_query : path_and_query.substr(0, end);
}

std::string ExtractQueryOnly(const std::string& path_and_query) {
    size_t q = path_and_query.find('?');
    if (q == std::string::npos) return {};
    size_t f = path_and_query.find('#', q);
    return (f == std::string::npos) ? path_and_query.substr(q + 1) : path_and_query.substr(q + 1, f - q - 1);
}

std::vector<std::string> SplitPathSegments(const std::string& path) {
    std::vector<std::string> segs;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        size_t next = path.find('/', i);
        if (next == std::string::npos) {
            segs.push_back(path.substr(i));
            break;
        }
        segs.push_back(path.substr(i, next - i));
        i = next + 1;
    }
    return segs;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            int v = std::stoi(s.substr(i + 1, 2), nullptr, 16);
            out.push_back(static_cast<char>(v));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string UrlEncode(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> ParseQueryPairs(const std::string& query) {
    std::vector<std::pair<std::string, std::string>> pairs;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string part = (amp == std::string::npos) ? query.substr(start) : query.substr(start, amp - start);
        if (!part.empty()) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) {
                pairs.emplace_back(UrlDecode(part), "");
            } else {
                pairs.emplace_back(UrlDecode(part.substr(0, eq)), UrlDecode(part.substr(eq + 1)));
            }
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return pairs;
}

std::string BuildUrl(const ParsedUrl& base, const std::string& host, const std::string& path,
                     const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::ostringstream oss;
    oss << base.scheme << "://" << host;
    const uint16_t def_port = (base.scheme == "https") ? 443 : 80;
    if (base.port != def_port) {
        oss << ":" << base.port;
    }
    oss << (path.empty() ? "/" : path);
    if (!pairs.empty()) {
        oss << "?";
        for (size_t i = 0; i < pairs.size(); ++i) {
            if (i > 0) oss << "&";
            oss << UrlEncode(pairs[i].first) << "=" << UrlEncode(pairs[i].second);
        }
    }
    return oss.str();
}

std::string HtmlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; continue; }
            if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 5; continue; }
            if (s.compare(i, 5, "&#39;") == 0 || s.compare(i, 6, "&apos;") == 0) {
                out.push_back('\'');
                i += (s[i + 1] == '#') ? 4 : 5;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::optional<std::string> ExtractHtmlAttr(const std::string& tag, const std::string& attr_name) {
    std::string lower_tag = ToLowerStr(tag);
    std::string target = ToLowerStr(attr_name);
    size_t pos = 0;
    while (pos < lower_tag.size()) {
        size_t found = lower_tag.find(target, pos);
        if (found == std::string::npos) return std::nullopt;
        if (found > 0 && !std::isspace(static_cast<unsigned char>(lower_tag[found - 1]))) {
            pos = found + target.size();
            continue;
        }
        size_t after = found + target.size();
        while (after < lower_tag.size() && std::isspace(static_cast<unsigned char>(lower_tag[after]))) ++after;
        if (after >= lower_tag.size() || lower_tag[after] != '=') {
            pos = after;
            continue;
        }
        ++after;
        while (after < lower_tag.size() && std::isspace(static_cast<unsigned char>(lower_tag[after]))) ++after;
        if (after >= tag.size()) return std::nullopt;
        char quote = tag[after];
        if (quote == '"' || quote == '\'') {
            size_t end_q = tag.find(quote, after + 1);
            if (end_q == std::string::npos) return std::nullopt;
            return tag.substr(after + 1, end_q - (after + 1));
        }
        size_t end_w = after;
        while (end_w < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end_w])) && tag[end_w] != '>') ++end_w;
        return tag.substr(after, end_w - after);
    }
    return std::nullopt;
}

std::vector<std::pair<size_t, std::string>> FindHtmlTags(const std::string& html, const std::string& tag_name) {
    std::vector<std::pair<size_t, std::string>> result;
    std::string lower = ToLowerStr(html);
    std::string needle = "<" + ToLowerStr(tag_name);
    size_t pos = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        size_t after = pos + needle.size();
        if (after < lower.size() &&
            !std::isspace(static_cast<unsigned char>(lower[after])) &&
            lower[after] != '>' && lower[after] != '/') {
            pos = after;
            continue;
        }
        size_t end = html.find('>', after);
        if (end == std::string::npos) break;
        result.emplace_back(pos, html.substr(pos, end - pos + 1));
        pos = end + 1;
    }
    return result;
}

std::optional<std::string> ExtractTextOfClass(const std::string& html, const std::string& class_name) {
    std::string lower = ToLowerStr(html);
    size_t cpos = lower.find(ToLowerStr(class_name));
    if (cpos == std::string::npos) return std::nullopt;
    size_t gt = html.find('>', cpos);
    if (gt == std::string::npos) return std::nullopt;
    size_t lt = html.find("</", gt + 1);
    if (lt == std::string::npos) lt = html.size();
    std::string raw = html.substr(gt + 1, lt - (gt + 1));
    std::string cleaned;
    bool in_tag = false;
    for (char c : raw) {
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) cleaned.push_back(c);
    }
    cleaned = HtmlDecode(cleaned);
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.front()))) cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && std::isspace(static_cast<unsigned char>(cleaned.back()))) cleaned.pop_back();
    if (cleaned.empty()) return std::nullopt;
    if (cleaned.size() > 300) cleaned = cleaned.substr(0, 297) + "...";
    return cleaned;
}

// --- Google Drive ---
constexpr const char* kDriveHost = "drive.google.com";
constexpr const char* kDocsHost = "docs.google.com";
constexpr const char* kDriveUserContent = "drive.usercontent.google.com";

bool IsDriveHost(const ParsedUrl& pu) {
    return (pu.scheme == "http" || pu.scheme == "https") &&
           (EqualsIgnoreCase(pu.host, kDriveHost) ||
            EqualsIgnoreCase(pu.host, kDocsHost) ||
            EqualsIgnoreCase(pu.host, kDriveUserContent));
}

bool IsDriveId(const std::string& s) {
    if (s.size() < 25) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    }
    return true;
}

std::optional<std::string> QueryFirst(const std::vector<std::pair<std::string, std::string>>& pairs, const std::string& key) {
    for (const auto& [k, v] : pairs) {
        if (k == key && !v.empty()) return v;
    }
    return std::nullopt;
}

bool IsDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// --- MediaFire ---
bool IsMediaFireHost(const ParsedUrl& pu) {
    return (pu.scheme == "http" || pu.scheme == "https") && UnderDomain(pu.host, "mediafire.com");
}

bool IsMediaFireFileHost(const ParsedUrl& pu) {
    if (!IsMediaFireHost(pu)) return false;
    std::string lh = ToLowerStr(pu.host);
    const std::string suffix = ".mediafire.com";
    if (!lh.ends_with(suffix)) return false;
    std::string sub = lh.substr(0, lh.size() - suffix.size());
    if (sub.rfind("download", 0) != 0) return false;
    std::string num = sub.substr(8);
    return IsDigits(num);
}

bool IsMediaFireKey(const std::string& s) {
    if (s.size() < 6 || s.size() > 64) return false;
    for (char c : s) if (!std::isalnum(static_cast<unsigned char>(c))) return false;
    return true;
}

std::optional<std::string> MediaFireFileKey(const ParsedUrl& pu) {
    if (!IsMediaFireFileHost(pu)) return std::nullopt;
    auto segs = SplitPathSegments(ExtractPathOnly(pu.path_and_query));
    if (segs.size() < 3) return std::nullopt;
    if (!segs[0].empty() && IsMediaFireKey(segs[1]) && !segs[2].empty()) {
        return segs[1];
    }
    return std::nullopt;
}

// --- Rapidgator ---
bool IsRapidgatorHost(const ParsedUrl& pu) {
    return (pu.scheme == "http" || pu.scheme == "https") &&
           (UnderDomain(pu.host, "rapidgator.net") || UnderDomain(pu.host, "rg.to"));
}

bool IsRapidgatorFileLink(const ParsedUrl& pu) {
    if (pu.scheme != "http" && pu.scheme != "https") return false;
    std::string lh = ToLowerStr(pu.host);
    const std::string suffix = ".rapidgator.net";
    if (!lh.ends_with(suffix)) return false;
    std::string label = lh.substr(0, lh.size() - suffix.size());
    if (label.empty() || label == "www" || label.find('.') != std::string::npos) return false;
    auto segs = SplitPathSegments(ExtractPathOnly(pu.path_and_query));
    if (segs.size() >= 2 && segs[0] == "download") return true;
    auto qpairs = ParseQueryPairs(ExtractQueryOnly(pu.path_and_query));
    for (const auto& [k, v] : qpairs) {
        if (k == "r" && v.rfind("download", 0) == 0) return true;
    }
    return false;
}

// --- LuluStream ---
bool IsLuluHost(const ParsedUrl& pu) {
    return (pu.scheme == "http" || pu.scheme == "https") &&
           (UnderDomain(pu.host, "lulustream.com") ||
            UnderDomain(pu.host, "luluvdo.com") ||
            UnderDomain(pu.host, "lplaygo.com"));
}

bool IsLuluCode(const std::string& s) {
    if (s.size() != 12) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

std::optional<std::pair<std::string, char>> ParseLuluCodeMode(const std::string& s) {
    size_t us = s.rfind('_');
    if (us == std::string::npos || us + 2 != s.size()) return std::nullopt;
    std::string code = s.substr(0, us);
    char mode = s[us + 1];
    if (!IsLuluCode(code) || mode < 'a' || mode > 'z') return std::nullopt;
    return std::make_pair(code, mode);
}

// --- Internet Archive ---
bool IsArchiveHost(const ParsedUrl& pu) {
    return (pu.scheme == "http" || pu.scheme == "https") && UnderDomain(pu.host, "archive.org");
}

} // namespace

std::optional<std::pair<std::string, std::string>> SiteHandlers::CheckNotAFile(const std::string& url) {
    auto pr = HttpClient::ParseUrl(url);
    if (!pr.ok()) return std::nullopt;
    const ParsedUrl& pu = *pr.value;
    auto segs = SplitPathSegments(ExtractPathOnly(pu.path_and_query));
    auto qpairs = ParseQueryPairs(ExtractQueryOnly(pu.path_and_query));

    // Google Drive folders
    if (EqualsIgnoreCase(pu.host, kDriveHost)) {
        if (segs.size() >= 3 && segs[0] == "drive") {
            size_t idx = 1;
            if (idx + 1 < segs.size() && segs[idx] == "u" && IsDigits(segs[idx + 1])) idx += 2;
            if (idx < segs.size() && segs[idx] == "mobile") ++idx;
            if (idx + 1 < segs.size() && segs[idx] == "folders" && IsDriveId(segs[idx + 1])) {
                return std::make_pair("ERR_SITE_FOLDER", "This is a Google Drive folder, not a file");
            }
        }
        if (segs.size() == 1 && (segs[0] == "folderview" || segs[0] == "embeddedfolderview")) {
            if (auto id = QueryFirst(qpairs, "id"); id && IsDriveId(*id)) {
                return std::make_pair("ERR_SITE_FOLDER", "This is a Google Drive folder, not a file");
            }
        }
    }

    // gofile.io/d/<id>
    if ((pu.scheme == "http" || pu.scheme == "https") &&
        (EqualsIgnoreCase(pu.host, "gofile.io") || EqualsIgnoreCase(pu.host, "www.gofile.io"))) {
        if (segs.size() >= 2 && segs[0] == "d" && !segs[1].empty()) {
            return std::make_pair("ERR_SITE_NEEDS_BROWSER",
                "This gofile link is a web page, not the file: open it in your browser and download from there");
        }
    }

    return std::nullopt;
}

std::optional<std::string> SiteHandlers::RewriteUrl(const std::string& url) {
    auto pr = HttpClient::ParseUrl(url);
    if (!pr.ok()) return std::nullopt;
    const ParsedUrl& pu = *pr.value;
    auto segs = SplitPathSegments(ExtractPathOnly(pu.path_and_query));
    auto qpairs = ParseQueryPairs(ExtractQueryOnly(pu.path_and_query));

    if (IsDriveHost(pu)) {
        std::optional<std::string> res_key = QueryFirst(qpairs, "resourcekey");
        std::optional<std::string> auth_user = QueryFirst(qpairs, "authuser");

        // Docs / Sheets / Slides / Drawings
        if (EqualsIgnoreCase(pu.host, kDocsHost) && segs.size() >= 3) {
            std::string doc_kind = segs[0];
            std::string format;
            if (doc_kind == "document") format = "docx";
            else if (doc_kind == "spreadsheets") format = "xlsx";
            else if (doc_kind == "presentation") format = "pptx";
            else if (doc_kind == "drawings") format = "png";
            if (!format.empty()) {
                size_t idx = 1;
                if (idx + 1 < segs.size() && segs[idx] == "u" && IsDigits(segs[idx + 1])) idx += 2;
                if (idx + 1 < segs.size() && segs[idx] == "d" && IsDriveId(segs[idx + 1])) {
                    std::string id = segs[idx + 1];
                    bool is_viewer = (idx + 2 == segs.size()) ||
                        (idx + 3 == segs.size() && (segs[idx + 2] == "edit" || segs[idx + 2] == "view" ||
                         segs[idx + 2] == "htmlview" || segs[idx + 2] == "preview" ||
                         segs[idx + 2] == "copy" || segs[idx + 2] == "mobilebasic"));
                    if (is_viewer) {
                        std::vector<std::pair<std::string, std::string>> out_pairs;
                        std::string path;
                        if (doc_kind == "drawings") {
                            path = "/drawings/d/" + id + "/export/png";
                        } else {
                            path = "/" + doc_kind + "/d/" + id + "/export";
                            out_pairs.emplace_back("format", format);
                        }
                        if (res_key) out_pairs.emplace_back("resourcekey", *res_key);
                        return BuildUrl(pu, kDocsHost, path, out_pairs);
                    }
                }
            }
        }

        // File viewer /file/d/<id>/...
        std::optional<std::string> file_id;
        if ((EqualsIgnoreCase(pu.host, kDriveHost) || EqualsIgnoreCase(pu.host, kDocsHost)) &&
            segs.size() >= 3 && segs[0] == "file") {
            size_t idx = 1;
            if (idx + 1 < segs.size() && segs[idx] == "u" && IsDigits(segs[idx + 1])) {
                if (!auth_user) auth_user = segs[idx + 1];
                idx += 2;
            }
            if (idx + 1 < segs.size() && segs[idx] == "d" && IsDriveId(segs[idx + 1])) {
                bool ok = (idx + 2 == segs.size()) ||
                          (idx + 3 == segs.size() && (segs[idx + 2] == "view" || segs[idx + 2] == "edit" || segs[idx + 2] == "preview"));
                if (ok) file_id = segs[idx + 1];
            }
        } else if (segs.size() == 1 && (segs[0] == "uc" || segs[0] == "open" || segs[0] == "download")) {
            bool already_confirmed = EqualsIgnoreCase(pu.host, kDriveUserContent) && QueryFirst(qpairs, "confirm").has_value();
            if (!already_confirmed) {
                if (auto id = QueryFirst(qpairs, "id"); id && IsDriveId(*id)) {
                    file_id = id;
                }
            }
        }
        if (file_id) {
            std::vector<std::pair<std::string, std::string>> out_pairs = {
                {"id", *file_id},
                {"export", "download"},
                {"confirm", "t"}
            };
            if (res_key) out_pairs.emplace_back("resourcekey", *res_key);
            if (auth_user) out_pairs.emplace_back("authuser", *auth_user);
            std::string rewritten = BuildUrl(pu, kDriveUserContent, "/download", out_pairs);
            if (rewritten != url) return rewritten;
        }
    }

    // LuluStream watch/player -> /d/<code>
    if (IsLuluHost(pu)) {
        if (segs.size() == 1 && IsLuluCode(segs[0])) {
            return BuildUrl(pu, pu.host, "/d/" + segs[0], {});
        }
        if (segs.size() == 2 && segs[0] == "e" && IsLuluCode(segs[1])) {
            return BuildUrl(pu, pu.host, "/d/" + segs[1], {});
        }
    }

    return std::nullopt;
}

SitePageAction SiteHandlers::ReadPage(const std::string& at_url, const std::string& html_body) {
    SitePageAction out;
    auto pr = HttpClient::ParseUrl(at_url);
    if (!pr.ok()) return out;
    const ParsedUrl& pu = *pr.value;
    auto segs = SplitPathSegments(ExtractPathOnly(pu.path_and_query));
    auto qpairs = ParseQueryPairs(ExtractQueryOnly(pu.path_and_query));

    // 1. Google Drive
    if (IsDriveHost(pu)) {
        auto forms = FindHtmlTags(html_body, "form");
        for (const auto& [start, tag] : forms) {
            auto fid = ExtractHtmlAttr(tag, "id");
            if (!fid || HtmlDecode(*fid) != "download-form") continue;
            auto action = ExtractHtmlAttr(tag, "action");
            std::string action_url = action ? HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*action)) : at_url;
            auto ap = HttpClient::ParseUrl(action_url);
            if (!ap.ok() || !IsDriveHost(*ap.value)) continue;

            size_t body_start = start + tag.size();
            std::string lower = ToLowerStr(html_body);
            size_t body_end = lower.find("</form", body_start);
            if (body_end == std::string::npos) body_end = html_body.size();

            auto pairs = ParseQueryPairs(ExtractQueryOnly(ap.value->path_and_query));
            for (const auto& [_, inp] : FindHtmlTags(html_body.substr(body_start, body_end - body_start), "input")) {
                auto name = ExtractHtmlAttr(inp, "name");
                if (!name || name->empty()) continue;
                std::string k = HtmlDecode(*name);
                std::string v = HtmlDecode(ExtractHtmlAttr(inp, "value").value_or(""));
                bool replaced = false;
                for (auto& p : pairs) {
                    if (p.first == k) { p.second = v; replaced = true; break; }
                }
                if (!replaced) pairs.emplace_back(k, v);
            }
            out.kind = SiteActionKind::kNextUrl;
            out.url = BuildUrl(*ap.value, ap.value->host, ExtractPathOnly(ap.value->path_and_query), pairs);
            return out;
        }

        for (const auto& [_, atag] : FindHtmlTags(html_body, "a")) {
            auto href = ExtractHtmlAttr(atag, "href");
            if (!href) continue;
            std::string resolved = HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*href));
            if (resolved.find("/uc?") != std::string::npos && resolved.find("confirm=") != std::string::npos) {
                out.kind = SiteActionKind::kNextUrl;
                out.url = resolved;
                return out;
            }
        }

        if (auto err_msg = ExtractTextOfClass(html_body, "uc-error-subcaption")
                               .value_or(ExtractTextOfClass(html_body, "uc-error-caption").value_or(""));
            !err_msg.empty()) {
            out.kind = SiteActionKind::kRefused;
            out.refusal_code = "ERR_SITE_DRIVE_REFUSED";
            out.refusal_message = "Google Drive won't give this file now: " + err_msg;
            return out;
        }
        return out;
    }

    // 2. MediaFire
    if (IsMediaFireHost(pu) && !IsMediaFireFileHost(pu)) {
        if (ExtractPathOnly(pu.path_and_query) == "/download_repair.php") {
            if (auto qkey = QueryFirst(qpairs, "qkey"); qkey && IsMediaFireKey(*qkey)) {
                out.kind = SiteActionKind::kNextUrl;
                out.url = BuildUrl(pu, "www.mediafire.com", "/file/" + *qkey, {});
                return out;
            }
            return out;
        }
        for (const auto& [_, atag] : FindHtmlTags(html_body, "a")) {
            auto aid = ExtractHtmlAttr(atag, "id");
            if (!aid || HtmlDecode(*aid) != "downloadButton") continue;
            auto href = ExtractHtmlAttr(atag, "href");
            if (!href) continue;
            std::string resolved = HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*href));
            auto rp = HttpClient::ParseUrl(resolved);
            if (rp.ok() && MediaFireFileKey(*rp.value).has_value()) {
                out.kind = SiteActionKind::kNextUrl;
                out.url = resolved;
                return out;
            }
        }
        return out;
    }

    // 3. Rapidgator
    if (IsRapidgatorHost(pu)) {
        for (const auto& [_, atag] : FindHtmlTags(html_body, "a")) {
            auto href = ExtractHtmlAttr(atag, "href");
            if (!href) continue;
            std::string resolved = HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*href));
            auto rp = HttpClient::ParseUrl(resolved);
            if (rp.ok() && IsRapidgatorFileLink(*rp.value)) {
                out.kind = SiteActionKind::kNextUrl;
                out.url = resolved;
                return out;
            }
        }
        return out;
    }

    // 4. LuluStream
    if (IsLuluHost(pu)) {
        if (segs.size() == 2 && segs[0] == "d" && IsLuluCode(segs[1])) {
            const std::string& code = segs[1];
            static constexpr char kModes[] = {'o', 'h', 'n', 'l'};
            for (char mode : kModes) {
                std::string suffix = "/d/" + code + "_" + std::string(1, mode);
                for (const auto& [_, atag] : FindHtmlTags(html_body, "a")) {
                    auto href = ExtractHtmlAttr(atag, "href");
                    if (!href) continue;
                    std::string resolved = HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*href));
                    if (resolved.find(suffix) != std::string::npos) {
                        out.kind = SiteActionKind::kNextUrl;
                        out.url = resolved;
                        return out;
                    }
                }
            }
        } else if (segs.size() == 2 && segs[0] == "d") {
            auto cm = ParseLuluCodeMode(segs[1]);
            if (cm) {
                const std::string& code = cm->first;
                char mode = cm->second;
                if (html_body.find("data-sitekey") != std::string::npos) {
                    out.kind = SiteActionKind::kRefused;
                    out.refusal_code = "ERR_SITE_NEEDS_BROWSER";
                    out.refusal_message = "This LuluStream link requires browser CAPTCHA verification";
                    return out;
                }
                std::string lower = ToLowerStr(html_body);
                for (const auto& [start, ftag] : FindHtmlTags(html_body, "form")) {
                    size_t body_start = start + ftag.size();
                    size_t body_end = lower.find("</form", body_start);
                    if (body_end == std::string::npos) body_end = html_body.size();
                    std::string fbody = html_body.substr(body_start, body_end - body_start);
                    std::vector<std::pair<std::string, std::string>> fields;
                    bool has_op = false;
                    for (const auto& [_, inp] : FindHtmlTags(fbody, "input")) {
                        auto name = ExtractHtmlAttr(inp, "name");
                        if (!name || name->empty()) continue;
                        std::string k = HtmlDecode(*name);
                        std::string v = HtmlDecode(ExtractHtmlAttr(inp, "value").value_or(""));
                        if (k == "op" && v == "download_orig") has_op = true;
                        fields.emplace_back(k, v);
                    }
                    if (has_op) {
                        auto action = ExtractHtmlAttr(ftag, "action");
                        std::string action_url = (action && !action->empty())
                            ? HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*action))
                            : at_url;
                        std::ostringstream enc;
                        for (size_t i = 0; i < fields.size(); ++i) {
                            if (i > 0) enc << "&";
                            enc << UrlEncode(fields[i].first) << "=" << UrlEncode(fields[i].second);
                        }
                        out.kind = SiteActionKind::kFormPost;
                        out.url = action_url;
                        out.form_body = enc.str();
                        return out;
                    }
                }
                std::string folder = ToLowerStr(code + "_" + std::string(1, mode));
                for (const auto& [_, atag] : FindHtmlTags(html_body, "a")) {
                    auto href = ExtractHtmlAttr(atag, "href");
                    if (!href) continue;
                    std::string resolved = HttpClient::ResolveRedirectTarget(pu, HtmlDecode(*href));
                    if (ToLowerStr(resolved).find(folder) != std::string::npos) {
                        auto rp = HttpClient::ParseUrl(resolved);
                        if (rp.ok() && !IsLuluHost(*rp.value)) {
                            out.kind = SiteActionKind::kNextUrl;
                            out.url = resolved;
                            return out;
                        }
                    }
                }
            }
        }
    }

    return out;
}

bool SiteHandlers::SameParty(const std::string& url_a, const std::string& url_b) {
    auto pa = HttpClient::ParseUrl(url_a);
    auto pb = HttpClient::ParseUrl(url_b);
    if (!pa.ok() || !pb.ok()) return false;
    if (pa.value->port != pb.value->port) return false;

    if (IsDriveHost(*pa.value) && IsDriveHost(*pb.value)) return true;
    if (IsArchiveHost(*pa.value) && IsArchiveHost(*pb.value)) return true;
    if (UnderDomain(pa.value->host, "rapidgator.net") && UnderDomain(pb.value->host, "rapidgator.net")) return true;
    if (IsLuluHost(*pa.value) && IsLuluHost(*pb.value)) return true;
    return false;
}

std::optional<std::string> SiteHandlers::SiteUserAgent(const std::string& url) {
    auto pr = HttpClient::ParseUrl(url);
    if (!pr.ok()) return std::nullopt;
    if (IsDriveHost(*pr.value)) {
        return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0.0.0 Safari/537.36";
    }
    return std::nullopt;
}

bool SiteHandlers::ExpectsFile(const std::string& url) {
    auto pr = HttpClient::ParseUrl(url);
    if (!pr.ok()) return false;
    const ParsedUrl& pu = *pr.value;
    if (IsDriveHost(pu)) return !CheckNotAFile(url).has_value();
    if (IsMediaFireHost(pu)) {
        if (IsMediaFireFileHost(pu)) return true;
        auto segs = SplitPathSegments(ExtractPathOnly(pu.path_and_query));
        std::string path = ExtractPathOnly(pu.path_and_query);
        return path == "/download_repair.php" ||
               (!segs.empty() && (segs[0] == "file" || segs[0] == "file_premium" || segs[0] == "download"));
    }
    if (IsRapidgatorFileLink(pu)) return true;
    return false;
}

std::optional<std::pair<std::string, std::string>> SiteHandlers::FormRefused(const std::string& url) {
    auto pr = HttpClient::ParseUrl(url);
    if (!pr.ok()) return std::nullopt;
    if (IsLuluHost(*pr.value)) {
        return std::make_pair("ERR_SITE_NOT_TAKEN", "LuluStream didn't give the file this time: try again in a moment");
    }
    return std::nullopt;
}

std::optional<IdentityPreset> SiteHandlers::MatchIdentityPreset(
    const std::string& host,
    const std::vector<IdentityPreset>& presets) {
    std::string lhost = ToLowerStr(host);
    for (const auto& preset : presets) {
        if (!preset.enabled) continue;
        for (const auto& pat : preset.hosts) {
            std::string lpat = ToLowerStr(pat);
            if (lpat.rfind("*.", 0) == 0) {
                std::string suffix = lpat.substr(2);
                if (lhost == suffix || UnderDomain(lhost, suffix)) return preset;
            } else if (lhost == lpat) {
                return preset;
            }
        }
    }
    return std::nullopt;
}

std::map<std::string, std::string> SiteHandlers::BuildBrowserImpersonationHeaders(
    const std::string& profile,
    const std::map<std::string, std::string>& base_headers) {
    std::map<std::string, std::string> hdrs = base_headers;
    auto has_ci = [&](const std::string& key) {
        for (const auto& [k, _] : hdrs) {
            if (EqualsIgnoreCase(k, key)) return true;
        }
        return false;
    };

    const std::string lprof = ToLowerStr(profile);
    if (!has_ci("user-agent")) {
        if (lprof == "firefox") {
            hdrs["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:132.0) Gecko/20100101 Firefox/132.0";
        } else if (lprof == "edge") {
            hdrs["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0";
        } else {
            hdrs["User-Agent"] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
        }
    }
    if (lprof != "firefox" && lprof != "safari") {
        if (!has_ci("sec-ch-ua")) hdrs["Sec-CH-UA"] = "\"Chromium\";v=\"131\", \"Not_A Brand\";v=\"24\"";
        if (!has_ci("sec-ch-ua-mobile")) hdrs["Sec-CH-UA-Mobile"] = "?0";
        if (!has_ci("sec-ch-ua-platform")) hdrs["Sec-CH-UA-Platform"] = "\"Windows\"";
    }
    if (!has_ci("accept")) {
        hdrs["Accept"] = "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8";
    }
    if (!has_ci("accept-language")) hdrs["Accept-Language"] = "en-US,en;q=0.9";
    if (!has_ci("sec-fetch-dest")) hdrs["Sec-Fetch-Dest"] = "document";
    if (!has_ci("sec-fetch-mode")) hdrs["Sec-Fetch-Mode"] = "navigate";
    if (!has_ci("sec-fetch-site")) hdrs["Sec-Fetch-Site"] = "none";
    if (!has_ci("upgrade-insecure-requests")) hdrs["Upgrade-Insecure-Requests"] = "1";
    return hdrs;
}

std::string SiteHandlers::FindCurlImpersonateBinary() {
    static constexpr const char* kCandidates[] = {
        "curl-impersonate-chrome.exe",
        "curl_chrome116.exe",
        "curl_ff117.exe",
        "curl-impersonate.exe"
    };
#ifdef _WIN32
    wchar_t exe_buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        std::filesystem::path dir = std::filesystem::path(exe_buf).parent_path();
        for (const char* name : kCandidates) {
            std::error_code ec;
            auto p1 = dir / name;
            if (std::filesystem::exists(p1, ec)) return p1.string();
            auto p2 = dir / "tools" / "win-x64" / name;
            if (std::filesystem::exists(p2, ec)) return p2.string();
        }
    }
#endif
    return {};
}

} // namespace openidm::brisk
