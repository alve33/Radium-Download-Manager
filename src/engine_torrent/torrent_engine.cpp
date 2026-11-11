// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Flux — BitTorrent Engine Coordinator
// Concurrent peer swarm, BEP 9 metadata exchange, BEP 15 UDP trackers, DHT peer discovery, Seeding & Restart Recovery
#include "engine_torrent/torrent_engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <set>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "engine_torrent/dht_client.hpp"
#include "engine_torrent/multi_file_storage.hpp"
#include "engine_torrent/peer_wire_protocol.hpp"
#include "engine_torrent/piece_manager.hpp"
#include "engine_torrent/tracker_client.hpp"
#include "engine_torrent/torrent_rate_meter.hpp"

namespace openidm::torrent {

namespace {

#ifdef _WIN32
struct WinSockInit {
    WinSockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinSockInit() {
        WSACleanup();
    }
};

void EnsureWinSock() {
    static WinSockInit s_init;
}
#else
void EnsureWinSock() {}
#endif

struct UniqueSocket {
    SOCKET s{INVALID_SOCKET};
    UniqueSocket() = default;
    explicit UniqueSocket(SOCKET sock) : s(sock) {}
    ~UniqueSocket() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
        }
    }
    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;
    UniqueSocket(UniqueSocket&& o) noexcept : s(o.s) { o.s = INVALID_SOCKET; }
    UniqueSocket& operator=(UniqueSocket&& o) noexcept {
        if (s != INVALID_SOCKET) closesocket(s);
        s = o.s;
        o.s = INVALID_SOCKET;
        return *this;
    }
};

static bool SendAll(SOCKET s, const uint8_t* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        int r = send(s, reinterpret_cast<const char*>(data + total_sent), static_cast<int>(len - total_sent), 0);
        if (r <= 0) return false;
        total_sent += static_cast<size_t>(r);
    }
    return true;
}

// Swarm registry for live task tracking
struct ActiveTorrentSession {
    std::string task_id;
    std::shared_ptr<PieceManager> piece_mgr;
    std::shared_ptr<MultiFileStorage> storage;
    std::vector<std::string> announce_urls;
    std::mutex session_lock;
    std::vector<TrackerEndpointInfo> trackers;
    std::vector<PeerDiagnosticsInfo> active_peers;
    std::deque<PeerEndpoint> peer_pool;
    bool sequential_mode{false};
    bool is_paused{false};
};

static std::mutex s_sessions_mutex;
static std::map<std::string, std::shared_ptr<ActiveTorrentSession>> s_active_sessions;

static std::shared_ptr<ActiveTorrentSession> GetOrCreateSession(const std::string& task_id, const std::vector<std::string>& announce_urls) {
    std::lock_guard<std::mutex> lock(s_sessions_mutex);
    auto it = s_active_sessions.find(task_id);
    if (it != s_active_sessions.end()) {
        it->second->is_paused = false;
        return it->second;
    }

    auto session = std::make_shared<ActiveTorrentSession>();
    session->task_id = task_id;
    session->announce_urls = announce_urls;
    for (size_t i = 0; i < announce_urls.size(); ++i) {
        TrackerEndpointInfo ti;
        ti.url = announce_urls[i];
        ti.tier = static_cast<int>(i);
        ti.status = "Active";
        session->trackers.push_back(std::move(ti));
    }
    s_active_sessions[task_id] = session;
    return session;
}

static void UnregisterSession(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(s_sessions_mutex);
    s_active_sessions.erase(task_id);
}


} // namespace

