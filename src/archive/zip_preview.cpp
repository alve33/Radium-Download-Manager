// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Non-Extracting ZIP & ZIP64 Archive Central Directory Previewer & Security Guard
#include "archive/zip_preview.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace openidm::archive {

namespace {

#pragma pack(push, 1)
struct ZipEOCD {
    uint32_t signature;           // 0x06054b50
    uint16_t disk_number;
    uint16_t cd_start_disk;
    uint16_t total_entries_disk;
    uint16_t total_entries;
    uint32_t cd_size;
    uint32_t cd_offset;
    uint16_t comment_length;
};

struct Zip64EOCDLocator {
    uint32_t signature;           // 0x07064b50
    uint32_t cd_start_disk;
    uint64_t eocd_offset;
    uint32_t total_disks;
};

struct Zip64EOCDRecord {
    uint32_t signature;           // 0x06064b50
    uint64_t record_size;
    uint16_t version_made_by;
    uint16_t version_needed;
    uint32_t disk_number;
    uint32_t cd_start_disk;
    uint64_t total_entries_disk;
    uint64_t total_entries;
    uint64_t cd_size;
    uint64_t cd_offset;
};

struct ZipCDHeader {
    uint32_t signature;           // 0x02014b50
    uint16_t version_made_by;
    uint16_t version_needed;
    uint16_t flags;
    uint16_t compression_method;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t filename_length;
    uint16_t extra_length;
    uint16_t comment_length;
    uint16_t disk_start;
    uint16_t internal_attrs;
    uint32_t external_attrs;
    uint32_t local_header_offset;
};
#pragma pack(pop)

} // namespace

bool ZipPreviewer::IsZipSlipPath(std::string_view path) {
    if (path.empty()) return false;

    // Check for drive letters (e.g. C:)
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return true;
    }

    // Check for NTFS Alternate Data Streams (ADS)
    if (path.find(':') != std::string_view::npos) {
        return true;
    }

    // Check for leading slashes
    if (path.front() == '/' || path.front() == '\\') {
        return true;
    }

    // Check for path traversal elements (..)
    std::string_view cur = path;
    while (!cur.empty()) {
        size_t slash = cur.find_first_of("/\\");
        std::string_view segment = (slash == std::string_view::npos) ? cur : cur.substr(0, slash);
        if (segment == "..") {
            return true;
        }
        if (slash == std::string_view::npos) break;
        cur = cur.substr(slash + 1);
    }

    return false;
}

