// SPDX-License-Identifier: Apache-2.0
#include "core/download_controller.hpp"
#include "brisk_engine/http_client.hpp"
#include "engine_stream/video_extractor.hpp"

#include "common/central_config.hpp"
#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/startup_diagnostic.hpp"
#include "platform/windows_integrations.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

namespace {
void TriggerSystemShutdown() {
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp;
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }
        CloseHandle(hToken);
    }
    InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, FALSE, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_OTHER);
}

void TriggerSystemSleep() {
    SetSystemPowerState(TRUE, FALSE);
}
} // namespace
#endif

namespace {
int64_t QpcNowUs() {
#ifdef _WIN32
    static const int64_t kFreq = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1000000LL;
    }();
    LARGE_INTEGER cnt{};
    QueryPerformanceCounter(&cnt);
    return (cnt.QuadPart * 1000000LL) / kFreq;
#else
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

std::string BytesToHexStr(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<unsigned int>(b);
    }
    return oss.str();
}

std::vector<uint8_t> HexStrToBytes(std::string_view hex) {
    std::vector<uint8_t> bytes;
    if (hex.size() % 2 != 0) return bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned int byte_val = 0;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        if (ss >> byte_val) {
            bytes.push_back(static_cast<uint8_t>(byte_val));
        } else {
            return {};
        }
    }
    return bytes;
}

std::string ExtractHostFromUrl(const std::string& url) {
    if (url.empty()) return "";
    auto parsed = radium::brisk::HttpClient::ParseUrl(url);
    if (parsed.ok()) return parsed.value->host;
    return "";
}

std::string ExtractExtensionLower(const std::string& filename) {
    if (filename.empty()) return "";
    auto dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= filename.size()) return "";
    std::string ext = filename.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return ext;
}

std::optional<std::filesystem::path> ResolveTaskOutputPath(
    const std::filesystem::path& save_dir,
    const std::filesystem::path& relative_path) {
    if (relative_path.empty() || relative_path.is_absolute() || relative_path.has_root_name() ||
        relative_path.has_root_directory()) {
        return std::nullopt;
    }
    for (const auto& component : relative_path) {
        if (component == "..") return std::nullopt;
    }

    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(save_dir, ec);
    if (ec) return std::nullopt;
    const auto target = std::filesystem::weakly_canonical(root / relative_path, ec);
    if (ec) return std::nullopt;
    const auto relative_target = target.lexically_relative(root);
    if (relative_target.empty() || relative_target.is_absolute()) return std::nullopt;
    const auto first_component = relative_target.begin();
    if (first_component == relative_target.end() || *first_component == "..") return std::nullopt;
    return target;
}

bool IsTransientRetryableError(const std::string& error_code) {
    return error_code == "ERR_NETWORK_TIMEOUT" ||
           error_code == "ERR_NET_TIMEOUT" ||
           error_code == "ERR_TIMEOUT" ||
           error_code == "ERR_CONNECTION_RESET" ||
           error_code == "ERR_CONNECTION_CLOSED_PREMATURELY" ||
           error_code == "ERR_NET_CONNECTION_RESET" ||
           error_code == "ERR_NET_CONNECT" ||
           error_code == "ERR_NET_SEND" ||
           error_code == "ERR_NET_DNS" ||
           error_code == "ERR_HTTP_TIMEOUT" ||
           error_code == "ERR_HTTP_5XX_TRANSIENT" ||
           error_code == "ERR_HTTP_429_RATE_LIMITED" ||
           error_code == "ERR_HTTP_503" ||
           error_code == "ERR_HTTP_502" ||
           error_code == "ERR_HTTP_504" ||
           error_code == "ERR_HTTP_429" ||
           error_code == "ERR_FTP_TIMEOUT" ||
           error_code == "ERR_FTP_DISCONNECTED" ||
           error_code == "ERR_FTP_CONNECT" ||
           error_code == "ERR_STREAM_WORKER_CRASH";
}

bool IsValidTimeFormat(const std::string& t) {
    if (t.empty()) return true;
    if (t.size() != 5 || t[2] != ':') return false;
    if (!std::isdigit(static_cast<unsigned char>(t[0])) || !std::isdigit(static_cast<unsigned char>(t[1])) ||
        !std::isdigit(static_cast<unsigned char>(t[3])) || !std::isdigit(static_cast<unsigned char>(t[4]))) {
        return false;
    }
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[3] - '0') * 10 + (t[4] - '0');
    return hh >= 0 && hh <= 23 && mm >= 0 && mm <= 59;
}

bool IsTimeInScheduledWindow(const std::string& cur, const std::string& start, const std::string& stop) {
    if (!IsValidTimeFormat(cur) || cur.empty()) return false;
    if (!IsValidTimeFormat(start) || !IsValidTimeFormat(stop)) return false;
    if (start.empty() && stop.empty()) return true;
    if (!start.empty() && stop.empty()) {
        return cur >= start;
    }
    if (start.empty() && !stop.empty()) {
        return cur < stop;
    }
    // Both start and stop are present
    if (start < stop) {
        // Normal intraday window, e.g. 14:00 to 18:00
        return cur >= start && cur < stop;
    } else if (start > stop) {
        // Overnight window crossing midnight, e.g. 23:00 to 06:00
        return cur >= start || cur < stop;
    } else {
        // start == stop
        return false;
    }
}

bool HasScheduleWindowElapsed(const std::string& cur, const std::string& start, const std::string& stop) {
    if (!IsValidTimeFormat(cur) || cur.empty()) return false;
    if (!IsValidTimeFormat(start) || !IsValidTimeFormat(stop)) return false;
    if (stop.empty()) return false;
    if (start < stop) {
        // Normal intraday window [start, stop)
        // If cur >= stop, the window has elapsed. If cur < start, it is in the future.
        return cur >= stop;
    } else if (start > stop) {
        // Overnight window [start, midnight) U [midnight, stop)
        // The non-active period is [stop, start).
        return cur >= stop && cur < start;
    }
    return false;
}
} // namespace

namespace radium {

DownloadController::DownloadController(ControllerConfig cfg)
    : m_cfg(std::move(cfg)),
      m_max_concurrent(m_cfg.max_concurrent_tasks),
      m_global_speed_limit_bps(m_cfg.global_speed_limit_bps) {
    m_settings.max_concurrent_tasks = m_cfg.max_concurrent_tasks;
    m_settings.speed_limit_bps = m_cfg.global_speed_limit_bps;
    m_settings.default_download_dir = m_cfg.default_download_dir.string();
    m_settings.stall_timeout_ms = m_cfg.stall_timeout_ms;
    m_settings.min_split_bytes = m_cfg.min_split_bytes;
}

void DownloadController::SetTorrentMetadataSelectionCallback(
    torrent::TorrentMetadataSelectionCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cfg.torrent_metadata_selection_callback = std::move(callback);
}

DownloadController::~DownloadController() {
    Shutdown();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

Result<void> DownloadController::Initialize() {
    RADIUM_LOG_INFO("Controller", "Initialize: Creating directories");
    std::error_code ec;
    std::filesystem::create_directories(m_cfg.default_download_dir, ec);
    std::filesystem::create_directories(m_cfg.temp_root_dir, ec);

    RADIUM_LOG_INFO("Controller", "Initialize: Opening store at " + m_cfg.state_db_path.string());
    auto open_res = m_store.Open(m_cfg.state_db_path);
    if (!open_res.ok()) {
        RADIUM_LOG_ERROR("Controller", "Initialize: m_store.Open failed: " + open_res.error->message);
        return open_res;
    }

    RADIUM_LOG_INFO("Controller", "Initialize: Initializing startup diagnostics");
    // Initialize startup diagnostic storage directory
    diagnostics::StartupDiagnosticRecorder::Instance().InitializeStorage(m_cfg.state_db_path.parent_path());

    RADIUM_LOG_INFO("Controller", "Initialize: Recovering interrupted tasks");
    auto rec_res = m_store.RecoverInterruptedTasks(65536);
    if (rec_res.ok() && *rec_res.value > 0) {
        RADIUM_LOG_INFO("Controller", "Recovered " + std::to_string(*rec_res.value) + " interrupted tasks from SQLite WAL");
    }

    RADIUM_LOG_INFO("Controller", "Initialize: Listing tasks from store");
    auto loaded = m_store.ListTasks();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (loaded.ok()) {
            std::string startup_hh_mm;
            if (m_cfg.local_time_override) {
                startup_hh_mm = m_cfg.local_time_override();
            } else {
                auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                std::tm local_tm{};
#ifdef _WIN32
                localtime_s(&local_tm, &now_t);
#else
                localtime_r(&now_t, &local_tm);
#endif
                char time_buf[16] = {0};
                std::strftime(time_buf, sizeof(time_buf), "%H:%M", &local_tm);
                startup_hh_mm = time_buf;
            }
            for (auto& t : *loaded.value) {
                if (t.scheduled && !t.schedule_stop_time_local.empty()) {
                    bool elapsed = HasScheduleWindowElapsed(startup_hh_mm, t.schedule_time_local, t.schedule_stop_time_local);
                    if (elapsed) {
                        t.scheduled = false;
                        t.status_detail = "Schedule Expired (Window elapsed while offline)";
                        t.schedule_time_local.clear();
                        t.schedule_stop_time_local.clear();
                        m_store.UpsertTask(t);
                    }
                }
                if (t.state == TaskState::kFailed && t.failure_notification_sent) {
                    m_notified_task_ids.insert(t.task_id);
                }
                m_tasks[t.task_id] = std::move(t);
            }
        }
        RADIUM_LOG_INFO("Controller", "Initialize: Loading persisted settings");
        LoadPersistedSettingsLocked();

        // Existing installations with persisted tasks must not be treated as a fresh install
        // for first-launch browser extension onboarding.
        if (!m_tasks.empty()) {
            auto ob_res = m_store.GetSetting("extension_onboarding", "");
            if (!ob_res.ok() || ob_res.value->empty()) {
                json::Value obj = json::Object{};
                obj["completed"] = true;
                obj["action"] = "existing_install";
                obj["installedVersion"] = "";
                obj["installedCommit"] = "";
                obj["installDir"] = "";
                m_store.SetSetting("extension_onboarding", obj.dump());
            }
        }
    }

    m_stopping = false;
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    RADIUM_LOG_INFO("Controller", "Initialize: Starting scheduler thread");
    m_scheduler_thread = std::thread(&DownloadController::ScheduleLoop, this);
    RADIUM_LOG_INFO("Controller", "Initialize: Complete");
    return Result<void>::Ok();
}

void DownloadController::LoadPersistedSettingsLocked() {
    auto sres = m_store.GetSetting("app_settings", "");
    if (!sres.ok() || sres.value->empty()) return;
    try {
        auto val = json::Parse(*sres.value);
        m_settings.theme = val["theme"].as_string("system");
        m_settings.language = val["language"].as_string("en");
        const bool migrate_legacy_system_language = m_settings.language == "system";
        if (migrate_legacy_system_language) m_settings.language = "en";
        if (m_settings.theme == "dark") {
            m_settings.theme = "coral_dark";
        }
        m_settings.max_concurrent_tasks = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["maxConcurrentTasks"].as_int64(m_cfg.max_concurrent_tasks)), 1u, 16u);
        m_settings.speed_limit_bps = static_cast<uint64_t>(val["speedLimitBps"].as_int64(m_cfg.global_speed_limit_bps));
        m_settings.default_connections_per_task = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["defaultConnectionsPerTask"].as_int64(8)), 1u, 32u);
        std::string saved_dir = val["defaultDownloadDir"].as_string("");
        const bool saved_dir_user_selected = val["defaultDownloadDirUserSelected"].as_bool(false);
        bool migrate_legacy_download_default = false;
#ifdef _WIN32
        if (!saved_dir.empty() && !saved_dir_user_selected && std::getenv("RADIUM_DOWNLOAD_DIR") == nullptr) {
            auto saved_path = std::filesystem::path(saved_dir).lexically_normal();
            std::string leaf = PathSanitizer::PathToUtf8(saved_path.filename());
            std::string parent = PathSanitizer::PathToUtf8(saved_path.parent_path().filename());
            std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(parent.begin(), parent.end(), parent.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (leaf == "radium" && parent == "downloads") {
                saved_dir = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir()).string();
                migrate_legacy_download_default = true;
            }
        }
#endif
        if (!saved_dir.empty()) {
            m_settings.default_download_dir = saved_dir;
            m_cfg.default_download_dir = saved_dir;
        }
        m_settings.default_download_dir_user_selected = saved_dir_user_selected;
        m_settings.last_download_dir = val["lastDownloadDir"].as_string("");
        // Keep the legacy preference disabled; category inference remains internal.
        const auto stored_conflict = val["conflictPolicy"].as_string("auto_rename");
        const bool migrate_legacy_conflict_default = val["conflictPolicyVersion"].as_int64(0) < 1 && stored_conflict == "auto_rename";
        m_settings.conflict_policy = migrate_legacy_conflict_default
            ? ConflictPolicy::kPrompt : ConflictPolicyFromString(stored_conflict);
        m_settings.auto_start_downloads = val["autoStartDownloads"].as_bool(true);
        m_settings.stall_timeout_ms = std::clamp<int>(
            static_cast<int>(val["stallTimeoutMs"].as_int64(m_cfg.stall_timeout_ms)), 500, 60000);
        m_settings.min_split_bytes = std::clamp<uint64_t>(
            static_cast<uint64_t>(val["minSplitBytes"].as_int64(static_cast<int64_t>(m_cfg.min_split_bytes))),
            65536ULL, 10485760ULL);
        m_settings.browser_capture_enabled = val["browserCaptureEnabled"].as_bool(true);
        m_settings.min_capture_size_bytes = std::clamp<uint64_t>(
            static_cast<uint64_t>(val["minCaptureSizeBytes"].as_int64(1048576)),
            0ULL, 10737418240ULL);
        m_settings.close_to_tray = val["closeToTray"].as_bool(true);
        m_settings.start_minimized = val["startMinimized"].as_bool(false);
        m_settings.reduced_motion = val["reducedMotion"].as_bool(false);
        m_settings.show_completion_notification = val["showCompletionNotification"].as_bool(true);
        m_settings.av_strict_mode = val["avStrictMode"].as_bool(false);

        m_settings.proxy_mode = val["proxyMode"].as_string("direct");
        m_settings.proxy_server = val["proxyServer"].as_string("");
        m_settings.proxy_bypass = val["proxyBypass"].as_string("");
        m_settings.proxy_username = val["proxyUsername"].as_string("");
        m_settings.proxy_password_encrypted = val["proxyPasswordEncrypted"].as_string("");
        m_settings.server_auth_username = val["serverAuthUsername"].as_string("");
        m_settings.server_auth_password_encrypted = val["serverAuthPasswordEncrypted"].as_string("");

        if (val.contains("customExtensionCategories") && val["customExtensionCategories"].is_object()) {
            m_settings.custom_extension_categories.clear();
            for (const auto& [k, v] : val["customExtensionCategories"].as_object()) {
                m_settings.custom_extension_categories[k] = v.as_string();
            }
        }
        if (val.contains("siteRules") && val["siteRules"].is_object()) {
            m_settings.site_rules.clear();
            for (const auto& [k, v] : val["siteRules"].as_object()) {
                m_settings.site_rules[k] = v.as_string();
            }
        }

        if (val.contains("tableColumnWidths")) {
            m_settings.table_column_widths = val["tableColumnWidths"].as_string();
        }
        m_settings.max_auto_retries = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["maxAutoRetries"].as_int64(5)), 0u, 50u);
        m_settings.retry_interval_sec = std::clamp<uint32_t>(
            static_cast<uint32_t>(val["retryIntervalSec"].as_int64(15)), 1u, 3600u);
        m_settings.queue_completion_action = val["queueCompletionAction"].as_string("NONE");

        if (migrate_legacy_conflict_default || migrate_legacy_download_default || migrate_legacy_system_language) SaveSettingsLocked();

        m_cfg.stall_timeout_ms = m_settings.stall_timeout_ms;
        m_cfg.min_split_bytes = m_settings.min_split_bytes;
        m_max_concurrent.store(m_settings.max_concurrent_tasks);
        m_global_speed_limit_bps.store(m_settings.speed_limit_bps);
    } catch (...) {}
}

Result<void> DownloadController::SaveSettingsLocked() {
    json::Value obj = json::Object{};
    obj["theme"] = m_settings.theme;
    obj["language"] = m_settings.language;
    obj["maxConcurrentTasks"] = static_cast<int64_t>(m_settings.max_concurrent_tasks);
    obj["speedLimitBps"] = static_cast<int64_t>(m_settings.speed_limit_bps);
    obj["defaultConnectionsPerTask"] = static_cast<int64_t>(m_settings.default_connections_per_task);
    obj["defaultDownloadDir"] = m_settings.default_download_dir;
    obj["defaultDownloadDirUserSelected"] = m_settings.default_download_dir_user_selected;
    obj["lastDownloadDir"] = m_settings.last_download_dir;
    obj["conflictPolicy"] = ConflictPolicyToString(m_settings.conflict_policy);
    obj["conflictPolicyVersion"] = static_cast<int64_t>(1);
    obj["autoStartDownloads"] = m_settings.auto_start_downloads;
    obj["stallTimeoutMs"] = static_cast<int64_t>(m_settings.stall_timeout_ms);
    obj["minSplitBytes"] = static_cast<int64_t>(m_settings.min_split_bytes);
    obj["browserCaptureEnabled"] = m_settings.browser_capture_enabled;
    obj["minCaptureSizeBytes"] = static_cast<int64_t>(m_settings.min_capture_size_bytes);
    obj["closeToTray"] = m_settings.close_to_tray;
    obj["startMinimized"] = m_settings.start_minimized;
    obj["reducedMotion"] = m_settings.reduced_motion;
    obj["showCompletionNotification"] = m_settings.show_completion_notification;
    obj["avStrictMode"] = m_settings.av_strict_mode;

    obj["proxyMode"] = m_settings.proxy_mode;
    obj["proxyServer"] = m_settings.proxy_server;
    obj["proxyBypass"] = m_settings.proxy_bypass;
    obj["proxyUsername"] = m_settings.proxy_username;
    obj["proxyPasswordEncrypted"] = m_settings.proxy_password_encrypted;
    obj["serverAuthUsername"] = m_settings.server_auth_username;
    obj["serverAuthPasswordEncrypted"] = m_settings.server_auth_password_encrypted;

    json::Value ext_cat_obj = json::Object{};
    for (const auto& [k, v] : m_settings.custom_extension_categories) ext_cat_obj[k] = v;
    obj["customExtensionCategories"] = ext_cat_obj;

    json::Value site_rules_obj = json::Object{};
    for (const auto& [k, v] : m_settings.site_rules) site_rules_obj[k] = v;
    obj["siteRules"] = site_rules_obj;

    obj["tableColumnWidths"] = m_settings.table_column_widths;
    obj["tableColumnWidths"] = m_settings.table_column_widths;
    obj["maxAutoRetries"] = static_cast<int64_t>(m_settings.max_auto_retries);
    obj["retryIntervalSec"] = static_cast<int64_t>(m_settings.retry_interval_sec);
    obj["queueCompletionAction"] = m_settings.queue_completion_action;

    return m_store.SetSetting("app_settings", obj.dump());
}

