// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Redacted Logger (Implements NFR-009, THREAT-008)
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace radium {

enum class LogLevel {
    kDebug,
    kInfo,
    kWarn,
    kError
};

class Logger {
public:
    static Logger& Instance();

    void SetLogFile(const std::filesystem::path& log_path);
    void Log(LogLevel level, std::string_view component, std::string_view message);

    // Mandatory redaction of Cookie, Set-Cookie, Authorization, Proxy-Authorization, and token query params
    static std::string RedactSensitiveData(std::string_view input);

    std::vector<std::string> GetRecentLines(size_t max_lines = 100) const;

private:
    Logger() = default;
    mutable std::mutex m_mutex;
    std::filesystem::path m_log_path;
    std::vector<std::string> m_ring_buffer;
};

#define RADIUM_LOG_DEBUG(comp, msg) ::radium::Logger::Instance().Log(::radium::LogLevel::kDebug, (comp), (msg))
#define RADIUM_LOG_INFO(comp, msg) ::radium::Logger::Instance().Log(::radium::LogLevel::kInfo, (comp), (msg))
#define RADIUM_LOG_WARN(comp, msg) ::radium::Logger::Instance().Log(::radium::LogLevel::kWarn, (comp), (msg))
#define RADIUM_LOG_ERROR(comp, msg) ::radium::Logger::Instance().Log(::radium::LogLevel::kError, (comp), (msg))

} // namespace radium
