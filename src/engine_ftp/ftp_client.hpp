// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — High-Performance Native WinSock2 FTP & FTPS Engine
// Implements RFC 959 (FTP), RFC 2428 (IPv6/EPSV), RFC 3659 (REST/SIZE),
// RFC 4217 (Explicit/Implicit FTPS via Schannel/OpenSSL), and SOCKS5/HTTP CONNECT proxy.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "brisk_engine/http_client.hpp"
#include "brisk_engine/http_download_engine.hpp"
#include "common/types.hpp"

namespace radium::ftp {

struct FtpUrl {
    std::string scheme{"ftp"}; // "ftp" or "ftps"
    std::string host;
    uint16_t port{21};
    std::string user{"anonymous"};
    std::string username{"anonymous"};
    std::string password{"anonymous@example.com"};
    std::string remote_path;
    std::string file_name;

    static Result<FtpUrl> Parse(std::string_view url_str);
};

struct FtpProgress {
    std::string task_id;
    TaskState state{TaskState::kDownloading};
    uint64_t downloaded_bytes{0};
    uint64_t total_bytes{0};
    uint64_t speed_bps{0};
    std::string status_detail;
};

using FtpProgressCallback = std::function<void(const FtpProgress&)>;

struct FtpProxyOptions {
    std::string mode{"direct"}; // "direct", "socks5", "http"
    std::string host;
    uint16_t port{0};
    std::string username;
    std::string password;
    bool try_explicit_tls{false}; // If true on ftp://, try AUTH TLS before falling back
};

class FtpClient {
public:
    FtpClient() = default;
    ~FtpClient() = default;

    // Probes remote FTP/FTPS server for existence, file size, and range/REST capability
    static Result<uint64_t> ProbeFileSize(
        const FtpUrl& url,
        int timeout_ms = 5000,
        bool allow_loopback = false,
        const FtpProxyOptions& proxy_opts = {});

    // Downloads the remote file to target_part_path with EPSV/PASV negotiation, resume validation,
    // progress reporting, dual rate limiting, and cancellation
    static Result<uint64_t> DownloadFile(
        const FtpUrl& url,
        const std::filesystem::path& target_part_path,
        uint64_t resume_offset,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        std::atomic<uint64_t>& task_speed_limit_bps,
        FtpProgressCallback on_progress,
        std::shared_ptr<brisk::TokenBucketLimiter> global_limiter = nullptr,
        std::atomic<uint64_t>* global_speed_limit_bps = nullptr,
        int timeout_ms = 10000,
        bool allow_loopback = false,
        const FtpProxyOptions& proxy_opts = {});
};

} // namespace radium::ftp
