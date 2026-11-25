// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Non-Extracting ZIP & ZIP64 Archive Central Directory Previewer & Security Guard
#include "archive/zip_preview.hpp"
#include "brisk_engine/http_client.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace radium::archive {

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
        uint64_t local_header_offset = hdr.local_header_offset;
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
                if (hdr.local_header_offset == 0xFFFFFFFF && p + 8 <= data_size) {
                    std::memcpy(&local_header_offset, extra_data + p, 8);
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
        entry.local_header_offset = local_header_offset;
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

// ============================================================================
// Minimal RFC 1951 Inflate (Stored, Fixed Huffman, Dynamic Huffman)
// ============================================================================

namespace {

struct BitStream {
    const uint8_t* data;
    size_t len;
    size_t byte_pos{0};
    uint32_t bit_buf{0};
    int bit_cnt{0};

    void Ensure(int n) {
        while (bit_cnt < n && byte_pos < len) {
            bit_buf |= static_cast<uint32_t>(data[byte_pos++]) << bit_cnt;
            bit_cnt += 8;
        }
    }
    bool ReadBits(int n, uint32_t& out) {
        Ensure(n);
        if (bit_cnt < n) return false;
        out = bit_buf & ((1U << n) - 1U);
        bit_buf >>= n;
        bit_cnt -= n;
        return true;
    }
    void AlignByte() {
        int drop = bit_cnt & 7;
        bit_buf >>= drop;
        bit_cnt -= drop;
    }
    bool ReadBytes(uint8_t* dst, size_t n) {
        AlignByte();
        while (n > 0 && bit_cnt >= 8) {
            *dst++ = static_cast<uint8_t>(bit_buf & 0xFF);
            bit_buf >>= 8;
            bit_cnt -= 8;
            --n;
        }
        if (byte_pos + n > len) return false;
        if (n > 0) {
            std::memcpy(dst, data + byte_pos, n);
            byte_pos += n;
        }
        return true;
    }
};

struct HuffTable {
    static constexpr int kMaxBits = 15;
    std::array<int, kMaxBits + 1> count{};
    std::vector<int> symbols;

    bool Build(const int* lengths, int num_syms) {
        count.fill(0);
        for (int i = 0; i < num_syms; ++i) {
            if (lengths[i] < 0 || lengths[i] > kMaxBits) return false;
            count[lengths[i]]++;
        }
        if (count[0] == num_syms) {
            symbols.clear();
            return true;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false;
        }
        std::array<int, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; ++len) {
            offs[len + 1] = offs[len] + count[len];
        }
        symbols.assign(num_syms, 0);
        for (int sym = 0; sym < num_syms; ++sym) {
            if (lengths[sym] != 0) {
                symbols[offs[lengths[sym]]++] = sym;
            }
        }
        return true;
    }

    bool Decode(BitStream& bs, int& out_sym) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            uint32_t bit = 0;
            if (!bs.ReadBits(1, bit)) return false;
            code |= static_cast<int>(bit);
            int cnt = count[len];
            if (code - cnt < first) {
                out_sym = symbols[index + (code - first)];
                return true;
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

static bool InflateRaw(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out, size_t max_out) {
    static constexpr int kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static constexpr int kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static constexpr int kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static constexpr int kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static constexpr int kClOrder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

    BitStream bs{src, src_len};
    out.clear();
    uint32_t bfinal = 0;
    do {
        uint32_t btype = 0;
        if (!bs.ReadBits(1, bfinal) || !bs.ReadBits(2, btype)) return false;
        if (btype == 0) {
            uint8_t hdr[4]{};
            if (!bs.ReadBytes(hdr, 4)) return false;
            uint16_t len = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
            uint16_t nlen = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
            if (static_cast<uint16_t>(len ^ 0xFFFF) != nlen) return false;
            if (out.size() + len > max_out) return false;
            size_t old_sz = out.size();
            out.resize(old_sz + len);
            if (!bs.ReadBytes(out.data() + old_sz, len)) return false;
        } else if (btype == 1 || btype == 2) {
            HuffTable lit_tab, dist_tab;
            if (btype == 1) {
                std::array<int, 288> ll{};
                for (int i = 0; i <= 143; ++i) ll[i] = 8;
                for (int i = 144; i <= 255; ++i) ll[i] = 9;
                for (int i = 256; i <= 279; ++i) ll[i] = 7;
                for (int i = 280; i <= 287; ++i) ll[i] = 8;
                std::array<int, 32> dl{};
                dl.fill(5);
                if (!lit_tab.Build(ll.data(), 288) || !dist_tab.Build(dl.data(), 32)) return false;
            } else {
                uint32_t hlit = 0, hdist = 0, hclen = 0;
                if (!bs.ReadBits(5, hlit) || !bs.ReadBits(5, hdist) || !bs.ReadBits(4, hclen)) return false;
                int nlit = static_cast<int>(hlit) + 257;
                int ndist = static_cast<int>(hdist) + 1;
                int ncl = static_cast<int>(hclen) + 4;
                if (nlit > 286 || ndist > 30) return false;
                std::array<int, 19> cl_lens{};
                for (int i = 0; i < ncl; ++i) {
                    uint32_t v = 0;
                    if (!bs.ReadBits(3, v)) return false;
                    cl_lens[kClOrder[i]] = static_cast<int>(v);
                }
                HuffTable cl_tab;
                if (!cl_tab.Build(cl_lens.data(), 19)) return false;
                std::vector<int> all_lens(nlit + ndist, 0);
                int idx = 0;
                while (idx < nlit + ndist) {
                    int sym = 0;
                    if (!cl_tab.Decode(bs, sym)) return false;
                    if (sym < 16) {
                        all_lens[idx++] = sym;
                    } else {
                        int rep = 0, val = 0;
                        uint32_t ex = 0;
                        if (sym == 16) {
                            if (idx == 0 || !bs.ReadBits(2, ex)) return false;
                            val = all_lens[idx - 1];
                            rep = 3 + static_cast<int>(ex);
                        } else if (sym == 17) {
                            if (!bs.ReadBits(3, ex)) return false;
                            rep = 3 + static_cast<int>(ex);
                        } else {
                            if (!bs.ReadBits(7, ex)) return false;
                            rep = 11 + static_cast<int>(ex);
                        }
                        if (idx + rep > nlit + ndist) return false;
                        for (int r = 0; r < rep; ++r) all_lens[idx++] = val;
                    }
                }
                if (!lit_tab.Build(all_lens.data(), nlit) || !dist_tab.Build(all_lens.data() + nlit, ndist)) return false;
            }

            while (true) {
                int sym = 0;
                if (!lit_tab.Decode(bs, sym)) return false;
                if (sym < 256) {
                    if (out.size() + 1 > max_out) return false;
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li < 0 || li >= 29) return false;
                    uint32_t lex = 0;
                    if (kLenExtra[li] > 0 && !bs.ReadBits(kLenExtra[li], lex)) return false;
                    size_t match_len = static_cast<size_t>(kLenBase[li] + static_cast<int>(lex));
                    int dsym = 0;
                    if (!dist_tab.Decode(bs, dsym) || dsym < 0 || dsym >= 30) return false;
                    uint32_t dex = 0;
                    if (kDistExtra[dsym] > 0 && !bs.ReadBits(kDistExtra[dsym], dex)) return false;
                    size_t dist = static_cast<size_t>(kDistBase[dsym] + static_cast<int>(dex));
                    if (dist == 0 || dist > out.size() || out.size() + match_len > max_out) return false;
                    for (size_t k = 0; k < match_len; ++k) {
                        out.push_back(out[out.size() - dist]);
                    }
                }
            }
        } else {
            return false;
        }
    } while (!bfinal);
    return true;
}

static bool ParseEocdFromTail(
    const uint8_t* tail,
    size_t tail_len,
    uint64_t total_file_size,
    uint64_t& out_cd_offset,
    uint64_t& out_cd_size) {
    if (tail_len < sizeof(ZipEOCD)) return false;
    size_t eocd_pos = SIZE_MAX;
    for (size_t i = tail_len - sizeof(ZipEOCD); ; --i) {
        uint32_t sig = 0;
        std::memcpy(&sig, tail + i, sizeof(sig));
        if (sig == 0x06054b50) {
            ZipEOCD cand;
            std::memcpy(&cand, tail + i, sizeof(cand));
            if (i + sizeof(ZipEOCD) + cand.comment_length == tail_len) {
                eocd_pos = i;
                break;
            }
        }
        if (i == 0) break;
    }
    if (eocd_pos == SIZE_MAX) return false;
    ZipEOCD eocd;
    std::memcpy(&eocd, tail + eocd_pos, sizeof(eocd));
    out_cd_offset = eocd.cd_offset;
    out_cd_size = eocd.cd_size;

    if ((eocd.cd_offset == 0xFFFFFFFF || eocd.cd_size == 0xFFFFFFFF) && eocd_pos >= sizeof(Zip64EOCDLocator)) {
        Zip64EOCDLocator loc;
        std::memcpy(&loc, tail + eocd_pos - sizeof(Zip64EOCDLocator), sizeof(loc));
        if (loc.signature == 0x07064b50 && total_file_size >= tail_len) {
            uint64_t tail_base = total_file_size - tail_len;
            if (loc.eocd_offset >= tail_base && (loc.eocd_offset - tail_base) + sizeof(Zip64EOCDRecord) <= tail_len) {
                Zip64EOCDRecord rec;
                std::memcpy(&rec, tail + (loc.eocd_offset - tail_base), sizeof(rec));
                if (rec.signature == 0x06064b50) {
                    out_cd_offset = rec.cd_offset;
                    out_cd_size = rec.cd_size;
                }
            }
        }
    }
    return true;
}

} // namespace

Result<ZipArchiveSummary> ZipPreviewer::InspectRemoteUrl(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    std::atomic<bool>& cancel_flag,
    bool allow_loopback) {
    uint64_t total_size = 0;
    auto probe_res = brisk::HttpClient::PerformGet(
        url, headers, 0, 0, 15000, allow_loopback, cancel_flag,
        [&](const brisk::HttpResponseMeta& meta) {
            if (meta.status_code == 206 && meta.has_range_total) {
                total_size = meta.range_total;
                return brisk::HeaderAction::kStopSuccess;
            }
            if (meta.status_code == 200 && meta.content_length > 0) {
                total_size = meta.content_length;
                return brisk::HeaderAction::kStopSuccess;
            }
            return brisk::HeaderAction::kAbort;
        },
        [](const uint8_t*, size_t) { return true; });
    if (!probe_res.ok() || total_size < sizeof(ZipEOCD)) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_REMOTE_PROBE", "Remote server did not return valid ZIP Content-Range/size");
    }

    const uint64_t tail_len = std::min<uint64_t>(total_size, 65536ULL + sizeof(ZipEOCD) + sizeof(Zip64EOCDLocator));
    const uint64_t tail_start = total_size - tail_len;
    std::vector<uint8_t> tail_buf;
    auto tail_res = brisk::HttpClient::PerformGet(
        url, headers, tail_start, total_size - 1, 15000, allow_loopback, cancel_flag,
        [](const brisk::HttpResponseMeta& meta) {
            return (meta.status_code == 206 || meta.status_code == 200)
                ? brisk::HeaderAction::kContinue : brisk::HeaderAction::kAbort;
        },
        [&](const uint8_t* data, size_t len) {
            tail_buf.insert(tail_buf.end(), data, data + len);
            return tail_buf.size() <= 262144ULL;
        });
    if (!tail_res.ok() || tail_buf.empty()) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_REMOTE_EOCD", "Failed to fetch remote ZIP EOCD tail");
    }

    uint64_t cd_offset = 0, cd_size = 0;
    if (!ParseEocdFromTail(tail_buf.data(), tail_buf.size(), total_size, cd_offset, cd_size) ||
        cd_offset >= total_size || cd_size > total_size - cd_offset || cd_size > 128ULL * 1024ULL * 1024ULL) {
        return Result<ZipArchiveSummary>::Err("ERR_ZIP_CORRUPT", "Invalid remote ZIP Central Directory bounds");
    }

    std::vector<uint8_t> cd_buf;
    if (cd_offset >= tail_start && (cd_offset + cd_size) <= total_size) {
        size_t rel = static_cast<size_t>(cd_offset - tail_start);
        cd_buf.assign(tail_buf.begin() + rel, tail_buf.begin() + rel + static_cast<size_t>(cd_size));
    } else {
        auto cd_res = brisk::HttpClient::PerformGet(
            url, headers, cd_offset, cd_offset + cd_size - 1, 20000, allow_loopback, cancel_flag,
            [](const brisk::HttpResponseMeta& meta) {
                return (meta.status_code == 206 || meta.status_code == 200)
                    ? brisk::HeaderAction::kContinue : brisk::HeaderAction::kAbort;
            },
            [&](const uint8_t* data, size_t len) {
                cd_buf.insert(cd_buf.end(), data, data + len);
                return cd_buf.size() <= 128ULL * 1024ULL * 1024ULL;
            });
        if (!cd_res.ok() || cd_buf.size() < cd_size) {
            return Result<ZipArchiveSummary>::Err("ERR_ZIP_REMOTE_CD", "Failed to fetch remote ZIP Central Directory");
        }
    }
    return InspectBuffer(cd_buf.data(), cd_buf.size());
}