void DownloadController::Shutdown() {
    std::vector<std::shared_ptr<ActiveTaskControl>> to_join;
    std::vector<std::shared_ptr<SpeculativeTransaction>> txs_to_join;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) return;
        m_stopping = true;
        for (auto& [id, ctrl] : m_active) {
            ctrl->pause_requested.store(true);
            to_join.push_back(ctrl);
        }
        for (auto& [id, tx] : m_pending_txs) {
            std::lock_guard<std::mutex> tx_lk(tx->mu);
            if (!tx->accepted) {
                tx->cancelled = true;
                tx->state = SpeculativeTxState::kCancelled;
                tx->ctrl->cancel_requested.store(true);
            } else {
                tx->ctrl->pause_requested.store(true);
            }
            tx->cv.notify_all();
            txs_to_join.push_back(tx);
        }
        m_pending_txs.clear();
    }
    m_cv.notify_all();
    if (m_scheduler_thread.joinable()) {
        m_scheduler_thread.join();
    }
    for (auto& ctrl : to_join) {
        if (ctrl->worker_thread.joinable()) {
            ctrl->worker_thread.join();
        }
    }
    for (auto& tx : txs_to_join) {
        if (tx->dest_thread.joinable()) {
            tx->dest_thread.join();
        }
        if (tx->bg_thread.joinable()) {
            tx->bg_thread.join();
        }
        std::lock_guard<std::mutex> tx_lk(tx->mu);
        if (!tx->accepted && !tx->staging_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(tx->staging_dir, ec);
        }
    }
    m_store.Close();
}

std::string DownloadController::DetermineCategoryImpl(const AppSettings& settings, const std::string& filename, const std::string& url) {
    // 1. Site rules: Domain -> Category check
    if (!url.empty()) {
        std::string host = ExtractHostFromUrl(url);
        if (!host.empty()) {
            auto it = settings.site_rules.find(host);
            if (it != settings.site_rules.end()) {
                const std::string& val = it->second;
                if (val == "Video" || val == "Audio" || val == "Compressed" ||
                    val == "Documents" || val == "Programs" || val == "General") {
                    return val;
                }
            }
        }
    }

    // 2. Custom extension mapping
    std::string ext = ExtractExtensionLower(filename);
    if (!ext.empty()) {
        auto it = settings.custom_extension_categories.find(ext);
        if (it != settings.custom_extension_categories.end() && !it->second.empty()) {
            return it->second;
        }
    }

    // 3. Fallback to standard extension inference
    return InferCategoryFromFilename(filename);
}

std::string DownloadController::DetermineCategory(const std::string& filename, const std::string& url) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return DetermineCategoryImpl(m_settings, filename, url);
}

std::filesystem::path DownloadController::ResolveCategorySaveDirImpl(
    const AppSettings& settings, const std::filesystem::path& default_dir, const std::string& category, const std::string& url) {
    (void)url;
    if (!settings.enable_category_subfolders) {
        return default_dir;
    }
    if (category.empty() || category == "General" || category == "All") {
        return default_dir;
    }
    std::string safe_cat = PathSanitizer::SanitizeFileName(category);
    if (safe_cat.empty() || safe_cat == "." || safe_cat == "..") {
        return default_dir;
    }
    return default_dir / safe_cat;
}

std::filesystem::path DownloadController::ResolveCategorySaveDir(const std::string& category, const std::string& url) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return ResolveCategorySaveDirImpl(m_settings, m_cfg.default_download_dir, category, url);
}

Result<TaskModel> DownloadController::AddTask(TaskModel task, bool start_immediately, bool force_duplicate) {
    if (task.url.empty()) {
        return Result<TaskModel>::Err("ERR_INVALID_URL", "Task URL cannot be empty");
    }
    if (task.url.find("ftp://") == 0 || task.url.find("ftps://") == 0) {
        task.task_type = TaskType::kFtp;
        auto ftp_res = ftp::FtpUrl::Parse(task.url);
        if (!ftp_res.ok()) return Result<TaskModel>::Err(ftp_res.error->code, ftp_res.error->message);
        if (task.file_name.empty()) task.file_name = ftp_res.value->file_name;
    } else if (task.url.find("magnet:?") == 0) {
        if constexpr (!radium::config::FeatureRegistry::kTorrentEngine.empty()) {
            task.task_type = TaskType::kTorrent;
            auto mag_res = torrent::TorrentParser::ParseMagnetUri(task.url);
            if (!mag_res.ok()) return Result<TaskModel>::Err(mag_res.error->code, mag_res.error->message);
            if (task.file_name.empty()) task.file_name = mag_res.value->name;
        } else {
            return Result<TaskModel>::Err("ERR_FEATURE_DISABLED", "Torrent downloads are disabled in build feature registry");
        }
    } else if (task.task_type == TaskType::kTorrent || task.url.ends_with(".torrent") ||
               (std::filesystem::exists(task.url) && std::filesystem::path(task.url).extension() == ".torrent")) {
        if constexpr (!radium::config::FeatureRegistry::kTorrentEngine.empty()) {
            task.task_type = TaskType::kTorrent;
            std::error_code ec;
            if (std::filesystem::exists(task.url, ec)) {
                auto tor_res = torrent::TorrentParser::ParseTorrentFile(task.url);
                if (!tor_res.ok()) return Result<TaskModel>::Err(tor_res.error->code, tor_res.error->message);
                if (task.file_name.empty()) task.file_name = tor_res.value->name;
                task.total_bytes = task.torrent_file_paths.empty() ? tor_res.value->total_length : 0;
                if (task.torrent_file_paths.empty()) {
                    for (const auto& file : tor_res.value->files) {
                        task.torrent_file_paths.push_back(file.relative_path.generic_string());
                        task.torrent_file_selection.push_back(file.selected ? 1 : 0);
                    }
                    task.total_bytes = tor_res.value->selected_bytes();
                } else if (task.torrent_file_paths.size() == tor_res.value->files.size()) {
                    uint64_t selected_bytes = 0;
                    for (size_t i = 0; i < tor_res.value->files.size() && i < task.torrent_file_selection.size(); ++i) {
                        selected_bytes += task.torrent_file_selection[i] ? tor_res.value->files[i].length : 0;
                    }
                    task.total_bytes = selected_bytes;
                }
            }
        } else {
            return Result<TaskModel>::Err("ERR_FEATURE_DISABLED", "Torrent downloads are disabled in build feature registry");
        }
    } else if (stream::RtspMmsAdapter::IsStreamUrl(task.url)) {
        task.task_type = TaskType::kRtsp;
        if (task.file_name.empty()) task.file_name = "stream_" + GenerateUuidV4().substr(0, 8) + ".mp4";
    } else if (task.url.rfind("blob:", 0) == 0 && !task.part_path.empty()) {
        // Staged browser blob transfer from IPC download.completeChunkedTransfer
        task.task_type = TaskType::kHttpFile;
    } else {
        if (task.task_type == TaskType::kHttpFile && PathSanitizer::IsWebpageWatchUrl(task.url)) {
            return Result<TaskModel>::Err(
                "ERR_URL_IS_WEBPAGE",
                "The specified URL is an HTML webpage, not a direct media file or stream. "
                "To download video from this site, play it in your web browser with the Radium extension enabled."
            );
        }
        auto parsed_u = brisk::HttpClient::ParseUrl(task.url);
        if (!parsed_u.ok()) {
            return Result<TaskModel>::Err(parsed_u.error->code, parsed_u.error->message);
        }
    }

    if (!force_duplicate && task.task_type == TaskType::kHttpFile) {
        if (auto matched = TryMatchFreshLink(task)) {
            return Result<TaskModel>::Ok(*matched);
        }
    }

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!force_duplicate) {
        auto dup = core::DuplicateDetector::FindDuplicate(m_tasks, task.url, task.file_name, task.total_bytes, task.etag);
        if (dup.kind != core::DuplicateKind::kNone) {
            const auto& existing = dup.matched_task;
            if (existing.state == TaskState::kPaused ||
                existing.state == TaskState::kQueued ||
                existing.state == TaskState::kProbing ||
                existing.state == TaskState::kDownloading ||
                existing.state == TaskState::kMuxing) {
                return Result<TaskModel>::Ok(existing);
            }
        }
        if (!task.correlation_id.empty()) {
            for (const auto& [id, existing] : m_tasks) {
                if (existing.correlation_id == task.correlation_id &&
                    (existing.state == TaskState::kPaused ||
                     existing.state == TaskState::kQueued ||
                     existing.state == TaskState::kProbing ||
                     existing.state == TaskState::kDownloading ||
                     existing.state == TaskState::kMuxing)) {
                    return Result<TaskModel>::Ok(existing);
                }
            }
        }
    }

    if (task.task_id.empty()) {
        task.task_id = GenerateUuidV4();
    }
    m_deleted_tasks.erase(task.task_id);

    if (task.file_name.empty()) {
        std::string fallback_ext = (task.task_type == TaskType::kHttpFile) ? "" : task.stream_options.output_format;
        task.file_name = PathSanitizer::ExtractFromUrl(task.url, fallback_ext);
    } else {
        task.file_name = PathSanitizer::SanitizeFileName(task.file_name);
    }
    if (task.category.empty() || task.category == "General") {
        task.category = DetermineCategoryImpl(m_settings, task.file_name, task.url);
    }
    if (task.save_dir.empty()) {
        task.save_dir = ResolveCategorySaveDirImpl(m_settings, m_cfg.default_download_dir, task.category, task.url).string();
    }
    if (task.max_connections == 0) {
        task.max_connections = m_settings.default_connections_per_task;
    }
    if (!task.conflict_policy_explicit) {
        task.conflict_policy = m_settings.conflict_policy;
    }
    if (task.created_at_ms <= 0) {
        task.created_at_ms = NowEpochMs();
    }
    task.updated_at_ms = task.created_at_ms;
    if (!task.file_name.empty() && !task.save_dir.empty() &&
        task.state != TaskState::kCompleted && task.state != TaskState::kFailed) {
        auto reserved = core::DuplicateDetector::FindDestinationConflict(
            m_tasks, task.save_dir, task.file_name, task.task_id, true);
        if (reserved.kind != core::DuplicateKind::kNone) {
            if (task.conflict_policy == ConflictPolicy::kOverwrite) {
                return Result<TaskModel>::Err(
                    "ERR_DESTINATION_IN_USE",
                    "Destination is currently owned by another active or paused download: " +
                        PathSanitizer::PathToUtf8(std::filesystem::path(task.save_dir) / task.file_name));
            }
            if (!force_duplicate && (task.conflict_policy == ConflictPolicy::kPrompt || task.conflict_policy == ConflictPolicy::kFailIfExists)) {
                return Result<TaskModel>::Err(
                    "ERR_FILE_EXISTS",
                    "Destination is reserved by another download: " +
                        PathSanitizer::PathToUtf8(std::filesystem::path(task.save_dir) / task.file_name));
            }
        }

        if (task.conflict_policy == ConflictPolicy::kAutoRename) {
            std::string base_name = PathSanitizer::SanitizeFileName(task.file_name);
            auto dest_res = PathSanitizer::ResolveSafeOutputPath(task.save_dir, base_name, ConflictPolicy::kAutoRename);
            if (!dest_res.ok()) {
                return Result<TaskModel>::Err(dest_res.error->code, dest_res.error->message);
            }
            std::string cand_name = PathSanitizer::PathToUtf8(dest_res.value->filename());
            if (core::DuplicateDetector::FindDestinationConflict(m_tasks, task.save_dir, cand_name, task.task_id, true).kind != core::DuplicateKind::kNone) {
                std::filesystem::path p = PathSanitizer::Utf8ToPath(base_name);
                std::string stem = PathSanitizer::PathToUtf8(p.stem());
                std::string ext = PathSanitizer::PathToUtf8(p.extension());
                std::error_code ec;
                for (int i = 1; i <= 9999; ++i) {
                    std::string try_name = stem + " (" + std::to_string(i) + ")" + ext;
                    std::filesystem::path try_path = PathSanitizer::Utf8ToPath(task.save_dir) / PathSanitizer::Utf8ToPath(try_name);
                    std::filesystem::path try_part = try_path;
                    try_part += ".part";
                    if (!std::filesystem::exists(try_path, ec) &&
                        !std::filesystem::exists(try_part, ec) &&
                        core::DuplicateDetector::FindDestinationConflict(m_tasks, task.save_dir, try_name, task.task_id, true).kind == core::DuplicateKind::kNone) {
                        cand_name = try_name;
                        break;
                    }
                }
            }
            task.file_name = cand_name;
        } else if (!force_duplicate &&
                   (task.conflict_policy == ConflictPolicy::kPrompt || task.conflict_policy == ConflictPolicy::kFailIfExists)) {
            auto dest_res = PathSanitizer::ResolveSafeOutputPath(task.save_dir, task.file_name, task.conflict_policy);
            if (!dest_res.ok() && dest_res.error->code == "ERR_FILE_EXISTS") {
                return Result<TaskModel>::Err(
                    "ERR_FILE_EXISTS",
                    "Destination file already exists: " +
                        PathSanitizer::PathToUtf8(std::filesystem::path(task.save_dir) / task.file_name));
            }
        }
    }

    if (task.state != TaskState::kCompleted && task.state != TaskState::kFailed && task.state != TaskState::kDownloading) {
        task.state = (start_immediately && !task.scheduled) ? TaskState::kQueued : TaskState::kPaused;
        if (task.state == TaskState::kPaused) {
            task.pause_reason = task.scheduled ? PauseReason::kNone : PauseReason::kUser;
        }
        task.status_detail = (task.state == TaskState::kQueued)
                                 ? "Queued"
                                 : (task.scheduled ? ("Scheduled (" + task.schedule_time_local + ")") : "Paused");
    }

    auto db_res = m_store.UpsertTask(task);
    if (!db_res.ok()) {
        return Result<TaskModel>::Err(db_res.error->code, db_res.error->message);
    }

    m_tasks[task.task_id] = task;
    lock.unlock();

    // Wake the scheduler before optional diagnostic persistence. Startup
    // diagnostics can touch disk; they must not sit on the task dispatch path.
    m_cv.notify_all();

    // Record task initialization event in startup diagnostics
    {
        std::string type_name = "HTTP";
        if (task.task_type == TaskType::kStreamHls) type_name = "HLS";
        else if (task.task_type == TaskType::kStreamDash) type_name = "DASH";
        else if (task.task_type == TaskType::kTorrent) type_name = "Torrent";
        else if (task.url.find("youtube.com") != std::string::npos || task.url.find("youtu.be") != std::string::npos) type_name = "YouTube";

        if (!diagnostics::StartupDiagnosticRecorder::Instance().HasSession(task.task_id) &&
            (task.correlation_id.empty() || !diagnostics::StartupDiagnosticRecorder::Instance().HasSession(task.correlation_id))) {
            diagnostics::StartupDiagnosticRecorder::Instance().StartSession(
                task.task_id,
                task.correlation_id,
                task.page_url,
                task.url,
                task.file_name,
                type_name,
                task.stream_options.video_filter,
                task.total_bytes,
                0);
        }
    }

    return Result<TaskModel>::Ok(task);
}

core::DuplicateMatch DownloadController::CheckDuplicateTask(
    const std::string& url,
    const std::string& file_name,
    uint64_t file_size,
    const std::string& etag,
    const std::string& exclude_task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return core::DuplicateDetector::FindDuplicate(m_tasks, url, file_name, file_size, etag, exclude_task_id);
}

core::DuplicateMatch DownloadController::CheckDestinationConflict(
    const std::string& save_dir,
    const std::string& file_name,
    const std::string& exclude_task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return core::DuplicateDetector::FindDestinationConflict(m_tasks, save_dir, file_name, exclude_task_id);
}

std::string DownloadController::ResolveAvailableFileName(
    const std::string& save_dir,
    const std::string& file_name,
    const std::string& exclude_task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::string base_name = PathSanitizer::SanitizeFileName(file_name);
    if (base_name.empty()) base_name = "download.bin";
    std::string dir = save_dir.empty()
        ? (m_settings.last_download_dir.empty() ? m_settings.default_download_dir : m_settings.last_download_dir)
        : save_dir;
    if (dir.empty()) dir = m_cfg.default_download_dir.string();

    std::error_code ec;
    auto is_free = [&](const std::string& cand) {
        std::filesystem::path p = PathSanitizer::Utf8ToPath(dir) / PathSanitizer::Utf8ToPath(cand);
        std::filesystem::path part_p = p;
        part_p += ".part";
        if (std::filesystem::exists(p, ec) || std::filesystem::exists(part_p, ec)) {
            return false;
        }
        return core::DuplicateDetector::FindDestinationConflict(m_tasks, dir, cand, exclude_task_id, true).kind ==
               core::DuplicateKind::kNone;
    };

    if (is_free(base_name)) return base_name;

    std::filesystem::path bp = PathSanitizer::Utf8ToPath(base_name);
    std::string stem = PathSanitizer::PathToUtf8(bp.stem());
    std::string ext = PathSanitizer::PathToUtf8(bp.extension());
    for (int i = 1; i <= 9999; ++i) {
        std::string cand = stem + " (" + std::to_string(i) + ")" + ext;
        if (is_free(cand)) return cand;
    }
    return base_name;
}

DownloadController::IncomingConflictReport DownloadController::InspectIncomingDownload(
    const TaskModel& candidate,
    const std::string& exclude_task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    IncomingConflictReport report;

    std::string fn = candidate.file_name;
    if (fn.empty()) {
        if (candidate.url.find("youtube.com") != std::string::npos || candidate.url.find("youtu.be") != std::string::npos) {
            fn = "youtube_video.mp4";
        } else {
            std::string fallback_ext = (candidate.task_type == TaskType::kHttpFile) ? "" : candidate.stream_options.output_format;
            fn = PathSanitizer::ExtractFromUrl(candidate.url, fallback_ext);
        }
    } else {
        fn = PathSanitizer::SanitizeFileName(fn);
    }

    std::string cat = candidate.category;
    if (cat.empty() || cat == "General") {
        cat = DetermineCategoryImpl(m_settings, fn, candidate.url);
    }

    std::string sd = candidate.save_dir;
    if (sd.empty()) {
        if (!m_settings.last_download_dir.empty()) {
            sd = m_settings.last_download_dir;
        } else {
            sd = ResolveCategorySaveDirImpl(m_settings, m_cfg.default_download_dir, cat, candidate.url).string();
        }
    }

    report.resolved_save_dir = sd;
    report.resolved_file_name = fn;
    if (!sd.empty() && !fn.empty()) {
        report.target_path = PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(sd) / PathSanitizer::Utf8ToPath(fn));
    }

    // 1. Check URL / Name+Size / ETag duplicate across existing tasks
    auto dup = core::DuplicateDetector::FindDuplicate(
        m_tasks, candidate.url, fn, candidate.total_bytes, candidate.etag, exclude_task_id);

    // 2. Check destination reservation by active/queued/paused tasks
    auto reserved = (!sd.empty() && !fn.empty())
        ? core::DuplicateDetector::FindDestinationConflict(m_tasks, sd, fn, exclude_task_id, true)
        : core::DuplicateMatch{core::DuplicateKind::kNone, {}};
    if (reserved.kind != core::DuplicateKind::kNone) {
        report.destination_reserved_by_task = true;
        report.overwrite_allowed = false;
    }

    // 2b. Check concurrent pending speculative transactions
    int64_t my_tx_created_us = 0;
    if (!exclude_task_id.empty()) {
        auto my_it = m_pending_txs.find(exclude_task_id);
        if (my_it != m_pending_txs.end()) {
            my_tx_created_us = my_it->second->metrics.t3_tx_created_us;
        }
    }
    std::optional<TaskModel> pending_same_url;
    std::optional<TaskModel> pending_same_dest;
    for (const auto& [tx_id, tx] : m_pending_txs) {
        if (!exclude_task_id.empty() && tx_id == exclude_task_id) continue;
        std::lock_guard<std::mutex> tx_lk(tx->mu);
        if (tx->cancelled || tx->state == SpeculativeTxState::kConflictRejected || tx->accepted) continue;
        if (my_tx_created_us > 0 &&
            (tx->metrics.t3_tx_created_us > my_tx_created_us ||
             (tx->metrics.t3_tx_created_us == my_tx_created_us && tx_id > exclude_task_id))) {
            continue;
        }
        if (!candidate.url.empty() && tx->draft_task.url == candidate.url && !pending_same_url.has_value()) {
            pending_same_url = tx->draft_task;
        }
        if (!sd.empty() && !fn.empty() &&
            tx->draft_task.save_dir == sd && tx->draft_task.file_name == fn &&
            !pending_same_dest.has_value()) {
            pending_same_dest = tx->draft_task;
            report.destination_reserved_by_task = true;
            report.overwrite_allowed = false;
        }
    }

    // 3. Check physical file on disk
    std::error_code ec;
    if (!sd.empty() && !fn.empty()) {
        std::filesystem::path target_p = PathSanitizer::Utf8ToPath(sd) / PathSanitizer::Utf8ToPath(fn);
        if (std::filesystem::exists(target_p, ec)) {
            report.destination_exists_on_disk = true;
        }
    }

    // Classify in strict priority order:
    // (a) Same download already active/queued
    if (dup.kind != core::DuplicateKind::kNone &&
        (dup.matched_task.state == TaskState::kQueued ||
         dup.matched_task.state == TaskState::kProbing ||
         dup.matched_task.state == TaskState::kDownloading ||
         dup.matched_task.state == TaskState::kMuxing ||
         dup.matched_task.state == TaskState::kVerifying ||
         dup.matched_task.state == TaskState::kFinalizing)) {
        report.kind = IncomingConflictKind::kActiveOrQueuedSameDownload;
        report.matched_task = dup.matched_task;
        report.overwrite_allowed = false;
        return report;
    }

    // (b) Previous partial download exists (paused/failed task with progress/part_path, or .part on disk)
    if (dup.kind != core::DuplicateKind::kNone &&
        (dup.matched_task.state == TaskState::kPaused || dup.matched_task.state == TaskState::kFailed)) {
        bool has_part = !dup.matched_task.part_path.empty() &&
                        std::filesystem::exists(PathSanitizer::Utf8ToPath(dup.matched_task.part_path), ec);
        if (has_part || dup.matched_task.downloaded_bytes > 0 || dup.matched_task.state == TaskState::kPaused) {
            report.kind = IncomingConflictKind::kResumablePartialExists;
            report.matched_task = dup.matched_task;
            if (dup.matched_task.state == TaskState::kPaused) {
                report.overwrite_allowed = false;
            }
            return report;
        }
    }

    // (c) Another active/paused task (or pending speculative transaction) has reserved the exact same destination
    if (reserved.kind != core::DuplicateKind::kNone) {
        report.kind = IncomingConflictKind::kReservedByAnotherTask;
        report.matched_task = reserved.matched_task;
        report.overwrite_allowed = false;
        return report;
    }
    if (pending_same_dest.has_value()) {
        report.kind = IncomingConflictKind::kReservedByAnotherTask;
        report.matched_task = *pending_same_dest;
        report.overwrite_allowed = false;
        return report;
    }
    if (pending_same_url.has_value()) {
        report.kind = IncomingConflictKind::kActiveOrQueuedSameDownload;
        report.matched_task = *pending_same_url;
        report.overwrite_allowed = false;
        return report;
    }

    // (d) File already exists on disk (also record completed history match if any)
    if (report.destination_exists_on_disk) {
        report.kind = IncomingConflictKind::kExistsOnDisk;
        if (dup.kind != core::DuplicateKind::kNone) {
            report.matched_task = dup.matched_task;
        }
        return report;
    }

    // (e) Completed download exists in history (even if file was moved/deleted from disk)
    if (dup.kind != core::DuplicateKind::kNone && dup.matched_task.state == TaskState::kCompleted) {
        report.kind = IncomingConflictKind::kCompletedInHistory;
        report.matched_task = dup.matched_task;
        return report;
    }

    return report;
}

