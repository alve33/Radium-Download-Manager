// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Non-Extracting ZIP Archive Central Directory Previewer & Security Guard
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.hpp"

namespace radium::archive {

struct ZipEntryInfo {
    std::string file_path;
    uint64_t uncompressed_size{0};
    uint64_t compressed_size{0};
    uint64_t local_header_offset{0};
    uint32_t crc32{0};
    uint16_t compression_method{0}; // 0 = Stored, 8 = Deflated
    bool is_directory{false};
    bool is_encrypted{false};
    bool is_suspicious_path{false}; // Zip Slip indicator (traversal, absolute path, ADS)
    std::string modified_date_str;
};

struct ZipArchiveSummary {
    std::vector<ZipEntryInfo> entries;
    uint64_t total_uncompressed_bytes{0};
    uint64_t total_compressed_bytes{0};
    uint32_t total_files{0};
    uint32_t total_folders{0};
    bool has_encrypted_entries{false};
    bool is_potential_zip_bomb{false};
    bool has_zip_slip_risk{false};
    std::string comment;
};

class ZipPreviewer {
public:
    ZipPreviewer() = default;
    ~ZipPreviewer() = default;

    // Parses the Central Directory of a local file without extracting file bodies
    static Result<ZipArchiveSummary> InspectArchive(const std::filesystem::path& file_path);

    // Parses Central Directory from a byte buffer (e.g. tail of partial download)
    static Result<ZipArchiveSummary> InspectBuffer(const uint8_t* data, size_t length);

    // Tests whether a file path contains path traversal or dangerous elements
    static bool IsZipSlipPath(std::string_view path);

    // Remote HTTP-Range ZIP EOCD + Central Directory inspection (GD3 zip_pack port)
    static Result<ZipArchiveSummary> InspectRemoteUrl(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        std::atomic<bool>& cancel_flag,
        bool allow_loopback = false);

    // Selective remote entry download & extraction via HTTP Range
    static Result<std::filesystem::path> DownloadAndExtractRemoteEntry(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const ZipEntryInfo& entry,
        const std::filesystem::path& dest_dir,
        std::atomic<bool>& cancel_flag,
        bool allow_loopback = false);
};

// Post-download safe archive extraction (MDM crates/extract port)
enum class ArchiveFormat {
    kUnknown = 0,
    kZip,
    kSevenZip,
    kRar,
    kTar,
    kTarGz,
    kTarXz,
    kGz,
    kXz
};

enum class SubfolderPolicy {
    kSmart = 0, // Subfolder named after archive when > 1 top-level entry
    kAlways,
    kNever
};

class ArchiveExtractor {
public:
    static ArchiveFormat DetectFormat(const std::filesystem::path& path);
    static Result<std::filesystem::path> ExtractArchive(
        const std::filesystem::path& archive_path,
        const std::filesystem::path& dest_dir,
        SubfolderPolicy subfolder_policy = SubfolderPolicy::kSmart,
        uint64_t max_total_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL,
        size_t max_files = 50000);
};

} // namespace radium::archive
