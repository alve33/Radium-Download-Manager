// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Unified Download Controller, Queue Scheduler & Settings Service
// Combines ordinary HTTP/HTTPS downloads (Brisk engine) and HLS/DASH streams (N_m3u8DL-RE worker)
// into a single persistent queue and unified task model.
// Implements Phase 3, Phase 5, Phase 7, and Phase 8 requirements.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "archive/zip_preview.hpp"
#include "brisk_engine/http_download_engine.hpp"
#include "common/types.hpp"
#include "core/duplicate_detector.hpp"
#include "crawler/site_grabber.hpp"
#include "engine_ftp/ftp_client.hpp"
#include "engine_stream/nm3u8dl_re_adapter.hpp"
#include "engine_stream/rtsp_mms_adapter.hpp"
#include "engine_torrent/torrent_creator.hpp"
#include "engine_torrent/torrent_engine.hpp"
#include "engine_torrent/torrent_parser.hpp"
#include "security/antivirus_scanner.hpp"
#include "storage/import_export.hpp"
#include "storage/sqlite_store.hpp"
#include "common/central_config.hpp"

namespace openidm {

// Sliding-window download quota (ported from MDM quota.rs: "Download no more than N MBytes every H hours")
class SlidingWindowQuota {
public:
    void Record(std::chrono::steady_clock::time_point now, const std::vector<std::pair<std::string, uint64_t>>& progress);
    uint64_t Used(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration window);
    std::optional<bool> Check(std::chrono::steady_clock::time_point now, uint64_t limit_bytes, std::chrono::steady_clock::duration window);
    std::chrono::steady_clock::duration RoomIn(std::chrono::steady_clock::time_point now, uint64_t limit_bytes, std::chrono::steady_clock::duration window);

    bool blocked{false};
    std::vector<std::string> held_task_ids;

private:
    std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> m_samples;
    std::map<std::string, uint64_t> m_last_bytes;
};

struct ControllerConfig {
    std::filesystem::path state_db_path;
    std::filesystem::path default_download_dir;
    std::filesystem::path temp_root_dir;
    std::filesystem::path nm3u8dl_binary_path;
    std::filesystem::path ffmpeg_binary_path;
    uint32_t max_concurrent_tasks{radium::config::DefaultSettingsConfig::kMaxConcurrentTasks};
    uint64_t global_speed_limit_bps{radium::config::DefaultSettingsConfig::kSpeedLimitBps};
    bool allow_loopback{false};
    int stall_timeout_ms{radium::config::NetworkPoliciesConfig::kStallTimeoutMs};
    uint64_t min_split_bytes{radium::config::NetworkPoliciesConfig::kMinSplitBytes};
    torrent::TorrentMetadataSelectionCallback torrent_metadata_selection_callback;
    std::function<std::string()> local_time_override;
    std::function<Result<security::ScanReport>(const std::filesystem::path&)> av_scan_override;
    std::function<bool()> av_available_override;
};

class DownloadController {
public:
    struct TaskNotification { std::string task_id; std::string file_name; bool success{false}; std::string message; };
    using TaskNotificationCallback = std::function<void(const TaskNotification&)>;
    explicit DownloadController(ControllerConfig cfg);
    ~DownloadController();

    Result<void> Initialize();
    void Shutdown();

    // Unified task creation (HTTP_FILE, STREAM_HLS, STREAM_DASH)
    // Automatically deduplicates active/queued tasks for the same URL unless force_duplicate=true.
    Result<TaskModel> AddTask(TaskModel task, bool start_immediately = true, bool force_duplicate = false);

    // Checks for duplicate tasks across 3 priority tiers: SAME_URL, SAME_NAME_AND_SIZE, SAME_ETAG (XDM pattern)
    core::DuplicateMatch CheckDuplicateTask(
        const std::string& url,
        const std::string& file_name = "",
        uint64_t file_size = 0,
        const std::string& etag = "",
        const std::string& exclude_task_id = "") const;

    // Checks for destination path conflict across existing tasks
    core::DuplicateMatch CheckDestinationConflict(
        const std::string& save_dir,
        const std::string& file_name,
        const std::string& exclude_task_id = "") const;

    enum class IncomingConflictKind {
        kNone = 0,
        kActiveOrQueuedSameDownload = 1,
        kCompletedInHistory = 2,
        kExistsOnDisk = 3,
        kReservedByAnotherTask = 4,
        kResumablePartialExists = 5
    };

    struct IncomingConflictReport {
        IncomingConflictKind kind{IncomingConflictKind::kNone};
        bool destination_exists_on_disk{false};
        bool destination_reserved_by_task{false};
        bool overwrite_allowed{true};
        std::string resolved_save_dir;
        std::string resolved_file_name;
        std::string target_path;
        std::optional<TaskModel> matched_task;
        std::vector<std::string> conflicting_torrent_files;

        bool has_conflict() const { return kind != IncomingConflictKind::kNone; }
    };