DownloadController::IncomingConflictReport DownloadController::InspectIncomingTorrent(
    const torrent::TorrentInfo& info,
    const std::string& save_dir,
    const std::string& exclude_task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    IncomingConflictReport report;

    std::string sd = save_dir;
    if (sd.empty()) {
        sd = !m_settings.last_download_dir.empty()
            ? m_settings.last_download_dir
            : m_cfg.default_download_dir.string();
    }
    report.resolved_save_dir = sd;
    report.resolved_file_name = info.name;
    if (!sd.empty() && !info.name.empty()) {
        report.target_path = PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(sd) / PathSanitizer::Utf8ToPath(info.name));
    }

    // Check magnet/infohash duplicate across existing tasks
    if (!info.info_hash_hex.empty()) {
        std::string mag = "magnet:?xt=urn:btih:" + info.info_hash_hex;
        auto dup = core::DuplicateDetector::FindDuplicate(m_tasks, mag, info.name, info.total_length, "", exclude_task_id);
        if (dup.kind != core::DuplicateKind::kNone) {
            report.matched_task = dup.matched_task;
            if (dup.matched_task.state == TaskState::kQueued ||
                dup.matched_task.state == TaskState::kDownloading ||
                dup.matched_task.state == TaskState::kProbing) {
                report.kind = IncomingConflictKind::kActiveOrQueuedSameDownload;
                report.overwrite_allowed = false;
                return report;
            }
            if (dup.matched_task.state == TaskState::kPaused) {
                report.kind = IncomingConflictKind::kResumablePartialExists;
                report.overwrite_allowed = false;
                return report;
            }
        }
    }

    std::error_code ec;
    std::filesystem::path base_dir = PathSanitizer::Utf8ToPath(sd);
    for (const auto& f : info.files) {
        if (!f.selected) continue;
        std::filesystem::path target_p = base_dir / f.relative_path;
        if (std::filesystem::exists(target_p, ec)) {
            report.destination_exists_on_disk = true;
            report.conflicting_torrent_files.push_back(f.relative_path.generic_string());
        }
        // Check if another active/paused task owns this exact path or torrent root
        for (const auto& [tid, existing] : m_tasks) {
            if (!exclude_task_id.empty() && tid == exclude_task_id) continue;
            if (!core::DuplicateDetector::IsTaskReservingDestination(existing.state)) continue;
            std::filesystem::path existing_root = PathSanitizer::Utf8ToPath(existing.save_dir) / PathSanitizer::Utf8ToPath(existing.file_name);
            if (PathSanitizer::PathsEquivalent(target_p, existing_root)) {
                report.destination_reserved_by_task = true;
                report.overwrite_allowed = false;
                if (!report.matched_task.has_value()) report.matched_task = existing;
            }
        }
    }

    if (report.destination_reserved_by_task) {
        report.kind = IncomingConflictKind::kReservedByAnotherTask;
        return report;
    }
    if (report.destination_exists_on_disk) {
        report.kind = IncomingConflictKind::kExistsOnDisk;
        if (report.conflicting_torrent_files.size() == 1) {
            report.target_path = PathSanitizer::PathToUtf8(base_dir / report.conflicting_torrent_files.front());
        }
        return report;
    }
    return report;
}

Result<TaskModel> DownloadController::IngestCompletedTask(TaskModel task, const std::filesystem::path& staged_file_path) {
    std::error_code ec;
    if (!std::filesystem::exists(staged_file_path, ec)) {
        return Result<TaskModel>::Err("ERR_STAGED_FILE_NOT_FOUND", "Staged file not found: " + staged_file_path.string());
    }
    uint64_t file_size = std::filesystem::file_size(staged_file_path, ec);

    AppSettings settings_snapshot;
    std::filesystem::path default_dir;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        settings_snapshot = m_settings;
        default_dir = m_cfg.default_download_dir;
    }

    if (task.file_name.empty()) {
        task.file_name = "browser_file_" + GenerateUuidV4().substr(0, 8);
    } else {
        task.file_name = PathSanitizer::SanitizeFileName(task.file_name);
    }
    if (task.task_id.empty()) {
        task.task_id = GenerateUuidV4();
    }
    task.task_type = TaskType::kHttpFile;
    if (task.category.empty() || task.category == "General") {
        task.category = DetermineCategoryImpl(settings_snapshot, task.file_name, task.url);
    }
    if (task.save_dir.empty()) {
        task.save_dir = ResolveCategorySaveDirImpl(settings_snapshot, default_dir, task.category, task.url).string();
    }
    std::filesystem::create_directories(task.save_dir, ec);

    ConflictPolicy effective_policy = (task.conflict_policy != ConflictPolicy::kPrompt)
        ? task.conflict_policy
        : settings_snapshot.conflict_policy;
    auto staged_commit = PathSanitizer::CommitPartFile(
        staged_file_path, task.save_dir, task.file_name, effective_policy, file_size);
    if (!staged_commit.ok()) {
        return Result<TaskModel>::Err(staged_commit.error->code, staged_commit.error->message);
    }
    std::filesystem::path dest_path = *staged_commit.value;
    task.file_name = PathSanitizer::PathToUtf8(dest_path.filename());
    task.part_path.clear();
    task.error_code.clear();
    task.error_message.clear();

    if (task.created_at_ms <= 0) {
        task.created_at_ms = NowEpochMs();
    }
    task.updated_at_ms = NowEpochMs();
    task.completed_at_ms = task.updated_at_ms;
    task.state = TaskState::kCompleted;
    task.total_bytes = file_size;
    task.downloaded_bytes = file_size;
    task.speed_bps = 0;
    task.stream_percent = 100.0;
    task.completed_segments = 1;
    task.total_segments = 1;
    task.range_supported = false;
    task.status_detail = "Completed";

    // Antivirus scan
    bool av_available = m_cfg.av_available_override ? m_cfg.av_available_override() : security::AntivirusScanner::IsWindowsDefenderAvailable();
    if (av_available) {
        if (std::filesystem::exists(dest_path, ec)) {
            auto scan_res = m_cfg.av_scan_override ? m_cfg.av_scan_override(dest_path) : security::AntivirusScanner::ScanWithDefender(dest_path);
            if (scan_res.ok()) {
                if (scan_res.value->status == security::ScanStatus::kThreatDetected) {
                    task.state = TaskState::kFailed;
                    std::filesystem::remove(dest_path, ec);
                    task.status_detail = "Threat detected by Antivirus (" + scan_res.value->scanner_name + ")";
                    task.error_code = "ERR_THREAT_DETECTED";
                    task.error_message = "A security threat was detected in the downloaded file.";
                    RADIUM_LOG_WARN("Controller", "Antivirus threat detected in ingested file: " + dest_path.string());
                } else if (scan_res.value->status == security::ScanStatus::kClean) {
                    task.status_detail = "Completed (Antivirus Clean)";
                } else if (scan_res.value->status == security::ScanStatus::kScannerError) {
                    if (m_settings.av_strict_mode) {
                        task.state = TaskState::kFailed;
                        task.error_code = "ERR_AV_SCAN_FAILED";
                        task.error_message = "Antivirus scanner failed in strict mode";
                        task.status_detail = "Failed: Antivirus scanner error (Strict mode)";
                    } else {
                        task.status_detail = "Completed (Antivirus Scan Failed)";
                    }
                } else {
                    task.status_detail = "Completed (Antivirus Skipped)";
                }
            } else {
                if (m_settings.av_strict_mode) {
                    task.state = TaskState::kFailed;
                    task.error_code = "ERR_AV_SCAN_FAILED";
                    task.error_message = "Antivirus scanner error: " + scan_res.error->message;
                    task.status_detail = "Failed: Antivirus scan error (Strict mode)";
                } else {
                    task.status_detail = "Completed (Antivirus Scan Error: " + scan_res.error->message + ")";
                }
            }
        }
    } else {
        if (m_settings.av_strict_mode) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_AV_UNAVAILABLE";
            task.error_message = "Antivirus scanner is unavailable in strict mode";
            task.status_detail = "Failed: Antivirus scanner unavailable (Strict mode)";
        } else {
            task.status_detail = "Completed (Not Scanned - Scanner Unavailable)";
        }
    }

    TaskNotificationCallback notification_callback;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deleted_tasks.erase(task.task_id);
        const bool is_failure = (task.state == TaskState::kFailed);
        const bool should_notify = m_settings.show_completion_notification &&
            (is_failure ? !task.failure_notification_sent
                        : m_notified_task_ids.insert(task.task_id).second);
        if (should_notify) {
            if (is_failure) {
                task.failure_notification_sent = true;
                m_notified_task_ids.insert(task.task_id);
            }
            notification_callback = m_task_notification_callback;
        }
        auto db_res = m_store.UpsertTask(task);
        if (!db_res.ok()) {
            return Result<TaskModel>::Err(db_res.error->code, db_res.error->message);
        }
        m_tasks[task.task_id] = task;
        m_cv.notify_all();
    }
    if (notification_callback) notification_callback(TaskNotification{
        task.task_id, task.file_name, task.state == TaskState::kCompleted,
        task.state == TaskState::kCompleted ? "Download completed" : task.error_message});
    return Result<TaskModel>::Ok(task);
}

Result<std::vector<TaskModel>> DownloadController::AddBatchTasks(
    const std::vector<std::string>& urls,
    const std::string& page_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers) {

    std::vector<TaskModel> added;
    for (const auto& u : urls) {
        TaskModel t;
        t.url = u;
        t.page_url = page_url;
        t.headers = headers;
        t.auth_headers = auth_headers;
        if (u.find(".m3u8") != std::string::npos) {
            t.task_type = TaskType::kStreamHls;
        } else if (u.find(".mpd") != std::string::npos) {
            t.task_type = TaskType::kStreamDash;
        } else {
            t.task_type = TaskType::kHttpFile;
        }
        auto res = AddTask(std::move(t), true, false);
        if (res.ok()) {
            added.push_back(*res.value);
        }
    }
    return Result<std::vector<TaskModel>>::Ok(std::move(added));
}

Result<stream::StreamProbeResult> DownloadController::ProbeStream(
    const std::string& manifest_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers,
    const std::string& cached_manifest_body) {

    stream::StreamAdapterConfig scfg{
        m_cfg.nm3u8dl_binary_path,
        m_cfg.ffmpeg_binary_path,
        m_cfg.temp_root_dir,
        m_cfg.allow_loopback
    };
    stream::Nm3u8dlReAdapter adapter(std::move(scfg));
    return adapter.ProbeManifest(manifest_url, headers, auth_headers, cached_manifest_body);
}

void DownloadController::ScheduleLoop() {
    std::map<std::string, std::chrono::steady_clock::time_point> retry_schedule;
    bool had_active = false;
    std::string last_queue_hh_mm;
    std::vector<QueueConfig> cached_queues;

    while (true) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
            if (m_stopping) return true;
            if (m_active.size() < m_max_concurrent.load()) {
                for (const auto& [id, t] : m_tasks) {
                    if (t.state == TaskState::kQueued && m_active.find(id) == m_active.end()) return true;
                }
            }
            for (const auto& [id, ctrl] : m_active) {
                if (ctrl->finished.load()) return true;
            }
            return false;
        });
        if (m_stopping) break;

        std::vector<std::thread> finished_workers;
        for (auto it = m_active.begin(); it != m_active.end();) {
            if (it->second->finished.load()) {
                if (it->second->worker_thread.joinable()) {
                    finished_workers.emplace_back(std::move(it->second->worker_thread));
                }
                it = m_active.erase(it);
            } else {
                ++it;
            }
        }

        // Workers merge their final state under m_mutex. Joining while holding
        // this lock deadlocks the scheduler when a worker is finishing a pause.
        lock.unlock();
        for (auto& worker : finished_workers) {
            if (worker.joinable()) worker.join();
        }
        lock.lock();

        if (!m_active.empty()) {
            had_active = true;
        }

        // Fast-path task dispatch: dispatch eligible queued tasks immediately to worker threads
        uint32_t max_conc = m_max_concurrent.load();
        while (m_active.size() < max_conc) {
            std::string next_id;
            int32_t min_order = INT32_MAX;
            int64_t oldest_ts = INT64_MAX;
            for (const auto& [id, t] : m_tasks) {
                if (t.state == TaskState::kQueued && m_active.find(id) == m_active.end()) {
                    if (t.queue_order < min_order || (t.queue_order == min_order && t.created_at_ms < oldest_ts)) {
                        min_order = t.queue_order;
                        oldest_ts = t.created_at_ms;
                        next_id = id;
                    }
                }
            }
            if (next_id.empty()) break;

            m_tasks[next_id].state = TaskState::kProbing;
            m_tasks[next_id].status_detail = "Connecting...";
            auto ctrl = std::make_shared<ActiveTaskControl>();
            ctrl->task_speed_limit_bps.store(m_tasks[next_id].speed_limit_bps);
            m_active[next_id] = ctrl;
            ctrl->worker_thread = std::thread(&DownloadController::RunTaskWorker, this, next_id, ctrl);
        }

        // 1. Check for scheduled tasks that reached their local schedule time
        std::string cur_hh_mm;
        if (m_cfg.local_time_override) {
            cur_hh_mm = m_cfg.local_time_override();
        } else {
            auto now_time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm local_tm{};
#ifdef _WIN32
            localtime_s(&local_tm, &now_time_t);
#else
            localtime_r(&now_time_t, &local_tm);
#endif
            char time_str[16] = {0};
            std::strftime(time_str, sizeof(time_str), "%H:%M", &local_tm);
            cur_hh_mm = time_str;
        }

        for (auto& [id, t] : m_tasks) {
            // Check scheduled start & stop time strictly on scheduled tasks
            if (t.scheduled) {
                bool in_window = IsTimeInScheduledWindow(cur_hh_mm, t.schedule_time_local, t.schedule_stop_time_local);
                if (t.state == TaskState::kPaused && t.pause_reason != PauseReason::kUser && t.pause_reason != PauseReason::kSystemSuspend && in_window) {
                    t.state = TaskState::kQueued;
                    t.status_detail = "Queued (Schedule Started)";
                    if (t.schedule_stop_time_local.empty()) {
                        // One-shot start schedule has fired
                        t.scheduled = false;
                        t.schedule_time_local.clear();
                    }
                    m_store.UpsertTask(t);
                } else if (!in_window && !t.schedule_stop_time_local.empty()) {
                    if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || t.state == TaskState::kQueued) {
                        auto ait = m_active.find(id);
                        if (ait != m_active.end()) {
                            ait->second->pause_requested.store(true);
                        }
                        t.state = TaskState::kPaused;
                        t.pause_reason = PauseReason::kQueueSchedule;
                        t.status_detail = "Paused (Scheduled Stop " + t.schedule_stop_time_local + ")";
                        // Cleanly expire the schedule once stop time has arrived so it never blocks manual Resume or Retry!
                        t.scheduled = false;
                        t.schedule_time_local.clear();
                        t.schedule_stop_time_local.clear();
                        m_store.UpsertTask(t);
                    }
                }
            }
        }

        // 2. Check multi-queue schedules (cached per minute to eliminate redundant repetitive SQLite queries)
        // An empty queue list is a valid cached result. Retrying ListQueues on
        // every 50 ms scheduler pass kept SQLite work inside m_mutex forever
        // for the common no-custom-queues case, blocking AddTask/StartTask from
        // acquiring the controller lock and delaying worker dispatch.
        if (cur_hh_mm != last_queue_hh_mm) {
            lock.unlock();
            auto queues_res = m_store.ListQueues();
            lock.lock();
            if (m_stopping) break;
            if (queues_res.ok()) {
                cached_queues = *queues_res.value;
                last_queue_hh_mm = cur_hh_mm;
            }
        }
        for (const auto& q : cached_queues) {
            if (q.schedule_enabled) {
                bool in_queue_window = IsTimeInScheduledWindow(cur_hh_mm, q.start_time_local, q.stop_time_local);
                if (in_queue_window) {
                    for (auto& [id, t] : m_tasks) {
                        if (t.queue_id == q.queue_id && t.state == TaskState::kPaused &&
                            t.pause_reason != PauseReason::kUser && t.pause_reason != PauseReason::kSystemSuspend && !t.scheduled) {
                            t.state = TaskState::kQueued;
                            t.status_detail = "Queued (Queue Schedule Started)";
                            m_store.UpsertTask(t);
                        }
                    }
                } else if (!q.stop_time_local.empty() && cur_hh_mm == q.stop_time_local) {
                    for (auto& [id, t] : m_tasks) {
                        if (t.queue_id == q.queue_id && !t.scheduled &&
                            (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || t.state == TaskState::kQueued)) {
                            auto ait = m_active.find(id);
                            if (ait != m_active.end()) {
                                ait->second->pause_requested.store(true);
                            }
                            t.state = TaskState::kPaused;
                            t.status_detail = "Paused (Queue Scheduled Stop " + q.stop_time_local + ")";
                            m_store.UpsertTask(t);
                        }
                    }
                }
            }
        }

        // 3. Exponential backoff auto-retry for transient network errors
        auto now_steady = std::chrono::steady_clock::now();

        // 3a. System sleep inhibition & sliding-window quota enforcement
        platform::WindowsIntegrations::SetDownloadKeepAlive(!m_active.empty() && m_settings.prevent_sleep);
        if (m_settings.quota_max_mb > 0) {
            static SlidingWindowQuota s_quota;
            std::vector<std::pair<std::string, uint64_t>> progress;
            progress.reserve(m_tasks.size());
            for (const auto& [tid, task_ref] : m_tasks) {
                progress.emplace_back(tid, task_ref.downloaded_bytes);
            }
            s_quota.Record(now_steady, progress);
            const uint64_t limit_bytes = m_settings.quota_max_mb * 1024ULL * 1024ULL;
            const auto window = std::chrono::hours(std::max<uint32_t>(1u, m_settings.quota_window_hours));
            if (auto state_chg = s_quota.Check(now_steady, limit_bytes, window)) {
                if (*state_chg) {
                    for (auto& [tid, t] : m_tasks) {
                        if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || t.state == TaskState::kQueued) {
                            if (auto act = m_active.find(tid); act != m_active.end()) {
                                act->second->pause_requested.store(true);
                                act->second->cancel_requested.store(true);
                            }
                            t.pause_reason = PauseReason::kQuota;
                            t.state = TaskState::kPaused;
                            t.status_detail = "Paused (Quota limit reached)";
                            m_store.UpsertTask(t);
                            s_quota.held_task_ids.push_back(tid);
                        }
                    }
                } else {
                    for (const auto& tid : s_quota.held_task_ids) {
                        auto it = m_tasks.find(tid);
                        if (it != m_tasks.end() && it->second.state == TaskState::kPaused && it->second.pause_reason == PauseReason::kQuota) {
                            it->second.pause_reason = PauseReason::kNone;
                            it->second.state = TaskState::kQueued;
                            it->second.status_detail = "Queued (Quota window available)";
                            m_store.UpsertTask(it->second);
                        }
                    }
                    s_quota.held_task_ids.clear();
                }
            }
        }
        for (auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kFailed && t.retry_count < m_settings.max_auto_retries) {
                if (IsTransientRetryableError(t.error_code)) {
                    auto rit = retry_schedule.find(id);
                    if (rit == retry_schedule.end()) {
                        uint32_t delay_sec = m_settings.retry_interval_sec * (1u << std::min<uint32_t>(t.retry_count, 6u));
                        retry_schedule[id] = now_steady + std::chrono::seconds(delay_sec);
                        t.status_detail = "Retrying in " + std::to_string(delay_sec) + "s (Attempt " +
                                          std::to_string(t.retry_count + 1) + "/" + std::to_string(m_settings.max_auto_retries) + ")";
                        m_store.UpsertTask(t);
                    } else if (now_steady >= rit->second && t.pause_reason != PauseReason::kUser) {
                        retry_schedule.erase(rit);
                        ++t.retry_count;
                        t.state = TaskState::kQueued;
                        t.status_detail = "Queued (Auto-Retry #" + std::to_string(t.retry_count) + ")";
                        t.error_code.clear();
                        t.error_message.clear();
                        m_store.UpsertTask(t);
                    }
                }
            } else if (t.state != TaskState::kFailed) {
                retry_schedule.erase(id);
            }
        }

        // 4. Queue / Global Post-Completion Action
        if (m_active.empty() && had_active) {
            bool has_pending = false;
            for (const auto& [id, t] : m_tasks) {
                if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
                    t.state == TaskState::kDownloading || t.state == TaskState::kMuxing ||
                    t.state == TaskState::kVerifying) {
                    has_pending = true;
                    break;
                }
            }
            if (!has_pending) {
                had_active = false;
                std::string action = m_settings.queue_completion_action;
                if (action == "SLEEP" || action == "SHUTDOWN") {
                    m_settings.queue_completion_action = "NONE";
                    SaveSettingsLocked();
                    RADIUM_LOG_INFO("Controller", "All downloads completed. Executing post-completion action: " + action);
#ifdef _WIN32
                    if (action == "SLEEP") {
                        TriggerSystemSleep();
                    } else if (action == "SHUTDOWN") {
                        TriggerSystemShutdown();
                    }
#endif
                }
            }
        }

        max_conc = m_max_concurrent.load();
        while (m_active.size() < max_conc) {
            std::string next_id;
            int32_t min_order = INT32_MAX;
            int64_t oldest_ts = INT64_MAX;
            for (const auto& [id, t] : m_tasks) {
                if (t.state == TaskState::kQueued && m_active.find(id) == m_active.end()) {
                    if (t.queue_order < min_order || (t.queue_order == min_order && t.created_at_ms < oldest_ts)) {
                        min_order = t.queue_order;
                        oldest_ts = t.created_at_ms;
                        next_id = id;
                    }
                }
            }
            if (next_id.empty()) break;

            m_tasks[next_id].state = TaskState::kProbing;
            m_tasks[next_id].status_detail = "Connecting...";
            auto ctrl = std::make_shared<ActiveTaskControl>();
            ctrl->task_speed_limit_bps.store(m_tasks[next_id].speed_limit_bps);
            m_active[next_id] = ctrl;
            ctrl->worker_thread = std::thread(&DownloadController::RunTaskWorker, this, next_id, ctrl);
        }
    }
}

