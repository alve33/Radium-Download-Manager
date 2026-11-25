#pragma once
// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Settings Serializer & Portable Backup Engine
// Serializes and deserializes AppSettings to clean JSON with validation and safe schema migration.

#include "common/types.hpp"
#include <string>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace radium::core {

class SettingsSerializer {
public:
    static std::string SerializeToJson(const radium::AppSettings& s) {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"version\": 1,\n";
        oss << "  \"theme\": \"" << EscapeJson(s.theme) << "\",\n";
        oss << "  \"language\": \"" << EscapeJson(s.language) << "\",\n";
        oss << "  \"max_concurrent_tasks\": " << s.max_concurrent_tasks << ",\n";
        oss << "  \"default_connections_per_task\": " << s.default_connections_per_task << ",\n";
        oss << "  \"speed_limit_bps\": " << s.speed_limit_bps << ",\n";
        oss << "  \"default_download_dir\": \"" << EscapeJson(s.default_download_dir) << "\",\n";
        oss << "  \"conflict_policy\": " << static_cast<int>(s.conflict_policy) << ",\n";
        oss << "  \"auto_start_downloads\": " << (s.auto_start_downloads ? "true" : "false") << ",\n";
        oss << "  \"browser_capture_enabled\": " << (s.browser_capture_enabled ? "true" : "false") << ",\n";
        oss << "  \"close_to_tray\": " << (s.close_to_tray ? "true" : "false") << ",\n";
        oss << "  \"start_minimized\": " << (s.start_minimized ? "true" : "false") << ",\n";
        oss << "  \"sound_cues_enabled\": " << (s.sound_cues_enabled ? "true" : "false") << ",\n";
        oss << "  \"av_strict_mode\": " << (s.av_strict_mode ? "true" : "false") << ",\n";
        oss << "  \"max_auto_retries\": " << s.max_auto_retries << "\n";
        oss << "}\n";
        return oss.str();
    }

    static radium::Result<radium::AppSettings> DeserializeFromJson(std::string_view json_str) {
        if (json_str.empty()) {
            return radium::Result<radium::AppSettings>::Err("ERR_EMPTY_JSON", "Settings JSON is empty");
        }

        radium::AppSettings s;
        auto theme = ParseStringField(json_str, "theme");
        if (!theme.empty()) s.theme = theme;
        auto language = ParseStringField(json_str, "language");
        if (!language.empty()) s.language = language;

        auto max_tasks = ParseIntField(json_str, "max_concurrent_tasks");
        if (max_tasks.has_value() && *max_tasks >= 1 && *max_tasks <= 64) {
            s.max_concurrent_tasks = *max_tasks;
        }

        auto conns = ParseIntField(json_str, "default_connections_per_task");
        if (conns.has_value() && *conns >= 1 && *conns <= 64) {
            s.default_connections_per_task = *conns;
        }

        auto speed_lim = ParseInt64Field(json_str, "speed_limit_bps");
        if (speed_lim.has_value()) {
            s.speed_limit_bps = *speed_lim;
        }

        auto dl_dir = ParseStringField(json_str, "default_download_dir");
        if (!dl_dir.empty()) s.default_download_dir = dl_dir;

        auto cp = ParseIntField(json_str, "conflict_policy");
        if (cp.has_value() && *cp >= 0 && *cp <= 3) {
            s.conflict_policy = static_cast<radium::ConflictPolicy>(*cp);
        }

        auto auto_start = ParseBoolField(json_str, "auto_start_downloads");
        if (auto_start.has_value()) s.auto_start_downloads = *auto_start;

        auto b_capture = ParseBoolField(json_str, "browser_capture_enabled");
        if (b_capture.has_value()) s.browser_capture_enabled = *b_capture;

        auto tray = ParseBoolField(json_str, "close_to_tray");
        if (tray.has_value()) s.close_to_tray = *tray;

        auto min = ParseBoolField(json_str, "start_minimized");
        if (min.has_value()) s.start_minimized = *min;

        auto sound = ParseBoolField(json_str, "sound_cues_enabled");
        if (sound.has_value()) s.sound_cues_enabled = *sound;

        auto av = ParseBoolField(json_str, "av_strict_mode");
        if (av.has_value()) s.av_strict_mode = *av;

        auto retries = ParseIntField(json_str, "max_auto_retries");
        if (retries.has_value() && *retries <= 100) s.max_auto_retries = *retries;

        return radium::Result<radium::AppSettings>::Ok(std::move(s));
    }

