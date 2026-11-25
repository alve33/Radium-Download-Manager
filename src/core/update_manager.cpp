// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Update Manager & Package Integrity Verifier

#include "core/update_manager.hpp"
#include "common/central_config.hpp"
#include "common/crypto_utils.hpp"
#include "common/semver.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>

namespace radium::core {

namespace {

std::string ToLower(std::string_view sv) {
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string_view ResolveReleasePrefix(std::string_view expected_release_prefix) {
    return expected_release_prefix.empty()
        ? radium::config::UpdateAndReleaseConfig::kReleasePrefixUrl
        : expected_release_prefix;
}

std::optional<std::string> FindHeaderCaseInsensitive(
    const std::map<std::string, std::string>& headers,
    std::string_view key) {
    const std::string lower_key = ToLower(key);
    for (const auto& [k, v] : headers) {
        if (ToLower(k) == lower_key) {
            return v;
        }
    }
    return std::nullopt;
}

bool MatchesTargetArchitecture(std::string_view lower_name, std::string_view target_arch) {
    const bool has_x64 = lower_name.find("x64") != std::string_view::npos ||
                         lower_name.find("win64") != std::string_view::npos ||
                         lower_name.find("amd64") != std::string_view::npos;
    const bool has_arm64 = lower_name.find("arm64") != std::string_view::npos ||
                           lower_name.find("aarch64") != std::string_view::npos;
    if (ToLower(target_arch) == "arm64") {
        return has_arm64 && !has_x64;
    }
    return has_x64 && !has_arm64;
}

} // namespace

std::optional<SemVer> SemVer::Parse(std::string_view str) {
    if (str.empty()) return std::nullopt;
    if (str.front() == 'v' || str.front() == 'V') {
        str.remove_prefix(1);
    }

    SemVer ver;
    size_t dash = str.find('-');
    std::string_view main_ver = (dash != std::string_view::npos) ? str.substr(0, dash) : str;
    if (dash != std::string_view::npos) {
        ver.prerelease = std::string(str.substr(dash + 1));
    }

    size_t p1 = main_ver.find('.');
    if (p1 == std::string_view::npos) {
        std::from_chars(main_ver.data(), main_ver.data() + main_ver.size(), ver.major);
        return ver;
    }
    std::from_chars(main_ver.data(), main_ver.data() + p1, ver.major);

    size_t p2 = main_ver.find('.', p1 + 1);
    if (p2 == std::string_view::npos) {
        std::from_chars(main_ver.data() + p1 + 1, main_ver.data() + main_ver.size(), ver.minor);
        return ver;
    }
    std::from_chars(main_ver.data() + p1 + 1, main_ver.data() + p2, ver.minor);
    std::from_chars(main_ver.data() + p2 + 1, main_ver.data() + main_ver.size(), ver.patch);

    return ver;
}

bool SemVer::operator<(const SemVer& other) const {
    auto cmp = radium::CompareSemanticVersions(ToString(), other.ToString());
    if (cmp.has_value()) {
        return *cmp < 0;
    }
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    if (patch != other.patch) return patch < other.patch;
    if (prerelease.empty() && !other.prerelease.empty()) return false;
    if (!prerelease.empty() && other.prerelease.empty()) return true;
    return prerelease < other.prerelease;
}

bool SemVer::operator>(const SemVer& other) const {
    return other < *this;
}

bool SemVer::operator==(const SemVer& other) const {
    return major == other.major && minor == other.minor && patch == other.patch && prerelease == other.prerelease;
}

std::string SemVer::ToString() const {
    std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    if (!prerelease.empty()) {
        s += "-" + prerelease;
    }
    return s;
}

std::string UpdateManager::CurrentArchitecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "x64";
#endif
}

bool UpdateManager::IsTrustedReleaseUrl(std::string_view url, std::string_view expected_prefix) {
    const std::string_view prefix = ResolveReleasePrefix(expected_prefix);
    if (url.size() < prefix.size()) return false;
    if (url.rfind("https://", 0) != 0) return false;
    if (url.substr(0, prefix.size()) != prefix) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7F) return false;
    }
    return true;
}

