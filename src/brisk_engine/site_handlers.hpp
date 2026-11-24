// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Site Interstitial Handlers, Per-Host Identity Presets & Browser Impersonation
// Ported from Mister Download Manager (crates/engine/src/sites/*.rs, browserlike.rs)
// and Ghost Downloader 3 (app/client.py).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/types.hpp"

namespace openidm::brisk {

enum class SiteActionKind {
    kNothing = 0,
    kNextUrl = 1,
    kFormPost = 2,
    kRefused = 3
};

struct SitePageAction {
    SiteActionKind kind{SiteActionKind::kNothing};
    std::string url;
    std::string form_body;
    std::string refusal_code;
    std::string refusal_message;
};

class SiteHandlers {
public:
    // Rejects links that name a folder or JS-only share page before making an HTTP request.
    static std::optional<std::pair<std::string, std::string>> CheckNotAFile(const std::string& url);

    // Rewrites viewer/watch URLs to direct download/export endpoints (Drive, Docs, LuluStream).
    static std::optional<std::string> RewriteUrl(const std::string& url);

    // Inspects an HTML interstitial page returned at `at_url` and returns the next URL, form POST, or refusal.
    static SitePageAction ReadPage(const std::string& at_url, const std::string& html_body);

    // Returns true if `url_a` and `url_b` belong to the same site party (preserving cookies across redirects).
    static bool SameParty(const std::string& url_a, const std::string& url_b);

    // Returns a site-required User-Agent override (e.g., Chrome UA for Google Drive hosts).
    static std::optional<std::string> SiteUserAgent(const std::string& url);

    // Returns true if `url` is expected to yield a file rather than an HTML page.
    static bool ExpectsFile(const std::string& url);

    // Returns a refusal if a site's form keeps returning itself after max retries (LuluStream).
    static std::optional<std::pair<std::string, std::string>> FormRefused(const std::string& url);

    // Matches a host against per-host IdentityPresets (exact or "*.domain.com").
    static std::optional<IdentityPreset> MatchIdentityPreset(
        const std::string& host,
        const std::vector<IdentityPreset>& presets);

    // Applies browser-impersonation headers (UA, Sec-CH-UA, Sec-Fetch-*, Accept) for bot-check fallback.
    static std::map<std::string, std::string> BuildBrowserImpersonationHeaders(
        const std::string& profile,
        const std::map<std::string, std::string>& base_headers);

    // Discovers an optional curl-impersonate (BoringSSL) executable if present beside the app or on PATH.
    static std::string FindCurlImpersonateBinary();
};

} // namespace openidm::brisk
