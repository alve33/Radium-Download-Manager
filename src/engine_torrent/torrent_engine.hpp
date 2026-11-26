// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — BitTorrent Engine Coordinator
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "brisk_engine/http_download_engine.hpp"
#include "common/types.hpp"
#include "engine_torrent/torrent_parser.hpp"

namespace radium::torrent {

struct TorrentProgress {
    std::string task_id;
    TaskState state{TaskState::kDownloading};
    uint64_t downloaded_bytes{0};
    // Includes proportionally weighted blocks in pieces that have not passed
    // their hash check yet. This is transient UI progress, not resume state.
    uint64_t received_bytes{0};
    uint64_t total_bytes{0};
    uint64_t speed_bps{0};
    uint64_t uploaded_bytes{0};
    uint64_t upload_speed_bps{0};
    uint32_t completed_pieces{0};
    uint32_t total_pieces{0};
    std::string status_detail;
    std::string fastresume_bitfield;
    int64_t fastresume_mtime{0};
    uint64_t fastresume_filesize{0};
    std::vector<std::string> torrent_file_paths;
    std::vector<uint8_t> torrent_file_selection;
};

struct TrackerEndpointInfo {
    std::string url;
    int tier{0};
    std::string status{"Active"}; // Active, Announcing, Error, Disabled
    uint32_t seeders{0};
    uint32_t leechers{0};
    std::string last_error;
};

struct PeerDiagnosticsInfo {
    std::string endpoint; // ip:port
    std::string ip;
    uint16_t port{0};
    std::string client_name;
    uint64_t download_bps{0};
    uint64_t upload_bps{0};
    double progress{0.0}; // 0.0 - 1.0
    bool is_choked{true};
    bool is_snubbed{false};
};

struct TorrentSwarmDetails {
    std::string task_id;
    std::vector<TrackerEndpointInfo> trackers;
    std::vector<PeerDiagnosticsInfo> peers;
    uint32_t active_peer_count{0};
    uint32_t total_seeds{0};
    uint32_t total_leechers{0};
    bool sequential_mode{false};
};

using TorrentProgressCallback = std::function<void(const TorrentProgress&)>;
// Invoked after BEP 9 metadata has been decoded and before storage is allocated.
// The callback may update TorrentInfo::files[].selected and can reject the task.
using TorrentMetadataSelectionCallback = std::function<Result<void>(TorrentInfo&)>;

class TorrentEngine {
public:
    TorrentEngine() = default;
    ~TorrentEngine() = default;

    static Result<void> ExecuteTorrentDownload(
        TaskModel& task,
        TorrentInfo& torrent_info,
        std::atomic<bool>& pause_requested,
        std::atomic<bool>& cancel_requested,
        std::atomic<uint64_t>& speed_limit_bps,
        TorrentProgressCallback on_progress,
        std::shared_ptr<brisk::TokenBucketLimiter> global_limiter = nullptr,
        std::atomic<uint64_t>* global_speed_limit_bps = nullptr,
        uint32_t max_peers = 32,
        bool seed_after_completion = false,
        double max_seed_ratio = 1.0,
        uint32_t max_seed_time_sec = 0,
        TorrentMetadataSelectionCallback on_metadata = nullptr,
        bool sequential_download = false,
        std::atomic<int>* task_priority = nullptr,
        std::atomic<uint64_t>* priority_speed_limit_bps = nullptr);

    // Swarm inspection and dynamic control
    static Result<void> AddTrackersToTask(const std::string& task_id, const std::vector<std::string>& new_trackers);
    static Result<TorrentSwarmDetails> GetSwarmDetails(const std::string& task_id);
    static Result<void> SetSequentialMode(const std::string& task_id, bool enable);
};

// IP Blocklist (MDM blocklist.rs port: PeerGuardian P2P & eMule .dat)
class TorrentIpBlocklist {
public:
    static TorrentIpBlocklist& Global();
    size_t LoadFromText(const std::string& text);
    void Clear();
    bool IsBlocked(const std::string& ip_str) const;
    size_t RangeCount() const;

private:
    struct V4Range { uint32_t start; uint32_t end; };
    mutable std::mutex m_mu;
    std::vector<V4Range> m_v4_ranges;
};

// RSS 2.0 / Atom Torrent Auto-Downloader (MDM rss.rs port)
struct RssTorrentArticle {
    std::string id;
    std::string title;
    std::string torrent_url;
    std::string date;
    uint64_t size_bytes{0};
};

struct RssEpisode {
    uint32_t season{0};
    uint32_t episode{0};
};

class RssTorrentFeedParser {
public:
    static std::vector<RssTorrentArticle> ParseFeedXml(const std::string& xml);
    static std::optional<RssEpisode> ExtractEpisode(const std::string& title);
    static bool MatchesFilter(
        const std::string& title,
        const std::string& include_words,
        const std::string& exclude_words,
        const std::string& episode_filter);
};

} // namespace radium::torrent
