// SPDX-License-Identifier: Apache-2.0
// Radium -- Main Desktop GUI, CLI & Daemon Application Entry Point (radium)
// Default launch (no arguments):
//   Starts the background IPC server and launches the Radium Desktop GUI application window.
// CLI flags:
//   --version                    Print version info
//   --gui-snapshot               Render full desktop UI snapshot (sidebar counts, table, details, status bar) and exit
//   --list --db <state.db>       Display the unified task queue table
//   --download <url> --out <dir> Download an ordinary HTTP/HTTPS file using Brisk engine
//   --stream <url> --out <dir>   Download & mux an HLS/DASH stream using N_m3u8DL-RE + FFmpeg
//   --probe-stream <url>         Probe an HLS/DASH manifest and print JSON tracks
//   --daemon --ipc-socket <path> Run background daemon with IPC Server listening for Browser Extension

#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/version.hpp"
#include "core/download_controller.hpp"
#include "ipc/ipc_server.hpp"
#include "platform/windows_integrations.hpp"
#include "remote/remote_control.hpp"
#include "ui/win32_main_window.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
std::atomic<bool> g_terminate_daemon{false};
void SignalHandler(int) { g_terminate_daemon.store(true); }

void WriteConsoleOutput(const std::string& str, bool is_error = false) {
#ifdef _WIN32
    HANDLE hStd = GetStdHandle(is_error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (hStd && hStd != INVALID_HANDLE_VALUE) {
        DWORD outType = GetFileType(hStd);
        if (outType == FILE_TYPE_PIPE || outType == FILE_TYPE_DISK || outType == FILE_TYPE_CHAR) {
            DWORD written = 0;
            if (WriteFile(hStd, str.data(), static_cast<DWORD>(str.size()), &written, nullptr) && written > 0) {
                FlushFileBuffers(hStd);
                return;
            }
        }
    }
#endif
    if (is_error) {
        std::cerr << str;
        std::cerr.flush();
        std::fflush(stderr);
    } else {
        std::cout << str;
        std::cout.flush();
        std::fflush(stdout);
    }
}
} // namespace