Result<std::filesystem::path> ZipPreviewer::DownloadAndExtractRemoteEntry(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const ZipEntryInfo& entry,
    const std::filesystem::path& dest_dir,
    std::atomic<bool>& cancel_flag,
    bool allow_loopback) {
    if (entry.is_suspicious_path || IsZipSlipPath(entry.file_path)) {
        return Result<std::filesystem::path>::Err("ERR_ZIP_SLIP", "Blocked unsafe ZIP entry path: " + entry.file_path);
    }
    if (entry.is_encrypted) {
        return Result<std::filesystem::path>::Err("ERR_ZIP_ENCRYPTED", "Encrypted ZIP entry requires local password extraction");
    }
    if (entry.compression_method != 0 && entry.compression_method != 8) {
        return Result<std::filesystem::path>::Err("ERR_ZIP_METHOD", "Unsupported ZIP compression method: " + std::to_string(entry.compression_method));
    }

    const uint64_t fetch_len = 30ULL + entry.file_path.size() + 1024ULL + entry.compressed_size;
    std::vector<uint8_t> raw;
    auto res = brisk::HttpClient::PerformGet(
        url, headers, entry.local_header_offset, entry.local_header_offset + fetch_len - 1,
        30000, allow_loopback, cancel_flag,
        [](const brisk::HttpResponseMeta& meta) {
            return (meta.status_code == 206 || meta.status_code == 200)
                ? brisk::HeaderAction::kContinue : brisk::HeaderAction::kAbort;
        },
        [&](const uint8_t* data, size_t len) {
            raw.insert(raw.end(), data, data + len);
            return raw.size() <= fetch_len + 65536ULL;
        });
    if (!res.ok() || raw.size() < 30) {
        return Result<std::filesystem::path>::Err("ERR_ZIP_REMOTE_ENTRY", "Failed to download remote ZIP entry");
    }

    uint32_t lfh_sig = 0;
    std::memcpy(&lfh_sig, raw.data(), 4);
    if (lfh_sig != 0x04034b50) {
        return Result<std::filesystem::path>::Err("ERR_ZIP_CORRUPT", "Invalid Local File Header signature");
    }
    uint16_t fn_len = 0, ex_len = 0;
    std::memcpy(&fn_len, raw.data() + 26, 2);
    std::memcpy(&ex_len, raw.data() + 28, 2);
    size_t payload_off = 30ULL + fn_len + ex_len;
    if (payload_off + entry.compressed_size > raw.size()) {
        return Result<std::filesystem::path>::Err("ERR_ZIP_CORRUPT", "Truncated remote ZIP entry payload");
    }

    std::vector<uint8_t> out_bytes;
    if (entry.compression_method == 0) {
        out_bytes.assign(raw.begin() + payload_off, raw.begin() + payload_off + static_cast<size_t>(entry.compressed_size));
    } else {
        if (!InflateRaw(raw.data() + payload_off, static_cast<size_t>(entry.compressed_size),
                        out_bytes, std::max<size_t>(static_cast<size_t>(entry.uncompressed_size), 64ULL * 1024ULL * 1024ULL))) {
            return Result<std::filesystem::path>::Err("ERR_ZIP_DEFLATE", "Failed to decompress remote Deflate entry");
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    std::string safe_name = PathSanitizer::SanitizeFileName(std::filesystem::path(entry.file_path).filename().string());
    if (safe_name.empty()) safe_name = "extracted_entry.bin";
    auto out_path = dest_dir / safe_name;
    std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return Result<std::filesystem::path>::Err("ERR_STORAGE_OPEN", "Failed to create output file");
    }
    ofs.write(reinterpret_cast<const char*>(out_bytes.data()), static_cast<std::streamsize>(out_bytes.size()));
    ofs.close();
    return Result<std::filesystem::path>::Ok(out_path);
}

// ============================================================================
// ArchiveExtractor (MDM crates/extract port)
// ============================================================================

ArchiveFormat ArchiveExtractor::DetectFormat(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return ArchiveFormat::kUnknown;
    uint8_t head[512]{};
    ifs.read(reinterpret_cast<char*>(head), sizeof(head));
    size_t n = static_cast<size_t>(ifs.gcount());
    std::string name = path.filename().string();
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (n >= 4 && ((head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4) ||
                   (head[0] == 'P' && head[1] == 'K' && head[2] == 5 && head[3] == 6))) {
        return ArchiveFormat::kZip;
    }
    if (n >= 6 && head[0] == '7' && head[1] == 'z' && head[2] == 0xBC && head[3] == 0xAF && head[4] == 0x27 && head[5] == 0x1C) {
        return ArchiveFormat::kSevenZip;
    }
    if (n >= 6 && head[0] == 'R' && head[1] == 'a' && head[2] == 'r' && head[3] == '!' && head[4] == 0x1A && head[5] == 0x07) {
        return ArchiveFormat::kRar;
    }
    if (n >= 2 && head[0] == 0x1F && head[1] == 0x8B) {
        if (name.ends_with(".tar.gz") || name.ends_with(".tgz")) return ArchiveFormat::kTarGz;
        return ArchiveFormat::kGz;
    }
    if (n >= 6 && head[0] == 0xFD && head[1] == '7' && head[2] == 'z' && head[3] == 'X' && head[4] == 'Z' && head[5] == 0x00) {
        if (name.ends_with(".tar.xz") || name.ends_with(".txz")) return ArchiveFormat::kTarXz;
        return ArchiveFormat::kXz;
    }
    if (n >= 262 && std::memcmp(head + 257, "ustar", 5) == 0) {
        return ArchiveFormat::kTar;
    }
    return ArchiveFormat::kUnknown;
}

Result<std::filesystem::path> ArchiveExtractor::ExtractArchive(
    const std::filesystem::path& archive_path,
    const std::filesystem::path& dest_dir,
    SubfolderPolicy subfolder_policy,
    uint64_t max_total_bytes,
    size_t max_files) {
    ArchiveFormat fmt = DetectFormat(archive_path);
    if (fmt == ArchiveFormat::kUnknown) {
        return Result<std::filesystem::path>::Err("ERR_ARCHIVE_UNKNOWN", "Unrecognized archive format");
    }

    if (fmt == ArchiveFormat::kZip) {
        auto summary = ZipPreviewer::InspectArchive(archive_path);
        if (!summary.ok()) return Result<std::filesystem::path>::Err(summary.error->code, summary.error->message);
        if (summary.value->has_zip_slip_risk) {
            return Result<std::filesystem::path>::Err("ERR_ZIP_SLIP", "Archive contains unsafe traversal paths");
        }
        if (summary.value->is_potential_zip_bomb || summary.value->total_uncompressed_bytes > max_total_bytes ||
            summary.value->entries.size() > max_files) {
            return Result<std::filesystem::path>::Err("ERR_ZIP_BOMB", "Archive exceeds safe extraction size or file count limits");
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    auto staging = dest_dir / (".radium_extract_" + std::to_string(std::hash<std::string>{}(archive_path.string())));
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);

    auto cleanup_staging = [&]() {
        std::error_code rm_ec;
        std::filesystem::remove_all(staging, rm_ec);
    };

    bool extracted_ok = false;
    if (fmt == ArchiveFormat::kZip) {
        auto summary = ZipPreviewer::InspectArchive(archive_path);
        std::ifstream ifs(archive_path, std::ios::binary);
        if (summary.ok() && ifs.is_open() && !summary.value->has_encrypted_entries) {
            bool all_supported = true;
            uint64_t total_written = 0;
            for (const auto& entry : summary.value->entries) {
                if (entry.is_directory) continue;
                if (entry.compression_method != 0 && entry.compression_method != 8) {
                    all_supported = false;
                    break;
                }
                ifs.seekg(static_cast<std::streamoff>(entry.local_header_offset), std::ios::beg);
                uint8_t lfh[30]{};
                ifs.read(reinterpret_cast<char*>(lfh), 30);
                if (ifs.gcount() != 30 || std::memcmp(lfh, "\x50\x4b\x03\x04", 4) != 0) {
                    all_supported = false;
                    break;
                }
                uint16_t fn_len = 0, ex_len = 0;
                std::memcpy(&fn_len, lfh + 26, 2);
                std::memcpy(&ex_len, lfh + 28, 2);
                ifs.seekg(static_cast<std::streamoff>(fn_len + ex_len), std::ios::cur);
                std::vector<uint8_t> comp(static_cast<size_t>(entry.compressed_size));
                if (!comp.empty()) {
                    ifs.read(reinterpret_cast<char*>(comp.data()), static_cast<std::streamsize>(comp.size()));
                    if (static_cast<size_t>(ifs.gcount()) != comp.size()) {
                        all_supported = false;
                        break;
                    }
                }
                std::vector<uint8_t> plain;
                if (entry.compression_method == 0) {
                    plain = std::move(comp);
                } else {
                    if (!InflateRaw(comp.data(), comp.size(), plain,
                                    static_cast<size_t>(std::min<uint64_t>(max_total_bytes - total_written, 512ULL * 1024ULL * 1024ULL)))) {
                        all_supported = false;
                        break;
                    }
                }
                total_written += plain.size();
                if (total_written > max_total_bytes) {
                    cleanup_staging();
                    return Result<std::filesystem::path>::Err("ERR_ZIP_BOMB", "Extracted size exceeds limit");
                }
                std::filesystem::path rel(entry.file_path);
                std::filesystem::path safe_rel;
                for (const auto& part : rel) {
                    std::string seg = PathSanitizer::SanitizeFileName(part.string());
                    if (!seg.empty() && seg != "." && seg != "..") safe_rel /= seg;
                }
                if (safe_rel.empty()) continue;
                auto target = staging / safe_rel;
                std::filesystem::create_directories(target.parent_path(), ec);
                std::ofstream ofs(target, std::ios::binary | std::ios::trunc);
                if (!ofs.is_open()) {
                    all_supported = false;
                    break;
                }
                if (!plain.empty()) {
                    ofs.write(reinterpret_cast<const char*>(plain.data()), static_cast<std::streamsize>(plain.size()));
                }
            }
            extracted_ok = all_supported;
        }
    }

#ifdef _WIN32
    if (!extracted_ok) {
        std::filesystem::path tool = PathSanitizer::FindWorkerBinary("7z", PathSanitizer::GetExecutableDir());
        std::wstring cmd;
        if (!tool.empty() && std::filesystem::exists(tool, ec)) {
            cmd = L"\"" + tool.wstring() + L"\" x -y -o\"" + staging.wstring() + L"\" \"" + archive_path.wstring() + L"\"";
        } else if (fmt == ArchiveFormat::kTar || fmt == ArchiveFormat::kTarGz || fmt == ArchiveFormat::kTarXz || fmt == ArchiveFormat::kZip) {
            cmd = L"tar.exe -xf \"" + archive_path.wstring() + L"\" -C \"" + staging.wstring() + L"\"";
        }
        if (cmd.empty()) {
            cleanup_staging();
            return Result<std::filesystem::path>::Err("ERR_ARCHIVE_TOOL_MISSING", "7-Zip is required to extract 7z/RAR/encrypted archives");
        }
        HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
        if (hJob) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
            if (hJob) AssignProcessToJobObject(hJob, pi.hProcess);
            ResumeThread(pi.hThread);
            WaitForSingleObject(pi.hProcess, 120000);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            extracted_ok = (code == 0);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (hJob) CloseHandle(hJob);
    }
#endif

    if (!extracted_ok) {
        cleanup_staging();
        return Result<std::filesystem::path>::Err("ERR_ARCHIVE_EXTRACT_FAILED", "Archive extraction failed");
    }

    size_t top_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(staging, ec)) {
        (void)entry;
        ++top_count;
    }

    bool use_subfolder = (subfolder_policy == SubfolderPolicy::kAlways) ||
                         (subfolder_policy == SubfolderPolicy::kSmart && top_count > 1);
    std::filesystem::path final_root = use_subfolder
        ? (dest_dir / PathSanitizer::SanitizeFileName(archive_path.stem().string()))
        : dest_dir;
    std::filesystem::create_directories(final_root, ec);

    for (const auto& entry : std::filesystem::directory_iterator(staging, ec)) {
        auto dst = final_root / entry.path().filename();
        std::filesystem::rename(entry.path(), dst, ec);
        if (ec) {
            std::filesystem::copy(entry.path(), dst, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    cleanup_staging();
    return Result<std::filesystem::path>::Ok(final_root);
}

} // namespace radium::archive