    // Pure, non-mutating preflight check for incoming downloads (distinguishes all 5 duplicate/conflict cases).
    IncomingConflictReport InspectIncomingDownload(
        const TaskModel& candidate,
        const std::string& exclude_task_id = "") const;

    // Pure, non-mutating preflight check for multi-file or single-file torrents.
    IncomingConflictReport InspectIncomingTorrent(
        const torrent::TorrentInfo& info,
        const std::string& save_dir,
        const std::string& exclude_task_id = "") const;

    // Finds a unique filename in save_dir that neither exists on disk nor is reserved by any active/paused task.
    std::string ResolveAvailableFileName(
        const std::string& save_dir,
        const std::string& file_name,
        const std::string& exclude_task_id = "") const;

    // Ingest an already completed/transferred file (e.g. from browser-generated Blob/Data transfer)
    // Moves staged file to final destination, runs AV scan, stores task in DB, and notifies UI.
    Result<TaskModel> IngestCompletedTask(TaskModel task, const std::filesystem::path& staged_file_path);

    // Batch add links (from browser text selection FR-010)
    Result<std::vector<TaskModel>> AddBatchTasks(
        const std::vector<std::string>& urls,
        const std::string& page_url,
        const std::map<std::string, std::string>& headers,
        const std::map<std::string, std::string>& auth_headers);

    // Probe a streaming manifest (HLS/DASH) and return available tracks (FR-019)
    Result<stream::StreamProbeResult> ProbeStream(
        const std::string& manifest_url,
        const std::map<std::string, std::string>& headers,
        const std::map<std::string, std::string>& auth_headers,
        const std::string& cached_manifest_body = "");

    // Unambiguous Task Lifecycle Controls (Phase 3):
    // - StartTask: Starts a Paused, Queued, or Scheduled task immediately
    // - PauseTask: Pauses an active or queued transfer while preserving .part / cached segments
    // - ResumeTask: Resumes a Paused transfer from saved byte/segment offsets
    // - RetryTask: Retries a Failed or Canceled transfer (increments retry_count, resets corrupted state if needed)
    // - CancelTask: Stops transfer, deletes .part/temp files, wipes credentials, marks CANCELED (never deletes completed files)
    // - RemoveTask: Removes task from queue & DB; only deletes completed output file on disk if delete_file == true
    // - ClearCompletedHistory: Removes all COMPLETED and CANCELED tasks from list & DB while keeping all files on disk
    Result<void> StartTask(const std::string& task_id);
    Result<void> PauseTask(const std::string& task_id);
    Result<void> ResumeTask(const std::string& task_id);
    Result<void> CancelTask(const std::string& task_id);
    Result<void> RetryTask(const std::string& task_id);
    Result<void> RemoveTask(const std::string& task_id, bool delete_file = false);
    Result<void> UpdateTask(const TaskModel& task);
    Result<int> ClearCompletedHistory();

    void PauseAllTasks();
    void ResumeAllTasks();
    void StartAllTasks();

    // Refresh expired signed download URL while preserving existing partial bytes (FR-006)
    Result<void> RefreshTaskUrl(
        const std::string& task_id,
        const std::string& new_url,
        const std::map<std::string, std::string>& new_headers = {},
        const std::map<std::string, std::string>& new_auth_headers = {});

    // Automatic expired-URL freshlink matching (MDM freshlink.rs port)
    static std::string ExtractRegistrableDomain(const std::string& url);
    static bool IsExpiringSiteUrl(const std::string& url);
    std::optional<TaskModel> TryMatchFreshLink(const TaskModel& offer);

    // Queue & bandwidth controls (FR-024, FR-025)
    void SetMaxConcurrentTasks(uint32_t max_tasks);
    uint32_t GetMaxConcurrentTasks() const;
    void SetGlobalSpeedLimit(uint64_t bps);
    uint64_t GetGlobalSpeedLimit() const;

    // Task-Specific Bandwidth Limiting (Phase 2 Feature A)
    Result<void> SetTaskSpeedLimit(const std::string& task_id, uint64_t bps);
    uint64_t GetTaskSpeedLimit(const std::string& task_id) const;

    // Alternate URLs / Mirrors (Phase 2 Feature B)
    Result<void> AddAlternateUrl(const std::string& task_id, const std::string& alt_url);
    std::vector<std::string> GetAlternateUrls(const std::string& task_id) const;

    // Task & Queue Scheduling (Phase 2 Feature D)
    Result<void> SetTaskSchedule(const std::string& task_id, const std::string& start_time_local, const std::string& stop_time_local);

    // Dynamic Categorization & Save Destinations (Phase 2 Feature E)
    std::string DetermineCategory(const std::string& filename, const std::string& url = "") const;
    std::filesystem::path ResolveCategorySaveDir(const std::string& category, const std::string& url = "") const;

    // Persistent Application Settings (Phase 3 & Phase 10)
    AppSettings GetSettings() const;
    Result<void> UpdateSettings(const AppSettings& settings);
    Result<AppSettings> RestoreDefaultSettings();
    Result<void> RememberDownloadDirectory(const std::string& directory);
    void SetTaskNotificationCallback(TaskNotificationCallback callback);