std::optional<UpdateAssetInfo> UpdateManager::SelectReleaseAsset(
    const radium::json::Array& assets,
    std::string_view target_arch,
    std::string_view expected_release_prefix) {
    const std::string_view prefix = ResolveReleasePrefix(expected_release_prefix);
    std::optional<UpdateAssetInfo> core_fallback;

    for (const auto& asset : assets) {
        if (!asset.is_object()) continue;
        const std::string name = asset["name"].as_string();
        const std::string url = asset["browser_download_url"].as_string();
        if (name.empty() || !IsTrustedReleaseUrl(url, prefix)) continue;

        const std::string lower_name = ToLower(name);
        if (!MatchesTargetArchitecture(lower_name, target_arch)) continue;

        const bool is_installer =
            lower_name.find("setup") != std::string::npos &&
            lower_name.ends_with(".exe");
        if (!is_installer) continue;

        UpdateAssetInfo candidate;
        candidate.name = name;
        candidate.download_url = url;
        const int64_t raw_size = asset["size"].as_int64(0);
        candidate.size_bytes = raw_size > 0 ? static_cast<uint64_t>(raw_size) : 0;
        candidate.is_installer = true;
        candidate.is_core_installer = (lower_name.find("core") != std::string::npos);

        if (!candidate.is_core_installer) {
            return candidate;
        }
        if (!core_fallback.has_value()) {
            core_fallback = std::move(candidate);
        }
    }

    return core_fallback;
}

GitHubUpdateEvaluation UpdateManager::EvaluateGitHubReleaseResponse(
    int http_status,
    const std::map<std::string, std::string>& response_headers,
    std::string_view response_body,
    std::string_view current_version,
    std::string_view target_arch,
    bool allow_prerelease,
    std::string_view expected_release_prefix) {
    GitHubUpdateEvaluation eval;
    const std::string_view release_prefix = ResolveReleasePrefix(expected_release_prefix);

    if (http_status != 200) {
        if (http_status == 404) {
            eval.is_not_found = true;
            eval.message = "No published GitHub releases were found for alve33/Radium-Download-Manager yet (HTTP 404).";
            return eval;
        }
        if (http_status == 403 || http_status == 429) {
            const auto remaining = FindHeaderCaseInsensitive(response_headers, "x-ratelimit-remaining");
            const auto retry_after = FindHeaderCaseInsensitive(response_headers, "retry-after");
            const bool rate_limited = (http_status == 429) || (remaining.has_value() && *remaining == "0");
            if (rate_limited) {
                eval.is_rate_limited = true;
                eval.message = "GitHub API rate limit exceeded. Please try again later.";
                if (retry_after.has_value() && !retry_after->empty()) {
                    eval.message += " Retry after " + *retry_after + " seconds.";
                }
                return eval;
            }
            eval.message = "Could not check GitHub Releases: HTTP 403 Forbidden.";
            return eval;
        }
        eval.message = "Could not check GitHub Releases: HTTP status " + std::to_string(http_status) + ".";
        return eval;
    }

    try {
        const auto release = radium::json::Parse(std::string(response_body));
        const std::string version = release["tag_name"].as_string();
        const auto comparison = radium::CompareSemanticVersions(version, current_version);
        const bool is_draft = release["draft"].as_bool(true);
        const bool is_prerelease = release["prerelease"].as_bool(false);
        if (!release.is_object() || version.empty() || is_draft ||
            (!allow_prerelease && is_prerelease) || !comparison.has_value()) {
            eval.message = "GitHub returned an invalid or non-stable release response.";
            return eval;
        }

        eval.success = true;
        eval.version = version;

        if (*comparison <= 0) {
            eval.update_available = false;
            eval.message = "Radium " + std::string(current_version) +
                " is up to date. Latest stable release: " + version + ".";
            return eval;
        }

        eval.update_available = true;
        const std::string html_url = release["html_url"].as_string();
        if (IsTrustedReleaseUrl(html_url, release_prefix)) {
            eval.release_url = html_url;
        }

        eval.selected_asset = SelectReleaseAsset(release["assets"].as_array(), target_arch, release_prefix);
        eval.matching_installer = eval.selected_asset.has_value();

        std::string notes = release["body"].as_string();
        if (notes.size() > 1000) {
            notes.resize(1000);
        }
        eval.release_notes = notes;

        std::string asset_line = eval.matching_installer
            ? ("A matching Windows installer is attached (" + eval.selected_asset->name + ").\n\n")
            : "No matching Windows installer is attached.\n\n";

        eval.message = "Version " + version + " is available.\n" +
            asset_line +
            "Release notes:\n" + notes +
            "\n\nAutomatic installation is disabled: this repository has no published Ed25519-signed update manifest/public key or configured Authenticode publisher identity. You can open the official release page and install manually.";
        return eval;
    } catch (...) {
        eval.message = "GitHub returned an invalid release response.";
        return eval;
    }
}

