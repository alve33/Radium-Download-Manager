// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — BitTorrent Metainfo Creator (.torrent Generator)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "common/types.hpp"

namespace radium::torrent {

struct TorrentCreatorOptions {
    uint32_t piece_length{0}; // 0 = automatic selection (256 KiB - 4 MiB)
    std::vector<std::string> announce_urls; // Primary and tier trackers
    std::string comment;
    std::string created_by{"Radium Download Manager 1.0"};
    bool is_private{false};
    std::string source;
};

struct TorrentFileItem {
    std::filesystem::path relative_path;
    uint64_t file_size{0};
};

class TorrentCreator {
public:
    // Create a .torrent file from a single file or a directory
    static Result<std::vector<uint8_t>> CreateTorrent(
        const std::filesystem::path& source_path,
        const TorrentCreatorOptions& options = TorrentCreatorOptions{});

    // Write .torrent file to disk directly
    static Result<void> CreateTorrentFile(
        const std::filesystem::path& source_path,
        const std::filesystem::path& output_torrent_path,
        const TorrentCreatorOptions& options = TorrentCreatorOptions{});

    // Generate canonical magnet link from torrent data
    static std::string GenerateMagnetLink(
        const std::string& info_hash_hex,
        const std::string& display_name,
        const std::vector<std::string>& announce_urls);

    // Auto-calculate optimal piece size in bytes based on total payload size
    static uint32_t CalculateOptimalPieceLength(uint64_t total_size_bytes);
};

} // namespace radium::torrent