void DownloadController::RunTaskWorker(std::string task_id, std::shared_ptr<ActiveTaskControl> ctrl) {
    TaskModel local_task;
    int stall_timeout_ms = radium::config::NetworkPoliciesConfig::kStallTimeoutMs;
    uint64_t min_split_bytes = radium::config::NetworkPoliciesConfig::kMinSplitBytes;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
            ctrl->finished.store(true);
            return;
        }
        local_task = it->second;
        stall_timeout_ms = m_cfg.stall_timeout_ms;
        min_split_bytes = m_cfg.min_split_bytes;
    }

    std::string task_type_str = TaskTypeToString(local_task.task_type);
    bool is_direct_manifest = (local_task.url.find(".m3u8") != std::string::npos ||
                               local_task.url.find(".mpd") != std::string::npos);
    bool is_youtube_task = (local_task.url.find("youtube.com") != std::string::npos ||
                            local_task.url.find("youtu.be") != std::string::npos ||
                            local_task.page_url.find("youtube.com") != std::string::npos ||
                            local_task.page_url.find("youtu.be") != std::string::npos ||
                            local_task.stream_options.cached_manifest_body.find("\"radiumType\":\"YOUTUBE_STREAM\"") != std::string::npos ||
                            local_task.stream_options.cached_manifest_body.find("\"radiumType\":\"SITE_EXTRACTION\"") != std::string::npos ||
                            local_task.stream_options.cached_manifest_body.find("\"fluxType\":\"YOUTUBE_STREAM\"") != std::string::npos ||
                            local_task.stream_options.cached_manifest_body.find("\"fluxType\": \"YOUTUBE_STREAM\"") != std::string::npos ||
                            (!is_direct_manifest && engine_stream::VideoExtractionManager::IsSupportedVideoSiteUrl(local_task.url)));
    if (is_youtube_task) {
        task_type_str = "YOUTUBE_EXTRACTION";
    }

    RADIUM_LOG_INFO("Controller", "Dispatching task=" + task_id + " type=" + task_type_str +
                     " correlation_id=" + local_task.correlation_id + " url=" + local_task.url);

    if (!diagnostics::StartupDiagnosticRecorder::Instance().HasSession(task_id) &&
        (local_task.correlation_id.empty() || !diagnostics::StartupDiagnosticRecorder::Instance().HasSession(local_task.correlation_id))) {
        diagnostics::StartupDiagnosticRecorder::Instance().StartSession(
            task_id,
            local_task.correlation_id,
            local_task.page_url,
            local_task.url,
            local_task.file_name,
            task_type_str,
            local_task.stream_options.video_filter,
            local_task.total_bytes,
            0);
    }

    diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
        task_id, "Scheduler dispatches task", "Worker thread executing for " + task_type_str);

    auto last_db_sync = std::chrono::steady_clock::now();

    bool preflight_conflict = false;
    if (local_task.task_type != TaskType::kTorrent &&
        (local_task.conflict_policy == ConflictPolicy::kPrompt || local_task.conflict_policy == ConflictPolicy::kFailIfExists)) {
        std::string cand_name = local_task.file_name;
        if (cand_name.empty()) {
            cand_name = is_youtube_task ? "youtube_video.mp4" : PathSanitizer::ExtractFromUrl(local_task.url);
        }
        auto destination = PathSanitizer::ResolveSafeOutputPath(
            local_task.save_dir, cand_name, local_task.conflict_policy);
        if (!destination.ok() && destination.error->code == "ERR_FILE_EXISTS") {
            preflight_conflict = true;
            local_task.file_name = cand_name;
            local_task.error_code = local_task.conflict_policy == ConflictPolicy::kPrompt ? "ERR_FILE_EXISTS" : std::string{};
            local_task.error_message = local_task.conflict_policy == ConflictPolicy::kPrompt
                ? "Destination file already exists: " + PathSanitizer::PathToUtf8(std::filesystem::path(local_task.save_dir) / cand_name)
                : std::string{};
            local_task.state = local_task.conflict_policy == ConflictPolicy::kPrompt ? TaskState::kFailed : TaskState::kCanceled;
            local_task.status_detail = local_task.conflict_policy == ConflictPolicy::kPrompt
                ? "Waiting for a file conflict choice"
                : "Canceled because the destination file already exists";
#ifdef _WIN32
            HWND hwnd_main = FindWindowW(L"Radium_MainWindowClass", nullptr);
            if (hwnd_main) PostMessageW(hwnd_main, WM_APP + 106 /* WM_RADIUM_SHOW_CONFLICT */, 0, 0);
#endif
        }
    }

    if (preflight_conflict) {
        // Keep the existing file untouched and let the normal terminal-state persistence run.
    } else if (local_task.task_type == TaskType::kFtp) {
        auto ftp_url_res = ftp::FtpUrl::Parse(local_task.url);
        if (ftp_url_res.ok()) {
            std::filesystem::path part_p = std::filesystem::path(local_task.save_dir) / (local_task.file_name + "." + local_task.task_id + ".part");
            local_task.part_path = part_p.string();
            local_task.state = TaskState::kDownloading;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.find(task_id);
                if (it != m_tasks.end()) {
                    it->second.part_path = part_p.string();
                }
            }

            auto dl_res = ftp::FtpClient::DownloadFile(
                ftp_url_res.value.value(),
                part_p,
                local_task.downloaded_bytes,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps,
                [&](const ftp::FtpProgress& fp) {
                    if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                    local_task.downloaded_bytes = fp.downloaded_bytes;
                    local_task.total_bytes = fp.total_bytes;
                    local_task.speed_bps = fp.speed_bps;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_tasks.find(task_id);
                        if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                            if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                            it->second.state = fp.state;
                            it->second.downloaded_bytes = fp.downloaded_bytes;
                            it->second.total_bytes = fp.total_bytes;
                            it->second.speed_bps = fp.speed_bps;
                            it->second.status_detail = fp.status_detail;
                        }
                    }
                },
                m_global_limiter,
                &m_global_speed_limit_bps,
                10000,
                m_cfg.allow_loopback);

            if (dl_res.ok()) {
                uint64_t final_size = dl_res.value.value();
                bool checksum_ok = true;
                if (local_task.total_bytes > 0 && final_size != local_task.total_bytes) {
                    checksum_ok = false;
                    local_task.state = TaskState::kFailed;
                    local_task.error_code = "ERR_FTP_SIZE_MISMATCH";
                    local_task.error_message = "Downloaded FTP size (" + std::to_string(final_size) +
                        ") did not match expected size (" + std::to_string(local_task.total_bytes) + ")";
                    local_task.status_detail = "Failed: Size mismatch";
                } else if (!local_task.expected_sha256.empty()) {
                    auto calc_sha = CryptoUtils::ComputeFileSha256Hex(part_p);
                    auto to_low = [](std::string s) {
                        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return s;
                    };
                    if (!calc_sha.ok() || to_low(calc_sha.value.value()) != to_low(local_task.expected_sha256)) {
                        checksum_ok = false;
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = "ERR_CHECKSUM_MISMATCH";
                        local_task.error_message = "Downloaded FTP file SHA-256 did not match expected checksum";
                        local_task.status_detail = "Failed: SHA-256 mismatch";
                    }
                } else if (!local_task.expected_md5.empty()) {
                    auto calc_md5 = CryptoUtils::ComputeFileMd5Hex(part_p);
                    auto to_low = [](std::string s) {
                        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        return s;
                    };
                    if (!calc_md5.ok() || to_low(calc_md5.value.value()) != to_low(local_task.expected_md5)) {
                        checksum_ok = false;
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = "ERR_CHECKSUM_MISMATCH";
                        local_task.error_message = "Downloaded FTP file MD5 did not match expected checksum";
                        local_task.status_detail = "Failed: MD5 mismatch";
                    }
                }
                if (checksum_ok) {
                    auto commit_res = PathSanitizer::CommitPartFile(
                        part_p,
                        local_task.save_dir,
                        local_task.file_name,
                        local_task.conflict_policy);
                    if (commit_res.ok()) {
                        local_task.file_name = PathSanitizer::PathToUtf8(commit_res.value->filename());
                        local_task.part_path.clear();
                        local_task.state = TaskState::kCompleted;
                        local_task.downloaded_bytes = final_size;
                        local_task.total_bytes = final_size;
                        local_task.completed_at_ms = NowEpochMs();
                        local_task.status_detail = "Completed (FTP)";
                    } else {
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = commit_res.error->code;
                        local_task.error_message = commit_res.error->message;
                        local_task.status_detail = "Failed to commit: " + commit_res.error->message;
                    }
                }
            } else {
                if (ctrl->cancel_requested.load()) {
                    local_task.state = TaskState::kCanceled;
                    local_task.status_detail = "Canceled";
                } else if (ctrl->pause_requested.load()) {
                    local_task.state = TaskState::kPaused;
                    local_task.status_detail = "Paused";
                } else {
                    local_task.state = TaskState::kFailed;
                    local_task.error_code = dl_res.error->code;
                    local_task.error_message = dl_res.error->message;
                    local_task.status_detail = "Failed: " + dl_res.error->message;
                }
            }
        } else {
            local_task.state = TaskState::kFailed;
            local_task.error_code = ftp_url_res.error->code;
            local_task.error_message = ftp_url_res.error->message;
            local_task.status_detail = "Failed: " + ftp_url_res.error->message;
        }
    } else if (local_task.task_type == TaskType::kTorrent) {
        torrent::TorrentInfo tinfo;
        Result<torrent::TorrentInfo> parse_res = Result<torrent::TorrentInfo>::Err("ERR_TORRENT_MALFORMED", "Torrent metainfo was not parsed");
        if (local_task.url.find("magnet:?") == 0) {
            parse_res = torrent::TorrentParser::ParseMagnetUri(local_task.url);
        } else {
            parse_res = torrent::TorrentParser::ParseTorrentFile(local_task.url);
        }
        if (!parse_res.ok()) {
            local_task.state = TaskState::kFailed;
            local_task.error_code = parse_res.error->code;
            local_task.error_message = parse_res.error->message;
            local_task.status_detail = "Failed: " + parse_res.error->message;
            RADIUM_LOG_ERROR("Controller", "Torrent task " + task_id + " parse failed [" + local_task.error_code + "]: " + local_task.error_message);
        } else {
            tinfo = parse_res.value.value();
            if (!local_task.torrent_file_paths.empty() && tinfo.files.size() == local_task.torrent_file_paths.size()) {
                for (size_t i = 0; i < tinfo.files.size() && i < local_task.torrent_file_selection.size(); ++i) {
                    if (tinfo.files[i].relative_path.generic_string() == local_task.torrent_file_paths[i]) {
                        tinfo.files[i].selected = local_task.torrent_file_selection[i] != 0;
                    }
                }
            }
        }
        torrent::TorrentMetadataSelectionCallback metadata_selection;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            metadata_selection = m_cfg.torrent_metadata_selection_callback;
        }
        auto tor_dl = parse_res.ok() ? torrent::TorrentEngine::ExecuteTorrentDownload(
            local_task,
            tinfo,
            ctrl->pause_requested,
            ctrl->cancel_requested,
            ctrl->task_speed_limit_bps,
            [&](const torrent::TorrentProgress& tp) {
                if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.find(task_id);
                if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                    if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                    if ((it->second.state == TaskState::kCompleted || it->second.state == TaskState::kFailed) &&
                        tp.state != TaskState::kCompleted && tp.state != TaskState::kFailed) {
                        RADIUM_LOG_WARN("Controller", "Ignoring stale torrent progress task=" + task_id +
                                         " phase=" + TaskStateToString(tp.state) + " after terminal state=" + TaskStateToString(it->second.state));
                        return;
                    }
                    it->second.state = tp.state;
                    // received_bytes includes partial, not-yet-hash-verified
                    // torrent blocks for live UI progress. ExecuteTorrentDownload
                    // keeps local_task.downloaded_bytes verified-only, so these
                    // transient bytes are never committed as resumable progress.
                    it->second.downloaded_bytes = std::max(tp.downloaded_bytes, tp.received_bytes);
                    it->second.total_bytes = tp.total_bytes;
                    it->second.speed_bps = tp.speed_bps;
                    it->second.uploaded_bytes = tp.uploaded_bytes;
                    it->second.upload_speed_bps = tp.upload_speed_bps;
                    it->second.completed_segments = tp.completed_pieces;
                    it->second.total_segments = tp.total_pieces;
                    it->second.status_detail = tp.status_detail;
                    local_task.uploaded_bytes = tp.uploaded_bytes;
                }
            },
            m_global_limiter,
            &m_global_speed_limit_bps,
            32,
            false,
            1.0,
            0,
            std::move(metadata_selection)) : Result<void>::Err(local_task.error_code, local_task.error_message);
        if (!tor_dl.ok() && parse_res.ok()) {
            if (ctrl->pause_requested.load() || tor_dl.error->code == "ERR_TORRENT_SELECTION_CANCELED") {
                local_task.state = TaskState::kPaused;
                local_task.pause_reason = PauseReason::kUser;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = tor_dl.error->code;
                local_task.error_message = tor_dl.error->message;
                local_task.status_detail = "Failed: " + tor_dl.error->message;
            }
        }
    } else if (local_task.task_type == TaskType::kRtsp) {
        stream::RtspMmsAdapter rtsp_ad(m_cfg.ffmpeg_binary_path, m_cfg.allow_loopback);
        auto rtsp_res = rtsp_ad.ExecuteStreamCapture(
            local_task,
            ctrl->pause_requested,
            ctrl->cancel_requested,
            [&](const stream::RtspProgress& rp) {
                if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                std::lock_guard<std::mutex> lock(m_mutex);
                auto it = m_tasks.find(task_id);
                if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                    if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                    it->second.state = rp.state;
                    it->second.downloaded_bytes = rp.downloaded_bytes;
                    it->second.speed_bps = rp.speed_bps;
                    it->second.status_detail = rp.status_detail;
                }
            });
        if (!rtsp_res.ok()) {
            if (ctrl->cancel_requested.load()) {
                local_task.state = TaskState::kCanceled;
                local_task.status_detail = "Canceled";
            } else if (ctrl->pause_requested.load()) {
                local_task.state = TaskState::kPaused;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = rtsp_res.error->code;
                local_task.error_message = rtsp_res.error->message;
                local_task.status_detail = "Failed: " + rtsp_res.error->message;
            }
        }
    } else if (local_task.task_type == TaskType::kHttpFile) {
        if (local_task.url.rfind("blob:", 0) == 0 && !local_task.part_path.empty()) {
            auto ingest_res = IngestCompletedTask(local_task, PathSanitizer::Utf8ToPath(local_task.part_path));
            if (ingest_res.ok()) {
                local_task = *ingest_res.value;
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = ingest_res.error->code;
                local_task.error_message = ingest_res.error->message;
                local_task.status_detail = (ingest_res.error->code == "ERR_FILE_EXISTS")
                    ? "Waiting for a file conflict choice"
                    : ("Failed: " + ingest_res.error->message);
                if (ingest_res.error->code == "ERR_FILE_EXISTS") {
#ifdef _WIN32
                    HWND hwnd_main = FindWindowW(L"Radium_MainWindowClass", nullptr);
                    if (hwnd_main) PostMessageW(hwnd_main, WM_APP + 106 /* WM_RADIUM_SHOW_CONFLICT */, 0, 0);
#endif
                }
            }
        } else {
        brisk::EngineConfig ecfg;
        ecfg.stall_timeout_ms = stall_timeout_ms;
        ecfg.min_split_bytes = min_split_bytes;
        ecfg.allow_loopback_redirect = m_cfg.allow_loopback;

        // Snapshot network & proxy settings under lock
        std::string p_mode, p_serv, p_by, p_user, p_pass_enc, s_user, s_pass_enc;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            p_mode = m_settings.proxy_mode;
            p_serv = m_settings.proxy_server;
            p_by = m_settings.proxy_bypass;
            p_user = m_settings.proxy_username;
            p_pass_enc = m_settings.proxy_password_encrypted;
            s_user = m_settings.server_auth_username;
            s_pass_enc = m_settings.server_auth_password_encrypted;
            ecfg.net_opts.identity_presets = m_settings.identity_presets;
            ecfg.net_opts.enable_bot_check_fallback = m_settings.enable_bot_check_fallback;
        }

        ecfg.net_opts.http_method = local_task.http_method;
        ecfg.net_opts.post_body = local_task.post_body;
        ecfg.net_opts.post_content_type = local_task.post_content_type;

        ecfg.net_opts.proxy_mode = p_mode;
        ecfg.net_opts.proxy_server = p_serv;
        ecfg.net_opts.proxy_bypass = p_by;
        ecfg.net_opts.proxy_username = p_user;
        if (!p_pass_enc.empty()) {
            auto raw = HexStrToBytes(p_pass_enc);
            auto unp = CryptoUtils::UnprotectSecret(raw);
            if (unp.ok()) ecfg.net_opts.proxy_password = *unp.value;
        }
        ecfg.net_opts.server_auth_username = s_user;
        if (!s_pass_enc.empty()) {
            auto raw = HexStrToBytes(s_pass_enc);
            auto unp = CryptoUtils::UnprotectSecret(raw);
            if (unp.ok()) ecfg.net_opts.server_auth_password = *unp.value;
        }
        ecfg.net_opts.enable_integrated_auth = true;

        std::vector<std::string> urls_to_try;
        urls_to_try.push_back(local_task.url);
        for (const auto& alt : local_task.alternate_urls) {
            if (!alt.empty() && alt != local_task.url) {
                urls_to_try.push_back(alt);
            }
        }

        Result<std::filesystem::path> res = Result<std::filesystem::path>::Err("ERR_NOT_ATTEMPTED", "Transfer not attempted");
        size_t start_idx = (local_task.current_url_index < urls_to_try.size()) ? local_task.current_url_index : 0;

        for (size_t uidx = start_idx; uidx < urls_to_try.size(); ++uidx) {
            if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) break;

            const std::string& candidate_url = urls_to_try[uidx];
            if (uidx != start_idx) {
                RADIUM_LOG_INFO("Controller", "Attempting alternate mirror (" + std::to_string(uidx) + "): " + candidate_url);
                auto probe_res = brisk::HttpClient::ProbeResource(
                    candidate_url, local_task.headers, local_task.auth_headers, ecfg.net_opts, m_cfg.allow_loopback);
                if (probe_res.ok()) {
                    if (local_task.total_bytes > 0 && probe_res.value->has_content_length && probe_res.value->content_length > 0) {
                        if (probe_res.value->content_length != local_task.total_bytes) {
                            RADIUM_LOG_WARN("Controller", "Mirror Content-Length mismatch (" +
                                             std::to_string(probe_res.value->content_length) + " vs " +
                                             std::to_string(local_task.total_bytes) + "). Skipping mirror: " + candidate_url);
                            continue;
                        }
                    }
                } else {
                    RADIUM_LOG_WARN("Controller", "Mirror probe failed: " + candidate_url + " (" + probe_res.error->message + ")");
                    continue;
                }
                local_task.url = candidate_url;
                local_task.current_url_index = static_cast<uint32_t>(uidx);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_tasks.find(task_id);
                    if (it != m_tasks.end()) {
                        it->second.url = candidate_url;
                        it->second.current_url_index = static_cast<uint32_t>(uidx);
                        m_store.UpsertTask(it->second);
                    }
                }
            }

            brisk::HttpDownloadEngine engine(ecfg);
            res = engine.ExecuteDownload(
                local_task,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps,
                [&](const brisk::EngineProgress& ep) {
                    if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_tasks.find(task_id);
                        if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                            auto& t = it->second;
                            if (t.state == TaskState::kCanceled || t.state == TaskState::kPaused) return;
                            t.state = ep.state;
                            t.downloaded_bytes = ep.downloaded_bytes;
                            t.total_bytes = ep.total_bytes;
                            t.speed_bps = ep.speed_bps;
                            t.range_supported = ep.range_supported;
                            if (!ep.status_detail.empty()) {
                                t.status_detail = ep.status_detail;
                            } else if (!local_task.status_detail.empty()) {
                                t.status_detail = local_task.status_detail;
                            }
                            if (!ep.resolved_filename.empty()) t.file_name = ep.resolved_filename;
                            if (!ep.etag.empty()) t.etag = ep.etag;
                            if (!ep.last_modified.empty()) t.last_modified = ep.last_modified;
                            t.segments = ep.segments;
                        }
                    }
                    diagnostics::StartupDiagnosticRecorder::Instance().UpdateProgress(
                        task_id, ep.downloaded_bytes, ep.total_bytes);
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_db_sync).count() >= 500) {
                        last_db_sync = now;
                        if (!ctrl->cancel_requested.load() && !ctrl->pause_requested.load()) {
                            m_store.UpdateTaskProgressAndSegments(
                                task_id, ep.state, ep.downloaded_bytes, ep.total_bytes, ep.range_supported, ep.segments);
                        }
                    }
                },
                m_global_limiter,
                &m_global_speed_limit_bps);

            if (res.ok() || ctrl->cancel_requested.load() || ctrl->pause_requested.load()) {
                break;
            }
        }
        if (!res.ok()) {
            if (ctrl->cancel_requested.load()) {
                local_task.state = TaskState::kCanceled;
                local_task.status_detail = "Canceled";
            } else if (ctrl->pause_requested.load()) {
                local_task.state = TaskState::kPaused;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = res.error->code;
                local_task.error_message = res.error->message;
                local_task.status_detail = "Failed: " + res.error->message;
                RADIUM_LOG_ERROR("Controller", "HTTP task " + task_id + " FAILED [" + local_task.error_code + "]: " + local_task.error_message + " (URL: " + local_task.url + ")");
            }
        }
        }
    } else {
        stream::StreamAdapterConfig scfg{
            m_cfg.nm3u8dl_binary_path,
            m_cfg.ffmpeg_binary_path,
            m_cfg.temp_root_dir,
            m_cfg.allow_loopback
        };
        stream::Nm3u8dlReAdapter adapter(std::move(scfg));
        Result<std::filesystem::path> res;
        bool is_yt = (task_type_str == "YOUTUBE_EXTRACTION" ||
                      local_task.url.find("googlevideo.com") != std::string::npos ||
                      local_task.url.find("youtube.com/watch") != std::string::npos ||
                      local_task.url.find("youtu.be/") != std::string::npos ||
                      local_task.url.find("youtube.com/shorts/") != std::string::npos ||
                      local_task.stream_options.cached_manifest_body.find("\"radiumType\":\"YOUTUBE_STREAM\"") != std::string::npos || local_task.stream_options.cached_manifest_body.find("\"fluxType\":\"YOUTUBE_STREAM\"") != std::string::npos ||
                      local_task.stream_options.cached_manifest_body.find("\"fluxType\": \"YOUTUBE_STREAM\"") != std::string::npos);
        auto progress_cb = [&](const stream::StreamProgress& sp) {
            if (ctrl->cancel_requested.load() || ctrl->pause_requested.load()) return;
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.find(task_id);
            if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                if (it->second.state == TaskState::kCanceled || it->second.state == TaskState::kPaused) return;
                if ((it->second.state == TaskState::kCompleted || it->second.state == TaskState::kFailed) &&
                    sp.phase != TaskState::kCompleted && sp.phase != TaskState::kFailed) {
                    RADIUM_LOG_WARN("Controller", "Ignoring stale stream progress task=" + task_id +
                                     " phase=" + TaskStateToString(sp.phase) + " after terminal state=" + TaskStateToString(it->second.state));
                    return;
                }
                auto& t = it->second;
                t.state = sp.phase;
                t.downloaded_bytes = sp.downloaded_bytes;
                t.total_bytes = sp.total_bytes;
                t.speed_bps = sp.speed_bps;
                t.stream_percent = sp.percent;
                t.completed_segments = sp.completed_segments;
                t.total_segments = sp.total_segments;
                t.status_detail = sp.status_line;
            }
            diagnostics::StartupDiagnosticRecorder::Instance().UpdateProgress(
                task_id, sp.downloaded_bytes, sp.total_bytes);
        };

        if (is_yt) {
            res = adapter.ExecuteYouTubeExtraction(
                local_task,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps.load(),
                progress_cb);
        } else {
            res = adapter.ExecuteStreamDownload(
                local_task,
                ctrl->pause_requested,
                ctrl->cancel_requested,
                ctrl->task_speed_limit_bps.load(),
                progress_cb);
        }
        if (!res.ok()) {
            if (ctrl->cancel_requested.load()) {
                local_task.state = TaskState::kCanceled;
                local_task.status_detail = "Canceled";
            } else if (ctrl->pause_requested.load()) {
                local_task.state = TaskState::kPaused;
                local_task.status_detail = "Paused";
            } else {
                local_task.state = TaskState::kFailed;
                local_task.error_code = res.error->code;
                local_task.error_message = res.error->message;
                local_task.status_detail = "Failed: " + res.error->message;
                RADIUM_LOG_ERROR("Controller", "Stream task " + task_id + " FAILED [" + local_task.error_code + "]: " + local_task.error_message + " (URL: " + local_task.url + ")");
            }
        }
    }

    local_task.speed_bps = 0;
    local_task.updated_at_ms = NowEpochMs();

    if (ctrl->cancel_requested.load()) {
        local_task.state = TaskState::kCanceled;
        local_task.status_detail = "Canceled";
    } else if (ctrl->pause_requested.load() && local_task.state != TaskState::kCompleted && local_task.state != TaskState::kFailed) {
        local_task.state = TaskState::kPaused;
        local_task.status_detail = "Paused";
    }

    if (local_task.state == TaskState::kCompleted) {
        // Post-Download Antivirus Scan (outside m_mutex to avoid locking controller for up to 30s)
        bool av_available = m_cfg.av_available_override ? m_cfg.av_available_override() : security::AntivirusScanner::IsWindowsDefenderAvailable();
        if (av_available) {
            std::filesystem::path final_p = std::filesystem::path(local_task.save_dir) / local_task.file_name;
            std::error_code ec;
            if (std::filesystem::exists(final_p, ec)) {
                // Update state to kFinalizing during scan so UI and IPC reflect that post-processing is running
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto it = m_tasks.find(task_id);
                    if (it != m_tasks.end() && !m_deleted_tasks.count(task_id)) {
                        it->second.state = TaskState::kFinalizing;
                        it->second.status_detail = "Finalizing output (Antivirus scan in progress)...";
                    }
                }
                auto scan_res = m_cfg.av_scan_override ? m_cfg.av_scan_override(final_p) : security::AntivirusScanner::ScanWithDefender(final_p);
                if (scan_res.ok()) {
                    if (scan_res.value->status == security::ScanStatus::kThreatDetected) {
                        local_task.state = TaskState::kFailed;
                        local_task.status_detail = "Threat detected by Antivirus (" + scan_res.value->scanner_name + ")";
                        local_task.error_code = "ERR_THREAT_DETECTED";
                        local_task.error_message = "A security threat was detected in the downloaded file.";
                        RADIUM_LOG_WARN("Controller", "Antivirus threat detected: " + final_p.string());
                        std::error_code rm_ec;
                        std::filesystem::remove(final_p, rm_ec);
                    } else if (scan_res.value->status == security::ScanStatus::kClean) {
                        local_task.state = TaskState::kCompleted;
                        local_task.status_detail = "Completed (Antivirus Clean)";
                    } else if (scan_res.value->status == security::ScanStatus::kScannerError) {
                        if (m_settings.av_strict_mode) {
                            local_task.state = TaskState::kFailed;
                            local_task.error_code = "ERR_AV_SCAN_FAILED";
                            local_task.error_message = "Antivirus scanner failed in strict mode";
                            local_task.status_detail = "Failed: Antivirus scanner error (Strict mode)";
                        } else {
                            local_task.status_detail = "Completed (Antivirus Scan Failed)";
                        }
                    } else {
                        local_task.state = TaskState::kCompleted;
                        local_task.status_detail = "Completed (Antivirus Skipped)";
                    }
                } else {
                    if (m_settings.av_strict_mode) {
                        local_task.state = TaskState::kFailed;
                        local_task.error_code = "ERR_AV_SCAN_FAILED";
                        local_task.error_message = "Antivirus scanner error: " + scan_res.error->message;
                        local_task.status_detail = "Failed: Antivirus scan error (Strict mode)";
                    } else {
                        local_task.status_detail = "Completed (Antivirus Scan Error: " + scan_res.error->message + ")";
                    }
                }
            }
        } else {
            if (m_settings.av_strict_mode) {
                local_task.state = TaskState::kFailed;
                local_task.error_code = "ERR_AV_UNAVAILABLE";
                local_task.error_message = "Antivirus scanner is unavailable in strict mode";
                local_task.status_detail = "Failed: Antivirus scanner unavailable (Strict mode)";
            } else {
                local_task.status_detail = "Completed (Not Scanned - Scanner Unavailable)";
            }
        }
    }

    // BUG-007 Fix: Granular merge under m_mutex without resurrecting deleted tasks
    bool should_persist = false;
    TaskModel to_save;
    TaskNotificationCallback callback;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_deleted_tasks.count(task_id)) {
            auto it = m_tasks.find(task_id);
            if (it != m_tasks.end()) {
                auto& t = it->second;
                // Terminal state protection: Never overwrite a user-canceled task with completed
                if (t.state == TaskState::kCanceled && local_task.state == TaskState::kCompleted) {
                    local_task.state = TaskState::kCanceled;
                    local_task.status_detail = "Canceled";
                }
                if (t.state == TaskState::kPaused && ctrl->pause_requested.load() &&
                    local_task.state != TaskState::kCanceled) {
                    local_task.state = TaskState::kPaused;
                    local_task.status_detail = "Paused";
                }
                // BUG-001 Fix: If the task has already been transitioned to kQueued (e.g. by ResumeTask or RefreshTaskUrl),
                // do NOT clobber it back to kPaused from this unwinding worker thread!
                if (t.state == TaskState::kQueued && local_task.state == TaskState::kPaused) {
                    // Retain kQueued so the scheduler can launch a new worker
                } else {
                    t.state = local_task.state;
                }
                t.file_name = local_task.file_name;
                t.category = local_task.category;
                t.part_path = local_task.part_path;
                t.total_bytes = local_task.total_bytes;
                t.downloaded_bytes = local_task.downloaded_bytes;
                t.speed_bps = 0;
                t.upload_speed_bps = 0;
                t.uploaded_bytes = local_task.uploaded_bytes;
                t.stream_percent = local_task.stream_percent;
                t.completed_segments = local_task.completed_segments;
                t.total_segments = local_task.total_segments;
                t.range_supported = local_task.range_supported;
                t.status_detail = local_task.status_detail;
                t.etag = local_task.etag;
                t.last_modified = local_task.last_modified;
                t.segments = local_task.segments;
                t.error_code = local_task.error_code;
                t.error_message = local_task.error_message;
                if (t.state == TaskState::kFailed &&
                    (t.error_code == "ERR_HTTP_403_EXPIRED_OR_AUTH" ||
                     t.error_code == "ERR_HTTP_AUTH_CHALLENGE" ||
                     t.error_code == "ERR_LINK_EXPIRED" ||
                     (t.error_code == "ERR_HTTP_404_NOT_FOUND" && IsExpiringSiteUrl(t.url)))) {
                    t.waits_for_address = true;
                }
                t.updated_at_ms = local_task.updated_at_ms;
                t.completed_at_ms = local_task.completed_at_ms;
                if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
                    t.auth_headers.clear();
                }

                const bool retry_pending = t.state == TaskState::kFailed &&
                    t.retry_count < m_settings.max_auto_retries &&
                    IsTransientRetryableError(t.error_code);
                if (t.state == TaskState::kFailed && !retry_pending &&
                    m_settings.show_completion_notification && !t.failure_notification_sent) {
                    // Persist failure_notification_sent before submitting the notification
                    // to guarantee at-most-once delivery across crashes and restarts.
                    t.failure_notification_sent = true;
                    m_notified_task_ids.insert(task_id);
                    callback = m_task_notification_callback;
                    notify = static_cast<bool>(callback);
                } else if (t.state == TaskState::kCompleted &&
                           m_settings.show_completion_notification &&
                           m_notified_task_ids.insert(task_id).second) {
                    callback = m_task_notification_callback;
                    notify = static_cast<bool>(callback);
                }

                to_save = t;
                should_persist = true;
            }
        }
    }
    if (should_persist) {
        auto persist_res = m_store.UpsertTask(to_save);
        if (notify && persist_res.ok()) {
            callback(TaskNotification{task_id, to_save.file_name,
                to_save.state == TaskState::kCompleted,
                to_save.state == TaskState::kCompleted ? "Download completed" : to_save.error_message});
        }

        // Record terminal status in startup diagnostic recorder
        diagnostics::StartupDiagnosticRecorder::Instance().CompleteSession(
            task_id,
            to_save.state == TaskState::kCompleted,
            to_save.error_message);

        if (to_save.state == TaskState::kCompleted) {
            std::filesystem::path final_p = std::filesystem::path(to_save.save_dir) / to_save.file_name;
            bool motw_enabled = true;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                motw_enabled = m_settings.mark_of_the_web;
            }
            platform::WindowsIntegrations::WriteZoneIdentifier(final_p, to_save.url, to_save.page_url, motw_enabled);
            if (to_save.post_action != PostCompletionAction::kNone) {
                platform::WindowsIntegrations::ExecutePostCompletionAction(to_save.post_action, final_p);
            }
        }
    }

    ctrl->finished.store(true);
    m_cv.notify_all();
}