int main(int argc, char* argv[]) {
    if (auto cm_args = radium::remote::CrashMonitorArgs::Parse(argc, argv)) {
        return radium::remote::CrashWatchdog::RunMonitorLoop(*cm_args);
    }
#if defined(__GLIBC__)
    // Cap glibc malloc arenas to 2 so 8-connection worker threads do not allocate 8 x 64 MiB virtual/resident arenas
    mallopt(M_ARENA_MAX, 2);
#endif
    std::filesystem::path exe_dir = radium::PathSanitizer::GetExecutableDir(argc > 0 ? argv[0] : nullptr);
    std::filesystem::path app_data_dir = radium::PathSanitizer::GetDefaultAppDataDir();

    radium::ControllerConfig cfg;
    cfg.state_db_path = app_data_dir / "state.db";
    cfg.default_download_dir = radium::PathSanitizer::GetDefaultDownloadsDir(exe_dir);
    cfg.temp_root_dir = app_data_dir / "tmp";
    cfg.nm3u8dl_binary_path = radium::PathSanitizer::FindWorkerBinary("N_m3u8DL-RE", exe_dir);
    cfg.ffmpeg_binary_path = radium::PathSanitizer::FindWorkerBinary("ffmpeg", exe_dir);
    // SEC-04: Block public-to-loopback/private redirects by default unless explicitly configured
    cfg.allow_loopback = (std::getenv("RADIUM_ALLOW_LOOPBACK") != nullptr || std::getenv("FLUX_ALLOW_LOOPBACK") != nullptr || std::getenv("OPENIDM_ALLOW_LOOPBACK") != nullptr);

    std::string mode;
    std::string target_url;
    std::vector<std::string> target_ids;
    std::vector<uint32_t> select_indexes;
    bool json_output = false;
    bool delete_files = false;
    uint32_t wait_timeout_ms = 300000;
    std::string ipc_socket_path = radium::ipc::IpcServer::DefaultSocketPath();
    uint32_t connections = 8;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            WriteConsoleOutput(RADIUM_FULL_VERSION_STRING, false);
            return 0;
        } else if (arg == "--db" && i + 1 < argc) {
            cfg.state_db_path = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            cfg.default_download_dir = argv[++i];
        } else if (arg == "--connections" && i + 1 < argc) {
            connections = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--download" && i + 1 < argc) {
            mode = "download";
            target_url = argv[++i];
        } else if (arg == "--stream" && i + 1 < argc) {
            mode = "stream";
            target_url = argv[++i];
        } else if (arg == "--probe-stream" && i + 1 < argc) {
            mode = "probe";
            target_url = argv[++i];
        } else if (arg == "--list" || arg == "tasks") {
            mode = "list";
        } else if (arg == "--mcp") {
            mode = "mcp";
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "--delete-files") {
            delete_files = true;
        } else if (arg == "--timeout" && i + 1 < argc) {
            wait_timeout_ms = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--select" && i + 1 < argc) {
            std::istringstream iss(argv[++i]);
            std::string tok;
            while (std::getline(iss, tok, ',')) {
                if (!tok.empty()) {
                    try { select_indexes.push_back(static_cast<uint32_t>(std::stoul(tok))); } catch (...) {}
                }
            }
        } else if ((arg == "wait" || arg == "pause" || arg == "start" || arg == "remove") && mode.empty()) {
            mode = arg;
        } else if (arg == "add" && mode.empty()) {
            mode = "add";
        } else if (arg == "--gui-snapshot") {
            mode = "gui-snapshot";
        } else if (arg == "--daemon") {
            mode = "daemon";
        } else if (arg == "--minimized") {
            mode = "minimized";
        } else if (arg == "--ipc-socket" && i + 1 < argc) {
            ipc_socket_path = argv[++i];
        } else if (arg == "--worker-binary" && i + 1 < argc) {
            cfg.nm3u8dl_binary_path = argv[++i];
        } else if (arg == "--ffmpeg-binary" && i + 1 < argc) {
            cfg.ffmpeg_binary_path = argv[++i];
        } else if (arg == "--allow-loopback") {
            cfg.allow_loopback = true;
        } else if (arg.rfind("radium://", 0) == 0) {
            auto parsed_uri = radium::platform::WindowsIntegrations::ParseLaunchUri(arg);
            if (parsed_uri.valid && !parsed_uri.wake_only && !parsed_uri.url.empty()) {
                target_url = parsed_uri.url;
            }
        } else if (arg.size() > 8 && arg.rfind(".torrent") == arg.size() - 8) {
            target_url = arg;
        } else if (!arg.empty() && arg[0] != '-') {
            if (mode == "add" && target_url.empty()) {
                target_url = arg;
            } else if (mode == "wait" || mode == "pause" || mode == "start" || mode == "remove") {
                target_ids.push_back(arg);
            }
        }
    }

    radium::Logger::Instance().SetLogFile(cfg.state_db_path.parent_path() / "radium.log");
    RADIUM_LOG_INFO("App", "Radium Download Manager v" + std::string(RADIUM_VERSION_STRING) + " initialized on Windows");