Result<ZipArchiveSummary> ZipPreviewer::InspectArchive(const std::filesystem::path& file_path) {
    std::error_code ec;
    if (!std::filesystem::exists(file_path, ec)) {
        return Result<ZipArchiveSummary>::Err("ERR_FILE_NOT_FOUND", "ZIP file does not exist: " + file_path.string());
    }

    auto file_size = std::filesystem::file_size(file_path, ec);
    if (ec || file_size < sizeof(ZipEOCD)) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_TOO_SMALL", "File is too small to be a valid ZIP archive");
    }

    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        return Result<ZipArchiveSummary>::Err("ERR_FILE_OPEN", "Could not open ZIP file for reading");
    }

    // Search last 65KB + EOCD (max ZIP comment length is 65535 bytes)
    const std::streamsize kMaxCommentSearch = 65535 + sizeof(ZipEOCD) + sizeof(Zip64EOCDLocator) + 128;
    std::streamsize search_size = (std::min)(static_cast<std::streamsize>(file_size), kMaxCommentSearch);
    std::vector<uint8_t> tail_buffer(static_cast<size_t>(search_size));

    ifs.seekg(file_size - search_size, std::ios::beg);
    ifs.read(reinterpret_cast<char*>(tail_buffer.data()), search_size);

    // Search backwards for standard EOCD signature: 0x06054b50
    const uint32_t kEocdSig = 0x06054b50;
    std::streamsize eocd_idx = -1;

    for (std::streamsize i = search_size - static_cast<std::streamsize>(sizeof(ZipEOCD)); i >= 0; --i) {
        uint32_t sig = 0;
        std::memcpy(&sig, &tail_buffer[static_cast<size_t>(i)], sizeof(sig));
        if (sig == kEocdSig) {
            eocd_idx = i;
            break;
        }
    }

    if (eocd_idx == -1) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_EOCD_NOT_FOUND", "Could not find End of Central Directory record");
    }

    ZipEOCD eocd;
    std::memcpy(&eocd, &tail_buffer[static_cast<size_t>(eocd_idx)], sizeof(ZipEOCD));

    uint64_t cd_offset = eocd.cd_offset;
    uint64_t cd_size = eocd.cd_size;
    uint64_t u_file_size = static_cast<uint64_t>(file_size);

    // Check for ZIP64 locator preceding standard EOCD
    // ZIP64 is used when cd_offset == 0xFFFFFFFF or cd_size == 0xFFFFFFFF or entries == 0xFFFF
    bool check_zip64 = (eocd.cd_offset == 0xFFFFFFFF || eocd.cd_size == 0xFFFFFFFF || eocd.total_entries == 0xFFFF);
    if (!check_zip64 && eocd_idx >= static_cast<std::streamsize>(sizeof(Zip64EOCDLocator))) {
        // Also check if ZIP64 locator is present even if fields didn't overflow
        uint32_t loc_sig = 0;
        std::memcpy(&loc_sig, &tail_buffer[static_cast<size_t>(eocd_idx - sizeof(Zip64EOCDLocator))], sizeof(loc_sig));
        if (loc_sig == 0x07064b50) {
            check_zip64 = true;
        }
    }

    if (check_zip64 && eocd_idx >= static_cast<std::streamsize>(sizeof(Zip64EOCDLocator))) {
        Zip64EOCDLocator locator;
        std::memcpy(&locator, &tail_buffer[static_cast<size_t>(eocd_idx - sizeof(Zip64EOCDLocator))], sizeof(Zip64EOCDLocator));
        if (locator.signature == 0x07064b50 && locator.eocd_offset < u_file_size) {
            ifs.seekg(locator.eocd_offset, std::ios::beg);
            Zip64EOCDRecord rec64;
            if (ifs.read(reinterpret_cast<char*>(&rec64), sizeof(rec64)) && rec64.signature == 0x06064b50) {
                cd_offset = rec64.cd_offset;
                cd_size = rec64.cd_size;
            }
        }
    }

    if (cd_offset >= u_file_size || cd_size > (u_file_size - cd_offset)) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Central Directory offset exceeds file size");
    }

    const uint64_t kMaxCentralDirectoryBytes = 128 * 1024 * 1024; // 128 MB max CD
    if (cd_size > kMaxCentralDirectoryBytes) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Central Directory size exceeds maximum allowed limit");
    }

    // Read Central Directory records
    ifs.seekg(cd_offset, std::ios::beg);
    if (!ifs) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Failed to seek to Central Directory");
    }
    std::vector<uint8_t> cd_buffer(static_cast<size_t>(cd_size));
    ifs.read(reinterpret_cast<char*>(cd_buffer.data()), static_cast<std::streamsize>(cd_size));
    if (ifs.gcount() != static_cast<std::streamsize>(cd_size)) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Incomplete read of Central Directory");
    }

    return InspectBuffer(cd_buffer.data(), cd_buffer.size());
}