Result<void> DownloadController::StartTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    if (it->second.state == TaskState::kPaused || it->second.state == TaskState::kFailed ||
        it->second.state == TaskState::kQueued) {
        it->second.scheduled = false;
        it->second.schedule_time_local.clear();
        it->second.schedule_stop_time_local.clear();
        it->second.pause_reason = PauseReason::kNone;
        it->second.state = TaskState::kQueued;
        m_notified_task_ids.erase(task_id);
        it->second.status_detail = "Queued";
        it->second.error_code.clear();
        it->second.error_message.clear();
        m_store.UpsertTask(it->second);
        m_cv.notify_all();
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::PauseTask(const std::string& task_id) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        it->second.pause_reason = PauseReason::kUser;
        if (it->second.state == TaskState::kQueued) {
            it->second.state = TaskState::kPaused;
            it->second.status_detail = "Paused";
            m_store.UpsertTask(it->second);
            return Result<void>::Ok();
        }
        auto ait = m_active.find(task_id);
        if (ait != m_active.end()) {
            // Do not synchronously wait for network workers on the UI/IPC caller.
            // The worker observes this flag, exits its current I/O operation, and
            // performs the final persistence merge below without resurrecting the task.
            ait->second->pause_requested.store(true);
            it->second.state = TaskState::kPaused;
            it->second.status_detail = "Pausing...";
            it->second.speed_bps = 0;
            m_store.UpsertTask(it->second);
        }
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::ResumeTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    if (it->second.state == TaskState::kPaused || it->second.state == TaskState::kFailed) {
        it->second.scheduled = false;
        it->second.schedule_time_local.clear();
        it->second.schedule_stop_time_local.clear();
        it->second.pause_reason = PauseReason::kNone;
        it->second.state = TaskState::kQueued;
        it->second.status_detail = "Queued (Resuming)";
        it->second.error_code.clear();
        it->second.error_message.clear();
        m_store.UpsertTask(it->second);
        m_cv.notify_all();
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::CancelTask(const std::string& task_id) {
    std::shared_ptr<ActiveTaskControl> ctrl;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        if (it->second.state == TaskState::kCompleted) {
            return Result<void>::Err("ERR_ALREADY_COMPLETED", "Completed task cannot be canceled");
        }
        auto ait = m_active.find(task_id);
        if (ait != m_active.end() && !ait->second->finished.load()) {
            ctrl = ait->second;
            ctrl->cancel_requested.store(true);
        } else {
            std::error_code ec;
            if (!it->second.part_path.empty()) {
                std::filesystem::remove_all(it->second.part_path, ec);
            }
            it->second.part_path.clear();
            it->second.segments.clear();
            it->second.downloaded_bytes = 0;
            it->second.speed_bps = 0;
            it->second.state = TaskState::kCanceled;
            it->second.status_detail = "Canceled";
            it->second.error_code.clear();
            it->second.error_message.clear();
            it->second.auth_headers.clear();
            m_store.UpsertTask(it->second);
            return Result<void>::Ok();
        }
    }
    if (ctrl) {
        while (!ctrl->finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it != m_tasks.end()) {
            std::error_code ec;
            if (!it->second.part_path.empty()) {
                std::filesystem::remove_all(it->second.part_path, ec);
            }
            it->second.part_path.clear();
            it->second.segments.clear();
            it->second.downloaded_bytes = 0;
            it->second.speed_bps = 0;
            it->second.state = TaskState::kCanceled;
            it->second.status_detail = "Canceled";
            it->second.error_code.clear();
            it->second.error_message.clear();
            it->second.auth_headers.clear();
            m_store.UpsertTask(it->second);
        }
    }
    return Result<void>::Ok();
}

Result<void> DownloadController::RetryTask(const std::string& task_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    auto& t = it->second;
    if (t.state != TaskState::kFailed && t.state != TaskState::kCanceled && t.state != TaskState::kPaused) {
        return Result<void>::Err("ERR_INVALID_STATE", "Only failed, canceled, or paused tasks can be retried");
    }

    // Deactivate any old schedules on Retry so task runs immediately
    t.scheduled = false;
    t.schedule_time_local.clear();
    t.schedule_stop_time_local.clear();
    t.pause_reason = PauseReason::kNone;

    // Clean up temporary files on Retry only for canceled tasks or unrecoverable validator/corruption errors
    if (t.state == TaskState::kCanceled ||
        t.error_code == "ERR_HTTP_VALIDATOR_MISMATCH" ||
        t.error_code == "ERR_STORAGE_PART_CORRUPTED" ||
        t.error_code == "ERR_STREAM_DRM_PROTECTED") {
        std::error_code ec;
        if (!t.part_path.empty()) {
            std::filesystem::remove_all(t.part_path, ec);
        }
        if (t.error_code == "ERR_HTTP_VALIDATOR_MISMATCH" || t.error_code == "ERR_STORAGE_PART_CORRUPTED") {
            t.part_path.clear();
            t.segments.clear();
            t.downloaded_bytes = 0;
            t.etag.clear();
            t.last_modified.clear();
        }
    }

    ++t.retry_count;
    t.state = TaskState::kQueued;
    t.status_detail = "Queued (Retry #" + std::to_string(t.retry_count) + ")";
    t.error_code.clear();
    t.error_message.clear();
    m_store.UpsertTask(t);
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<void> DownloadController::RemoveTask(const std::string& task_id, bool delete_file) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        if (m_deleted_tasks.count(task_id)) {
            return Result<void>::Err("ERR_TASK_REMOVING", "Task removal is already in progress");
        }
        m_deleted_tasks.insert(task_id);
    }

    // Stop active worker if running and cleanly join thread
    std::shared_ptr<ActiveTaskControl> ctrl;
    std::thread worker_to_join;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto ait = m_active.find(task_id);
        if (ait != m_active.end()) {
            ctrl = ait->second;
            ctrl->cancel_requested.store(true);
        }
    }
    if (ctrl) {
        auto start_wait = std::chrono::steady_clock::now();
        while (!ctrl->finished.load() && 
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_wait).count() < 3000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        auto ait = m_active.find(task_id);
        if (ait != m_active.end()) {
            if (ait->second->worker_thread.joinable()) {
                worker_to_join = std::move(ait->second->worker_thread);
            }
            m_active.erase(ait);
        }
    }
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }

    TaskModel task_snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) {
            m_deleted_tasks.erase(task_id);
            return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
        }
        task_snapshot = it->second;
    }

    auto fail_removal = [this, &task_id](const std::string& code, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deleted_tasks.erase(task_id);
        m_cv.notify_all();
        return Result<void>::Err(code, message);
    };

    // Clean only the task-owned partial/temp path. Keep the completed user output
    // untouched unless the explicit permanent-delete action was requested.
    if (!task_snapshot.part_path.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(task_snapshot.part_path, ec);
        if (ec) {
            RADIUM_LOG_ERROR("Controller", "Failed to remove partial output " + task_snapshot.part_path + ": " + ec.message());
            return fail_removal("ERR_PART_DELETE_FAILED", "Failed to remove partial download data: " + ec.message());
        }
    }

    // Delete physical file(s) permanently when requested (regardless of task state: paused, completed, failed)
    if (delete_file) {
        auto remove_output_path = [&](const std::string& relative_path, bool recursive) -> Result<void> {
            const auto output_path = ResolveTaskOutputPath(
                std::filesystem::u8path(task_snapshot.save_dir), std::filesystem::u8path(relative_path));
            if (!output_path.has_value()) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "Refusing to delete a path outside the download folder.");
            }

            std::error_code ec;
            const auto file_status = std::filesystem::symlink_status(*output_path, ec);
            if (ec == std::errc::no_such_file_or_directory ||
                (!ec && file_status.type() == std::filesystem::file_type::not_found)) {
                return Result<void>::Ok();
            }
            if (ec) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "Failed to inspect file on disk: " + ec.message());
            }