#ifdef _WIN32
    std::wstring wpipe(ipc_socket_path.begin(), ipc_socket_path.end());

    // 1. Single-instance check for GUI mode (mode.empty() || mode == "minimized")
    if (mode.empty() || mode == "minimized") {
        HWND existing_hwnd = FindWindowW(L"Radium_MainWindowClass", nullptr);
        if (existing_hwnd) {
            DWORD existing_pid = 0;
            GetWindowThreadProcessId(existing_hwnd, &existing_pid);
            RADIUM_LOG_INFO("App", "Found existing Radium window hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(existing_hwnd)) + " pid=" + std::to_string(existing_pid));
            if (mode.empty()) {
                ShowWindow(existing_hwnd, SW_SHOW);
                ShowWindow(existing_hwnd, SW_RESTORE);
                SetForegroundWindow(existing_hwnd);
                PostMessageW(existing_hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            }
            return 0; // Seamless single instance: bring existing GUI window to front and exit
        }

        // If a background daemon is running, signal it to exit so the user's interactive GUI can take over the pipe & database
        HANDLE hDaemonShutdown = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, L"Local\\Radium_Daemon_Shutdown_Event");
        if (hDaemonShutdown) {
            RADIUM_LOG_INFO("App", "Signaling background daemon shutdown");
            SetEvent(hDaemonShutdown);
            CloseHandle(hDaemonShutdown);
            for (int i = 0; i < 30; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                HANDLE hTest = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (hTest == INVALID_HANDLE_VALUE) {
                    break;
                }
                CloseHandle(hTest);
            }
        }
    }

    // 2. Single-instance check for background daemon mode
    if (mode == "daemon") {
        HWND existing_hwnd = FindWindowW(L"Radium_MainWindowClass", nullptr);
        if (existing_hwnd) {
            // Desktop GUI is already running and providing the IPC server on this pipe
            return 0;
        }
        HANDLE hTest = CreateFileW(wpipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hTest != INVALID_HANDLE_VALUE) {
            CloseHandle(hTest);
            // Another background daemon or host is already actively serving this IPC pipe
            return 0;
        }
    }
#endif

    radium::DownloadController controller(cfg);
    RADIUM_LOG_INFO("App", "Calling controller.Initialize()");
    auto init_res = controller.Initialize();
    if (!init_res.ok()) {
        RADIUM_LOG_INFO("App", "ERROR: controller.Initialize failed: " + init_res.error->message);
        WriteConsoleOutput("ERROR: Failed to initialize Radium controller: " + init_res.error->message + "\n", true);
        return 1;
    }
    RADIUM_LOG_INFO("App", "controller.Initialize() succeeded, mode=" + mode);

    // Default launch path: Launch the Desktop GUI Application + IPC Server (BUG-002 Fix)
    if (mode.empty() || mode == "minimized") {
        std::string exe_arg0 = (argc > 0 && argv[0]) ? std::string(argv[0]) : std::string();
        std::thread([exe_arg0, app_data_dir]() {
            if (std::getenv("RADIUM_NO_CRASH_MONITOR") == nullptr && !exe_arg0.empty()) {
                radium::remote::CrashWatchdog::StartMonitorProcess(exe_arg0.c_str(), app_data_dir);
            }
            radium::platform::WindowsIntegrations::RegisterUrlScheme();
            radium::platform::WindowsIntegrations::RegisterTorrentAssociation();
        }).detach();
        if (!target_url.empty()) {
            radium::TaskModel t;
            t.url = target_url;
            controller.AddTask(std::move(t), true);
        }
        RADIUM_LOG_INFO("App", "Starting IpcServer on pipe=" + ipc_socket_path);
        radium::ipc::IpcServer server(controller, ipc_socket_path);
        server.Start();
        RADIUM_LOG_INFO("App", "Creating DesktopView");
        radium::ui::DesktopView view(controller, &server);
        if (mode == "minimized") {
            view.SetStartVisible(false);
            view.SetFirstLaunchOnboardingEligible(false);
        } else {
            view.SetStartVisible(true);
            view.SetFirstLaunchOnboardingEligible(target_url.empty());
        }
        RADIUM_LOG_INFO("App", "Entering RunApplicationLoop");
        int rc = view.RunApplicationLoop(false);
        RADIUM_LOG_INFO("App", "Exited RunApplicationLoop with rc=" + std::to_string(rc));
        server.Stop();
        controller.Shutdown();
        return rc;
    }

    if (mode == "list") {
        if (json_output) {
            radium::json::Array arr;
            for (const auto& t : controller.ListTasks()) {
                radium::json::Object obj;
                obj["id"] = t.task_id;
                obj["url"] = t.url;
                obj["file_name"] = t.file_name;
                obj["state"] = radium::TaskStateToString(t.state);
                obj["downloaded_bytes"] = static_cast<int64_t>(t.downloaded_bytes);
                obj["total_bytes"] = static_cast<int64_t>(t.total_bytes);
                arr.push_back(radium::json::Value(std::move(obj)));
            }
            radium::json::Object out;
            out["result"] = radium::json::Value(std::move(arr));
            WriteConsoleOutput(radium::json::Value(std::move(out)).dump() + "\n", false);
        } else {
            radium::ui::DesktopView view(controller);
            WriteConsoleOutput(view.RenderQueueTableText(), false);
        }
        controller.Shutdown();
        return 0;
    }

    if (mode == "mcp") {
        std::atomic<bool> stop_mcp{false};
        radium::remote::McpStdioServer mcp(controller);
        mcp.Run(std::cin, std::cout, stop_mcp);
        controller.Shutdown();
        return 0;
    }

    if (mode == "add") {
        if (target_url.empty()) {
            if (json_output) {
                WriteConsoleOutput(R"({"error":{"code":"ERR_USAGE","message":"Missing URL"}})" "\n", false);
            }
            controller.Shutdown();
            return 2;
        }
        radium::TaskModel t;
        t.url = target_url;
        t.max_connections = connections;
        t.save_dir = cfg.default_download_dir.string();
        if (!select_indexes.empty()) {
            uint32_t max_idx = *std::max_element(select_indexes.begin(), select_indexes.end());
            t.torrent_file_selection.assign(static_cast<size_t>(max_idx) + 1, 0);
            for (uint32_t idx : select_indexes) {
                t.torrent_file_selection[idx] = 1;
            }
        }
        auto added = controller.AddTask(std::move(t), true, true);
        if (!added.ok()) {
            if (json_output) {
                radium::json::Object e;
                e["code"] = added.error ? added.error->code : "ERR_ADD";
                e["message"] = added.error ? added.error->message : "AddTask failed";
                radium::json::Object err;
                err["error"] = radium::json::Value(std::move(e));
                WriteConsoleOutput(radium::json::Value(std::move(err)).dump() + "\n", false);
            } else {
                WriteConsoleOutput("ERROR: " + added.error->message + "\n", true);
            }
            controller.Shutdown();
            return 1;
        }
        if (json_output) {
            radium::json::Object res;
            res["id"] = added.value->task_id;
            res["file_name"] = added.value->file_name;
            radium::json::Object out;
            out["result"] = radium::json::Value(std::move(res));
            WriteConsoleOutput(radium::json::Value(std::move(out)).dump() + "\n", false);
        } else {
            WriteConsoleOutput(added.value->task_id + "\n", false);
        }
        controller.Shutdown();
        return 0;
    }

    if (mode == "wait") {
        bool all_ok = !target_ids.empty();
        radium::json::Array arr;
        for (const auto& tid : target_ids) {
            auto done = controller.WaitForTaskCompletion(tid, wait_timeout_ms);
            bool ok = done.ok() && done.value->state == radium::TaskState::kCompleted;
            if (!ok) all_ok = false;
            radium::json::Object item;
            item["id"] = tid;
            item["state"] = done.ok() ? radium::TaskStateToString(done.value->state) : "ERROR";
            arr.push_back(radium::json::Value(std::move(item)));
        }
        if (json_output) {
            radium::json::Object out;
            out["result"] = radium::json::Value(std::move(arr));
            WriteConsoleOutput(radium::json::Value(std::move(out)).dump() + "\n", false);
        }
        controller.Shutdown();
        return all_ok ? 0 : 1;
    }

    if (mode == "pause" || mode == "start" || mode == "remove") {
        bool all_ok = !target_ids.empty();
        radium::json::Array arr;
        for (const auto& tid : target_ids) {
            radium::Result<void> r = radium::Result<void>::Err("ERR_USAGE", "Unknown");
            if (mode == "pause") r = controller.PauseTask(tid);
            else if (mode == "start") r = controller.ResumeTask(tid);
            else if (mode == "remove") r = controller.RemoveTask(tid, delete_files);
            if (!r.ok()) all_ok = false;
            radium::json::Object item;
            item["id"] = tid;
            item["ok"] = r.ok();
            arr.push_back(radium::json::Value(std::move(item)));
        }
        if (json_output) {
            radium::json::Object out;
            out["result"] = radium::json::Value(std::move(arr));
            WriteConsoleOutput(radium::json::Value(std::move(out)).dump() + "\n", false);
        }
        controller.Shutdown();
        return all_ok ? 0 : 1;
    }

    if (mode == "gui-snapshot") {
        radium::ui::DesktopView view(controller, nullptr);
        view.SelectFirstRow();
        WriteConsoleOutput(view.RenderFullDesktopSnapshotText(), false);
        controller.Shutdown();
        return 0;
    }

    if (mode == "probe") {
        auto pres = controller.ProbeStream(target_url, {}, {});
        if (!pres.ok()) {
            std::string err_str = "ERROR: " + pres.error->code + ": " + pres.error->message + "\n";
            WriteConsoleOutput(err_str, true);
            controller.Shutdown();
            return 2;
        }
        std::string probe_summary = "Probed " + std::to_string(pres.value->tracks.size()) + " tracks from " + pres.value->manifest_url + "\n";
        for (const auto& tr : pres.value->tracks) {
            std::string upper_type = tr.media_type;
            for (char& c : upper_type) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            probe_summary += "  [" + upper_type + "] id=" + tr.id
                          + " res=" + tr.resolution + " bw=" + std::to_string(tr.bandwidth)
                          + " lang=" + tr.language + " segs=" + std::to_string(tr.segments_count) + "\n";
        }
        WriteConsoleOutput(probe_summary, false);
        controller.Shutdown();
        return 0;
    }

    if (mode == "download" || mode == "stream") {
        radium::TaskModel t;
        t.url = target_url;
        t.max_connections = connections;
        t.save_dir = cfg.default_download_dir.string();
        if (mode == "stream") {
            t.task_type = (target_url.find(".mpd") != std::string::npos)
                              ? radium::TaskType::kStreamDash
                              : radium::TaskType::kStreamHls;
        } else {
            t.task_type = radium::TaskType::kHttpFile;
        }
        auto added = controller.AddTask(std::move(t), true, true);
        if (!added.ok()) {
            WriteConsoleOutput("ERROR: " + added.error->message + "\n", true);
            controller.Shutdown();
            return 1;
        }
        auto done = controller.WaitForTaskCompletion(added.value->task_id, 300000);
        controller.Shutdown();
        if (!done.ok() || done.value->state != radium::TaskState::kCompleted) {
            std::string err_msg = "ERROR: Task failed: " + (done.ok() ? done.value->error_message : done.error->message) + "\n";
            WriteConsoleOutput(err_msg, true);
            return 2;
        }
        std::string comp_msg = "COMPLETED: " + (std::filesystem::path(done.value->save_dir) / done.value->file_name).string()
                             + " (" + std::to_string(done.value->downloaded_bytes) + " bytes)\n";
        WriteConsoleOutput(comp_msg, false);
        return 0;
    }

    if (mode == "daemon") {
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
#ifdef _WIN32
        HANDLE hShutdownEvent = CreateEventW(nullptr, FALSE, FALSE, L"Local\\Radium_Daemon_Shutdown_Event");
#endif
        radium::ipc::IpcServer server(controller, ipc_socket_path);
        server.SetHeadlessDaemonMode(true);
        auto sres = server.Start();
        if (!sres.ok()) {
            WriteConsoleOutput("ERROR: Failed to start IPC server: " + sres.error->message + "\n", true);
            controller.Shutdown();
#ifdef _WIN32
            if (hShutdownEvent) CloseHandle(hShutdownEvent);
#endif
            return 1;
        }
        while (!g_terminate_daemon.load()) {
#ifdef _WIN32
            if (hShutdownEvent) {
                DWORD wr = WaitForSingleObject(hShutdownEvent, 50);
                if (wr == WAIT_OBJECT_0) {
                    g_terminate_daemon.store(true);
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
#endif
        }
        server.Stop();
        controller.Shutdown();
#ifdef _WIN32
        if (hShutdownEvent) CloseHandle(hShutdownEvent);
#endif
        return 0;
    }

    controller.Shutdown();
    return 0;
}

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // If CLI arguments were passed and we're not redirected to a pipe/file, attach to parent console
    if (argc > 1) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD outType = (hOut && hOut != INVALID_HANDLE_VALUE) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;
        if (outType != FILE_TYPE_PIPE && outType != FILE_TYPE_DISK) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
            }
        }
    }

    std::ios::sync_with_stdio(true);

    std::vector<std::string> utf8_args;
    std::vector<char*> argv_ptrs;
    for (int i = 0; i < argc; ++i) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(std::max(0, len > 0 ? len - 1 : 0), '\0');
        if (len > 1) {
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len, nullptr, nullptr);
        }
        utf8_args.push_back(std::move(s));
    }
    if (wargv) LocalFree(wargv);
    for (auto& s : utf8_args) {
        argv_ptrs.push_back(s.data());
    }
    int ret = main(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    return ret;
}
#endif