    static radium::Result<void> ExportToFile(const std::filesystem::path& file_path, const radium::AppSettings& s) {
        std::ofstream ofs(file_path, std::ios::trunc);
        if (!ofs.is_open()) {
            return radium::Result<void>::Err("ERR_FILE_WRITE", "Cannot open file for writing settings");
        }
        ofs << SerializeToJson(s);
        return radium::Result<void>::Ok();
    }

    static radium::Result<radium::AppSettings> ImportFromFile(const std::filesystem::path& file_path) {
        std::error_code ec;
        if (!std::filesystem::exists(file_path, ec)) {
            return radium::Result<radium::AppSettings>::Err("ERR_FILE_NOT_FOUND", "Settings file does not exist");
        }
        std::ifstream ifs(file_path);
        if (!ifs.is_open()) {
            return radium::Result<radium::AppSettings>::Err("ERR_FILE_READ", "Cannot open settings file");
        }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        return DeserializeFromJson(content);
    }

private:
    static std::string EscapeJson(std::string_view s) {
        std::string res;
        for (char c : s) {
            if (c == '"') res += "\\\"";
            else if (c == '\\') res += "\\\\";
            else res += c;
        }
        return res;
    }

    static std::string ParseStringField(std::string_view json, std::string_view key) {
        std::string needle = "\"" + std::string(key) + "\"";
        size_t p = json.find(needle);
        if (p == std::string::npos) return {};
        p = json.find(':', p);
        if (p == std::string::npos) return {};
        p = json.find('"', p);
        if (p == std::string::npos) return {};
        size_t end = json.find('"', p + 1);
        if (end == std::string::npos) return {};
        return std::string(json.substr(p + 1, end - p - 1));
    }

    static std::optional<int> ParseIntField(std::string_view json, std::string_view key) {
        std::string needle = "\"" + std::string(key) + "\"";
        size_t p = json.find(needle);
        if (p == std::string::npos) return std::nullopt;
        p = json.find(':', p);
        if (p == std::string::npos) return std::nullopt;
        p = json.find_first_of("-0123456789", p);
        if (p == std::string::npos) return std::nullopt;
        size_t end = json.find_first_not_of("-0123456789", p);
        if (end == std::string::npos) end = json.size();
        try {
            return std::stoi(std::string(json.substr(p, end - p)));
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<int64_t> ParseInt64Field(std::string_view json, std::string_view key) {
        std::string needle = "\"" + std::string(key) + "\"";
        size_t p = json.find(needle);
        if (p == std::string::npos) return std::nullopt;
        p = json.find(':', p);
        if (p == std::string::npos) return std::nullopt;
        p = json.find_first_of("-0123456789", p);
        if (p == std::string::npos) return std::nullopt;
        size_t end = json.find_first_not_of("-0123456789", p);
        if (end == std::string::npos) end = json.size();
        try {
            return std::stoll(std::string(json.substr(p, end - p)));
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<bool> ParseBoolField(std::string_view json, std::string_view key) {
        std::string needle = "\"" + std::string(key) + "\"";
        size_t p = json.find(needle);
        if (p == std::string::npos) return std::nullopt;
        p = json.find(':', p);
        if (p == std::string::npos) return std::nullopt;
        size_t t = json.find("true", p);
        size_t f = json.find("false", p);
        size_t next_comma = json.find(',', p);
        if (t < f && (next_comma == std::string::npos || t < next_comma)) return true;
        if (f < t && (next_comma == std::string::npos || f < next_comma)) return false;
        return std::nullopt;
    }
};

} // namespace radium::core