Result<void> TorrentEngine::ExecuteTorrentDownload(
    TaskModel& task,
    TorrentInfo& torrent_info,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    std::atomic<uint64_t>& speed_limit_bps,
    TorrentProgressCallback on_progress,
    std::shared_ptr<brisk::TokenBucketLimiter> global_limiter,
    std::atomic<uint64_t>* global_speed_limit_bps,
    uint32_t max_peers,
    bool seed_after_completion,
    double max_seed_ratio,
    uint32_t max_seed_time_sec,
    TorrentMetadataSelectionCallback on_metadata,
    bool sequential_download) {

    EnsureWinSock();

    std::string client_peer_id = PeerWireProtocol::GeneratePeerId();

    // 0. Magnet link: exchange metadata via BEP 9 (ut_metadata) if needed
    if (torrent_info.is_magnet && (torrent_info.total_length == 0 || torrent_info.num_pieces() == 0)) {
        task.status_detail = "Fetching magnet metadata via BEP 9...";
        if (on_progress) {
            TorrentProgress tp;
            tp.task_id = task.task_id;
            tp.state = TaskState::kDownloading;
            tp.status_detail = task.status_detail;
            on_progress(tp);
        }

        std::vector<std::string> ann_urls = torrent_info.announce_list;
        if (ann_urls.empty() && !torrent_info.announce.empty()) ann_urls.push_back(torrent_info.announce);

        // Tracker announce for magnet
        std::vector<PeerEndpoint> magnet_peers;
        if (!ann_urls.empty()) {
            auto tr_res = TrackerClient::AnnounceAny(ann_urls, torrent_info.info_hash_raw, client_peer_id, 6881, 0, 0, 0, "started", 4000);
            if (tr_res.ok()) magnet_peers = tr_res.value->peers;
        }

        // Query DHT bootstrap nodes if tracker gave no peers
        if (magnet_peers.empty()) {
            std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes = {
                {std::string("dht.transmissionbt.com"), static_cast<uint16_t>(6881)},
                {std::string("dht.libtorrent.org"), static_cast<uint16_t>(25401)},
                {std::string("router.bittorrent.com"), static_cast<uint16_t>(6881)},
                {std::string("router.utorrent.com"), static_cast<uint16_t>(6881)}
            };
            magnet_peers = DhtClient::QueryDhtNodes(bootstrap_nodes, torrent_info.info_hash_raw, client_peer_id, 3000);
        }

        std::string fetched_metadata_bencode;
        for (const auto& peer : magnet_peers) {
            if (cancel_requested.load() || pause_requested.load()) break;

            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) continue;
            UniqueSocket sock(s);

#ifdef _WIN32
            DWORD to_ms = 4000;
            setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
            setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
#else
            struct timeval tv;
            tv.tv_sec = 4;
            tv.tv_usec = 0;
            setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

            sockaddr_in saddr{};
            saddr.sin_family = AF_INET;
            saddr.sin_port = htons(peer.port);
            if (inet_pton(AF_INET, peer.ip.c_str(), &saddr.sin_addr) <= 0) continue;
            if (connect(sock.s, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == SOCKET_ERROR) continue;

            auto hs = PeerWireProtocol::CreateHandshake(torrent_info.info_hash_raw, client_peer_id);
            if (send(sock.s, reinterpret_cast<const char*>(hs.data()), static_cast<int>(hs.size()), 0) <= 0) continue;

            uint8_t peer_hs[68];
            int hs_read = 0;
            while (hs_read < 68) {
                int r = recv(sock.s, reinterpret_cast<char*>(peer_hs + hs_read), 68 - hs_read, 0);
                if (r <= 0) break;
                hs_read += r;
            }
            if (!PeerWireProtocol::VerifyHandshake(peer_hs, hs_read, torrent_info.info_hash_raw)) continue;

            // Send BEP 10 extended handshake advertising ut_metadata support
            auto ext_hs = PeerWireProtocol::CreateExtendedHandshake(1, 0);
            send(sock.s, reinterpret_cast<const char*>(ext_hs.data()), static_cast<int>(ext_hs.size()), 0);

            uint8_t peer_ut_metadata_id = 0;
            uint64_t metadata_size = 0;
            std::vector<uint8_t> recv_buf;
            recv_buf.reserve(65536);
            std::vector<uint8_t> metadata_bytes;

            while (!cancel_requested.load() && !pause_requested.load()) {
                uint8_t temp[16384];
                int r = recv(sock.s, reinterpret_cast<char*>(temp), sizeof(temp), 0);
                if (r <= 0) break;
                recv_buf.insert(recv_buf.end(), temp, temp + r);

                bool got_all_metadata = false;
                while (recv_buf.size() >= 4) {
                    uint32_t payload_len = PeerWireProtocol::ReadUint32BE(recv_buf.data());
                    if (payload_len == 0) {
                        recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4);
                        continue;
                    }
                    if (payload_len > 2 * 1024 * 1024) {
                        break;
                    }
                    if (recv_buf.size() < 4 + payload_len) break;

                    auto msg_opt = PeerWireProtocol::ParseMessage(recv_buf.data(), 4 + payload_len);
                    recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4 + payload_len);
                    if (!msg_opt) continue;

                    if (msg_opt->id == BtMessageId::kExtended) {
                        if (msg_opt->ext_id == kExtHandshakeId) {
                            // Parse extension handshake dictionary
                            std::string_view hs_str(reinterpret_cast<const char*>(msg_opt->payload.data()), msg_opt->payload.size());
                            auto dec_hs = BencodeParser::Decode(hs_str);
                            if (dec_hs.ok() && dec_hs.value->is_dict()) {
                                const auto& hsd = dec_hs.value.value();
                                if (hsd.contains("m") && hsd["m"].is_dict() && hsd["m"].contains("ut_metadata") && hsd["m"]["ut_metadata"].is_int()) {
                                    peer_ut_metadata_id = static_cast<uint8_t>(hsd["m"]["ut_metadata"].as_int());
                                }
                                if (hsd.contains("metadata_size") && hsd["metadata_size"].is_int()) {
                                    metadata_size = static_cast<uint64_t>(hsd["metadata_size"].as_int());
                                }
                            }
                            if (peer_ut_metadata_id > 0 && metadata_size > 0 && metadata_size <= 10 * 1024 * 1024) {
                                metadata_bytes.resize(static_cast<size_t>(metadata_size));
                                // Request piece 0 (or all pieces)
                                uint32_t num_pieces = static_cast<uint32_t>((metadata_size + 16383) / 16384);
                                for (uint32_t pi = 0; pi < num_pieces; ++pi) {
                                    auto ut_req = PeerWireProtocol::CreateUtMetadataRequest(peer_ut_metadata_id, pi);
                                    send(sock.s, reinterpret_cast<const char*>(ut_req.data()), static_cast<int>(ut_req.size()), 0);
                                }
                            }
                        } else if (msg_opt->ext_id == 1 || msg_opt->ext_id == peer_ut_metadata_id) {
                            // ut_metadata data message
                            std::string_view msg_sv(reinterpret_cast<const char*>(msg_opt->payload.data()), msg_opt->payload.size());
                            size_t pos = 0;
                            if (TorrentParser::GetBencodeValueRange(msg_sv, pos)) {
                                auto d_res = BencodeParser::Decode(msg_sv.substr(0, pos));
                                if (d_res.ok() && d_res.value->is_dict()) {
                                    const auto& md = d_res.value.value();
                                    if (md.contains("msg_type") && md["msg_type"].as_int() == kUtMetadataData) {
                                        uint32_t piece_idx = static_cast<uint32_t>(md.contains("piece") ? md["piece"].as_int() : 0);
                                        const uint8_t* raw_data = msg_opt->payload.data() + pos;
                                        size_t raw_len = msg_opt->payload.size() - pos;
                                        size_t offset = piece_idx * 16384;
                                        if (offset + raw_len <= metadata_bytes.size()) {
                                            std::memcpy(metadata_bytes.data() + offset, raw_data, raw_len);
                                        }
                                        if (piece_idx + 1 >= (metadata_size + 16383) / 16384) {
                                            got_all_metadata = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (got_all_metadata) break;
            }

            if (!metadata_bytes.empty()) {
                auto parsed_res = TorrentParser::ParseInfoDictionary(
                    std::string_view(reinterpret_cast<const char*>(metadata_bytes.data()), metadata_bytes.size()),
                    torrent_info.announce,
                    torrent_info.announce_list);
                if (parsed_res.ok()) {
                    torrent_info = parsed_res.value.value();
                    break;
                }
            }
        }

        if (torrent_info.total_length == 0 || torrent_info.num_pieces() == 0) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_MAGNET_METADATA_UNSUPPORTED";
            task.error_message = "Failed to retrieve magnet metainfo from available peers or DHT";
            task.status_detail = "Failed: Magnet metadata retrieval failed";
            return Result<void>::Err("ERR_MAGNET_METADATA_UNSUPPORTED", task.error_message);
        }

        if (on_metadata) {
            task.status_detail = "Select files to download...";
            if (on_progress) {
                TorrentProgress tp;
                tp.task_id = task.task_id;
                tp.state = TaskState::kDownloading;
                tp.total_bytes = torrent_info.selected_bytes();
                tp.total_pieces = static_cast<uint32_t>(torrent_info.num_pieces());
                tp.status_detail = task.status_detail;
                on_progress(tp);
            }
            auto selection_res = on_metadata(torrent_info);
            if (!selection_res.ok()) {
                task.state = TaskState::kPaused;
                task.pause_reason = PauseReason::kUser;
                task.error_code = selection_res.error->code;
                task.error_message = selection_res.error->message;
                task.status_detail = "Paused: " + selection_res.error->message;
                return selection_res;
            }
            // Persist the post-metadata selection on the task before any files are
            // allocated. The controller stores this model at worker completion,
            // allowing a restart to reconstruct the same selective torrent mask.
            task.torrent_file_paths.clear();
            task.torrent_file_selection.clear();
            task.torrent_file_paths.reserve(torrent_info.files.size());
            task.torrent_file_selection.reserve(torrent_info.files.size());
            for (const auto& file : torrent_info.files) {
                task.torrent_file_paths.push_back(file.relative_path.generic_string());
                task.torrent_file_selection.push_back(file.selected ? 1 : 0);
            }
        }
    }

    // 1. Metainfo Pre-flight Validation
    if (torrent_info.total_length == 0 || torrent_info.num_pieces() == 0 || torrent_info.piece_length == 0) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_TORRENT_MALFORMED";
        task.error_message = "Invalid torrent metainfo: zero length or zero pieces";
        task.status_detail = "Failed: Invalid metainfo";
        OPENIDM_LOG_ERROR("TorrentEngine", "Malformed metainfo for task " + task.task_id);
        return Result<void>::Err("ERR_TORRENT_MALFORMED", "Invalid torrent metainfo: zero length or zero pieces");
    }

    if (!torrent_info.has_selected_files()) {
        task.state = TaskState::kFailed;
        task.error_code = "ERR_TORRENT_NO_FILES_SELECTED";
        task.error_message = "At least one torrent file must be selected";
        task.status_detail = "Failed: No files selected";
        return Result<void>::Err(task.error_code, task.error_message);
    }
    uint64_t total_bytes = torrent_info.selected_bytes();
    task.total_bytes = total_bytes;
    task.total_segments = 0;
    task.state = TaskState::kDownloading;
    task.status_detail = "Allocating storage files...";

    if (on_progress) {
        TorrentProgress tp;
        tp.task_id = task.task_id;
        tp.state = task.state;
        tp.total_bytes = total_bytes;
        tp.total_pieces = static_cast<uint32_t>(torrent_info.num_pieces());
        tp.status_detail = task.status_detail;
        on_progress(tp);
    }

    // 2. Storage Setup (aria2 MultiDiskAdaptor) & Stateful Session Retention
    auto announce_list = torrent_info.announce_list.empty() ? std::vector<std::string>{torrent_info.announce} : torrent_info.announce_list;
    auto active_session = GetOrCreateSession(task.task_id, announce_list);
    active_session->sequential_mode = sequential_download;

    std::shared_ptr<MultiFileStorage> storage = active_session->storage;
    if (!storage) {
        storage = std::make_shared<MultiFileStorage>();
        auto stor_res = storage->Initialize(task.save_dir, torrent_info);
        if (!stor_res.ok()) {
            task.state = TaskState::kFailed;
            task.error_code = stor_res.error->code;
            task.error_message = stor_res.error->message;
            task.status_detail = "Failed: " + stor_res.error->message;
            return stor_res;
        }
        storage->AllocateFiles();
        active_session->storage = storage;
    }

    // 3. Piece & Block Scheduler (aria2 PieceStorage)
    std::shared_ptr<PieceManager> piece_mgr = active_session->piece_mgr;
    bool needs_disk_check = false;
    if (!piece_mgr) {
        piece_mgr = std::make_shared<PieceManager>(torrent_info, storage);
        active_session->piece_mgr = piece_mgr;
        needs_disk_check = true;
    }
    if (sequential_download) piece_mgr->SetSequentialMode(true, true);
    task.total_segments = static_cast<uint32_t>(piece_mgr->RequiredPieces());

    // Fastresume Bitfield & Bypass Full Disk SHA-1 Check on Resume:
    // Check if files are untouched using modification timestamp and file size
    if (needs_disk_check) {
        bool fastresume_applied = false;
        if (!task.torrent_fastresume_bitfield.empty()) {
            // Check disk file modification time and size
            std::error_code ec;
            uint64_t current_disk_size = 0;
            int64_t latest_mtime = 0;
            for (const auto& f : torrent_info.files) {
                auto p = std::filesystem::path(task.save_dir) / f.relative_path;
                if (std::filesystem::exists(p, ec)) {
                    current_disk_size += std::filesystem::file_size(p, ec);
                    auto ftime = std::filesystem::last_write_time(p, ec);
                    auto s = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
                    latest_mtime = std::max(latest_mtime, static_cast<int64_t>(s));
                }
            }
            if (torrent_info.files.empty()) {
                auto p = std::filesystem::path(task.save_dir) / PathSanitizer::SanitizeFileName(torrent_info.name);
                if (std::filesystem::exists(p, ec)) {
                    current_disk_size = std::filesystem::file_size(p, ec);
                    auto ftime = std::filesystem::last_write_time(p, ec);
                    latest_mtime = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
                }
            }

            // If fastresume timestamp and file size match disk, bypass full hash checking!
            if (task.torrent_fastresume_filesize > 0 && current_disk_size == task.torrent_fastresume_filesize &&
                latest_mtime <= task.torrent_fastresume_mtime) {
                size_t imported = piece_mgr->ImportBitfieldHex(task.torrent_fastresume_bitfield);
                if (imported > 0) {
                    fastresume_applied = true;
                    task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
                    task.downloaded_bytes = piece_mgr->DownloadedBytes();
                    OPENIDM_LOG_INFO("TorrentEngine", "Fastresume verified instantaneously without disk re-hashing (" +
                                                      std::to_string(imported) + " pieces restored) for task " + task.task_id);
                }
            }
        }

        if (!fastresume_applied) {
            size_t recovered = piece_mgr->CheckExistingPieces(false);
            if (recovered > 0) {
                task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
                task.downloaded_bytes = piece_mgr->DownloadedBytes();
                OPENIDM_LOG_INFO("TorrentEngine", "Recovered " + std::to_string(recovered) + " pieces from existing disk data for task " + task.task_id);
            }
        }
    }

    OPENIDM_LOG_INFO("TorrentEngine", "Starting BitTorrent download for task " + task.task_id +
                                      " (hash: " + torrent_info.info_hash_hex +
                                      ", pieces: " + std::to_string(torrent_info.num_pieces()) + ")");

    // If already fully completed from on-disk pieces
    if (piece_mgr->IsAllCompleted()) {
        task.state = TaskState::kCompleted;
        task.downloaded_bytes = total_bytes;
        task.completed_segments = task.total_segments;
        task.status_detail = "Completed (BitTorrent)";
        if (on_progress) {
            TorrentProgress tp;
            tp.task_id = task.task_id;
            tp.state = TaskState::kCompleted;
            tp.downloaded_bytes = total_bytes;
            tp.total_bytes = total_bytes;
            tp.completed_pieces = task.total_segments;
            tp.total_pieces = task.total_segments;
            tp.status_detail = task.status_detail;
            on_progress(tp);
        }
        return Result<void>::Ok();
    }

    // 4. Tracker Announcement & DHT Peer Discovery
    task.status_detail = "Connecting to trackers...";
    if (on_progress) {
        TorrentProgress tp;
        tp.task_id = task.task_id;
        tp.state = task.state;
        tp.total_bytes = total_bytes;
        tp.total_pieces = static_cast<uint32_t>(torrent_info.num_pieces());
        tp.status_detail = task.status_detail;
        on_progress(tp);
    }

    std::vector<std::string> announce_urls = torrent_info.announce_list;
    if (announce_urls.empty() && !torrent_info.announce.empty()) {
        announce_urls.push_back(torrent_info.announce);
    }

    std::vector<PeerEndpoint> peer_swarm;
    {
        std::lock_guard<std::mutex> slk(active_session->session_lock);
        if (!active_session->peer_pool.empty()) {
            peer_swarm.assign(active_session->peer_pool.begin(), active_session->peer_pool.end());
            OPENIDM_LOG_INFO("TorrentEngine", "Reusing " + std::to_string(peer_swarm.size()) +
                                              " discovered peers from preserved session for task " + task.task_id);
        }
    }

    if (peer_swarm.empty() && !announce_urls.empty()) {
        auto tr_res = TrackerClient::AnnounceAny(
            announce_urls,
            torrent_info.info_hash_raw,
            client_peer_id,
            6881,
            task.uploaded_bytes,
            task.downloaded_bytes,
            total_bytes - task.downloaded_bytes,
            "started",
            4000);

        if (tr_res.ok()) {
            peer_swarm = tr_res.value->peers;
        } else {
            OPENIDM_LOG_WARN("TorrentEngine", "All announce trackers failed for task " + task.task_id + ": " + tr_res.error->message);
        }
    }

    // Secondary DHT Discovery if peer swarm is small
    if (peer_swarm.size() < 20) {
        std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes = {
            {std::string("router.bittorrent.com"), static_cast<uint16_t>(6881)},
            {std::string("dht.transmissionbt.com"), static_cast<uint16_t>(6881)},
            {std::string("router.utorrent.com"), static_cast<uint16_t>(6881)}
        };
        auto dht_peers = DhtClient::QueryDhtNodes(bootstrap_nodes, torrent_info.info_hash_raw, client_peer_id, 1000);
        for (const auto& dp : dht_peers) {
            if (std::none_of(peer_swarm.begin(), peer_swarm.end(), [&](const PeerEndpoint& p) { return p.ip == dp.ip && p.port == dp.port; })) {
                peer_swarm.push_back(dp);
            }
        }
    }

    bool ever_discovered_peer = !peer_swarm.empty();

    task.status_detail = peer_swarm.empty()
        ? "No peers yet; continuing tracker/DHT discovery..."
        : "Connected to tracker; swarm has " + std::to_string(peer_swarm.size()) + " peers...";
    if (on_progress) {
        TorrentProgress tp;
        tp.task_id = task.task_id;
        tp.state = task.state;
        tp.total_bytes = total_bytes;
        tp.completed_pieces = task.completed_segments;
        tp.total_pieces = task.total_segments;
        tp.status_detail = task.status_detail;
        on_progress(tp);
    }

    auto start_time = std::chrono::steady_clock::now();
    TorrentRateMeter download_rate_meter;
    TorrentRateMeter upload_rate_meter;
    download_rate_meter.Reset(piece_mgr->ReceivedBytes(), start_time);
    upload_rate_meter.Reset(task.uploaded_bytes, start_time);
    std::atomic<uint64_t> uploaded_payload_bytes{task.uploaded_bytes};
    std::atomic<uint64_t> live_download_bps{0};
    std::atomic<uint64_t> live_upload_bps{0};
    auto task_limiter = std::make_shared<brisk::TokenBucketLimiter>();

    // Incoming peer support is intentionally kept in the same engine and
    // storage context as outbound peers. The listener advertises only pieces
    // that are both verified and fully selected; selective torrents must not
    // serve zero-filled ranges belonging to unselected files.
    std::mutex upload_mutex;
    std::mutex progress_emit_mutex;
    auto last_payload_progress_emit = start_time - std::chrono::milliseconds(100);
    std::atomic<bool> incoming_stop{false};
    std::thread incoming_listener([&]() {
        SOCKET listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET) return;

        UniqueSocket listener(listen_socket);
        int reuse = 1;
        setsockopt(listener.s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(6881);
        if (bind(listener.s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) == SOCKET_ERROR ||
            listen(listener.s, 16) == SOCKET_ERROR) {
            OPENIDM_LOG_WARN("TorrentEngine", "Incoming peer listener unavailable on port 6881");
            return;
        }

        auto recv_exact = [](SOCKET peer, uint8_t* dst, size_t length) {
            size_t received = 0;
            while (received < length) {
                int r = recv(peer, reinterpret_cast<char*>(dst + received), static_cast<int>(length - received), 0);
                if (r <= 0) return false;
                received += static_cast<size_t>(r);
            }
            return true;
        };

        while (!incoming_stop.load() && !cancel_requested.load() && !pause_requested.load()) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listener.s, &read_set);
            timeval wait_time{0, 250000};
#ifdef _WIN32
            int ready = select(0, &read_set, nullptr, nullptr, &wait_time);
#else
            int ready = select(listener.s + 1, &read_set, nullptr, nullptr, &wait_time);
#endif
            if (ready <= 0) continue;

            SOCKET peer_socket = accept(listener.s, nullptr, nullptr);
            if (peer_socket == INVALID_SOCKET) continue;
            UniqueSocket peer(peer_socket);
#ifdef _WIN32
            DWORD timeout_ms = 4000;
            setsockopt(peer.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(peer.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#endif

            uint8_t peer_handshake[68]{};
            if (!recv_exact(peer.s, peer_handshake, sizeof(peer_handshake)) ||
                !PeerWireProtocol::VerifyHandshake(peer_handshake, sizeof(peer_handshake), torrent_info.info_hash_raw)) {
                continue;
            }
            auto response_handshake = PeerWireProtocol::CreateHandshake(torrent_info.info_hash_raw, client_peer_id);
            if (!SendAll(peer.s, response_handshake.data(), response_handshake.size())) continue;

            auto verified = piece_mgr->HavePieces();
            std::vector<bool> upload_have(verified.size(), false);
            for (size_t piece_index = 0; piece_index < verified.size(); ++piece_index) {
                upload_have[piece_index] = verified[piece_index] &&
                    torrent_info.selected_bytes_in_piece(piece_index) == torrent_info.get_piece_size(piece_index);
            }
            auto bitfield = PeerWireProtocol::CreateBitfield(upload_have);
            if (!bitfield.empty() && !SendAll(peer.s, bitfield.data(), bitfield.size())) continue;
            auto unchoke = PeerWireProtocol::CreateUnchoke();
            if (!SendAll(peer.s, unchoke.data(), unchoke.size())) continue;

            while (!incoming_stop.load() && !cancel_requested.load() && !pause_requested.load()) {
                uint8_t length_buf[4]{};
                if (!recv_exact(peer.s, length_buf, sizeof(length_buf))) break;
                uint32_t payload_length = PeerWireProtocol::ReadUint32BE(length_buf);
                if (payload_length == 0) continue;
                if (payload_length > (1u << 20)) break;
                std::vector<uint8_t> message(4 + payload_length);
                std::copy(length_buf, length_buf + 4, message.begin());
                if (!recv_exact(peer.s, message.data() + 4, payload_length)) break;
                auto parsed = PeerWireProtocol::ParseMessage(message.data(), message.size());
                if (!parsed || parsed->id != BtMessageId::kRequest) continue;

                const auto& request = *parsed;
                if (request.piece_index >= upload_have.size() || !upload_have[request.piece_index] ||
                    request.block_length == 0 || request.block_length > 32768) continue;
                uint64_t piece_size = torrent_info.get_piece_size(request.piece_index);
                if (static_cast<uint64_t>(request.block_begin) + request.block_length > piece_size) continue;

                uint64_t offset = static_cast<uint64_t>(request.piece_index) * torrent_info.piece_length + request.block_begin;
                std::vector<uint8_t> block(request.block_length);
                auto read_result = storage->ReadBytes(offset, block.data(), block.size());
                if (!read_result.ok()) continue;
                auto piece_message = PeerWireProtocol::CreatePiece(request.piece_index, request.block_begin, block.data(), block.size());
                if (!SendAll(peer.s, piece_message.data(), piece_message.size())) break;

                uint64_t uploaded = 0;
                uint64_t upload_speed = 0;
                {
                    std::lock_guard<std::mutex> metric_lock(upload_mutex);
                    uploaded = uploaded_payload_bytes.fetch_add(block.size(), std::memory_order_relaxed) + block.size();
                    task.upload_speed_bps = upload_rate_meter.Update(uploaded);
                    live_upload_bps.store(task.upload_speed_bps, std::memory_order_relaxed);
                    upload_speed = task.upload_speed_bps;
                }
                if (on_progress) {
                    TorrentProgress progress;
                    progress.task_id = task.task_id;
                    progress.state = task.state;
                    progress.downloaded_bytes = task.downloaded_bytes;
                    progress.total_bytes = total_bytes;
                    progress.speed_bps = live_download_bps.load(std::memory_order_relaxed);
                    progress.uploaded_bytes = uploaded;
                    progress.upload_speed_bps = upload_speed;
                    progress.completed_pieces = task.completed_segments;
                    progress.total_pieces = task.total_segments;
                    progress.status_detail = "Uploading verified pieces";
                    on_progress(progress);
                }
            }
        }
    });

    struct IncomingListenerGuard {
        std::atomic<bool>& stop;
        std::thread& worker;
        ~IncomingListenerGuard() {
            stop.store(true);
            if (worker.joinable()) worker.join();
        }
    } incoming_guard{incoming_stop, incoming_listener};

    // Tit-for-Tat Choking & Unchoking State (BEP 3)
    struct PeerSession {
        uint64_t peer_id_num = 0;
        std::atomic<bool> is_choked_by_us{true};
        std::atomic<uint64_t> downloaded_recent{0};
    };
    std::mutex session_mutex;
    std::vector<std::shared_ptr<PeerSession>> active_peer_sessions;
    std::atomic<uint64_t> next_peer_session_id{1};

    // Peer connection worker lambda
    std::mutex swarm_mutex;
    std::atomic<uint32_t> active_connections{0};
    uint32_t connection_limit = std::max<uint32_t>(1, std::min<uint32_t>(max_peers, 64));

    auto peer_worker = [&](PeerEndpoint peer) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return;
        UniqueSocket sock(s);

#ifdef _WIN32
        DWORD to_ms = 4000;
        setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
        setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to_ms), sizeof(to_ms));
#else
        struct timeval tv;
        tv.tv_sec = 4;
        tv.tv_usec = 0;
        setsockopt(sock.s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock.s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(peer.port);
        if (inet_pton(AF_INET, peer.ip.c_str(), &saddr.sin_addr) <= 0) return;
        if (connect(sock.s, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == SOCKET_ERROR) return;

        auto hs = PeerWireProtocol::CreateHandshake(torrent_info.info_hash_raw, client_peer_id);
        if (!SendAll(sock.s, hs.data(), hs.size())) return;

        uint8_t peer_hs[68];
        int hs_read = 0;
        while (hs_read < 68) {
            int r = recv(sock.s, reinterpret_cast<char*>(peer_hs + hs_read), 68 - hs_read, 0);
            if (r <= 0) break;
            hs_read += r;
        }

        std::string remote_peer_id;
        if (!PeerWireProtocol::VerifyHandshake(peer_hs, hs_read, torrent_info.info_hash_raw, &remote_peer_id)) return;

        // Register with Tit-for-Tat Choking Coordinator
        auto session = std::make_shared<PeerSession>();
        session->peer_id_num = next_peer_session_id++;
        bool initially_unchoked = false;
        {
            std::lock_guard<std::mutex> slk(session_mutex);
            // If we have fewer than 4 peers, unchoke immediately; otherwise choke until rechoke cycle
            if (active_peer_sessions.size() < 4) {
                session->is_choked_by_us.store(false);
                initially_unchoked = true;
            } else {
                session->is_choked_by_us.store(true);
                initially_unchoked = false;
            }
            active_peer_sessions.push_back(session);
        }

        // Clean up session upon peer exit
        struct SessionGuard {
            std::mutex& mtx;
            std::vector<std::shared_ptr<PeerSession>>& list;
            std::shared_ptr<PeerSession> sess;
            ~SessionGuard() {
                std::lock_guard<std::mutex> lk(mtx);
                auto it = std::find(list.begin(), list.end(), sess);
                if (it != list.end()) list.erase(it);
            }
        } session_guard{session_mutex, active_peer_sessions, session};

        // Exchange interested & initial choking state
        auto interested = PeerWireProtocol::CreateInterested();
        SendAll(sock.s, interested.data(), interested.size());
        if (initially_unchoked) {
            auto unchoke_msg = PeerWireProtocol::CreateUnchoke();
            SendAll(sock.s, unchoke_msg.data(), unchoke_msg.size());
        } else {
            auto choke_msg = PeerWireProtocol::CreateChoke();
            SendAll(sock.s, choke_msg.data(), choke_msg.size());
        }

        auto my_have = piece_mgr->HavePieces();
        if (std::any_of(my_have.begin(), my_have.end(), [](bool b) { return b; })) {
            auto bf = PeerWireProtocol::CreateBitfield(my_have);
            SendAll(sock.s, bf.data(), bf.size());
        }

        std::vector<bool> peer_have(torrent_info.num_pieces(), false);
        bool peer_choked_us = true;
        std::vector<uint8_t> recv_buf;
        recv_buf.reserve(65536);
        std::vector<BlockRequest> in_flight_requests;

        active_connections++;

        while (!cancel_requested.load() && !pause_requested.load() && !piece_mgr->IsAllCompleted()) {
            uint8_t temp[16384];
            int r = recv(sock.s, reinterpret_cast<char*>(temp), sizeof(temp), 0);
            if (r <= 0) break;

            if (speed_limit_bps.load(std::memory_order_relaxed) > 0) {
                task_limiter->Consume(static_cast<size_t>(r), speed_limit_bps, cancel_requested);
            }
            if (global_limiter && global_speed_limit_bps && global_speed_limit_bps->load(std::memory_order_relaxed) > 0) {
                global_limiter->Consume(static_cast<size_t>(r), *global_speed_limit_bps, cancel_requested);
            }

            recv_buf.insert(recv_buf.end(), temp, temp + r);

            while (recv_buf.size() >= 4) {
                uint32_t payload_len = PeerWireProtocol::ReadUint32BE(recv_buf.data());
                if (payload_len == 0) {
                    recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4);
                    continue;
                }
                if (payload_len > 2 * 1024 * 1024) {
                    break;
                }
                if (recv_buf.size() < 4 + payload_len) break;

                auto msg_opt = PeerWireProtocol::ParseMessage(recv_buf.data(), 4 + payload_len);
                recv_buf.erase(recv_buf.begin(), recv_buf.begin() + 4 + payload_len);
                if (!msg_opt) continue;
                const auto& msg = *msg_opt;

                if (msg.id == BtMessageId::kChoke) {
                    peer_choked_us = true;
                } else if (msg.id == BtMessageId::kUnchoke) {
                    peer_choked_us = false;
                } else if (msg.id == BtMessageId::kHaveAll) {
                    std::fill(peer_have.begin(), peer_have.end(), true);
                    piece_mgr->UpdatePeerAvailability(peer_have, true);
                } else if (msg.id == BtMessageId::kHaveNone) {
                    std::fill(peer_have.begin(), peer_have.end(), false);
                } else if (msg.id == BtMessageId::kHave) {
                    if (msg.piece_index < peer_have.size()) {
                        peer_have[msg.piece_index] = true;
                        piece_mgr->OnPeerHave(msg.piece_index);
                    }
                } else if (msg.id == BtMessageId::kBitfield) {
                    for (size_t i = 0; i < peer_have.size(); ++i) {
                        size_t byte_idx = i / 8;
                        size_t bit_idx = 7 - (i % 8);
                        if (byte_idx < msg.payload.size()) {
                            peer_have[i] = (msg.payload[byte_idx] & (1 << bit_idx)) != 0;
                        }
                    }
                    piece_mgr->UpdatePeerAvailability(peer_have, true);
                } else if (msg.id == BtMessageId::kPiece) {
                    auto fit = std::find_if(in_flight_requests.begin(), in_flight_requests.end(),
                        [&](const BlockRequest& br) {
                            return br.piece_index == msg.piece_index && br.begin == msg.block_begin;
                        });
                    if (fit != in_flight_requests.end()) {
                        in_flight_requests.erase(fit);
                    }

                    bool verified = false;
                    bool corrupt = false;
                    session->downloaded_recent.fetch_add(msg.payload.size(), std::memory_order_relaxed);
                    piece_mgr->ProcessBlock(msg.piece_index, msg.block_begin, msg.payload.data(), msg.payload.size(), verified, corrupt);

                    if (verified) {
                        auto have_msg = PeerWireProtocol::CreateHave(msg.piece_index);
                        send(sock.s, reinterpret_cast<const char*>(have_msg.data()), static_cast<int>(have_msg.size()), 0);
                    }

                    // Report received blocks immediately so the task does not
                    // sit at 0% while the first (possibly large) piece is being
                    // assembled. Incomplete bytes remain transient; resume
                    // recovery still trusts only hash-verified pieces.
                    const uint64_t received_bytes = piece_mgr->ReceivedBytes();
                    live_download_bps.store(download_rate_meter.Update(received_bytes), std::memory_order_relaxed);
                    const uint32_t completed_now = static_cast<uint32_t>(piece_mgr->CompletedPieces());
                    const std::string status_detail = "Downloading pieces: " + std::to_string(completed_now) +
                                         " / " + std::to_string(task.total_segments) +
                                         " verified; " + std::to_string(received_bytes / (1024 * 1024)) +
                                         " MiB received (peers: " + std::to_string(active_connections.load()) + ")";

                    bool emit_progress = false;
                    if (on_progress) {
                        std::lock_guard<std::mutex> emit_lock(progress_emit_mutex);
                        const auto emit_now = std::chrono::steady_clock::now();
                        if (emit_now - last_payload_progress_emit >= std::chrono::milliseconds(100)) {
                            last_payload_progress_emit = emit_now;
                            emit_progress = true;
                        }
                    }
                    if (on_progress && emit_progress) {
                        TorrentProgress tp;
                        tp.task_id = task.task_id;
                        tp.state = TaskState::kDownloading;
                        tp.downloaded_bytes = piece_mgr->DownloadedBytes();
                        tp.received_bytes = received_bytes;
                        tp.total_bytes = total_bytes;
                        tp.completed_pieces = completed_now;
                        tp.total_pieces = task.total_segments;
                        tp.speed_bps = live_download_bps.load(std::memory_order_relaxed);
                        tp.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
                        tp.upload_speed_bps = live_upload_bps.load(std::memory_order_relaxed);
                        tp.status_detail = status_detail;
                        on_progress(tp);
                    }
                } else if (msg.id == BtMessageId::kRequest) {
                    // Upload piece block to peer only if peer is unchoked by us
                    if (session->is_choked_by_us.load(std::memory_order_relaxed)) {
                        // Peer is choked, ignore or reject upload request
                        continue;
                    }
                    if (msg.piece_index < torrent_info.num_pieces() &&
                        msg.block_length > 0 && msg.block_length <= 32768) {
                        auto cur_have = piece_mgr->HavePieces();
                        if (msg.piece_index < cur_have.size() && cur_have[msg.piece_index]) {
                            uint64_t piece_len = torrent_info.get_piece_size(msg.piece_index);
                            if (static_cast<uint64_t>(msg.block_begin) + msg.block_length <= piece_len) {
                                uint64_t torrent_offset = static_cast<uint64_t>(msg.piece_index) * torrent_info.piece_length + msg.block_begin;
                                std::vector<uint8_t> block_buf(msg.block_length);
                                auto r_res = storage->ReadBytes(torrent_offset, block_buf.data(), msg.block_length);
                                if (r_res.ok()) {
                                    auto p_msg = PeerWireProtocol::CreatePiece(msg.piece_index, msg.block_begin, block_buf.data(), msg.block_length);
                                    if (SendAll(sock.s, p_msg.data(), p_msg.size())) {
                                        std::lock_guard<std::mutex> metric_lock(upload_mutex);
                                        const uint64_t uploaded = uploaded_payload_bytes.fetch_add(msg.block_length, std::memory_order_relaxed) + msg.block_length;
                                        task.upload_speed_bps = upload_rate_meter.Update(uploaded);
                                        live_upload_bps.store(task.upload_speed_bps, std::memory_order_relaxed);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!peer_choked_us && !piece_mgr->IsAllCompleted()) {
                size_t slots = (kDefaultMaxOutstandingRequests > in_flight_requests.size())
                    ? (kDefaultMaxOutstandingRequests - in_flight_requests.size()) : 0;
                if (slots > 0) {
                    auto reqs = piece_mgr->GetNextRequestsForPeer(peer_have, slots);
                    for (const auto& rq : reqs) {
                        auto req_bytes = PeerWireProtocol::CreateRequest(rq.piece_index, rq.begin, rq.length);
                        if (SendAll(sock.s, req_bytes.data(), req_bytes.size())) {
                            in_flight_requests.push_back(rq);
                        } else {
                            piece_mgr->CancelInFlightRequests({rq});
                            break;
                        }
                    }
                }
            }
        }

        if (!in_flight_requests.empty()) {
            piece_mgr->CancelInFlightRequests(in_flight_requests);
            in_flight_requests.clear();
        }

        active_connections--;
    };

    // 5. Dynamic Peer Swarm Dispatcher Loop
    // Continually launches workers up to connection_limit, rotating through peer_swarm
    // and re-querying trackers/DHT if needed, until complete, paused, or canceled.
    std::vector<std::thread> workers;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::atomic<bool> dispatcher_stop{false};
    std::deque<PeerEndpoint> peer_queue(peer_swarm.begin(), peer_swarm.end());
    std::set<std::string> active_endpoints;
    auto last_announce_time = std::chrono::steady_clock::now();
    const auto discovery_deadline = last_announce_time + std::chrono::seconds(8);

    auto swarm_worker = [&]() {
        while (!dispatcher_stop.load() && !cancel_requested.load() && !pause_requested.load() && !piece_mgr->IsAllCompleted()) {
            PeerEndpoint peer;
            {
                std::unique_lock<std::mutex> lk(queue_mutex);
                queue_cv.wait_for(lk, std::chrono::milliseconds(250), [&]() {
                    return dispatcher_stop.load() || cancel_requested.load() || pause_requested.load() || piece_mgr->IsAllCompleted() || !peer_queue.empty();
                });
                if (dispatcher_stop.load() || cancel_requested.load() || pause_requested.load() || piece_mgr->IsAllCompleted()) break;
                if (peer_queue.empty()) continue;
                peer = peer_queue.front();
                peer_queue.pop_front();
                std::string ep_key = peer.ip + ":" + std::to_string(peer.port);
                if (active_endpoints.count(ep_key)) {
                    continue;
                }
                active_endpoints.insert(ep_key);
            }

            peer_worker(peer);

            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                std::string ep_key = peer.ip + ":" + std::to_string(peer.port);
                active_endpoints.erase(ep_key);
            }

            // Brief pause before pulling next peer to avoid socket thrashing
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    };

    size_t pool_size = std::min<size_t>(connection_limit, std::max<size_t>(4, peer_swarm.size()));
    for (size_t i = 0; i < pool_size; ++i) {
        workers.emplace_back(swarm_worker);
    }

    // Monitor loop: if all pieces complete, or user pauses/cancels, break
    auto last_rechoke_time = std::chrono::steady_clock::now();
    auto last_optimistic_time = std::chrono::steady_clock::now();
    auto last_rate_update = start_time;
    uint64_t optimistic_peer_id = 0;

    while (!cancel_requested.load() && !pause_requested.load() && !piece_mgr->IsAllCompleted()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        auto stats_now = std::chrono::steady_clock::now();
        if (stats_now - last_rate_update >= TorrentRateMeter::kMinimumSampleInterval) {
            last_rate_update = stats_now;
            const uint64_t received_now = piece_mgr->ReceivedBytes();
            live_download_bps.store(download_rate_meter.Update(received_now, stats_now), std::memory_order_relaxed);
            uint64_t uploaded_now = 0;
            uint64_t upload_rate_now = 0;
            {
                std::lock_guard<std::mutex> metric_lock(upload_mutex);
                uploaded_now = uploaded_payload_bytes.load(std::memory_order_relaxed);
                upload_rate_now = upload_rate_meter.Update(uploaded_now, stats_now);
                task.upload_speed_bps = upload_rate_now;
            }
            live_upload_bps.store(upload_rate_now, std::memory_order_relaxed);
            if (on_progress) {
                TorrentProgress tp;
                tp.task_id = task.task_id;
                tp.state = TaskState::kDownloading;
                tp.downloaded_bytes = piece_mgr->DownloadedBytes();
                tp.received_bytes = received_now;
                tp.total_bytes = total_bytes;
                tp.speed_bps = live_download_bps.load(std::memory_order_relaxed);
                tp.uploaded_bytes = uploaded_now;
                tp.upload_speed_bps = upload_rate_now;
                tp.completed_pieces = static_cast<uint32_t>(piece_mgr->CompletedPieces());
                tp.total_pieces = task.total_segments;
                tp.status_detail = "Downloading torrent payload";
                on_progress(tp);
            }
        }

        // Tit-for-Tat Choking & Unchoking Evaluation (BEP 3)
        // 10s cycle: unchoke top 4 fastest peers, choke others (except optimistic)
        // 30s cycle: pick 1 random peer for optimistic unchoke
        auto loop_now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(loop_now - last_optimistic_time).count() >= 30) {
            last_optimistic_time = loop_now;
            std::lock_guard<std::mutex> slk(session_mutex);
            if (!active_peer_sessions.empty()) {
                size_t rand_idx = static_cast<size_t>(rand()) % active_peer_sessions.size();
                optimistic_peer_id = active_peer_sessions[rand_idx]->peer_id_num;
            }
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(loop_now - last_rechoke_time).count() >= 10) {
            last_rechoke_time = loop_now;
            std::lock_guard<std::mutex> slk(session_mutex);
            if (!active_peer_sessions.empty()) {
                // Sort active peers by recent download volume descending
                std::vector<std::shared_ptr<PeerSession>> sorted_peers = active_peer_sessions;
                std::sort(sorted_peers.begin(), sorted_peers.end(), [](const auto& a, const auto& b) {
                    return a->downloaded_recent.load(std::memory_order_relaxed) > b->downloaded_recent.load(std::memory_order_relaxed);
                });

                for (size_t i = 0; i < sorted_peers.size(); ++i) {
                    bool should_unchoke = (i < 4) || (sorted_peers[i]->peer_id_num == optimistic_peer_id);
                    sorted_peers[i]->is_choked_by_us.store(!should_unchoke, std::memory_order_relaxed);
                    // Reset recent counter for rolling window
                    sorted_peers[i]->downloaded_recent.store(0, std::memory_order_relaxed);
                }
            }
        }

        // Keep workers alive while discovery replenishes a dead initial peer batch.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_announce_time).count() >= 1) {
            last_announce_time = now;
            std::vector<PeerEndpoint> discovered;
            if (!announce_urls.empty()) {
                auto tr_res = TrackerClient::AnnounceAny(
                    announce_urls,
                    torrent_info.info_hash_raw,
                    client_peer_id,
                    6881,
                    uploaded_payload_bytes.load(std::memory_order_relaxed),
                    task.downloaded_bytes,
                    total_bytes - task.downloaded_bytes,
                    "",
                    1000);
                if (tr_res.ok()) {
                    for (const auto& p : tr_res.value->peers) {
                        discovered.push_back(p);
                    }
                }
            }
            bool queue_empty = false;
            {
                std::lock_guard<std::mutex> lk(queue_mutex);
                queue_empty = peer_queue.empty();
            }
            if (discovered.empty() && queue_empty && active_connections.load() == 0) {
                std::vector<std::pair<std::string, uint16_t>> bootstrap_nodes = {
                    {"router.bittorrent.com", static_cast<uint16_t>(6881)},
                    {"dht.transmissionbt.com", static_cast<uint16_t>(6881)},
                    {"router.utorrent.com", static_cast<uint16_t>(6881)}
                };
                discovered = DhtClient::QueryDhtNodes(bootstrap_nodes, torrent_info.info_hash_raw, client_peer_id, 500);
            }
            if (!discovered.empty()) {
                ever_discovered_peer = true;
                std::lock_guard<std::mutex> lk(queue_mutex);
                for (const auto& p : discovered) {
                    std::string ep_key = p.ip + ":" + std::to_string(p.port);
                    if (!active_endpoints.count(ep_key) &&
                        std::none_of(peer_queue.begin(), peer_queue.end(), [&](const PeerEndpoint& queued) {
                            return queued.ip == p.ip && queued.port == p.port;
                        })) {
                        peer_queue.push_back(p);
                    }
                }
                queue_cv.notify_all();
            }
        }

        // Only fail after the bounded discovery window; a temporarily empty queue
        // is recoverable because the waiting workers remain available.
        bool any_active = (active_connections.load() > 0);
        {
            std::lock_guard<std::mutex> lk(queue_mutex);
            if (now >= discovery_deadline && !any_active && peer_queue.empty()) {
                break;
            }
        }
    }

    dispatcher_stop.store(true);
    queue_cv.notify_all();
    for (auto& w : workers) {
        if (w.joinable()) w.join();
    }

    if (cancel_requested.load() || pause_requested.load() || !piece_mgr->IsAllCompleted() || !seed_after_completion) {
        incoming_stop.store(true);
        if (incoming_listener.joinable()) incoming_listener.join();
    }

    if (cancel_requested.load()) {
        task.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
        task.downloaded_bytes = piece_mgr->DownloadedBytes();
        task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
        task.speed_bps = 0;
        task.upload_speed_bps = 0;
        task.state = TaskState::kCanceled;
        task.status_detail = "Canceled";
        if (storage) storage->FlushHandles();
        UnregisterSession(task.task_id);
        return Result<void>::Err("ERR_CANCELED", "BitTorrent task canceled by user");
    }

    if (pause_requested.load()) {
        task.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
        task.downloaded_bytes = piece_mgr->DownloadedBytes();
        task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
        task.speed_bps = 0;
        task.upload_speed_bps = 0;
        task.state = TaskState::kPaused;
        task.status_detail = "Paused";

        // Non-destructive Pause & In-Memory Session Preservation:
        // Do NOT destroy the ActiveTorrentSession or wipe peer lists!
        // Preserve piece_mgr, storage, and discovered peers so resume is instantaneous.
        {
            std::lock_guard<std::mutex> slk(active_session->session_lock);
            active_session->is_paused = true;
            active_session->peer_pool.clear();
            std::set<std::string> pool_seen;
            for (const auto& p : peer_queue) {
                if (pool_seen.insert(p.ip + ":" + std::to_string(p.port)).second) {
                    active_session->peer_pool.push_back(p);
                }
            }
            for (const auto& p : peer_swarm) {
                if (pool_seen.insert(p.ip + ":" + std::to_string(p.port)).second) {
                    active_session->peer_pool.push_back(p);
                }
            }
        }
        if (storage) storage->FlushHandles();

        // Update fastresume metadata in task so state persistence captures verified pieces & timestamps
        task.torrent_fastresume_bitfield = piece_mgr->ExportBitfieldHex();
        uint64_t current_disk_size = 0;
        int64_t latest_mtime = 0;
        std::error_code ec;
        for (const auto& f : torrent_info.files) {
            auto p = std::filesystem::path(task.save_dir) / f.relative_path;
            if (std::filesystem::exists(p, ec)) {
                current_disk_size += std::filesystem::file_size(p, ec);
                auto ftime = std::filesystem::last_write_time(p, ec);
                auto s = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
                latest_mtime = std::max(latest_mtime, static_cast<int64_t>(s));
            }
        }
        if (torrent_info.files.empty()) {
            auto p = std::filesystem::path(task.save_dir) / PathSanitizer::SanitizeFileName(torrent_info.name);
            if (std::filesystem::exists(p, ec)) {
                current_disk_size = std::filesystem::file_size(p, ec);
                auto ftime = std::filesystem::last_write_time(p, ec);
                latest_mtime = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            }
        }
        task.torrent_fastresume_filesize = current_disk_size;
        task.torrent_fastresume_mtime = latest_mtime;

        return Result<void>::Err("ERR_PAUSED", "BitTorrent task paused by user");
    }

    if (piece_mgr->IsAllCompleted()) {
        task.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
        task.state = TaskState::kCompleted;
        task.downloaded_bytes = total_bytes;
        task.completed_segments = task.total_segments;
        task.speed_bps = 0;
        task.status_detail = "Completed (BitTorrent)";

        // Announce completion to tracker
        TrackerClient::AnnounceAny(announce_urls, torrent_info.info_hash_raw, client_peer_id, 6881, task.uploaded_bytes, total_bytes, 0, "completed", 4000);

        if (on_progress) {
            TorrentProgress tp;
            tp.task_id = task.task_id;
            tp.state = TaskState::kCompleted;
            tp.downloaded_bytes = total_bytes;
            tp.total_bytes = total_bytes;
            tp.completed_pieces = task.total_segments;
            tp.total_pieces = task.total_segments;
            tp.speed_bps = 0;
            tp.uploaded_bytes = task.uploaded_bytes;
            tp.upload_speed_bps = 0;
            tp.status_detail = task.status_detail;
            on_progress(tp);
        }

        // 6. Persistent Seeding Session if requested
        if (seed_after_completion) {
            auto seed_start = std::chrono::steady_clock::now();
            task.status_detail = "Seeding to BitTorrent swarm...";
            if (on_progress) {
                TorrentProgress tp;
                tp.task_id = task.task_id;
                tp.state = TaskState::kCompleted;
                tp.downloaded_bytes = total_bytes;
                tp.total_bytes = total_bytes;
                tp.completed_pieces = task.total_segments;
                tp.total_pieces = task.total_segments;
                tp.status_detail = task.status_detail;
                on_progress(tp);
            }

            auto last_seed_stats = seed_start;
            while (!cancel_requested.load() && !pause_requested.load()) {
                const auto seed_now = std::chrono::steady_clock::now();
                if (seed_now - last_seed_stats >= TorrentRateMeter::kMinimumSampleInterval) {
                    last_seed_stats = seed_now;
                    uint64_t uploaded_now = 0;
                    uint64_t upload_rate_now = 0;
                    {
                        std::lock_guard<std::mutex> metric_lock(upload_mutex);
                        uploaded_now = uploaded_payload_bytes.load(std::memory_order_relaxed);
                        upload_rate_now = upload_rate_meter.Update(uploaded_now, seed_now);
                        task.upload_speed_bps = upload_rate_now;
                    }
                    live_upload_bps.store(upload_rate_now, std::memory_order_relaxed);
                    if (on_progress) {
                        TorrentProgress tp;
                        tp.task_id = task.task_id;
                        tp.state = TaskState::kCompleted;
                        tp.downloaded_bytes = total_bytes;
                        tp.received_bytes = total_bytes;
                        tp.total_bytes = total_bytes;
                        tp.uploaded_bytes = uploaded_now;
                        tp.upload_speed_bps = upload_rate_now;
                        tp.completed_pieces = task.total_segments;
                        tp.total_pieces = task.total_segments;
                        tp.status_detail = "Seeding to BitTorrent swarm...";
                        on_progress(tp);
                    }
                }
                const uint64_t uploaded_total = uploaded_payload_bytes.load(std::memory_order_relaxed);
                double ratio = (total_bytes > 0) ? (static_cast<double>(uploaded_total) / static_cast<double>(total_bytes)) : 0.0;
                if (max_seed_ratio > 0.0 && ratio >= max_seed_ratio) {
                    break;
                }
                if (max_seed_time_sec > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - seed_start).count();
                    if (elapsed >= max_seed_time_sec) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            incoming_stop.store(true);
            if (incoming_listener.joinable()) incoming_listener.join();
            // Announce stopped to tracker
            task.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
            TrackerClient::AnnounceAny(announce_urls, torrent_info.info_hash_raw, client_peer_id, 6881, task.uploaded_bytes, total_bytes, 0, "stopped", 3000);
        }

        UnregisterSession(task.task_id);
        return Result<void>::Ok();
    }

    UnregisterSession(task.task_id);
    task.state = TaskState::kFailed;
    if (!ever_discovered_peer) {
        task.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
        task.speed_bps = 0;
        task.upload_speed_bps = 0;
        task.error_code = "ERR_TORRENT_NO_PEERS";
        task.error_message = "No BitTorrent peers available from announce trackers or DHT";
        task.status_detail = "Failed: No peers available";
        return Result<void>::Err("ERR_TORRENT_NO_PEERS", task.error_message);
    }
    task.error_code = "ERR_TORRENT_TRANSFER_INTERRUPTED";
    task.uploaded_bytes = uploaded_payload_bytes.load(std::memory_order_relaxed);
    task.downloaded_bytes = piece_mgr->DownloadedBytes();
    task.completed_segments = static_cast<uint32_t>(piece_mgr->CompletedPieces());
    task.speed_bps = 0;
    task.upload_speed_bps = 0;
    task.error_message = "Peer connection closed before torrent completion";
    task.status_detail = "Failed: Transfer interrupted";
    return Result<void>::Err("ERR_TORRENT_TRANSFER_INTERRUPTED", task.error_message);
}



Result<void> TorrentEngine::AddTrackersToTask(const std::string& task_id, const std::vector<std::string>& new_trackers) {
    std::lock_guard<std::mutex> lock(s_sessions_mutex);
    auto it = s_active_sessions.find(task_id);
    if (it == s_active_sessions.end()) {
        return Result<void>::Err("ERR_TASK_NOT_FOUND", "Torrent task is not actively downloading or seeding: " + task_id);
    }
    auto& session = it->second;
    std::lock_guard<std::mutex> slock(session->session_lock);
    for (const auto& tr : new_trackers) {
        if (std::find(session->announce_urls.begin(), session->announce_urls.end(), tr) == session->announce_urls.end()) {
            session->announce_urls.push_back(tr);
            TrackerEndpointInfo ti;
            ti.url = tr;
            ti.tier = static_cast<int>(session->trackers.size());
            ti.status = "Active";
            session->trackers.push_back(std::move(ti));
        }
    }
    return Result<void>::Ok();
}

Result<TorrentSwarmDetails> TorrentEngine::GetSwarmDetails(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(s_sessions_mutex);
    auto it = s_active_sessions.find(task_id);
    if (it == s_active_sessions.end()) {
        return Result<TorrentSwarmDetails>::Err("ERR_TASK_NOT_FOUND", "Torrent task not found in active session registry");
    }
    auto& session = it->second;
    std::lock_guard<std::mutex> slock(session->session_lock);
    TorrentSwarmDetails details;
    details.task_id = task_id;
    details.trackers = session->trackers;
    details.peers = session->active_peers;
    details.active_peer_count = static_cast<uint32_t>(session->active_peers.size());
    details.sequential_mode = session->sequential_mode;
    for (const auto& tr : session->trackers) {
        details.total_seeds += tr.seeders;
        details.total_leechers += tr.leechers;
    }
    return Result<TorrentSwarmDetails>::Ok(details);
}

Result<void> TorrentEngine::SetSequentialMode(const std::string& task_id, bool enable) {
    std::lock_guard<std::mutex> lock(s_sessions_mutex);
    auto it = s_active_sessions.find(task_id);
    if (it == s_active_sessions.end()) {
        return Result<void>::Err("ERR_TASK_NOT_FOUND", "Torrent task not found in active session registry");
    }
    auto& session = it->second;
    std::lock_guard<std::mutex> slock(session->session_lock);
    session->sequential_mode = enable;
    if (session->piece_mgr) {
        session->piece_mgr->SetSequentialMode(enable, true);
    }
    return Result<void>::Ok();
}

} // namespace openidm::torrent