#ifdef _WIN32
            const DWORD attributes = GetFileAttributesW(output_path->c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "The downloaded file is read-only. Clear its read-only attribute and retry.");
            }
#endif
            if (recursive && file_status.type() == std::filesystem::file_type::directory) {
                std::filesystem::remove_all(*output_path, ec);
            } else {
                std::filesystem::remove(*output_path, ec);
            }
            if (ec) {
                return Result<void>::Err("ERR_FILE_DELETE_FAILED", "Failed to delete file from disk: " + ec.message());
            }
            return Result<void>::Ok();
        };

        if (task_snapshot.task_type == TaskType::kTorrent && !task_snapshot.torrent_file_paths.empty()) {
            // Multi-file torrents are written at their metadata-relative paths;
            // task.file_name is only the torrent's display/root name and may not
            // exist on disk. Delete each selected payload and propagate failures.
            for (size_t i = 0; i < task_snapshot.torrent_file_paths.size(); ++i) {
                if (i < task_snapshot.torrent_file_selection.size() && !task_snapshot.torrent_file_selection[i]) continue;
                auto result = remove_output_path(task_snapshot.torrent_file_paths[i], false);
                if (!result.ok()) {
                    RADIUM_LOG_ERROR("Controller", "Failed to permanently delete torrent payload for task " + task_id + ": " + result.error->message);
                    return fail_removal(result.error->code, result.error->message);
                }
            }
        } else {
            auto result = remove_output_path(task_snapshot.file_name, true);
            if (!result.ok()) {
                RADIUM_LOG_ERROR("Controller", "Failed to permanently delete output for task " + task_id + ": " + result.error->message);
                return fail_removal(result.error->code, result.error->message);
            }
        }

        // Clean up partial extension file if present
        if (task_snapshot.task_type != TaskType::kTorrent) {
            auto part_ext_path = ResolveTaskOutputPath(
                std::filesystem::u8path(task_snapshot.save_dir), std::filesystem::u8path(task_snapshot.file_name + ".part"));
            if (!part_ext_path.has_value()) {
                return fail_removal("ERR_FILE_DELETE_FAILED", "Refusing to delete a partial file outside the download folder.");
            }
            std::error_code part_ec;
            std::filesystem::remove(*part_ext_path, part_ec);
            if (part_ec && part_ec != std::errc::no_such_file_or_directory) {
                return fail_removal("ERR_FILE_DELETE_FAILED", "Failed to delete partial output file: " + part_ec.message());
            }
        }
    }

    // Persist removal before erasing the in-memory model so a storage failure
    // cannot make the task disappear only until the next refresh/restart.
    auto db_result = m_store.DeleteTask(task_id);
    if (!db_result.ok()) {
        return fail_removal(db_result.error->code, db_result.error->message);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.erase(task_id);
        // Worker completion is synchronized above; no callback can resurrect it.
        m_deleted_tasks.erase(task_id);
    }
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<void> DownloadController::UpdateTask(const TaskModel& task) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task.task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task.task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    it->second = task;
    it->second.updated_at_ms = NowEpochMs();
    auto db_res = m_store.UpsertTask(it->second);
    if (!db_res.ok()) {
        return Result<void>::Err(db_res.error->code, db_res.error->message);
    }
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<int> DownloadController::ClearCompletedHistory() {
    std::vector<std::string> to_remove;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
                to_remove.push_back(id);
            }
        }
        for (const auto& id : to_remove) {
            m_deleted_tasks.insert(id);
            m_tasks.erase(id);
        }
    }
    for (const auto& id : to_remove) {
        m_store.DeleteTask(id);
    }
    return Result<int>::Ok(static_cast<int>(to_remove.size()));
}

void DownloadController::PauseAllTasks() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
                t.state == TaskState::kDownloading || t.state == TaskState::kMuxing) {
                ids.push_back(id);
                auto ait = m_active.find(id);
                if (ait != m_active.end()) {
                    ait->second->pause_requested.store(true);
                }
            }
        }
    }
    for (const auto& id : ids) {
        PauseTask(id);
    }
}

void DownloadController::ResumeAllTasks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, t] : m_tasks) {
        if (t.state == TaskState::kPaused && (!t.scheduled || t.pause_reason == PauseReason::kUser)) {
            // Resume All is an explicit user action and has the same meaning as
            // Resume on an individual task, including clearing its old schedule.
            t.scheduled = false;
            t.schedule_time_local.clear();
            t.schedule_stop_time_local.clear();
            t.state = TaskState::kQueued;
            t.pause_reason = PauseReason::kNone;
            t.status_detail = "Queued";
            m_store.UpsertTask(t);
        }
    }
    m_cv.notify_all();
}

void DownloadController::StartAllTasks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, t] : m_tasks) {
        if ((t.state == TaskState::kPaused || t.state == TaskState::kFailed || t.state == TaskState::kQueued) && !t.scheduled) {
            t.scheduled = false;
            t.schedule_time_local.clear();
            t.schedule_stop_time_local.clear();
            t.pause_reason = PauseReason::kNone;
            t.state = TaskState::kQueued;
            t.status_detail = "Queued";
            t.error_code.clear();
            t.error_message.clear();
            m_store.UpsertTask(t);
        }
    }
    m_cv.notify_all();
}

Result<void> DownloadController::RefreshTaskUrl(
    const std::string& task_id,
    const std::string& new_url,
    const std::map<std::string, std::string>& new_headers,
    const std::map<std::string, std::string>& new_auth_headers) {

    PauseTask(task_id);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");

    it->second.url = new_url;

    // Clean replacement of auth headers
    if (!new_auth_headers.empty()) {
        it->second.auth_headers = new_auth_headers;
    }

    // Clean replacement of cookies (XDM rule: refreshed cookie replaces stale one instead of merging)
    bool has_new_cookie = false;
    for (const auto& [k, v] : new_headers) {
        std::string kl = k;
        for (char& c : kl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (kl == "cookie") {
            has_new_cookie = true;
            break;
        }
    }
    if (has_new_cookie) {
        for (auto hit = it->second.headers.begin(); hit != it->second.headers.end();) {
            std::string kl = hit->first;
            for (char& c : kl) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (kl == "cookie") {
                hit = it->second.headers.erase(hit);
            } else {
                ++hit;
            }
        }
    }
    for (const auto& [k, v] : new_headers) it->second.headers[k] = v;

    it->second.retry_count = 0;
    it->second.waits_for_address = false;
    m_notified_task_ids.erase(task_id);
    it->second.state = TaskState::kQueued;
    it->second.status_detail = "Queued (Refreshed URL)";
    it->second.error_code.clear();
    it->second.error_message.clear();
    m_store.UpsertTask(it->second);
    m_cv.notify_all();
    return Result<void>::Ok();
}

std::string DownloadController::ExtractRegistrableDomain(const std::string& url) {
    auto pu = brisk::HttpClient::ParseUrl(url);
    if (!pu.ok() || pu.value->host.empty()) return {};
    std::string host = pu.value->host;
    while (!host.empty() && host.back() == '.') host.pop_back();
    for (char& c : host) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (host.find(':') != std::string::npos) return host; // IPv6 literal

    bool all_digits_or_dots = !host.empty();
    for (char c : host) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') {
            all_digits_or_dots = false;
            break;
        }
    }
    if (all_digits_or_dots) return host; // IPv4 literal

    std::vector<std::string> labels;
    size_t start = 0;
    while (start <= host.size()) {
        size_t dot = host.find('.', start);
        if (dot == std::string::npos) {
            if (start < host.size()) labels.push_back(host.substr(start));
            break;
        }
        labels.push_back(host.substr(start, dot - start));
        start = dot + 1;
    }
    const size_t n = labels.size();
    if (n <= 2) return host;

    static constexpr const char* kCountrySecondLevel[] = {
        "co", "com", "net", "org", "gov", "edu", "ac", "or", "ne", "go", "gob", "nic", "mil"
    };
    bool country = (labels[n - 1].size() == 2);
    if (country) {
        bool found = false;
        for (const char* sld : kCountrySecondLevel) {
            if (labels[n - 2] == sld) {
                found = true;
                break;
            }
        }
        country = found;
    }
    const size_t keep = country ? 3 : 2;
    const size_t begin_idx = (n > keep) ? (n - keep) : 0;
    std::string result;
    for (size_t i = begin_idx; i < n; ++i) {
        if (!result.empty()) result += '.';
        result += labels[i];
    }
    return result;
}

bool DownloadController::IsExpiringSiteUrl(const std::string& url) {
    const std::string site = ExtractRegistrableDomain(url);
    return site == "rapidgator.net" || site == "rg.to";
}

std::optional<TaskModel> DownloadController::TryMatchFreshLink(const TaskModel& offer) {
    if (offer.url.empty() || offer.file_name.empty()) return std::nullopt;
    const std::string offer_site = ExtractRegistrableDomain(offer.url);
    const std::string offer_page_site = offer.page_url.empty() ? std::string{} : ExtractRegistrableDomain(offer.page_url);

    auto eq_ignore_case = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };

    std::string best_id;
    int best_score = 0;
    int64_t best_created_at = -1;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [tid, item] : m_tasks) {
            if (item.task_type != TaskType::kHttpFile) continue;
            if (item.state == TaskState::kCompleted || item.state == TaskState::kCanceled) continue;
            if (item.url == offer.url) continue;

            const bool eligible_state =
                item.waits_for_address ||
                ((item.state == TaskState::kFailed || item.state == TaskState::kPaused) &&
                 (item.error_code == "ERR_HTTP_403_EXPIRED_OR_AUTH" ||
                  item.error_code == "ERR_HTTP_AUTH_CHALLENGE" ||
                  item.error_code == "ERR_LINK_EXPIRED"));
            if (!eligible_state) continue;

            const std::string item_site = ExtractRegistrableDomain(item.url);
            const std::string item_page_site = item.page_url.empty() ? std::string{} : ExtractRegistrableDomain(item.page_url);
            const bool same_site =
                (!item_site.empty() && item_site == offer_site) ||
                (!item_page_site.empty() && item_page_site == offer_page_site);
            if (!same_site) continue;

            if (!eq_ignore_case(item.file_name, offer.file_name)) continue;

            if (item.total_bytes > 0 && offer.total_bytes > 0 && item.total_bytes != offer.total_bytes) {
                continue;
            }

            const int score = (item.total_bytes > 0 && offer.total_bytes > 0 && item.total_bytes == offer.total_bytes) ? 2 : 1;
            if (score > best_score || (score == best_score && item.created_at_ms > best_created_at)) {
                best_score = score;
                best_created_at = item.created_at_ms;
                best_id = tid;
            }
        }
    }

    if (best_id.empty()) return std::nullopt;

    if (!offer.post_body.empty() || offer.http_method == "POST") {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(best_id);
        if (it != m_tasks.end()) {
            it->second.http_method = offer.http_method;
            it->second.post_body = offer.post_body;
            it->second.post_content_type = offer.post_content_type;
        }
    }

    auto ref_res = RefreshTaskUrl(best_id, offer.url, offer.headers, offer.auth_headers);
    if (!ref_res.ok()) return std::nullopt;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(best_id);
    if (it == m_tasks.end()) return std::nullopt;
    RADIUM_LOG_INFO("Controller", "Freshlink matched offer for '" + offer.file_name + "' -> resumed task " + best_id);
    return it->second;
}

void DownloadController::SetMaxConcurrentTasks(uint32_t max_tasks) {
    uint32_t clamped = std::clamp(max_tasks, 1u, 16u);
    m_max_concurrent.store(clamped);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings.max_concurrent_tasks = clamped;
        SaveSettingsLocked();
    }
    m_cv.notify_all();
}

uint32_t DownloadController::GetMaxConcurrentTasks() const {
    return m_max_concurrent.load();
}

void DownloadController::SetGlobalSpeedLimit(uint64_t bps) {
    m_global_speed_limit_bps.store(bps);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings.speed_limit_bps = bps;
    SaveSettingsLocked();
}

uint64_t DownloadController::GetGlobalSpeedLimit() const {
    return m_global_speed_limit_bps.load();
}

Result<void> DownloadController::SetTaskSpeedLimit(const std::string& task_id, uint64_t bps) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    it->second.speed_limit_bps = bps;
    auto ait = m_active.find(task_id);
    if (ait != m_active.end() && ait->second) {
        ait->second->task_speed_limit_bps.store(bps);
    }
    m_store.UpsertTask(it->second);
    return Result<void>::Ok();
}

uint64_t DownloadController::GetTaskSpeedLimit(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) return it->second.speed_limit_bps;
    return 0;
}

Result<void> DownloadController::AddAlternateUrl(const std::string& task_id, const std::string& alt_url) {
    if (alt_url.empty()) {
        return Result<void>::Err("ERR_INVALID_URL", "Alternate URL cannot be empty");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    for (const auto& existing : it->second.alternate_urls) {
        if (existing == alt_url) return Result<void>::Ok();
    }
    it->second.alternate_urls.push_back(alt_url);
    m_store.UpsertTask(it->second);
    return Result<void>::Ok();
}

std::vector<std::string> DownloadController::GetAlternateUrls(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) return it->second.alternate_urls;
    return {};
}

