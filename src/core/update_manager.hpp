#pragma once
// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Update Manager & Package Integrity Verifier
// Checks for available software releases, compares Semantic Versions,
// and cryptographically verifies release package SHA-256 hashes before staging.

#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include "common/types.hpp"

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

class UpdateManager {
public:
    UpdateManager() = default;
    ~UpdateManager() = default;

    // Parses a JSON release manifest (e.g. from GitHub releases or official Radium update server)
    static openidm::Result<UpdatePackageInfo> ParseReleaseManifest(std::string_view json_content);

    // Determines whether latest_version is newer than current_version
    static bool IsNewerVersion(std::string_view current_version, std::string_view latest_version);

    // Cryptographically checks downloaded file SHA-256 against expected hash
    static openidm::Result<bool> VerifyPackageSha256(const std::filesystem::path& file_path, std::string_view expected_hex_hash);
};

} // namespace radium::core