    // Persistent Browser Extension Onboarding & Installation State (stored independently of AppSettings)
    struct ExtensionOnboardingState {
        bool completed{false};
        std::string action;
        std::string installed_version;
        std::string installed_commit;
        std::string install_dir;
    };
    ExtensionOnboardingState GetExtensionOnboardingState() const;
    bool ShouldShowExtensionOnboarding() const;
    Result<void> MarkExtensionOnboardingDisplayed(const std::string& action);
    Result<void> RecordInstalledExtension(
        const std::string& version,
        const std::string& commit_sha,
        const std::string& install_dir);

    const ControllerConfig& GetConfig() const { return m_cfg; }
    // Installs the UI-side selector used when a magnet link obtains multi-file metadata.
    // A null callback preserves the non-interactive/default-all behavior for headless clients.
    void SetTorrentMetadataSelectionCallback(torrent::TorrentMetadataSelectionCallback callback);
    bool AreWorkersAvailable() const;

    // Sleep / wake & network change recovery (Phase 8)
    void OnSystemSuspend();
    void OnSystemResume();

    // Advanced BitTorrent Capabilities (Creator, Swarm Inspector, Sequential Mode)
    Result<std::vector<uint8_t>> CreateTorrent(
        const std::filesystem::path& source_path,
        const torrent::TorrentCreatorOptions& options = torrent::TorrentCreatorOptions{});

    Result<void> CreateTorrentFile(
        const std::filesystem::path& source_path,
        const std::filesystem::path& output_torrent_path,
        const torrent::TorrentCreatorOptions& options = torrent::TorrentCreatorOptions{});

    Result<void> AddTrackersToTask(const std::string& task_id, const std::vector<std::string>& new_trackers);
    Result<torrent::TorrentSwarmDetails> GetTorrentSwarmDetails(const std::string& task_id);
    Result<void> SetTorrentSequentialMode(const std::string& task_id, bool enable);

    std::vector<TaskModel> ListTasks() const;
    std::optional<TaskModel> GetTask(const std::string& task_id) const;
    bool HasActiveTasks() const;
    size_t GetTaskCount() const;

    // Multi-Queue Management (Phase 3 & Advanced Scheduler)
    Result<void> UpsertQueue(const QueueConfig& queue);
    std::vector<QueueConfig> ListQueues();
    Result<void> MoveTaskToQueue(const std::string& task_id, const std::string& new_queue_id);

    // Site Grabber & Crawler
    Result<std::vector<crawler::GrabbedItem>> CrawlSite(
        const crawler::SiteGrabberConfig& cfg,
        std::atomic<bool>& cancel_requested,
        crawler::GrabbedItemCallback on_item_found = nullptr);

    // Post-Download Antivirus Scan
    Result<security::ScanReport> ScanTaskFile(const std::string& task_id);

    // ZIP Archive Preview
    Result<archive::ZipArchiveSummary> PreviewZipArchive(const std::filesystem::path& zip_path);

    // Import / Export
    Result<std::string> ExportTasksData(storage::ExportFormat format, bool include_credentials = false);
    Result<int> ImportTasksData(const std::string& data, storage::ExportFormat format);

    // Waits until a specific task reaches a terminal/paused state (useful for CLI & integration tests)
    Result<TaskModel> WaitForTaskCompletion(const std::string& task_id, int timeout_ms = 30000);

private:
    struct ActiveTaskControl {
        std::atomic<bool> pause_requested{false};
        std::atomic<bool> cancel_requested{false};
        std::atomic<uint64_t> task_speed_limit_bps{0};
        std::thread worker_thread;
        std::atomic<bool> finished{false};
    };

    void ScheduleLoop();
    void RunTaskWorker(std::string task_id, std::shared_ptr<ActiveTaskControl> ctrl);
    static std::string DetermineCategoryImpl(const AppSettings& settings, const std::string& filename, const std::string& url);
    static std::filesystem::path ResolveCategorySaveDirImpl(const AppSettings& settings, const std::filesystem::path& default_dir, const std::string& category, const std::string& url);
    void LoadPersistedSettingsLocked();
    Result<void> SaveSettingsLocked();

    ControllerConfig m_cfg;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stopping{false};

    SqliteStore m_store;
    std::map<std::string, TaskModel> m_tasks;
    std::set<std::string> m_deleted_tasks;
    std::map<std::string, std::shared_ptr<ActiveTaskControl>> m_active;
    std::atomic<uint32_t> m_max_concurrent{3};
    std::atomic<uint64_t> m_global_speed_limit_bps{0};
    std::shared_ptr<brisk::TokenBucketLimiter> m_global_limiter{std::make_shared<brisk::TokenBucketLimiter>()};
    AppSettings m_settings;
    TaskNotificationCallback m_task_notification_callback;
    std::set<std::string> m_notified_task_ids;
    std::thread m_scheduler_thread;
};

} // namespace openidm
