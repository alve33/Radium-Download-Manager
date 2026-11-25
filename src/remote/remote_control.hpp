// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common/types.hpp"
#include "core/download_controller.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <istream>
#include <map>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <vector>

namespace radium::remote {

// ============================================================================
// 1. Stdio MCP Server (MDM crates/mcp port)
// ============================================================================

class McpStdioServer {
public:
    explicit McpStdioServer(DownloadController& controller);

    // Processes a single JSON-RPC 2.0 line and returns the serialized JSON response (empty for notifications)
    std::string HandleMessage(const std::string& json_line);

    // Blocking stdio loop until EOF or stop_flag
    void Run(std::istream& in, std::ostream& out, std::atomic<bool>& stop_flag);

private:
    DownloadController& m_controller;
};

// ============================================================================
// 2. Paired-Device Remote Control Web UI (MDM crates/remote port, port 8463)
// ============================================================================

struct PairedDevice {
    std::string token;
    std::string name;
    int64_t paired_at_epoch_s{0};
};

class RemoteControlServer {
public:
    static constexpr uint16_t kDefaultPort = 8463;

    explicit RemoteControlServer(DownloadController& controller);
    ~RemoteControlServer();

    std::string GeneratePairingCode();
    Result<PairedDevice> PairWithCode(const std::string& code, const std::string& device_name);
    bool ValidateToken(const std::string& token) const;
    std::vector<PairedDevice> ListPairedDevices() const;
    bool RevokeDevice(const std::string& token);

    std::string HandleHttpRequest(
        const std::string& method,
        const std::string& path,
        const std::string& auth_header,
        const std::string& body,
        int& out_status,
        std::string& out_content_type);

    bool Start(uint16_t port = kDefaultPort);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

private:
    DownloadController& m_controller;
    mutable std::mutex m_auth_mu;
    std::string m_active_code;
    int64_t m_code_expires_at_s{0};
    std::vector<PairedDevice> m_devices;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    uintptr_t m_listen_sock{static_cast<uintptr_t>(-1)};
};

// ============================================================================
// 3. Aria2 JSON-RPC 2.0 Listener (GD3 aria2_rpc.py port, port 6800)
// ============================================================================

class Aria2RpcServer {
public:
    static constexpr uint16_t kDefaultPort = 6800;

    Aria2RpcServer(DownloadController& controller, std::string secret_token = "");
    ~Aria2RpcServer();

    std::string HandleJsonRpc(const std::string& json_body);

    bool Start(uint16_t port = kDefaultPort);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

private:
    bool CheckSecret(const std::string& token_param) const;
    DownloadController& m_controller;
    std::string m_secret;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    uintptr_t m_listen_sock{static_cast<uintptr_t>(-1)};
};

// ============================================================================
// 4. Out-of-Process Crash & UI-Freeze Watchdog (MDM crates/crash port)
// ============================================================================

struct CrashMonitorArgs {
    std::string pipe_name;
    std::filesystem::path data_dir;
    uint32_t parent_pid{0};

    static std::optional<CrashMonitorArgs> Parse(int argc, const char* const* argv);
};

class CrashWatchdog {
public:
    static constexpr const char* kMonitorArg = "--crash-monitor";

    static bool StartMonitorProcess(
        const std::filesystem::path& exe_path,
        const std::filesystem::path& data_dir);
    static int RunMonitorLoop(const CrashMonitorArgs& args);
    static Result<std::filesystem::path> WriteParentMinidump(
        uint32_t parent_pid,
        const std::filesystem::path& dump_dir,
        const std::string& reason);
};

} // namespace radium::remote
