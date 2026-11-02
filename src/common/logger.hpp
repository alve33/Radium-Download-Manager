// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Redacted Logger (Implements NFR-009, THREAT-008)
#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace openidm {

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

#define RADIUM_LOG_INFO(comp, msg) ::openidm::Logger::Instance().Log(::openidm::LogLevel::kInfo, (comp), (msg))
#define RADIUM_LOG_WARN(comp, msg) ::openidm::Logger::Instance().Log(::openidm::LogLevel::kWarn, (comp), (msg))
#define RADIUM_LOG_ERROR(comp, msg) ::openidm::Logger::Instance().Log(::openidm::LogLevel::kError, (comp), (msg))

// Backward compatibility macros
#ifndef OPENIDM_LOG_INFO
#define OPENIDM_LOG_INFO(comp, msg) RADIUM_LOG_INFO(comp, msg)
#endif
#ifndef OPENIDM_LOG_WARN
#define OPENIDM_LOG_WARN(comp, msg) RADIUM_LOG_WARN(comp, msg)
#endif
#ifndef OPENIDM_LOG_ERROR
#define OPENIDM_LOG_ERROR(comp, msg) RADIUM_LOG_ERROR(comp, msg)
#endif

} // namespace openidm
