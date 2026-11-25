// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace radium::packs {

// ============================================================================
// Debrid Providers & Cloud Torrent Resolution (MDM crates/debrid port)
// ============================================================================

enum class DebridServiceKind {
    kRealDebrid = 0,
    kAllDebrid,
    kPremiumize,
    kTorBox,
    kMegaDebrid
};

struct DebridUnlockedLink {
    std::string direct_url;
    std::string file_name;
    uint64_t file_size_bytes{0};
};

class DebridManager {
public:
    static std::string ServiceName(DebridServiceKind kind);
    static std::string TokenPageUrl(DebridServiceKind kind);
    static std::string ApiBaseUrl(DebridServiceKind kind);
    static bool SupportsMagnets(DebridServiceKind kind);

    static Result<DebridUnlockedLink> UnlockLink(
        DebridServiceKind kind,
        const std::string& api_token,
        const std::string& host_url,
        std::atomic<bool>& cancel_flag);

    static Result<std::string> AddMagnet(
        DebridServiceKind kind,
        const std::string& api_token,
        const std::string& magnet_url,
        std::atomic<bool>& cancel_flag);
};

// ============================================================================
// Click'n'Load 2 Listener (127.0.0.1:9666, AES-128-CBC without JS eval)
// ============================================================================

struct Cnl2Package {
    std::string name;
    std::vector<std::string> passwords;
    std::vector<std::string> links;
    std::string source_site;
};

class Cnl2Server {
public:
    static constexpr uint16_t kDefaultPort = 9666;
    static constexpr size_t kMaxBodyBytes = 256 * 1024;
    static constexpr size_t kMaxLinks = 500;

    static std::optional<std::vector<uint8_t>> ExtractKeyFromJk(const std::string& jk_js);
    static Result<std::vector<std::string>> DecryptCrypted2(
        const std::string& crypted_b64,
        const std::string& jk_js);

    using OnPackageCallback = std::function<bool(const Cnl2Package&)>;

    Cnl2Server() = default;
    ~Cnl2Server();

    bool Start(OnPackageCallback cb, uint16_t port = kDefaultPort);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

private:
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    uintptr_t m_listen_sock{static_cast<uintptr_t>(-1)};
};

// ============================================================================
// Specialized URL Packs (GD3 github_pack, arxiv_pack, huggingface_pack,
// wrapped_link_pack, ed2k_pack)
// ============================================================================

struct ArxivDownloadBundle {
    std::string arxiv_id;
    std::string pdf_url;
    std::string latex_source_url;
    std::string bibtex_url;
};

struct HuggingFaceTarget {
    std::string owner;
    std::string repo;
    std::string revision{"main"};
    std::string file_path;
};

struct Ed2kLinkInfo {
    std::string file_name;
    uint64_t file_size{0};
    std::string md4_hash;
};

class SpecializedUrlPacks {
public:
    // Legacy wrapped links: thunder://, flashget://, qqdl://
    static Result<std::string> DecodeWrappedLink(const std::string& wrapped_url);

    // GitHub proxy mirror racing & failover
    static std::vector<std::string> BuildGitHubMirrorCandidates(const std::string& url);
    static std::string ResolveFastestGitHubMirror(
        const std::string& url,
        std::atomic<bool>& cancel_flag);

    // arXiv PDF + LaTeX + BibTeX
    static std::optional<std::string> ParseArxivId(const std::string& url);
    static std::optional<ArxivDownloadBundle> BuildArxivDownloads(const std::string& url);

    // Hugging Face commit-SHA pinned repo/mirror URLs
    static std::optional<HuggingFaceTarget> ParseHuggingFaceUrl(const std::string& url);
    static std::string BuildHuggingFaceFileUrl(
        const HuggingFaceTarget& target,
        const std::string& commit_sha = "",
        bool use_mirror = false);

    // eD2k link parser
    static Result<Ed2kLinkInfo> ParseEd2kLink(const std::string& ed2k_url);
};

} // namespace radium::packs
