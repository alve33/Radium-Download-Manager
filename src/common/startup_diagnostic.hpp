// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Startup Diagnostics Data Model & Recorder
// Implements admin-only high-resolution download lifecycle tracking.
#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <memory>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include "common/json_mini.hpp"

namespace openidm::diagnostics {

struct StartupEvent {
    std::string event_name;
    uint64_t epoch_ms{0};          // Unix epoch timestamp in ms (for display/wall clock)
    uint64_t elapsed_us{0};        // Elapsed microseconds from session start
    uint64_t duration_us{0};       // Duration in microseconds since previous event
    std::string detail;
};

struct StartupReport {
    std::string download_id;       // task_id or session id
    std::string correlation_id;
    uint64_t created_epoch_ms{0};
    std::string website;           // domain / website
    std::string source_url;
    std::string download_url;
    std::string filename;
    std::string download_type;     // HTTP, YouTube, HLS, DASH, Torrent
    std::string selected_quality;
    uint64_t file_size{0};
    uint64_t downloaded_bytes{0};
    double startup_time_ms{0.0};   // From download click to first received data byte
    double transfer_duration_ms{0.0}; // Active download transfer duration post-first-byte
    double avg_speed_mbps{0.0};    // Downloaded bytes / transfer duration post-first-byte (MB/s)
    std::string status{"Active"};  // Active, Completed, Failed, Canceled
    std::string error_message;
    std::vector<StartupEvent> events;

    // Helper to calculate summary metrics
    void FinalizeMetrics() {
        if (events.empty()) return;
        
        // Enforce stable sort on events by elapsed_us
        std::stable_sort(events.begin(), events.end(), [](const StartupEvent& a, const StartupEvent& b) {
            return a.elapsed_us < b.elapsed_us;
        });

        // Re-normalize duration_us sequentially to eliminate negative deltas: D_0 = E_0, D_i = E_i - E_{i-1}
        for (size_t i = 0; i < events.size(); ++i) {
            if (i == 0) {
                events[i].duration_us = events[i].elapsed_us;
            } else {
                events[i].duration_us = (events[i].elapsed_us >= events[i - 1].elapsed_us)
                    ? (events[i].elapsed_us - events[i - 1].elapsed_us)
                    : 0;
            }
        }

        // Find first byte event if any
        uint64_t first_byte_elapsed_us = 0;
        bool found_first_byte = false;
        for (const auto& ev : events) {
            if (ev.event_name == "first_byte" || ev.event_name == "FIRST_BYTE_RECEIVED" ||
                ev.event_name == "First data bytes received") {
                first_byte_elapsed_us = ev.elapsed_us;
                found_first_byte = true;
                break;
            }
        }
        
        if (found_first_byte) {
            startup_time_ms = static_cast<double>(first_byte_elapsed_us) / 1000.0;
            const uint64_t last_elapsed_us = events.back().elapsed_us;
            if (last_elapsed_us > first_byte_elapsed_us) {
                transfer_duration_ms = static_cast<double>(last_elapsed_us - first_byte_elapsed_us) / 1000.0;
            }
        } else {
            startup_time_ms = static_cast<double>(events.back().elapsed_us) / 1000.0;
        }

        if (transfer_duration_ms > 0.0 && downloaded_bytes > 0) {
            double sec = transfer_duration_ms / 1000.0;
            avg_speed_mbps = (static_cast<double>(downloaded_bytes) / (1024.0 * 1024.0)) / sec;
        }
    }
};

class StartupDiagnosticRecorder {
public:
    static StartupDiagnosticRecorder& Instance() {
        static StartupDiagnosticRecorder s_instance;
        return s_instance;
    }