Result<ZipArchiveSummary> ZipPreviewer::InspectBuffer(const uint8_t* data, size_t length) {
    ZipArchiveSummary summary;
    const uint32_t kCdHeaderSig = 0x02014b50;
    const size_t kMaxZipEntries = 200000;
    size_t offset = 0;

    while (offset + sizeof(ZipCDHeader) <= length) {
        if (summary.entries.size() >= kMaxZipEntries) {
            summary.is_potential_zip_bomb = true;
            break;
        }

        uint32_t sig = 0;
        std::memcpy(&sig, data + offset, sizeof(sig));
        if (sig != kCdHeaderSig) {
            break;
        }

        ZipCDHeader hdr;
        std::memcpy(&hdr, data + offset, sizeof(ZipCDHeader));
        offset += sizeof(ZipCDHeader);

        uint64_t variable_lengths = static_cast<uint64_t>(hdr.filename_length) +
                                    static_cast<uint64_t>(hdr.extra_length) +
                                    static_cast<uint64_t>(hdr.comment_length);
        if (variable_lengths > (length - offset)) {
            return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Malformed Central Directory variable lengths exceed buffer");
        }

        std::string filename(reinterpret_cast<const char*>(data + offset), hdr.filename_length);
        offset += hdr.filename_length;

        // Parse extra fields (specifically checking for ZIP64 Extended Information Extra Field 0x0001)
        uint64_t uncompressed_size = hdr.uncompressed_size;
        uint64_t compressed_size = hdr.compressed_size;
        const uint8_t* extra_data = data + offset;
        size_t extra_rem = hdr.extra_length;
        offset += hdr.extra_length;
        offset += hdr.comment_length;

        while (extra_rem >= 4) {
            uint16_t header_id = 0;
            uint16_t data_size = 0;
            std::memcpy(&header_id, extra_data, 2);
            std::memcpy(&data_size, extra_data + 2, 2);
            extra_data += 4;
            extra_rem -= 4;

            if (data_size > extra_rem) break;

            if (header_id == 0x0001) { // ZIP64 Extra Field Tag
                size_t p = 0;
                if (hdr.uncompressed_size == 0xFFFFFFFF && p + 8 <= data_size) {
                    std::memcpy(&uncompressed_size, extra_data + p, 8);
                    p += 8;
                }
                if (hdr.compressed_size == 0xFFFFFFFF && p + 8 <= data_size) {
                    std::memcpy(&compressed_size, extra_data + p, 8);
                    p += 8;
                }
            }

            extra_data += data_size;
            extra_rem -= data_size;
        }

        ZipEntryInfo entry;
        entry.file_path = filename;
        entry.uncompressed_size = uncompressed_size;
        entry.compressed_size = compressed_size;
        entry.crc32 = hdr.crc32;
        entry.compression_method = hdr.compression_method;
        entry.is_encrypted = (hdr.flags & 1) != 0;
        entry.is_directory = (!filename.empty() && (filename.back() == '/' || filename.back() == '\\'));
        entry.is_suspicious_path = IsZipSlipPath(filename);

        // Date formatting: DOS date/time format
        uint32_t year = 1980 + ((hdr.last_mod_date >> 9) & 0x7F);
        uint32_t month = (hdr.last_mod_date >> 5) & 0x0F;
        uint32_t day = hdr.last_mod_date & 0x1F;
        uint32_t hour = (hdr.last_mod_time >> 11) & 0x1F;
        uint32_t minute = (hdr.last_mod_time >> 5) & 0x3F;
        char date_buf[32];
        snprintf(date_buf, sizeof(date_buf), "%04u-%02u-%02u %02u:%02u", year, month, day, hour, minute);
        entry.modified_date_str = date_buf;

        if (entry.is_encrypted) summary.has_encrypted_entries = true;
        if (entry.is_suspicious_path) summary.has_zip_slip_risk = true;

        if (entry.is_directory) {
            summary.total_folders++;
        } else {
            summary.total_files++;
            summary.total_uncompressed_bytes += entry.uncompressed_size;
            summary.total_compressed_bytes += entry.compressed_size;
        }

        summary.entries.push_back(std::move(entry));
    }

    // Zip Bomb detection
    if (summary.total_compressed_bytes > 0) {
        double ratio = static_cast<double>(summary.total_uncompressed_bytes) / static_cast<double>(summary.total_compressed_bytes);
        if (ratio > 100.0 && summary.total_uncompressed_bytes > 104857600ULL) { // >100:1 ratio and >100MB
            summary.is_potential_zip_bomb = true;
        }
    }
    if (summary.total_uncompressed_bytes > 1099511627776ULL) { // > 1 TB
        summary.is_potential_zip_bomb = true;
    }

    return Result<ZipArchiveSummary>::Ok(std::move(summary));
}

} // namespace openidm::archive
