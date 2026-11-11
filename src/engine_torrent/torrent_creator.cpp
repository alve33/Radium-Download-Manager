// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — BitTorrent Metainfo Creator (.torrent Generator)
#include "engine_torrent/torrent_creator.hpp"
#include "engine_torrent/bencode.hpp"
#include "common/crypto_utils.hpp"
#include "common/logger.hpp"

#include <chrono>
#include <fstream>
#include <algorithm>

namespace openidm::torrent {

uint32_t TorrentCreator::CalculateOptimalPieceLength(uint64_t total_size_bytes) {
    if (total_size_bytes < 512ULL * 1024 * 1024) {
        return 256 * 1024; // 256 KiB
    } else if (total_size_bytes < 1024ULL * 1024 * 1024) {
        return 512 * 1024; // 512 KiB
    } else if (total_size_bytes < 4ULL * 1024 * 1024 * 1024) {
        return 1024 * 1024; // 1 MiB
    } else if (total_size_bytes < 8ULL * 1024 * 1024 * 1024) {
        return 2 * 1024 * 1024; // 2 MiB
    } else {
        return 4 * 1024 * 1024; // 4 MiB
    }
}

Result<std::vector<uint8_t>> TorrentCreator::CreateTorrent(
    const std::filesystem::path& source_path,
    const TorrentCreatorOptions& options) {
    std::error_code ec;
    if (!std::filesystem::exists(source_path, ec)) {
        return Result<std::vector<uint8_t>>::Err("ERR_FILE_NOT_FOUND", "Source path does not exist: " + source_path.string());
    }

    bool is_directory = std::filesystem::is_directory(source_path, ec);
    std::vector<TorrentFileItem> files;
    uint64_t total_size = 0;

    if (is_directory) {
        for (auto const& dir_entry : std::filesystem::recursive_directory_iterator(source_path, ec)) {
            if (dir_entry.is_regular_file()) {
                auto rel = std::filesystem::relative(dir_entry.path(), source_path, ec);
                uint64_t sz = dir_entry.file_size(ec);
                files.push_back({rel, sz});
                total_size += sz;
            }
        }
        std::sort(files.begin(), files.end(), [](const TorrentFileItem& a, const TorrentFileItem& b) {
            return a.relative_path.generic_string() < b.relative_path.generic_string();
        });
    } else {
        uint64_t sz = std::filesystem::file_size(source_path, ec);
        files.push_back({source_path.filename(), sz});
        total_size = sz;
    }

    if (files.empty() || total_size == 0) {
        return Result<std::vector<uint8_t>>::Err("ERR_EMPTY_PAYLOAD", "Source contains no regular files or has 0 bytes");
    }

    uint32_t piece_len = options.piece_length > 0 ? options.piece_length : CalculateOptimalPieceLength(total_size);
    if (piece_len < 16 * 1024) piece_len = 16 * 1024;

    std::vector<uint8_t> piece_hashes;
    std::vector<uint8_t> piece_buffer;
    piece_buffer.reserve(piece_len);

    for (const auto& f : files) {
        std::filesystem::path full_p = is_directory ? (source_path / f.relative_path) : source_path;
        std::ifstream ifs(full_p, std::ios::binary);
        if (!ifs) {
            return Result<std::vector<uint8_t>>::Err("ERR_IO_READ", "Failed to open file for hashing: " + full_p.string());
        }

        std::vector<uint8_t> read_chunk(64 * 1024);
        while (ifs) {
            ifs.read(reinterpret_cast<char*>(read_chunk.data()), static_cast<std::streamsize>(read_chunk.size()));
            size_t bytes_read = static_cast<size_t>(ifs.gcount());
            if (bytes_read == 0) break;

            size_t offset = 0;
            while (offset < bytes_read) {
                size_t needed = piece_len - piece_buffer.size();
                size_t take = std::min(needed, bytes_read - offset);
                piece_buffer.insert(piece_buffer.end(), read_chunk.begin() + offset, read_chunk.begin() + offset + take);
                offset += take;

                if (piece_buffer.size() == piece_len) {
                    auto sha1 = CryptoUtils::ComputeBufferSha1Raw(piece_buffer.data(), piece_buffer.size());
                    piece_hashes.insert(piece_hashes.end(), sha1.begin(), sha1.end());
                    piece_buffer.clear();
                }
            }
        }
    }

    if (!piece_buffer.empty()) {
        auto sha1 = CryptoUtils::ComputeBufferSha1Raw(piece_buffer.data(), piece_buffer.size());
        piece_hashes.insert(piece_hashes.end(), sha1.begin(), sha1.end());
        piece_buffer.clear();
    }

    // Construct Bencoded Info Dictionary
    BencodeValue info_dict = BencodeValue::MakeDict();
    info_dict["piece length"] = BencodeValue(static_cast<int64_t>(piece_len));
    info_dict["pieces"] = BencodeValue(std::string(piece_hashes.begin(), piece_hashes.end()));
    info_dict["name"] = BencodeValue(source_path.filename().string());

    if (options.is_private) {
        info_dict["private"] = BencodeValue(static_cast<int64_t>(1));
    }
    if (!options.source.empty()) {
        info_dict["source"] = BencodeValue(options.source);
    }

    if (is_directory) {
        BencodeValue files_list = BencodeValue::MakeList();
        for (const auto& f : files) {
            BencodeValue f_dict = BencodeValue::MakeDict();
            f_dict["length"] = BencodeValue(static_cast<int64_t>(f.file_size));
            BencodeValue path_list = BencodeValue::MakeList();
            for (const auto& part : f.relative_path) {
                path_list.as_list().push_back(BencodeValue(part.string()));
            }
            f_dict["path"] = std::move(path_list);
            files_list.as_list().push_back(std::move(f_dict));
        }
        info_dict["files"] = std::move(files_list);
    } else {
        info_dict["length"] = BencodeValue(static_cast<int64_t>(total_size));
    }

    // Construct Root Metainfo Dictionary
    BencodeValue root = BencodeValue::MakeDict();
    if (!options.announce_urls.empty()) {
        root["announce"] = BencodeValue(options.announce_urls[0]);
        if (options.announce_urls.size() > 1) {
            BencodeValue announce_list = BencodeValue::MakeList();
            for (const auto& tr : options.announce_urls) {
                BencodeValue tier = BencodeValue::MakeList();
                tier.as_list().push_back(BencodeValue(tr));
                announce_list.as_list().push_back(std::move(tier));
            }
            root["announce-list"] = std::move(announce_list);
        }
    }

    if (!options.comment.empty()) {
        root["comment"] = BencodeValue(options.comment);
    }
    if (!options.created_by.empty()) {
        root["created by"] = BencodeValue(options.created_by);
    }
    auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    root["creation date"] = BencodeValue(static_cast<int64_t>(now_epoch));
    root["info"] = std::move(info_dict);

    std::string encoded = BencodeParser::Encode(root);
    return Result<std::vector<uint8_t>>::Ok(std::vector<uint8_t>(encoded.begin(), encoded.end()));
}

Result<void> TorrentCreator::CreateTorrentFile(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_torrent_path,
    const TorrentCreatorOptions& options) {
    auto res = CreateTorrent(source_path, options);
    if (!res.ok()) {
        return Result<void>::Err(res.error->code, res.error->message);
    }

    std::ofstream ofs(output_torrent_path, std::ios::binary);
    if (!ofs) {
        return Result<void>::Err("ERR_IO_WRITE", "Failed to create output .torrent file: " + output_torrent_path.string());
    }
    const auto& data = res.value.value();
    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return Result<void>::Ok();
}

std::string TorrentCreator::GenerateMagnetLink(
    const std::string& info_hash_hex,
    const std::string& display_name,
    const std::vector<std::string>& announce_urls) {
    std::string magnet = "magnet:?xt=urn:btih:" + info_hash_hex;
    if (!display_name.empty()) {
        magnet += "&dn=" + display_name;
    }
    for (const auto& tr : announce_urls) {
        if (!tr.empty()) {
            magnet += "&tr=" + tr;
        }
    }
    return magnet;
}

} // namespace openidm::torrent
