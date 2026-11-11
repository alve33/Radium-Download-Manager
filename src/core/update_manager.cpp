// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Update Manager & Package Integrity Verifier

#include "core/update_manager.hpp"
#include "common/crypto_utils.hpp"
#include <charconv>
#include <fstream>
#include <algorithm>
#include <cctype>

namespace radium::core {

namespace {

std::string ToLower(std::string_view sv) {
    std::string s(sv);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ExtractJsonField(std::string_view json, std::string_view key) {
    std::string needle = "\"" + std::string(key) + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    
    // Find opening quote or token
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos) return {};

    if (json[pos] == '"') {
        size_t start = pos + 1;
        size_t end = json.find('"', start);
        if (end == std::string::npos) return {};
        return std::string(json.substr(start, end - start));
    } else {
        size_t start = pos;
        size_t end = json.find_first_of(",}\r\n \t", start);
        if (end == std::string::npos) end = json.size();
        return std::string(json.substr(start, end - start));
    }
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
    if (major != other.major) return major < other.major;
    if (minor != other.minor) return minor < other.minor;
    if (patch != other.patch) return patch < other.patch;
    // Release version is greater than prerelease version
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

openidm::Result<UpdatePackageInfo> UpdateManager::ParseReleaseManifest(std::string_view json_content) {
    if (json_content.empty()) {
        return openidm::Result<UpdatePackageInfo>::Err("ERR_EMPTY_MANIFEST", "Release manifest is empty");
    }

    UpdatePackageInfo info;
    info.version = ExtractJsonField(json_content, "tag_name");
    if (info.version.empty()) {
        info.version = ExtractJsonField(json_content, "version");
    }
    if (info.version.empty()) {
        return openidm::Result<UpdatePackageInfo>::Err("ERR_INVALID_MANIFEST", "Manifest missing version or tag_name");
    }

    info.release_title = ExtractJsonField(json_content, "name");
    info.release_notes = ExtractJsonField(json_content, "body");
    info.download_url = ExtractJsonField(json_content, "browser_download_url");
    if (info.download_url.empty()) {
        info.download_url = ExtractJsonField(json_content, "download_url");
    }
    info.expected_sha256 = ExtractJsonField(json_content, "sha256");
    info.published_at = ExtractJsonField(json_content, "published_at");
    
    std::string crit_str = ExtractJsonField(json_content, "mandatory");
    info.is_critical = (crit_str == "true" || crit_str == "1");

    return openidm::Result<UpdatePackageInfo>::Ok(std::move(info));
}

bool UpdateManager::IsNewerVersion(std::string_view current_version, std::string_view latest_version) {
    auto cur = SemVer::Parse(current_version);
    auto lat = SemVer::Parse(latest_version);
    if (!cur.has_value() || !lat.has_value()) {
        return false;
    }
    return *cur < *lat;
}

openidm::Result<bool> UpdateManager::VerifyPackageSha256(const std::filesystem::path& file_path, std::string_view expected_hex_hash) {
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec) || !std::filesystem::is_regular_file(file_path, ec)) {
        return openidm::Result<bool>::Err("ERR_FILE_NOT_FOUND", "Update file not found on disk");
    }

    auto file_res = openidm::CryptoUtils::ComputeFileSha256Hex(file_path);
    if (!file_res.ok()) {
        return openidm::Result<bool>::Err(file_res.error->code, file_res.error->message);
    }
    std::string actual_hash = file_res.value.value();

    std::string expected_clean = ToLower(expected_hex_hash);
    std::string actual_clean = ToLower(actual_hash);

    if (actual_clean.empty()) {
        return openidm::Result<bool>::Err("ERR_HASH_FAILED", "Failed to calculate SHA-256 hash");
    }

    if (actual_clean == expected_clean) {
        return openidm::Result<bool>::Ok(true);
    }

    return openidm::Result<bool>::Err("ERR_HASH_MISMATCH", 
        "Package SHA-256 verification failed. Expected: " + expected_clean + ", Actual: " + actual_clean);
}

} // namespace radium::core