Result<void> DownloadController::SetTaskSchedule(
    const std::string& task_id,
    const std::string& start_time_local,
    const std::string& stop_time_local) {
    if (!IsValidTimeFormat(start_time_local)) {
        return Result<void>::Err("ERR_INVALID_SCHEDULE", "Invalid start time format (expected HH:MM 00:00-23:59)");
    }
    if (!IsValidTimeFormat(stop_time_local)) {
        return Result<void>::Err("ERR_INVALID_SCHEDULE", "Invalid stop time format (expected HH:MM 00:00-23:59)");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end() || m_deleted_tasks.count(task_id)) {
        return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    }
    const bool user_paused = it->second.pause_reason == PauseReason::kUser &&
                             it->second.state != TaskState::kCompleted &&
                             it->second.state != TaskState::kCanceled;
    const bool was_schedule_paused = it->second.state == TaskState::kPaused &&
                                     it->second.pause_reason == PauseReason::kQueueSchedule;
    it->second.scheduled = !start_time_local.empty();
    it->second.schedule_time_local = start_time_local;
    it->second.schedule_stop_time_local = stop_time_local;
    if (user_paused) {
        // User intent outranks task and queue schedules. Editing a schedule must
        // never clear an explicit pause or make the task eligible for auto-retry.
        it->second.pause_reason = PauseReason::kUser;
        if (it->second.state == TaskState::kPaused) {
            it->second.status_detail = "Paused (User)";
        }
    } else if (it->second.scheduled) {
        it->second.pause_reason = PauseReason::kQueueSchedule;
        if (it->second.state == TaskState::kQueued) {
            it->second.state = TaskState::kPaused;
            it->second.status_detail = "Scheduled (" + start_time_local + ")";
        }
    } else if (was_schedule_paused) {
        // Removing a schedule does not itself start the task. Resume remains an
        // explicit user action, matching the behavior for other paused tasks.
        it->second.pause_reason = PauseReason::kNone;
        it->second.status_detail = "Paused (Schedule Removed)";
    }
    m_store.UpsertTask(it->second);
    m_cv.notify_all();
    return Result<void>::Ok();
}

AppSettings DownloadController::GetSettings() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_settings;
}

Result<void> DownloadController::RememberDownloadDirectory(const std::string& directory) {
    if (directory.empty()) return Result<void>::Err("ERR_INVALID_DOWNLOAD_DIR", "Download directory cannot be empty");
    std::error_code ec;
    const auto path = std::filesystem::weakly_canonical(std::filesystem::path(directory), ec);
    if (ec || !std::filesystem::is_directory(path, ec))
        return Result<void>::Err("ERR_INVALID_DOWNLOAD_DIR", "Selected download directory is not available");
    std::lock_guard<std::mutex> lock(m_mutex);
    m_settings.last_download_dir = path.string();
    return SaveSettingsLocked();
}

void DownloadController::SetTaskNotificationCallback(TaskNotificationCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_task_notification_callback = std::move(callback);
}

Result<void> DownloadController::UpdateSettings(const AppSettings& settings) {
    if (settings.proxy_mode != "direct" && settings.proxy_mode != "system" && settings.proxy_mode != "manual")
        return Result<void>::Err("ERR_SETTINGS_PROXY_MODE", "Proxy mode must be direct, system, or manual");
    if (settings.proxy_mode == "manual" && settings.proxy_server.empty())
        return Result<void>::Err("ERR_SETTINGS_PROXY_SERVER", "Manual proxy mode requires a server address");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool directory_changed = settings.default_download_dir != m_settings.default_download_dir;
        m_settings = settings;
        if (directory_changed) {
            const auto factory_dir = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir()).string();
            m_settings.default_download_dir_user_selected = settings.default_download_dir != factory_dir;
            m_settings.last_download_dir = settings.default_download_dir;
        }
        m_settings.max_concurrent_tasks = std::clamp(m_settings.max_concurrent_tasks, 1u, 16u);
        m_settings.default_connections_per_task = std::clamp(m_settings.default_connections_per_task, 1u, 32u);
        m_settings.stall_timeout_ms = std::clamp(m_settings.stall_timeout_ms, 500, 60000);
        m_settings.min_split_bytes = std::clamp<uint64_t>(m_settings.min_split_bytes, 65536ULL, 10485760ULL);
        m_settings.min_capture_size_bytes = std::clamp<uint64_t>(m_settings.min_capture_size_bytes, 0ULL, 10737418240ULL);
        m_settings.max_auto_retries = std::clamp<uint32_t>(m_settings.max_auto_retries, 0u, 50u);
        m_settings.retry_interval_sec = std::clamp<uint32_t>(m_settings.retry_interval_sec, 1u, 3600u);
        if (m_settings.theme != "coral_dark" && m_settings.theme != "dark" && m_settings.theme != "light" && m_settings.theme != "system" && m_settings.theme != "classic_blue") {
            m_settings.theme = "coral_dark";
        }
        static const std::set<std::string> supported_languages{
            "en", "ja", "zh-CN", "zh-TW", "ko", "es", "fr", "vi", "pt-BR", "de-DE", "ru-RU", "it-IT",
            "nl-NL", "pl-PL", "tr-TR", "id-ID", "ar-SA", "fa-IR", "uk-UA", "cs-CZ", "ro-RO", "th-TH", "sv-SE", "el-GR"
        };
        if (!supported_languages.contains(m_settings.language)) m_settings.language = "en";
        if (!m_settings.default_download_dir.empty()) {
            m_cfg.default_download_dir = m_settings.default_download_dir;
            std::error_code ec;
            std::filesystem::create_directories(m_cfg.default_download_dir, ec);
        }
        m_cfg.stall_timeout_ms = m_settings.stall_timeout_ms;
        m_cfg.min_split_bytes = m_settings.min_split_bytes;
        m_max_concurrent.store(m_settings.max_concurrent_tasks);
        m_global_speed_limit_bps.store(m_settings.speed_limit_bps);
        auto sres = SaveSettingsLocked();
        if (!sres.ok()) return sres;
    }
    m_cv.notify_all();
    return Result<void>::Ok();
}

Result<AppSettings> DownloadController::RestoreDefaultSettings() {
    AppSettings defaults;
    defaults.theme = std::string(radium::config::DefaultSettingsConfig::kTheme);
    defaults.max_concurrent_tasks = radium::config::DefaultSettingsConfig::kMaxConcurrentTasks;
    defaults.speed_limit_bps = radium::config::DefaultSettingsConfig::kSpeedLimitBps;
    defaults.default_connections_per_task = radium::config::DefaultSettingsConfig::kDefaultConnectionsPerTask;
    defaults.default_download_dir = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir()).string();
    defaults.default_download_dir_user_selected = false;
    defaults.conflict_policy = ConflictPolicy::kPrompt;
    defaults.auto_start_downloads = radium::config::DefaultSettingsConfig::kAutoStartDownloads;
    defaults.stall_timeout_ms = radium::config::DefaultSettingsConfig::kStallTimeoutMs;
    defaults.min_split_bytes = radium::config::DefaultSettingsConfig::kMinSplitBytes;
    defaults.browser_capture_enabled = radium::config::DefaultSettingsConfig::kBrowserCaptureEnabled;
    defaults.min_capture_size_bytes = radium::config::DefaultSettingsConfig::kMinCaptureSizeBytes;
    defaults.close_to_tray = radium::config::DefaultSettingsConfig::kCloseToTray;
    defaults.start_minimized = radium::config::DefaultSettingsConfig::kStartMinimized;
    defaults.reduced_motion = radium::config::DefaultSettingsConfig::kReducedMotion;
    defaults.show_completion_notification = radium::config::DefaultSettingsConfig::kShowCompletionNotification;

    defaults.proxy_mode = std::string(radium::config::DefaultSettingsConfig::kProxyMode);
    defaults.proxy_server = "";
    defaults.proxy_bypass = "";
    defaults.proxy_username = "";
    defaults.proxy_password_encrypted = "";
    defaults.server_auth_username = "";
    defaults.server_auth_password_encrypted = "";
    defaults.custom_extension_categories = {};
    defaults.site_rules = {};
    defaults.max_auto_retries = radium::config::DefaultSettingsConfig::kMaxAutoRetries;
    defaults.retry_interval_sec = radium::config::DefaultSettingsConfig::kRetryIntervalSec;
    defaults.queue_completion_action = std::string(radium::config::DefaultSettingsConfig::kQueueCompletionAction);

    auto ures = UpdateSettings(defaults);
    if (!ures.ok()) return Result<AppSettings>::Err(ures.error->code, ures.error->message);
    return Result<AppSettings>::Ok(defaults);
}

bool DownloadController::AreWorkersAvailable() const {
    std::error_code ec;
    return std::filesystem::exists(m_cfg.nm3u8dl_binary_path, ec) &&
           std::filesystem::exists(m_cfg.ffmpeg_binary_path, ec);
}

void DownloadController::OnSystemSuspend() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, t] : m_tasks) {
            if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
                t.state == TaskState::kDownloading || t.state == TaskState::kMuxing) {
                ids.push_back(id);
            }
        }
    }
    for (const auto& id : ids) {
        PauseTask(id);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& id : ids) {
            auto it = m_tasks.find(id);
            if (it != m_tasks.end() && it->second.state == TaskState::kPaused) {
                it->second.pause_reason = PauseReason::kSystemSuspend;
                it->second.status_detail = "Paused (System Suspend)";
                m_store.UpsertTask(it->second);
            }
        }
    }
}

void DownloadController::OnSystemResume() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& [id, t] : m_tasks) {
        // Only resume tasks that were automatically paused due to system suspend (CTRL-02)
        // User-paused tasks (pause_reason == kUser) remain strictly paused!
        if (t.state == TaskState::kPaused && t.pause_reason == PauseReason::kSystemSuspend && !t.scheduled) {
            t.state = TaskState::kQueued;
            t.pause_reason = PauseReason::kNone;
            t.status_detail = "Queued (Resumed after sleep)";
            m_store.UpsertTask(t);
        }
    }
    m_cv.notify_all();
}

std::vector<TaskModel> DownloadController::ListTasks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TaskModel> out;
    out.reserve(m_tasks.size());
    for (const auto& [id, t] : m_tasks) {
        out.push_back(t);
    }
    std::sort(out.begin(), out.end(), [](const TaskModel& a, const TaskModel& b) {
        return a.created_at_ms > b.created_at_ms;
    });
    return out;
}

std::optional<TaskModel> DownloadController::GetTask(const std::string& task_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it != m_tasks.end()) return it->second;
    return std::nullopt;
}

bool DownloadController::HasActiveTasks() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [id, t] : m_tasks) {
        if (t.state == TaskState::kDownloading ||
            t.state == TaskState::kProbing ||
            t.state == TaskState::kMuxing ||
            t.state == TaskState::kVerifying ||
            t.state == TaskState::kFinalizing) {
            return true;
        }
    }
    return false;
}

size_t DownloadController::GetTaskCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_tasks.size();
}

Result<TaskModel> DownloadController::WaitForTaskCompletion(const std::string& task_id, int timeout_ms) {
    auto last_progress = std::chrono::steady_clock::now();
    uint64_t last_bytes = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_tasks.find(task_id);
            if (it == m_tasks.end()) {
                return Result<TaskModel>::Err("ERR_NOT_FOUND", "Task not found");
            }
            auto st = it->second.state;
            auto ait = m_active.find(task_id);
            bool worker_done = (ait == m_active.end() || ait->second->finished.load());
            if (worker_done && (st == TaskState::kCompleted || st == TaskState::kFailed ||
                                st == TaskState::kPaused || st == TaskState::kCanceled)) {
                return Result<TaskModel>::Ok(it->second);
            }
            if (it->second.downloaded_bytes > last_bytes) {
                last_bytes = it->second.downloaded_bytes;
                last_progress = std::chrono::steady_clock::now();
            }
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_progress).count();
        if (timeout_ms > 0 && elapsed > timeout_ms) {
            return Result<TaskModel>::Err("ERR_TIMEOUT", "Timed out waiting for task completion");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

Result<void> DownloadController::UpsertQueue(const QueueConfig& queue) {
    return m_store.UpsertQueue(queue);
}

std::vector<QueueConfig> DownloadController::ListQueues() {
    auto res = m_store.ListQueues();
    return res.ok() ? res.value.value() : std::vector<QueueConfig>{};
}

Result<void> DownloadController::MoveTaskToQueue(const std::string& task_id, const std::string& new_queue_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tasks.find(task_id);
    if (it == m_tasks.end()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    it->second.queue_id = new_queue_id;
    m_store.UpsertTask(it->second);
    return Result<void>::Ok();
}

Result<std::vector<crawler::GrabbedItem>> DownloadController::CrawlSite(
    const crawler::SiteGrabberConfig& cfg,
    std::atomic<bool>& cancel_requested,
    crawler::GrabbedItemCallback on_item_found) {
    return crawler::SiteGrabber::Crawl(cfg, cancel_requested, on_item_found);
}

Result<security::ScanReport> DownloadController::ScanTaskFile(const std::string& task_id) {
    std::filesystem::path file_p;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_tasks.find(task_id);
        if (it == m_tasks.end()) return Result<security::ScanReport>::Err("ERR_NOT_FOUND", "Task not found");
        file_p = std::filesystem::path(it->second.save_dir) / it->second.file_name;
    }
    return security::AntivirusScanner::ScanWithDefender(file_p);
}

Result<archive::ZipArchiveSummary> DownloadController::PreviewZipArchive(const std::filesystem::path& zip_path) {
    return archive::ZipPreviewer::InspectArchive(zip_path);
}

Result<std::string> DownloadController::ExportTasksData(storage::ExportFormat format, bool include_credentials) {
    std::vector<TaskModel> task_list = ListTasks();
    return storage::TaskImportExport::ExportTasksToString(task_list, format, include_credentials);
}

Result<int> DownloadController::ImportTasksData(const std::string& data, storage::ExportFormat format) {
    Result<std::vector<TaskModel>> imported;
    if (format == storage::ExportFormat::kJson) {
        imported = storage::TaskImportExport::ImportFromJson(data);
    } else {
        imported = storage::TaskImportExport::ImportFromCsv(data);
    }
    if (!imported.ok()) return Result<int>::Err(imported.error->code, imported.error->message);

    int count = 0;
    for (auto& t : imported.value.value()) {
        auto add_res = AddTask(std::move(t), true, false);
        if (add_res.ok()) count++;
    }
    return Result<int>::Ok(count);
}

Result<std::vector<uint8_t>> DownloadController::CreateTorrent(
    const std::filesystem::path& source_path,
    const torrent::TorrentCreatorOptions& options) {
    return torrent::TorrentCreator::CreateTorrent(source_path, options);
}

Result<void> DownloadController::CreateTorrentFile(
    const std::filesystem::path& source_path,
    const std::filesystem::path& output_torrent_path,
    const torrent::TorrentCreatorOptions& options) {
    return torrent::TorrentCreator::CreateTorrentFile(source_path, output_torrent_path, options);
}

Result<void> DownloadController::AddTrackersToTask(const std::string& task_id, const std::vector<std::string>& new_trackers) {
    return torrent::TorrentEngine::AddTrackersToTask(task_id, new_trackers);
}

Result<torrent::TorrentSwarmDetails> DownloadController::GetTorrentSwarmDetails(const std::string& task_id) {
    return torrent::TorrentEngine::GetSwarmDetails(task_id);
}

Result<void> DownloadController::SetTorrentSequentialMode(const std::string& task_id, bool enable) {
    return torrent::TorrentEngine::SetSequentialMode(task_id, enable);
}

void SlidingWindowQuota::Record(
    std::chrono::steady_clock::time_point now,
    const std::vector<std::pair<std::string, uint64_t>>& progress) {
    uint64_t delta = 0;
    for (const auto& [id, bytes] : progress) {
        auto it = m_last_bytes.find(id);
        uint64_t prev = (it == m_last_bytes.end()) ? bytes : it->second;
        m_last_bytes[id] = bytes;
        if (bytes > prev) delta += (bytes - prev);
    }
    if (delta == 0) return;
    if (!m_samples.empty() && (now - m_samples.back().first) < std::chrono::seconds(1)) {
        m_samples.back().second += delta;
    } else {
        m_samples.emplace_back(now, delta);
    }
}

uint64_t SlidingWindowQuota::Used(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration window) {
    while (!m_samples.empty() && (now - m_samples.front().first) >= window) {
        m_samples.pop_front();
    }
    uint64_t total = 0;
    for (const auto& [_, n] : m_samples) total += n;
    return total;
}

std::optional<bool> SlidingWindowQuota::Check(
    std::chrono::steady_clock::time_point now,
    uint64_t limit_bytes,
    std::chrono::steady_clock::duration window) {
    uint64_t used = Used(now, window);
    if (!blocked && used >= limit_bytes) {
        blocked = true;
        return true;
    }
    if (blocked && used < limit_bytes) {
        blocked = false;
        return false;
    }
    return std::nullopt;
}

std::chrono::steady_clock::duration SlidingWindowQuota::RoomIn(
    std::chrono::steady_clock::time_point now,
    uint64_t limit_bytes,
    std::chrono::steady_clock::duration window) {
    uint64_t used = Used(now, window);
    for (const auto& [at, n] : m_samples) {
        if (used < limit_bytes) break;
        used = (used > n) ? (used - n) : 0;
        if (used < limit_bytes) {
            auto expires = at + window;
            return (expires > now) ? (expires - now) : std::chrono::steady_clock::duration::zero();
        }
    }
    return std::chrono::steady_clock::duration::zero();
}

DownloadController::ExtensionOnboardingState DownloadController::GetExtensionOnboardingState() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    ExtensionOnboardingState state;
    auto res = m_store.GetSetting("extension_onboarding", "{}");
    if (!res.ok() || res.value->empty()) return state;
    try {
        auto val = json::Parse(*res.value);
        if (!val.is_object()) return state;
        state.completed = val["completed"].as_bool(false);
        state.action = val["action"].as_string("");
        state.installed_version = val["installedVersion"].as_string("");
        state.installed_commit = val["installedCommit"].as_string("");
        state.install_dir = val["installDir"].as_string("");
    } catch (...) {}
    return state;
}

bool DownloadController::ShouldShowExtensionOnboarding() const {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_tasks.empty()) {
            return false;
        }
    }
    return !GetExtensionOnboardingState().completed;
}

Result<void> DownloadController::MarkExtensionOnboardingDisplayed(const std::string& action) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ExtensionOnboardingState state;
    if (auto res = m_store.GetSetting("extension_onboarding", "{}"); res.ok() && !res.value->empty()) {
        try {
            auto val = json::Parse(*res.value);
            if (val.is_object()) {
                state.completed = val["completed"].as_bool(false);
                state.action = val["action"].as_string("");
                state.installed_version = val["installedVersion"].as_string("");
                state.installed_commit = val["installedCommit"].as_string("");
                state.install_dir = val["installDir"].as_string("");
            }
        } catch (...) {}
    }

    json::Value obj = json::Object{};
    obj["completed"] = true;
    obj["action"] = action.empty() ? state.action : action;
    obj["installedVersion"] = state.installed_version;
    obj["installedCommit"] = state.installed_commit;
    obj["installDir"] = state.install_dir;
    return m_store.SetSetting("extension_onboarding", obj.dump());
}