    bool HasSession(const std::string& id_or_correlation) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active_sessions.count(id_or_correlation)) return true;
        auto it = m_id_lookup.find(id_or_correlation);
        if (it != m_id_lookup.end() && m_active_sessions.count(it->second)) return true;
        return false;
    }

    void StartSession(
        const std::string& download_id,
        const std::string& correlation_id,
        const std::string& source_url,
        const std::string& download_url,
        const std::string& filename,
        const std::string& download_type,
        const std::string& selected_quality,
        uint64_t initial_file_size,
        uint64_t client_click_epoch_ms = 0) {
        
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now_tp = std::chrono::steady_clock::now();
        uint64_t epoch_now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        SessionState s{};
        s.report.download_id = download_id;
        s.report.correlation_id = correlation_id;
        s.report.created_epoch_ms = (client_click_epoch_ms > 0) ? client_click_epoch_ms : epoch_now;
        s.report.source_url = source_url;
        s.report.download_url = download_url;
        s.report.website = ExtractDomain(source_url.empty() ? download_url : source_url);
        s.report.filename = filename;
        s.report.download_type = download_type;
        s.report.selected_quality = selected_quality;
        s.report.file_size = initial_file_size;
        s.report.status = "Active";

        uint64_t click_offset_us = 0;
        if (client_click_epoch_ms > 0 && epoch_now > client_click_epoch_ms) {
            click_offset_us = (epoch_now - client_click_epoch_ms) * 1000;
        }

        s.origin_tp = now_tp - std::chrono::microseconds(click_offset_us);
        s.last_tp = s.origin_tp;

        // If client reported click timestamp, add initial click event
        if (client_click_epoch_ms > 0) {
            StartupEvent ev{};
            ev.event_name = "Download button clicked";
            ev.epoch_ms = client_click_epoch_ms;
            ev.elapsed_us = 0;
            ev.duration_us = 0;
            ev.detail = "User initiated download via browser extension or dialog";
            s.report.events.push_back(ev);

            // Time delta to server arrival
            StartupEvent ev2{};
            ev2.event_name = "Download task created";
            ev2.epoch_ms = epoch_now;
            ev2.elapsed_us = click_offset_us;
            ev2.duration_us = click_offset_us;
            ev2.detail = "Session registered in core engine";
            s.report.events.push_back(ev2);
            s.last_tp = now_tp;
        } else {
            StartupEvent ev{};
            ev.event_name = "Download task created";
            ev.epoch_ms = epoch_now;
            ev.elapsed_us = 0;
            ev.duration_us = 0;
            ev.detail = "Session registered in core engine";
            s.report.events.push_back(ev);
            s.last_tp = now_tp;
        }

        m_active_sessions[download_id] = s;
        if (!correlation_id.empty()) {
            m_id_lookup[correlation_id] = download_id;
        }
    }

    void RecordEvent(
        const std::string& id_or_correlation,
        const std::string& event_name,
        const std::string& detail = "") {
        
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string actual_id = id_or_correlation;
        if (m_id_lookup.count(id_or_correlation)) {
            actual_id = m_id_lookup[id_or_correlation];
        }

        auto it = m_active_sessions.find(actual_id);
        if (it == m_active_sessions.end()) return;

        // Deduplicate "First data bytes received"
        if (event_name == "First data bytes received" || event_name == "first_byte" || event_name == "FIRST_BYTE_RECEIVED") {
            if (it->second.has_first_byte_recorded) {
                return;
            }
            it->second.has_first_byte_recorded = true;
        }

        auto now_tp = std::chrono::steady_clock::now();
        uint64_t epoch_now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        uint64_t elapsed_us = (now_tp >= it->second.origin_tp)
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now_tp - it->second.origin_tp).count())
            : 0;
        uint64_t duration_us = (now_tp >= it->second.last_tp)
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now_tp - it->second.last_tp).count())
            : 0;

        it->second.last_tp = now_tp;

        StartupEvent ev{};
        ev.event_name = event_name;
        ev.epoch_ms = epoch_now;
        ev.elapsed_us = elapsed_us;
        ev.duration_us = duration_us;
        ev.detail = detail;

        it->second.report.events.push_back(ev);
    }

    void IngestExtensionStages(
        const std::string& id_or_correlation,
        const std::vector<std::pair<std::string, uint64_t>>& stages) {
        
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string actual_id = id_or_correlation;
        if (m_id_lookup.count(id_or_correlation)) {
            actual_id = m_id_lookup[id_or_correlation];
        }

        auto it = m_active_sessions.find(actual_id);
        if (it == m_active_sessions.end() || stages.empty()) return;

        uint64_t base_epoch = it->second.report.created_epoch_ms;
        for (const auto& [stage_name, stage_epoch] : stages) {
            StartupEvent ev{};
            ev.event_name = stage_name;
            ev.epoch_ms = stage_epoch;
            ev.elapsed_us = (stage_epoch >= base_epoch) ? (stage_epoch - base_epoch) * 1000 : 0;
            ev.duration_us = 0; // calculated relative during display
            ev.detail = "Extension pipeline stage";
            it->second.report.events.push_back(ev);
        }
    }

    void UpdateTaskDetails(
        const std::string& id_or_correlation,
        const std::string& filename,
        const std::string& download_url,
        uint64_t file_size,
        const std::string& quality = "") {
        
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string actual_id = id_or_correlation;
        if (m_id_lookup.count(id_or_correlation)) {
            actual_id = m_id_lookup[id_or_correlation];
        }

        auto it = m_active_sessions.find(actual_id);
        if (it == m_active_sessions.end()) return;

        if (!filename.empty()) it->second.report.filename = filename;
        if (!download_url.empty()) it->second.report.download_url = download_url;
        if (file_size > 0) it->second.report.file_size = file_size;
        if (!quality.empty()) it->second.report.selected_quality = quality;
    }

    void UpdateProgress(
        const std::string& id_or_correlation,
        uint64_t downloaded_bytes,
        uint64_t total_bytes) {
        
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string actual_id = id_or_correlation;
        if (m_id_lookup.count(id_or_correlation)) {
            actual_id = m_id_lookup[id_or_correlation];
        }

        auto it = m_active_sessions.find(actual_id);
        if (it == m_active_sessions.end()) return;

        it->second.report.downloaded_bytes = downloaded_bytes;
        if (total_bytes > 0) it->second.report.file_size = total_bytes;
    }

    void CompleteSession(
        const std::string& id_or_correlation,
        bool success,
        const std::string& error_msg = "") {
        
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string actual_id = id_or_correlation;
        if (m_id_lookup.count(id_or_correlation)) {
            actual_id = m_id_lookup[id_or_correlation];
        }

        auto it = m_active_sessions.find(actual_id);
        if (it == m_active_sessions.end()) return;

        it->second.report.status = success ? "Completed" : "Failed";
        it->second.report.error_message = error_msg;

        auto now_tp = std::chrono::steady_clock::now();
        uint64_t epoch_now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        uint64_t elapsed_us = (now_tp >= it->second.origin_tp)
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now_tp - it->second.origin_tp).count())
            : 0;
        uint64_t duration_us = (now_tp >= it->second.last_tp)
            ? static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now_tp - it->second.last_tp).count())
            : 0;

        it->second.last_tp = now_tp;

        StartupEvent ev{};
        ev.event_name = success ? "Download completed" : "Download failed";
        ev.epoch_ms = epoch_now;
        ev.elapsed_us = elapsed_us;
        ev.duration_us = duration_us;
        ev.detail = success ? "File finalized and committed" : error_msg;
        it->second.report.events.push_back(ev);

        it->second.report.FinalizeMetrics();

        // Save report to stored reports
        m_stored_reports.push_back(it->second.report);
        if (m_stored_reports.size() > 500) {
            m_stored_reports.erase(m_stored_reports.begin());
        }

        // Auto-save to persistence
        SaveReportsToDiskInternal();

        m_active_sessions.erase(it);
    }

    std::vector<StartupReport> GetReports() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<StartupReport> all = m_stored_reports;
        for (auto& pair : m_active_sessions) {
            StartupReport r = pair.second.report;
            r.FinalizeMetrics();
            all.push_back(r);
        }
        return all;
    }

    void ClearReports() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stored_reports.clear();
        m_active_sessions.clear();
        m_id_lookup.clear();
        SaveReportsToDiskInternal();
    }

    std::string ExportToJson() {
        auto reports = GetReports();
        json::Value root = json::Array{};
        for (const auto& rep : reports) {
            json::Value obj = json::Object{};
            obj["download_id"] = rep.download_id;
            obj["correlation_id"] = rep.correlation_id;
            obj["created_epoch_ms"] = static_cast<int64_t>(rep.created_epoch_ms);
            obj["website"] = rep.website;
            obj["source_url"] = rep.source_url;
            obj["download_url"] = rep.download_url;
            obj["filename"] = rep.filename;
            obj["download_type"] = rep.download_type;
            obj["selected_quality"] = rep.selected_quality;
            obj["file_size"] = static_cast<int64_t>(rep.file_size);
            obj["downloaded_bytes"] = static_cast<int64_t>(rep.downloaded_bytes);
            obj["startup_time_ms"] = rep.startup_time_ms;
            obj["transfer_duration_ms"] = rep.transfer_duration_ms;
            obj["avg_speed_mbps"] = rep.avg_speed_mbps;
            obj["status"] = rep.status;
            obj["error_message"] = rep.error_message;

            json::Value events_arr = json::Array{};
            for (const auto& ev : rep.events) {
                json::Value ev_obj = json::Object{};
                ev_obj["event_name"] = ev.event_name;
                ev_obj["epoch_ms"] = static_cast<int64_t>(ev.epoch_ms);
                ev_obj["elapsed_ms"] = static_cast<double>(ev.elapsed_us) / 1000.0;
                ev_obj["duration_ms"] = static_cast<double>(ev.duration_us) / 1000.0;
                ev_obj["detail"] = ev.detail;
                events_arr.mutable_array().push_back(std::move(ev_obj));
            }
            obj["events"] = std::move(events_arr);
            root.mutable_array().push_back(std::move(obj));
        }
        return root.dump();
    }

    std::string ExportToCsv() {
        auto reports = GetReports();
        std::ostringstream ss;
        ss << "Download ID,Correlation ID,Date/Time,Website,Download Type,Filename,File Size,Startup Time (ms),Avg Speed (MB/s),Duration (ms),Status,Event Name,Event Elapsed (ms),Event Delta (ms),Event Detail\n";
        for (const auto& rep : reports) {
            std::string dt_str = FormatEpoch(rep.created_epoch_ms);
            if (rep.events.empty()) {
                ss << EscapeCsv(rep.download_id) << ","
                   << EscapeCsv(rep.correlation_id) << ","
                   << EscapeCsv(dt_str) << ","
                   << EscapeCsv(rep.website) << ","
                   << EscapeCsv(rep.download_type) << ","
                   << EscapeCsv(rep.filename) << ","
                   << rep.file_size << ","
                   << std::fixed << std::setprecision(2) << rep.startup_time_ms << ","
                   << std::fixed << std::setprecision(2) << rep.avg_speed_mbps << ","
                   << std::fixed << std::setprecision(2) << rep.transfer_duration_ms << ","
                   << EscapeCsv(rep.status) << ",,,,\n";
            } else {
                for (const auto& ev : rep.events) {
                    ss << EscapeCsv(rep.download_id) << ","
                       << EscapeCsv(rep.correlation_id) << ","
                       << EscapeCsv(dt_str) << ","
                       << EscapeCsv(rep.website) << ","
                       << EscapeCsv(rep.download_type) << ","
                       << EscapeCsv(rep.filename) << ","
                       << rep.file_size << ","
                       << std::fixed << std::setprecision(2) << rep.startup_time_ms << ","
                       << std::fixed << std::setprecision(2) << rep.avg_speed_mbps << ","
                       << std::fixed << std::setprecision(2) << rep.transfer_duration_ms << ","
                       << EscapeCsv(rep.status) << ","
                       << EscapeCsv(ev.event_name) << ","
                       << std::fixed << std::setprecision(2) << (static_cast<double>(ev.elapsed_us) / 1000.0) << ","
                       << std::fixed << std::setprecision(2) << (static_cast<double>(ev.duration_us) / 1000.0) << ","
                       << EscapeCsv(ev.detail) << "\n";
                }
            }
        }
        return ss.str();
    }

    void InitializeStorage(const std::filesystem::path& base_dir) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_storage_path = base_dir / "diagnostics" / "startup_reports.json";
        LoadReportsFromDiskInternal();
    }