std::string UpdateManager::FormatNetworkCheckError(const radium::Error& error) {
    if (error.code == "ERR_NET_TIMEOUT") {
        return "Could not check GitHub Releases: request timed out (" + error.message + ").";
    }
    if (error.code == "ERR_NET_DNS" || error.code == "ERR_NET_CONNECT" || error.code == "ERR_NET_CONNECTION_RESET") {
        return "Could not check GitHub Releases: network connection failed (" + error.message + ").";
    }
    if (error.code == "ERR_SSL_PROTOCOL") {
        return "Could not check GitHub Releases: TLS/SSL verification failed (" + error.message + ").";
    }
    return "Could not check GitHub Releases: " + error.message;
}

radium::Result<UpdatePackageInfo> UpdateManager::ParseReleaseManifest(std::string_view json_content) {
    if (json_content.empty()) {
        return radium::Result<UpdatePackageInfo>::Err("ERR_EMPTY_MANIFEST", "Release manifest is empty");
    }

    try {
        const auto root = radium::json::Parse(std::string(json_content));
        if (!root.is_object()) {
            return radium::Result<UpdatePackageInfo>::Err("ERR_INVALID_MANIFEST", "Release manifest is not a JSON object");
        }

        UpdatePackageInfo info;
        info.version = root["tag_name"].as_string();
        if (info.version.empty()) {
            info.version = root["version"].as_string();
        }
        if (info.version.empty()) {
            return radium::Result<UpdatePackageInfo>::Err("ERR_INVALID_MANIFEST", "Manifest missing version or tag_name");
        }

        info.release_title = root["name"].as_string();
        info.release_notes = root["body"].as_string();
        info.expected_sha256 = root["sha256"].as_string();
        info.published_at = root["published_at"].as_string();
        info.is_critical = root["mandatory"].as_bool(false);

        if (root.contains("assets") && root["assets"].is_array()) {
            if (auto asset = SelectReleaseAsset(root["assets"].as_array(), CurrentArchitecture())) {
                info.download_url = asset->download_url;
                info.package_size_bytes = asset->size_bytes;
            }
        }
        if (info.download_url.empty()) {
            info.download_url = root["browser_download_url"].as_string();
        }
        if (info.download_url.empty()) {
            info.download_url = root["download_url"].as_string();
        }

        return radium::Result<UpdatePackageInfo>::Ok(std::move(info));
    } catch (...) {
        return radium::Result<UpdatePackageInfo>::Err("ERR_INVALID_MANIFEST", "Release manifest JSON is malformed");
    }
}

bool UpdateManager::IsNewerVersion(std::string_view current_version, std::string_view latest_version) {
    const auto cmp = radium::CompareSemanticVersions(latest_version, current_version);
    return cmp.has_value() && *cmp > 0;
}

radium::Result<bool> UpdateManager::VerifyPackageSha256(const std::filesystem::path& file_path, std::string_view expected_hex_hash) {
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec) || !std::filesystem::is_regular_file(file_path, ec)) {
        return radium::Result<bool>::Err("ERR_FILE_NOT_FOUND", "Update file not found on disk");
    }

    auto file_res = radium::CryptoUtils::ComputeFileSha256Hex(file_path);
    if (!file_res.ok()) {
        return radium::Result<bool>::Err(file_res.error->code, file_res.error->message);
    }
    std::string actual_hash = file_res.value.value();

    std::string expected_clean = ToLower(expected_hex_hash);
    std::string actual_clean = ToLower(actual_hash);

    if (actual_clean.empty()) {
        return radium::Result<bool>::Err("ERR_HASH_FAILED", "Failed to calculate SHA-256 hash");
    }

    if (actual_clean == expected_clean) {
        return radium::Result<bool>::Ok(true);
    }

    return radium::Result<bool>::Err("ERR_HASH_MISMATCH",
        "Package SHA-256 verification failed. Expected: " + expected_clean + ", Actual: " + actual_clean);
}

} // namespace radium::core