Result<void> DownloadController::RecordInstalledExtension(
    const std::string& version,
    const std::string& commit_sha,
    const std::string& install_dir) {
    std::lock_guard<std::mutex> lock(m_mutex);
    bool was_completed = true;
    std::string prev_action = "downloaded";
    if (auto res = m_store.GetSetting("extension_onboarding", "{}"); res.ok() && !res.value->empty()) {
        try {
            auto val = json::Parse(*res.value);
            if (val.is_object()) {
                was_completed = val["completed"].as_bool(true);
                std::string existing_action = val["action"].as_string("");
                if (!existing_action.empty()) prev_action = existing_action;
            }
        } catch (...) {}
    }

    json::Value obj = json::Object{};
    obj["completed"] = was_completed;
    obj["action"] = prev_action;
    obj["installedVersion"] = version;
    obj["installedCommit"] = commit_sha;
    obj["installDir"] = install_dir;
    return m_store.SetSetting("extension_onboarding", obj.dump());
}

Result<std::string> DownloadController::BeginSpeculativeOffer(
    TaskModel draft_task,
    SpeculativeDecisionCallback on_decision,
    int64_t ipc_recv_qpc_us) {
    const int64_t t2 = (ipc_recv_qpc_us > 0) ? ipc_recv_qpc_us : QpcNowUs();
    if (draft_task.url.empty()) {
        return Result<std::string>::Err("ERR_INVALID_URL", "Task URL cannot be empty");
    }

    if (draft_task.task_id.empty()) {
        draft_task.task_id = GenerateUuidV4();
    }
    const std::string tx_id = draft_task.task_id;

    // Resolve filename, category, and save_dir in-memory without blocking on disk/network
    AppSettings settings_copy;
    std::filesystem::path default_dl_dir;
    std::filesystem::path temp_root;
    bool allow_loopback = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        settings_copy = m_settings;
        default_dl_dir = m_cfg.default_download_dir;
        temp_root = m_cfg.temp_root_dir;
        allow_loopback = m_cfg.allow_loopback;

        // Reap completed/cancelled speculative transactions whose threads are finished
        for (auto it = m_pending_txs.begin(); it != m_pending_txs.end();) {
            auto& old_tx = it->second;
            bool reapable = false;
            {
                std::lock_guard<std::mutex> tx_lk(old_tx->mu);
                reapable = old_tx->dest_check_done &&
                           (!old_tx->speculative_eligible || old_tx->engine_finished) &&
                           (old_tx->cancelled || old_tx->accepted ||
                            old_tx->state == SpeculativeTxState::kConflictRejected);
            }
            if (reapable) {
                if (old_tx->dest_thread.joinable()) old_tx->dest_thread.join();
                if (old_tx->bg_thread.joinable()) old_tx->bg_thread.join();
                it = m_pending_txs.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (draft_task.file_name.empty()) {
        std::string fallback_ext = (draft_task.task_type == TaskType::kHttpFile)
            ? ""
            : draft_task.stream_options.output_format;
        draft_task.file_name = PathSanitizer::ExtractFromUrl(draft_task.url, fallback_ext);
    } else {
        draft_task.file_name = PathSanitizer::SanitizeFileName(draft_task.file_name);
    }
    if (draft_task.category.empty() || draft_task.category == "General") {
        draft_task.category = DetermineCategoryImpl(settings_copy, draft_task.file_name, draft_task.url);
    }
    if (draft_task.save_dir.empty()) {
        if (!settings_copy.last_download_dir.empty()) {
            draft_task.save_dir = settings_copy.last_download_dir;
        } else {
            draft_task.save_dir = ResolveCategorySaveDirImpl(
                settings_copy, default_dl_dir, draft_task.category, draft_task.url).string();
        }
    }
    if (draft_task.max_connections == 0) {
        draft_task.max_connections = settings_copy.default_connections_per_task;
    }

    const bool is_http_get =
        draft_task.task_type == TaskType::kHttpFile &&
        (draft_task.url.rfind("http://", 0) == 0 || draft_task.url.rfind("https://", 0) == 0) &&
        draft_task.http_method != "POST" &&
        !PathSanitizer::IsWebpageWatchUrl(draft_task.url);

    auto tx = std::make_shared<SpeculativeTransaction>();
    tx->tx_id = tx_id;
    tx->draft_task = draft_task;
    tx->speculative_eligible = is_http_get;
    tx->state = SpeculativeTxState::kPreflighting;
    tx->metrics.t2_ipc_recv_us = t2;
    tx->metrics.t3_tx_created_us = QpcNowUs();

    if (is_http_get) {
        tx->staging_dir = temp_root / ("spec_" + tx_id);
        std::filesystem::path part_p = tx->staging_dir / (draft_task.file_name + "." + tx_id + ".part");
        tx->draft_task.part_path = PathSanitizer::PathToUtf8(part_p);
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return Result<std::string>::Err("ERR_SHUTDOWN", "Controller is shutting down");
        }
        m_pending_txs[tx_id] = tx;
    }

    // Task A: Launch speculative background HTTP download immediately (if eligible)
    if (is_http_get) {
        tx->metrics.t4_bg_queued_us = QpcNowUs();
        tx->bg_thread = std::thread([this, tx, settings_copy, allow_loopback]() {
            TaskModel local_task;
            {
                std::lock_guard<std::mutex> tx_lk(tx->mu);
                if (tx->cancelled || tx->state == SpeculativeTxState::kConflictRejected) {
                    tx->engine_finished = true;
                    tx->ctrl->finished.store(true);
                    return;
                }
                tx->state = SpeculativeTxState::kPrefetching;
                local_task = tx->draft_task;
                tx->metrics.t6_http_init_us = QpcNowUs();
            }

            brisk::EngineConfig ecfg;
            ecfg.stall_timeout_ms = settings_copy.stall_timeout_ms;
            ecfg.min_split_bytes = settings_copy.min_split_bytes;
            ecfg.allow_loopback_redirect = allow_loopback;

            brisk::HttpDownloadEngine::SpeculativeHooks hooks;
            hooks.use_explicit_part_path = true;
            hooks.wait_for_acceptance = [tx](std::atomic<bool>& cancel_flag, TaskModel& task_to_commit) -> bool {
                std::unique_lock<std::mutex> tx_lk(tx->mu);
                tx->cv.wait(tx_lk, [&]() {
                    return tx->accepted || tx->cancelled || cancel_flag.load();
                });
                if (tx->cancelled || cancel_flag.load() || !tx->accepted) {
                    return false;
                }
                task_to_commit.save_dir = tx->draft_task.save_dir;
                task_to_commit.file_name = tx->draft_task.file_name;
                task_to_commit.conflict_policy = tx->draft_task.conflict_policy;
                return true;
            };
            hooks.sync_before_commit = [tx](TaskModel& task_to_commit) {
                std::lock_guard<std::mutex> tx_lk(tx->mu);
                if (tx->accepted) {
                    task_to_commit.save_dir = tx->draft_task.save_dir;
                    task_to_commit.file_name = tx->draft_task.file_name;
                    task_to_commit.conflict_policy = tx->draft_task.conflict_policy;
                }
            };

            brisk::HttpDownloadEngine engine(ecfg);
            bool first_byte_seen = false;
            auto res = engine.ExecuteDownload(
                local_task,
                tx->ctrl->pause_requested,
                tx->ctrl->cancel_requested,
                tx->ctrl->task_speed_limit_bps,
                [this, tx, &first_byte_seen](const brisk::EngineProgress& ep) {
                    bool is_accepted = false;
                    {
                        std::lock_guard<std::mutex> tx_lk(tx->mu);
                        if (!first_byte_seen && ep.downloaded_bytes > 0) {
                            first_byte_seen = true;
                            tx->metrics.t7_first_byte_us = QpcNowUs();
                        }
                        if (tx->cancelled || tx->state == SpeculativeTxState::kConflictRejected) return;
                        tx->draft_task.downloaded_bytes = ep.downloaded_bytes;
                        tx->draft_task.total_bytes = ep.total_bytes;
                        tx->draft_task.speed_bps = ep.speed_bps;
                        tx->draft_task.range_supported = ep.range_supported;
                        if (!ep.resolved_filename.empty()) tx->draft_task.file_name = ep.resolved_filename;
                        if (!ep.etag.empty()) tx->draft_task.etag = ep.etag;
                        if (!ep.last_modified.empty()) tx->draft_task.last_modified = ep.last_modified;
                        tx->draft_task.segments = ep.segments;
                        is_accepted = tx->accepted;
                    }
                    if (is_accepted) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        auto it = m_tasks.find(tx->tx_id);
                        if (it != m_tasks.end() && !m_deleted_tasks.count(tx->tx_id)) {
                            auto& t = it->second;
                            if (t.state == TaskState::kCanceled || t.state == TaskState::kPaused) return;
                            t.state = ep.state;
                            t.downloaded_bytes = ep.downloaded_bytes;
                            t.total_bytes = ep.total_bytes;
                            t.speed_bps = ep.speed_bps;
                            t.range_supported = ep.range_supported;
                            if (!ep.status_detail.empty()) t.status_detail = ep.status_detail;
                            if (!ep.etag.empty()) t.etag = ep.etag;
                            if (!ep.last_modified.empty()) t.last_modified = ep.last_modified;
                            t.segments = ep.segments;
                        }
                    }
                },
                m_global_limiter,
                &m_global_speed_limit_bps,
                &hooks);

            bool was_accepted = false;
            {
                std::lock_guard<std::mutex> tx_lk(tx->mu);
                tx->engine_finished = true;
                tx->engine_succeeded = res.ok();
                if (!res.ok()) {
                    tx->engine_error = res.error;
                } else {
                    tx->final_path = *res.value;
                }
                if (tx->cancelled || tx->state == SpeculativeTxState::kConflictRejected || tx->ctrl->cancel_requested.load()) {
                    tx->metrics.t13_bg_stopped_us = QpcNowUs();
                }
                was_accepted = tx->accepted;
                tx->draft_task.state = local_task.state;
                tx->draft_task.downloaded_bytes = local_task.downloaded_bytes;
                tx->draft_task.total_bytes = local_task.total_bytes;
                tx->draft_task.segments = local_task.segments;
                tx->draft_task.part_path = local_task.part_path;
                tx->draft_task.etag = local_task.etag;
                tx->draft_task.last_modified = local_task.last_modified;
                tx->draft_task.completed_at_ms = local_task.completed_at_ms;
                tx->draft_task.status_detail = local_task.status_detail;
                tx->draft_task.error_code = local_task.error_code;
                tx->draft_task.error_message = local_task.error_message;
                if (res.ok()) {
                    tx->draft_task.file_name = local_task.file_name;
                    tx->state = SpeculativeTxState::kCompleted;
                }
            }

            if (!was_accepted) {
                if (!tx->staging_dir.empty()) {
                    std::error_code ec;
                    std::filesystem::remove_all(tx->staging_dir, ec);
                }
                tx->ctrl->finished.store(true);
                return;
            }

            // Accepted transaction finished: merge final state into m_tasks + m_store
            bool should_persist = false;
            TaskModel to_save;
            TaskNotificationCallback notify_cb;
            bool should_notify = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_active.erase(tx->tx_id);
                if (!m_deleted_tasks.count(tx->tx_id)) {
                    auto it = m_tasks.find(tx->tx_id);
                    if (it != m_tasks.end()) {
                        auto& t = it->second;
                        if (t.state == TaskState::kCanceled) {
                            local_task.state = TaskState::kCanceled;
                            local_task.status_detail = "Canceled";
                        } else if (t.state == TaskState::kPaused && tx->ctrl->pause_requested.load()) {
                            local_task.state = TaskState::kPaused;
                            local_task.status_detail = "Paused";
                        }
                        t.state = local_task.state;
                        t.file_name = local_task.file_name;
                        t.part_path = local_task.part_path;
                        t.total_bytes = local_task.total_bytes;
                        t.downloaded_bytes = local_task.downloaded_bytes;
                        t.speed_bps = 0;
                        t.range_supported = local_task.range_supported;
                        t.status_detail = local_task.status_detail;
                        t.etag = local_task.etag;
                        t.last_modified = local_task.last_modified;
                        t.segments = local_task.segments;
                        t.error_code = local_task.error_code;
                        t.error_message = local_task.error_message;
                        t.updated_at_ms = NowEpochMs();
                        t.completed_at_ms = local_task.completed_at_ms;
                        if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
                            t.auth_headers.clear();
                        }
                        if (t.state == TaskState::kCompleted && m_settings.show_completion_notification &&
                            m_notified_task_ids.insert(tx->tx_id).second) {
                            notify_cb = m_task_notification_callback;
                            should_notify = static_cast<bool>(notify_cb);
                        }
                        to_save = t;
                        should_persist = true;
                    }
                }
            }
            if (should_persist) {
                m_store.UpsertTask(to_save);
                if (to_save.state == TaskState::kCompleted && !tx->staging_dir.empty()) {
                    std::error_code ec;
                    std::filesystem::remove_all(tx->staging_dir, ec);
                }
            }
            if (should_notify) {
                notify_cb(TaskNotification{tx->tx_id, to_save.file_name, true, "Completed"});
            }
            tx->ctrl->finished.store(true);
        });
    } else {
        tx->engine_finished = true;
        tx->ctrl->finished.store(true);
    }

    // Task B: Run destination & duplicate check concurrently off the IPC/UI thread
    tx->metrics.t5_dest_check_queued_us = QpcNowUs();
    tx->dest_thread = std::thread([this, tx, settings_copy, on_decision = std::move(on_decision)]() {
        auto report = InspectIncomingDownload(tx->draft_task, tx->tx_id);
        const int64_t t8 = QpcNowUs();
        const bool is_conflict =
            (report.kind == IncomingConflictKind::kActiveOrQueuedSameDownload && report.matched_task.has_value()) ||
            (report.has_conflict() && settings_copy.conflict_policy == ConflictPolicy::kPrompt);

        TaskModel draft_snapshot;
        bool should_notify_ui = false;
        {
            std::lock_guard<std::mutex> tx_lk(tx->mu);
            tx->dest_check_done = true;
            tx->conflict_report = report;
            tx->metrics.t8_dest_check_done_us = t8;
            if (tx->cancelled || tx->accepted) {
                return;
            }
            if (is_conflict) {
                tx->state = SpeculativeTxState::kConflictRejected;
                tx->cancelled = true;
                tx->ctrl->cancel_requested.store(true);
                tx->cv.notify_all();
            } else if (tx->state == SpeculativeTxState::kPreflighting) {
                tx->state = SpeculativeTxState::kWaitingForUser;
            }
            draft_snapshot = tx->draft_task;
            should_notify_ui = true;
        }
        if (should_notify_ui && on_decision) {
            on_decision(tx->tx_id, draft_snapshot, report);
        }
    });

    return Result<std::string>::Ok(tx_id);
}

std::optional<TaskModel> DownloadController::GetSpeculativeSnapshot(const std::string& tx_id) const {
    std::shared_ptr<SpeculativeTransaction> tx;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending_txs.find(tx_id);
        if (it == m_pending_txs.end()) return std::nullopt;
        tx = it->second;
    }
    std::lock_guard<std::mutex> tx_lk(tx->mu);
    return tx->draft_task;
}

std::optional<DownloadController::SpeculativeTxMetrics> DownloadController::GetSpeculativeMetrics(const std::string& tx_id) const {
    std::shared_ptr<SpeculativeTransaction> tx;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending_txs.find(tx_id);
        if (it == m_pending_txs.end()) return std::nullopt;
        tx = it->second;
    }
    std::lock_guard<std::mutex> tx_lk(tx->mu);
    return tx->metrics;
}

void DownloadController::RecordSpeculativeDialogVisible(const std::string& tx_id) {
    std::shared_ptr<SpeculativeTransaction> tx;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending_txs.find(tx_id);
        if (it == m_pending_txs.end()) return;
        tx = it->second;
    }
    std::lock_guard<std::mutex> tx_lk(tx->mu);
    if (tx->metrics.t9_dialog_visible_us == 0) {
        tx->metrics.t9_dialog_visible_us = QpcNowUs();
    }
}

Result<TaskModel> DownloadController::AcceptSpeculativeTransaction(
    const std::string& tx_id,
    const std::string& final_filename,
    const std::string& final_save_dir,
    bool start_immediately,
    bool scheduled,
    const std::string& schedule_time_local,
    ConflictPolicy conflict_policy) {
    std::shared_ptr<SpeculativeTransaction> tx;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending_txs.find(tx_id);
        if (it == m_pending_txs.end()) {
            return Result<TaskModel>::Err("ERR_NOT_FOUND", "Speculative transaction not found");
        }
        tx = it->second;
    }

    TaskModel accepted_task;
    bool need_fallback_start = false;
    {
        std::lock_guard<std::mutex> tx_lk(tx->mu);
        if (tx->cancelled || tx->state == SpeculativeTxState::kConflictRejected) {
            return Result<TaskModel>::Err("ERR_TX_CANCELLED", "Speculative transaction was already cancelled");
        }
        if (tx->accepted) {
            return Result<TaskModel>::Ok(tx->draft_task);
        }

        tx->metrics.t10_user_download_us = QpcNowUs();
        tx->metrics.bytes_before_decision = tx->draft_task.downloaded_bytes;
        tx->accepted = true;
        tx->state = SpeculativeTxState::kAccepted;

        if (!final_filename.empty()) {
            tx->draft_task.file_name = PathSanitizer::SanitizeFileName(final_filename);
        }
        if (!final_save_dir.empty()) {
            tx->draft_task.save_dir = final_save_dir;
        }
        tx->draft_task.conflict_policy = conflict_policy;
        tx->draft_task.scheduled = scheduled;
        tx->draft_task.schedule_time_local = scheduled ? schedule_time_local : "";
        tx->draft_task.user_edited_filename = true;
        tx->draft_task.created_at_ms = NowEpochMs();
        tx->draft_task.updated_at_ms = tx->draft_task.created_at_ms;

        if (!tx->speculative_eligible || (tx->engine_finished && !tx->engine_succeeded)) {
            need_fallback_start = true;
            tx->draft_task.state = start_immediately ? TaskState::kQueued : TaskState::kPaused;
            tx->draft_task.status_detail = start_immediately ? "Queued" : (scheduled ? "Scheduled" : "Paused");
        } else if (tx->engine_finished && tx->engine_succeeded && tx->state == SpeculativeTxState::kCompleted) {
            tx->draft_task.state = TaskState::kCompleted;
            tx->draft_task.status_detail = "Completed";
        } else if (!start_immediately || scheduled) {
            tx->ctrl->pause_requested.store(true);
            tx->draft_task.state = TaskState::kPaused;
            tx->draft_task.status_detail = scheduled ? "Scheduled" : "Paused";
        } else {
            tx->draft_task.state = TaskState::kDownloading;
            tx->draft_task.status_detail = "Downloading";
        }

        accepted_task = tx->draft_task;
        tx->cv.notify_all();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deleted_tasks.erase(tx_id);
        m_tasks[tx_id] = accepted_task;
        if (!tx->engine_finished) {
            m_active[tx_id] = tx->ctrl;
        }
        m_store.UpsertTask(accepted_task);
        if (need_fallback_start && start_immediately) {
            m_cv.notify_all();
        }
    }

    {
        std::lock_guard<std::mutex> tx_lk(tx->mu);
        tx->metrics.t11_progress_visible_us = QpcNowUs();
    }
    return Result<TaskModel>::Ok(accepted_task);
}

Result<void> DownloadController::CancelSpeculativeTransaction(const std::string& tx_id) {
    std::shared_ptr<SpeculativeTransaction> tx;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending_txs.find(tx_id);
        if (it == m_pending_txs.end()) {
            return Result<void>::Ok();
        }
        tx = it->second;
    }

    std::lock_guard<std::mutex> tx_lk(tx->mu);
    if (tx->accepted) {
        return Result<void>::Err("ERR_ALREADY_ACCEPTED", "Transaction already accepted");
    }
    if (!tx->cancelled) {
        tx->metrics.t12_user_cancel_us = QpcNowUs();
        tx->cancelled = true;
        tx->state = SpeculativeTxState::kCancelled;
        tx->ctrl->cancel_requested.store(true);
        tx->cv.notify_all();
    }
    return Result<void>::Ok();
}

} // namespace radium