private:
    StartupDiagnosticRecorder() = default;

    struct SessionState {
        StartupReport report;
        std::chrono::steady_clock::time_point origin_tp;
        std::chrono::steady_clock::time_point last_tp;
        bool has_first_byte_recorded{false};
    };

    static std::string ExtractDomain(const std::string& url) {
        if (url.empty()) return "";
        size_t p = url.find("://");
        size_t start = (p == std::string::npos) ? 0 : p + 3;
        size_t end = url.find('/', start);
        if (end == std::string::npos) end = url.find('?', start);
        if (end == std::string::npos) end = url.length();
        std::string host = url.substr(start, end - start);
        size_t port_pos = host.find(':');
        if (port_pos != std::string::npos) host = host.substr(0, port_pos);
        return host;
    }

    static std::string FormatEpoch(uint64_t epoch_ms) {
        if (epoch_ms == 0) return "";
        std::time_t t = static_cast<std::time_t>(epoch_ms / 1000);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        char ms_buf[16];
        snprintf(ms_buf, sizeof(ms_buf), ".%03u", static_cast<unsigned>(epoch_ms % 1000));
        return std::string(buf) + ms_buf;
    }

    static std::string EscapeCsv(const std::string& val) {
        if (val.find(',') == std::string::npos && val.find('"') == std::string::npos && val.find('\n') == std::string::npos) {
            return val;
        }
        std::string res = "\"";
        for (char c : val) {
            if (c == '"') res += "\"\"";
            else res += c;
        }
        res += "\"";
        return res;
    }

    void SaveReportsToDiskInternal() {
        if (m_storage_path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(m_storage_path.parent_path(), ec);
        std::ofstream out(m_storage_path, std::ios::trunc | std::ios::binary);
        if (out.is_open()) {
            json::Value root = json::Array{};
            for (const auto& rep : m_stored_reports) {
                json::Value obj = json::Object{};
                obj["download_id"] = rep.download_id;
                obj["correlation_id"] = rep.correlation_id;
                obj["created_epoch_ms"] = static_cast<int64_t>(rep.created_epoch_ms);
                obj["website"] = rep.website;
                obj["source_url"] = rep.source_url;
                obj["download_url"] = rep.download_url;
                obj["filename"] = rep.filename;
                obj["download_type"] = rep.download_type;
                obj["selected_quality"] = rep.selected_quality;
                obj["file_size"] = static_cast<int64_t>(rep.file_size);
                obj["downloaded_bytes"] = static_cast<int64_t>(rep.downloaded_bytes);
                obj["startup_time_ms"] = rep.startup_time_ms;
                obj["transfer_duration_ms"] = rep.transfer_duration_ms;
                obj["avg_speed_mbps"] = rep.avg_speed_mbps;
                obj["status"] = rep.status;
                obj["error_message"] = rep.error_message;

                json::Value events_arr = json::Array{};
                for (const auto& ev : rep.events) {
                    json::Value ev_obj = json::Object{};
                    ev_obj["event_name"] = ev.event_name;
                    ev_obj["epoch_ms"] = static_cast<int64_t>(ev.epoch_ms);
                    ev_obj["elapsed_us"] = static_cast<int64_t>(ev.elapsed_us);
                    ev_obj["duration_us"] = static_cast<int64_t>(ev.duration_us);
                    ev_obj["detail"] = ev.detail;
                    events_arr.mutable_array().push_back(std::move(ev_obj));
                }
                obj["events"] = std::move(events_arr);
                root.mutable_array().push_back(std::move(obj));
            }
            out << root.dump();
        }
    }

    void LoadReportsFromDiskInternal() {
        if (m_storage_path.empty() || !std::filesystem::exists(m_storage_path)) return;
        std::ifstream in(m_storage_path, std::ios::binary);
        if (!in.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto parsed = json::Parse(content);
        if (!parsed.is_array()) return;

        m_stored_reports.clear();
        for (const auto& item : parsed.as_array()) {
            if (!item.is_object()) continue;
            StartupReport rep{};
            rep.download_id = item["download_id"].as_string();
            rep.correlation_id = item["correlation_id"].as_string();
            rep.created_epoch_ms = static_cast<uint64_t>(item["created_epoch_ms"].as_int64(0));
            rep.website = item["website"].as_string();
            rep.source_url = item["source_url"].as_string();
            rep.download_url = item["download_url"].as_string();
            rep.filename = item["filename"].as_string();
            rep.download_type = item["download_type"].as_string();
            rep.selected_quality = item["selected_quality"].as_string();
            rep.file_size = static_cast<uint64_t>(item["file_size"].as_int64(0));
            rep.downloaded_bytes = static_cast<uint64_t>(item["downloaded_bytes"].as_int64(0));
            rep.startup_time_ms = item["startup_time_ms"].as_double(0.0);
            rep.transfer_duration_ms = item["transfer_duration_ms"].as_double(0.0);
            rep.avg_speed_mbps = item["avg_speed_mbps"].as_double(0.0);
            rep.status = item["status"].as_string("Completed");
            rep.error_message = item["error_message"].as_string();

            if (item["events"].is_array()) {
                for (const auto& ev_val : item["events"].as_array()) {
                    StartupEvent ev{};
                    ev.event_name = ev_val["event_name"].as_string();
                    ev.epoch_ms = static_cast<uint64_t>(ev_val["epoch_ms"].as_int64(0));
                    ev.elapsed_us = static_cast<uint64_t>(ev_val["elapsed_us"].as_int64(0));
                    ev.duration_us = static_cast<uint64_t>(ev_val["duration_us"].as_int64(0));
                    ev.detail = ev_val["detail"].as_string();
                    rep.events.push_back(ev);
                }
            }
            rep.FinalizeMetrics();
            m_stored_reports.push_back(rep);
        }
    }

    std::mutex m_mutex;
    std::filesystem::path m_storage_path;
    std::map<std::string, SessionState> m_active_sessions;
    std::map<std::string, std::string> m_id_lookup; // correlation_id -> download_id
    std::vector<StartupReport> m_stored_reports;
};

} // namespace openidm::diagnostics
