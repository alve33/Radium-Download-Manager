#pragma once
// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Update Manager & Package Integrity Verifier
// Checks for available software releases, compares Semantic Versions,
// and cryptographically verifies release package SHA-256 hashes before staging.

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <optional>
#include <filesystem>
#include "common/types.hpp"
#include "common/json_mini.hpp"

namespace radium::core {

struct SemVer {
    int major{0};
    int minor{0};
    int patch{0};
    std::string prerelease;

    static std::optional<SemVer> Parse(std::string_view version_str);
    bool operator<(const SemVer& other) const;
    bool operator>(const SemVer& other) const;
    bool operator==(const SemVer& other) const;
    std::string ToString() const;
};

struct UpdatePackageInfo {
    std::string version;
    std::string release_title;
    std::string release_notes;
    std::string download_url;
    std::string expected_sha256;
    uint64_t package_size_bytes{0};
    std::string published_at;
    bool is_critical{false};
};

struct UpdateCheckResult {
    bool update_available{false};
    std::string current_version;
    UpdatePackageInfo latest_package;
};

struct UpdateAssetInfo {
    std::string name;
    std::string download_url;
    uint64_t size_bytes{0};
    bool is_installer{false};
    bool is_core_installer{false};
};

struct GitHubUpdateEvaluation {
    bool success{false};
    bool update_available{false};
    bool matching_installer{false};
    bool is_rate_limited{false};
    bool is_not_found{false};
    std::string version;
    std::string release_url;
    std::string release_notes;
    std::optional<UpdateAssetInfo> selected_asset;
    std::string message;
};

class UpdateManager {
public:
    UpdateManager() = default;
    ~UpdateManager() = default;

    static std::string CurrentArchitecture();

    static bool IsTrustedReleaseUrl(std::string_view url, std::string_view expected_prefix = {});

    static std::optional<UpdateAssetInfo> SelectReleaseAsset(
        const openidm::json::Array& assets,
        std::string_view target_arch,
        std::string_view expected_release_prefix = {});

    static GitHubUpdateEvaluation EvaluateGitHubReleaseResponse(
        int http_status,
        const std::map<std::string, std::string>& response_headers,
        std::string_view response_body,
        std::string_view current_version,
        std::string_view target_arch,
        bool allow_prerelease = false,
        std::string_view expected_release_prefix = {});

    static std::string FormatNetworkCheckError(const openidm::Error& error);

    // Parses a JSON release manifest (e.g. from GitHub releases or official Radium update server)
    static openidm::Result<UpdatePackageInfo> ParseReleaseManifest(std::string_view json_content);

    // Determines whether latest_version is newer than current_version
    static bool IsNewerVersion(std::string_view current_version, std::string_view latest_version);

    // Cryptographically checks downloaded file SHA-256 against expected hash
    static openidm::Result<bool> VerifyPackageSha256(const std::filesystem::path& file_path, std::string_view expected_hex_hash);
};

} // namespace radium::core
