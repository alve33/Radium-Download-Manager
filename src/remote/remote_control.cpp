// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/remote_control.hpp"
#include "common/json_mini.hpp"
#include "common/path_sanitizer.hpp"
#include "common/version.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <random>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dbghelp.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace radium::remote {

namespace {

static int64_t NowEpochS() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string ToLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string RandomHexToken(size_t bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 255);
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        int b = dist(gen);
        out.push_back(kHex[(b >> 4) & 0x0F]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

static json::Value TaskToJson(const TaskModel& t) {
    json::Object obj;
    obj["id"] = t.task_id;
    obj["url"] = t.url;
    obj["file_name"] = t.file_name;
    obj["save_dir"] = t.save_dir;
    obj["category"] = t.category;
    obj["queue_id"] = t.queue_id;
    obj["state"] = TaskStateToString(t.state);
    obj["downloaded_bytes"] = static_cast<int64_t>(t.downloaded_bytes);
    obj["total_bytes"] = static_cast<int64_t>(t.total_bytes);
    obj["speed_bps"] = static_cast<int64_t>(t.speed_bps);
    return json::Value(std::move(obj));
}

static std::string MakeRpcError(const json::Value& id, int code, const std::string& msg) {
    json::Object err;
    err["code"] = static_cast<int64_t>(code);
    err["message"] = msg;
    json::Object resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["error"] = json::Value(std::move(err));
    return json::Value(std::move(resp)).dump();
}

static std::string MakeRpcResult(const json::Value& id, json::Value result) {
    json::Object resp;
    resp["jsonrpc"] = "2.0";
    resp["id"] = id;
    resp["result"] = std::move(result);
    return json::Value(std::move(resp)).dump();
}

} // namespace

// ============================================================================
// 1. McpStdioServer
// ============================================================================

McpStdioServer::McpStdioServer(DownloadController& controller) : m_controller(controller) {}

std::string McpStdioServer::HandleMessage(const std::string& json_line) {
    auto req = json::Parse(json_line);
    if (!req.is_object()) {
        return MakeRpcError(json::Value(nullptr), -32700, "Parse error");
    }
    const json::Value id = req["id"];
    std::string method = req["method"].as_string();
    if (method.rfind("notifications/", 0) == 0 || id.is_null()) {
        return "";
    }

    if (method == "initialize") {
        json::Object caps;
        caps["tools"] = json::Value(json::Object{});
        json::Object info;
        info["name"] = "radium-download-manager";
        info["version"] = RADIUM_VERSION_STRING;
        json::Object res;
        res["protocolVersion"] = "2024-11-05";
        res["capabilities"] = json::Value(std::move(caps));
        res["serverInfo"] = json::Value(std::move(info));
        return MakeRpcResult(id, json::Value(std::move(res)));
    }

    if (method == "tools/list") {
        json::Array tools;
        auto add_tool = [&](const char* name, const char* desc) {
            json::Object schema;
            schema["type"] = "object";
            json::Object t;
            t["name"] = name;
            t["description"] = desc;
            t["inputSchema"] = json::Value(std::move(schema));
            tools.push_back(json::Value(std::move(t)));
        };
        add_tool("list_downloads", "List Radium downloads with optional status, category, queue, or search filters.");
        add_tool("add_download", "Add a download URL/magnet with optional folder, file_name, category, headers, or later flag.");
        add_tool("pause_downloads", "Pause downloads by ids array.");
        add_tool("resume_downloads", "Resume downloads by ids array.");
        add_tool("remove_downloads", "Remove downloads by ids array with optional delete_files flag.");
        add_tool("queue", "Start or stop a download queue (action: start|stop, queue: name).");
        json::Object res;
        res["tools"] = json::Value(std::move(tools));
        return MakeRpcResult(id, json::Value(std::move(res)));
    }

    if (method == "tools/call") {
        const auto& params = req["params"];
        std::string tool_name = params["name"].as_string();
        const auto& args = params["arguments"];

        auto wrap_text = [&](const json::Value& payload, bool is_err = false) {
            json::Array content;
            json::Object item;
            item["type"] = "text";
            item["text"] = payload.dump();
            content.push_back(json::Value(std::move(item)));
            json::Object res;
            res["content"] = json::Value(std::move(content));
            if (is_err) res["isError"] = true;
            return MakeRpcResult(id, json::Value(std::move(res)));
        };

        if (tool_name == "list_downloads" || tool_name == "list") {
            std::string filter_id = args["id"].as_string();
            std::string filter_status = ToLowerStr(args["status"].as_string());
            std::string filter_search = ToLowerStr(args["search"].as_string());
            std::string filter_cat = ToLowerStr(args["category"].as_string());
            size_t limit = static_cast<size_t>(std::max<int64_t>(1, args["limit"].as_int64(100)));

            json::Array arr;
            for (const auto& t : m_controller.ListTasks()) {
                if (!filter_id.empty() && t.task_id != filter_id) continue;
                if (!filter_status.empty() && ToLowerStr(TaskStateToString(t.state)) != filter_status) continue;
                if (!filter_cat.empty() && ToLowerStr(t.category) != filter_cat) continue;
                if (!filter_search.empty()) {
                    if (ToLowerStr(t.file_name).find(filter_search) == std::string::npos &&
                        ToLowerStr(t.url).find(filter_search) == std::string::npos) continue;
                }
                arr.push_back(TaskToJson(t));
                if (arr.size() >= limit) break;
            }
            json::Object out;
            out["downloads"] = json::Value(std::move(arr));
            return wrap_text(json::Value(std::move(out)));
        }

        if (tool_name == "add_download" || tool_name == "add") {
            std::string url = args["url"].as_string();
            if (url.empty()) return MakeRpcError(id, -32602, "Missing url parameter");
            TaskModel t;
            t.url = url;
            if (args["folder"].is_string() && !args["folder"].as_string().empty()) t.save_dir = args["folder"].as_string();
            if (args["file_name"].is_string() && !args["file_name"].as_string().empty()) t.file_name = args["file_name"].as_string();
            if (args["category"].is_string() && !args["category"].as_string().empty()) t.category = args["category"].as_string();
            if (args["queue"].is_string() && !args["queue"].as_string().empty()) t.queue_id = args["queue"].as_string();
            if (args["referer"].is_string() && !args["referer"].as_string().empty()) t.headers["Referer"] = args["referer"].as_string();
            if (args["user_agent"].is_string() && !args["user_agent"].as_string().empty()) t.headers["User-Agent"] = args["user_agent"].as_string();
            if (args["cookie"].is_string() && !args["cookie"].as_string().empty()) t.auth_headers["Cookie"] = args["cookie"].as_string();
            bool start_now = !args["later"].as_bool(false);
            auto r = m_controller.AddTask(std::move(t), start_now);
            if (!r.ok()) {
                json::Object err;
                err["error"] = r.error ? r.error->message : "AddTask failed";
                return wrap_text(json::Value(std::move(err)), true);
            }
            return wrap_text(TaskToJson(*r.value));
        }

        if (tool_name == "pause_downloads" || tool_name == "pause") {
            json::Array ok_ids;
            if (args["ids"].is_array()) {
                for (const auto& v : args["ids"].as_array()) {
                    std::string tid = v.as_string();
                    if (!tid.empty() && m_controller.PauseTask(tid).ok()) ok_ids.push_back(json::Value(tid));
                }
            }
            json::Object out;
            out["paused"] = json::Value(std::move(ok_ids));
            return wrap_text(json::Value(std::move(out)));
        }

        if (tool_name == "resume_downloads" || tool_name == "resume") {
            json::Array ok_ids;
            if (args["ids"].is_array()) {
                for (const auto& v : args["ids"].as_array()) {
                    std::string tid = v.as_string();
                    if (!tid.empty() && m_controller.ResumeTask(tid).ok()) ok_ids.push_back(json::Value(tid));
                }
            }
            json::Object out;
            out["resumed"] = json::Value(std::move(ok_ids));
            return wrap_text(json::Value(std::move(out)));
        }

        if (tool_name == "remove_downloads" || tool_name == "remove") {
            bool del_files = args["delete_files"].as_bool(false);
            json::Array ok_ids;
            if (args["ids"].is_array()) {
                for (const auto& v : args["ids"].as_array()) {
                    std::string tid = v.as_string();
                    if (!tid.empty() && m_controller.RemoveTask(tid, del_files).ok()) ok_ids.push_back(json::Value(tid));
                }
            }
            json::Object out;
            out["removed"] = json::Value(std::move(ok_ids));
            return wrap_text(json::Value(std::move(out)));
        }

        if (tool_name == "queue") {
            std::string action = ToLowerStr(args["action"].as_string("start"));
            std::string qid = args["queue"].as_string("main");
            if (action == "stop" || action == "pause") {
                m_controller.PauseAllTasks();
            } else {
                m_controller.StartAllTasks();
            }
            json::Object out;
            out["queue"] = qid;
            out["action"] = action;
            return wrap_text(json::Value(std::move(out)));
        }

        return MakeRpcError(id, -32601, "Unknown tool: " + tool_name);
    }

    return MakeRpcError(id, -32601, "Method not found: " + method);
}

void McpStdioServer::Run(std::istream& in, std::ostream& out, std::atomic<bool>& stop_flag) {
    std::string line;
    while (!stop_flag.load() && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string resp = HandleMessage(line);
        if (!resp.empty()) {
            out << resp << "\n" << std::flush;
        }
    }
}

// ============================================================================
// 2. RemoteControlServer (port 8463, paired devices)
// ============================================================================

RemoteControlServer::RemoteControlServer(DownloadController& controller)
    : m_controller(controller) {}

RemoteControlServer::~RemoteControlServer() {
    Stop();
}

std::string RemoteControlServer::GeneratePairingCode() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 999999);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06d", dist(gen));
    std::lock_guard<std::mutex> lk(m_auth_mu);
    m_active_code = buf;
    m_code_expires_at_s = NowEpochS() + 300;
    return m_active_code;
}

Result<PairedDevice> RemoteControlServer::PairWithCode(const std::string& code, const std::string& device_name) {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    if (m_active_code.empty() || code != m_active_code || NowEpochS() > m_code_expires_at_s) {
        return Result<PairedDevice>::Err("ERR_REMOTE_PAIR_CODE", "Invalid or expired pairing code");
    }
    m_active_code.clear();
    PairedDevice dev;
    dev.token = RandomHexToken(24);
    dev.name = device_name.empty() ? "Paired Device" : device_name;
    dev.paired_at_epoch_s = NowEpochS();
    m_devices.push_back(dev);
    return Result<PairedDevice>::Ok(dev);
}

bool RemoteControlServer::ValidateToken(const std::string& token) const {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(m_auth_mu);
    return std::any_of(m_devices.begin(), m_devices.end(), [&](const PairedDevice& d) { return d.token == token; });
}

std::vector<PairedDevice> RemoteControlServer::ListPairedDevices() const {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    return m_devices;
}

bool RemoteControlServer::RevokeDevice(const std::string& token) {
    std::lock_guard<std::mutex> lk(m_auth_mu);
    auto old_sz = m_devices.size();
    m_devices.erase(std::remove_if(m_devices.begin(), m_devices.end(),
                                   [&](const PairedDevice& d) { return d.token == token; }),
                    m_devices.end());
    return m_devices.size() != old_sz;
}

std::string RemoteControlServer::HandleHttpRequest(
    const std::string& method,
    const std::string& path,
    const std::string& auth_header,
    const std::string& body,
    int& out_status,
    std::string& out_content_type) {
    out_content_type = "application/json";
    if (method == "GET" && path == "/") {
        out_status = 200;
        out_content_type = "text/html; charset=utf-8";
        return "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
               "<title>Radium Remote</title></head><body><h1>Radium Remote Control</h1>"
               "<p>Authenticate via <code>POST /api/pair</code> with your 6-digit pairing code.</p></body></html>";
    }
    if (method == "POST" && path == "/api/pair") {
        auto j = json::Parse(body);
        auto pr = PairWithCode(j["code"].as_string(), j["device_name"].as_string());
        if (!pr.ok()) {
            out_status = 403;
            return R"({"error":"Invalid or expired pairing code"})";
        }
        out_status = 200;
        json::Object res;
        res["token"] = pr.value->token;
        res["name"] = pr.value->name;
        return json::Value(std::move(res)).dump();
    }

    std::string tok;
    if (auth_header.rfind("Bearer ", 0) == 0) tok = auth_header.substr(7);
    if (!ValidateToken(tok)) {
        out_status = 401;
        return R"({"error":"Unauthorized"})";
    }

    if (method == "GET" && path == "/api/tasks") {
        json::Array arr;
        for (const auto& t : m_controller.ListTasks()) arr.push_back(TaskToJson(t));
        out_status = 200;
        json::Object res;
        res["tasks"] = json::Value(std::move(arr));
        return json::Value(std::move(res)).dump();
    }
    if (method == "POST" && path == "/api/add") {
        auto j = json::Parse(body);
        std::string url = j["url"].as_string();
        if (url.empty()) {
            out_status = 400;
            return R"({"error":"Missing url"})";
        }
        TaskModel t;
        t.url = url;
        auto r = m_controller.AddTask(std::move(t), true);
        out_status = r.ok() ? 200 : 400;
        return r.ok() ? TaskToJson(*r.value).dump() : R"({"error":"AddTask failed"})";
    }
    if (method == "POST" && (path == "/api/pause" || path == "/api/resume" || path == "/api/remove")) {
        auto j = json::Parse(body);
        std::string tid = j["id"].as_string();
        Result<void> r = Result<void>::Err("ERR_INVALID", "Invalid action");
        if (path == "/api/pause") r = m_controller.PauseTask(tid);
        else if (path == "/api/resume") r = m_controller.ResumeTask(tid);
        else if (path == "/api/remove") r = m_controller.RemoveTask(tid, j["delete_files"].as_bool(false));
        out_status = r.ok() ? 200 : 404;
        return r.ok() ? R"({"ok":true})" : R"({"error":"Task action failed"})";
    }
    out_status = 404;
    return R"({"error":"Not found"})";
}

bool RemoteControlServer::Start(uint16_t port) {
#ifdef _WIN32
    if (m_running.load()) return true;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 16) != 0) {
        closesocket(ls);
        return false;
    }
    m_listen_sock = static_cast<uintptr_t>(ls);
    m_running.store(true);
    m_thread = std::thread([this, ls]() {
        while (m_running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv{0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            char buf[8192];
            int n = recv(c, buf, sizeof(buf), 0);
            if (n > 0) {
                std::string raw(buf, static_cast<size_t>(n));
                auto hdr_end = raw.find("\r\n\r\n");
                std::string hdrs = (hdr_end == std::string::npos) ? raw : raw.substr(0, hdr_end);
                std::string body = (hdr_end == std::string::npos) ? "" : raw.substr(hdr_end + 4);
                std::istringstream hss(hdrs);
                std::string method, path, auth;
                hss >> method >> path;
                std::string line;
                while (std::getline(hss, line)) {
                    if (ToLowerStr(line).rfind("authorization:", 0) == 0) {
                        auto col = line.find(':');
                        if (col != std::string::npos) {
                            auth = line.substr(col + 1);
                            while (!auth.empty() && (auth.front() == ' ' || auth.back() == '\r')) {
                                if (auth.front() == ' ') auth.erase(0, 1);
                                else auth.pop_back();
                            }
                        }
                    }
                }
                int status = 200;
                std::string ct;
                std::string resp_body = HandleHttpRequest(method, path, auth, body, status, ct);
                std::ostringstream oss;
                oss << "HTTP/1.1 " << status << " OK\r\nContent-Type: " << ct
                    << "\r\nContent-Length: " << resp_body.size() << "\r\nConnection: close\r\n\r\n" << resp_body;
                std::string resp = oss.str();
                send(c, resp.data(), static_cast<int>(resp.size()), 0);
            }
            closesocket(c);
        }
    });
    return true;
#else
    (void)port;
    return false;
#endif
}

void RemoteControlServer::Stop() {
#ifdef _WIN32
    m_running.store(false);
    if (m_listen_sock != static_cast<uintptr_t>(-1)) {
        closesocket(static_cast<SOCKET>(m_listen_sock));
        m_listen_sock = static_cast<uintptr_t>(-1);
    }
    if (m_thread.joinable()) m_thread.join();
#endif
}

// ============================================================================
// 3. Aria2RpcServer (JSON-RPC 2.0, port 6800)
// ============================================================================

Aria2RpcServer::Aria2RpcServer(DownloadController& controller, std::string secret_token)
    : m_controller(controller), m_secret(std::move(secret_token)) {}

Aria2RpcServer::~Aria2RpcServer() {
    Stop();
}

bool Aria2RpcServer::CheckSecret(const std::string& token_param) const {
    if (m_secret.empty()) return true;
    return token_param == ("token:" + m_secret);
}

std::string Aria2RpcServer::HandleJsonRpc(const std::string& json_body) {
    auto req = json::Parse(json_body);
    if (!req.is_object()) {
        return MakeRpcError(json::Value(nullptr), -32700, "Parse error");
    }
    const json::Value id = req["id"];
    std::string method = req["method"].as_string();
    const auto& params = req["params"];

    size_t param_idx = 0;
    if (!m_secret.empty()) {
        if (!params.is_array() || params.as_array().empty() || !CheckSecret(params.as_array()[0].as_string())) {
            return MakeRpcError(id, 1, "Unauthorized");
        }
        param_idx = 1;
    } else if (params.is_array() && !params.as_array().empty() &&
               params.as_array()[0].is_string() && params.as_array()[0].as_string().rfind("token:", 0) == 0) {
        param_idx = 1;
    }

    auto get_param = [&](size_t offset) -> json::Value {
        if (!params.is_array() || param_idx + offset >= params.as_array().size()) return json::Value(nullptr);
        return params.as_array()[param_idx + offset];
    };

    if (method == "aria2.getVersion") {
        json::Object res;
        res["version"] = "1.37.0";
        return MakeRpcResult(id, json::Value(std::move(res)));
    }

    if (method == "aria2.addUri") {
        auto uris = get_param(0);
        if (!uris.is_array() || uris.as_array().empty()) {
            return MakeRpcError(id, -32602, "Missing URIs");
        }
        TaskModel t;
        t.url = uris.as_array()[0].as_string();
        auto opts = get_param(1);
        if (opts.is_object()) {
            if (opts["dir"].is_string() && !opts["dir"].as_string().empty()) t.save_dir = opts["dir"].as_string();
            if (opts["out"].is_string() && !opts["out"].as_string().empty()) t.file_name = opts["out"].as_string();
            if (opts["referer"].is_string() && !opts["referer"].as_string().empty()) t.headers["Referer"] = opts["referer"].as_string();
            if (opts["user-agent"].is_string() && !opts["user-agent"].as_string().empty()) t.headers["User-Agent"] = opts["user-agent"].as_string();
        }
        auto r = m_controller.AddTask(std::move(t), true);
        if (!r.ok()) return MakeRpcError(id, 1, r.error ? r.error->message : "addUri failed");
        return MakeRpcResult(id, json::Value(r.value->task_id));
    }

    if (method == "aria2.tellActive" || method == "aria2.tellWaiting" || method == "aria2.tellStopped") {
        json::Array arr;
        for (const auto& t : m_controller.ListTasks()) {
            bool match = (method == "aria2.tellActive" && (t.state == TaskState::kDownloading || t.state == TaskState::kProbing)) ||
                         (method == "aria2.tellWaiting" && (t.state == TaskState::kQueued || t.state == TaskState::kPaused)) ||
                         (method == "aria2.tellStopped" && (t.state == TaskState::kCompleted || t.state == TaskState::kFailed || t.state == TaskState::kCanceled));
            if (match) {
                json::Object item;
                item["gid"] = t.task_id;
                item["status"] = TaskStateToString(t.state);
                item["totalLength"] = std::to_string(t.total_bytes);
                item["completedLength"] = std::to_string(t.downloaded_bytes);
                item["downloadSpeed"] = std::to_string(t.speed_bps);
                arr.push_back(json::Value(std::move(item)));
            }
        }
        return MakeRpcResult(id, json::Value(std::move(arr)));
    }

    if (method == "aria2.pause" || method == "aria2.forcePause") {
        std::string gid = get_param(0).as_string();
        auto r = m_controller.PauseTask(gid);
        if (!r.ok()) return MakeRpcError(id, 1, "Task not found");
        return MakeRpcResult(id, json::Value(gid));
    }

    if (method == "aria2.unpause") {
        std::string gid = get_param(0).as_string();
        auto r = m_controller.ResumeTask(gid);
        if (!r.ok()) return MakeRpcError(id, 1, "Task not found");
        return MakeRpcResult(id, json::Value(gid));
    }

    if (method == "aria2.remove" || method == "aria2.forceRemove") {
        std::string gid = get_param(0).as_string();
        auto r = m_controller.RemoveTask(gid, false);
        if (!r.ok()) return MakeRpcError(id, 1, "Task not found");
        return MakeRpcResult(id, json::Value(gid));
    }

    return MakeRpcError(id, -32601, "Method not found: " + method);
}

bool Aria2RpcServer::Start(uint16_t port) {
#ifdef _WIN32
    if (m_running.load()) return true;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 16) != 0) {
        closesocket(ls);
        return false;
    }
    m_listen_sock = static_cast<uintptr_t>(ls);
    m_running.store(true);
    m_thread = std::thread([this, ls]() {
        while (m_running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv{0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            char buf[16384];
            int n = recv(c, buf, sizeof(buf), 0);
            if (n > 0) {
                std::string raw(buf, static_cast<size_t>(n));
                auto hdr_end = raw.find("\r\n\r\n");
                std::string body = (hdr_end == std::string::npos) ? "" : raw.substr(hdr_end + 4);
                std::string resp_body = HandleJsonRpc(body);
                std::ostringstream oss;
                oss << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *"
                    << "\r\nContent-Length: " << resp_body.size() << "\r\nConnection: close\r\n\r\n" << resp_body;
                std::string resp = oss.str();
                send(c, resp.data(), static_cast<int>(resp.size()), 0);
            }
            closesocket(c);
        }
    });
    return true;
#else
    (void)port;
    return false;
#endif
}

void Aria2RpcServer::Stop() {
#ifdef _WIN32
    m_running.store(false);
    if (m_listen_sock != static_cast<uintptr_t>(-1)) {
        closesocket(static_cast<SOCKET>(m_listen_sock));
        m_listen_sock = static_cast<uintptr_t>(-1);
    }
    if (m_thread.joinable()) m_thread.join();
#endif
}

// ============================================================================
// 4. CrashWatchdog (Out-of-process crash & UI-freeze minidump monitor)
// ============================================================================

std::optional<CrashMonitorArgs> CrashMonitorArgs::Parse(int argc, const char* const* argv) {
    CrashMonitorArgs out;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == CrashWatchdog::kMonitorArg && i + 1 < argc) {
            out.pipe_name = argv[++i];
            found = true;
        } else if (a == "--data" && i + 1 < argc) {
            out.data_dir = argv[++i];
        } else if (a == "--parent" && i + 1 < argc) {
            try { out.parent_pid = static_cast<uint32_t>(std::stoul(argv[++i])); } catch (...) {}
        }
    }
    if (!found || out.pipe_name.empty() || out.parent_pid == 0) return std::nullopt;
    return out;
}

Result<std::filesystem::path> CrashWatchdog::WriteParentMinidump(
    uint32_t parent_pid,
    const std::filesystem::path& dump_dir,
    const std::string& reason) {
#ifdef _WIN32
    (void)reason;
    std::error_code ec;
    std::filesystem::create_directories(dump_dir, ec);
    auto dump_path = dump_dir / ("radium_" + std::to_string(parent_pid) + "_" + std::to_string(NowEpochS()) + ".dmp");

    HMODULE hDbgHelp = LoadLibraryW(L"dbghelp.dll");
    if (!hDbgHelp) {
        return Result<std::filesystem::path>::Err("ERR_DBGHELP", "Failed to load dbghelp.dll");
    }
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                              PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION,
                                              PMINIDUMP_CALLBACK_INFORMATION);
    auto fn = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(hDbgHelp, "MiniDumpWriteDump"));
    if (!fn) {
        FreeLibrary(hDbgHelp);
        return Result<std::filesystem::path>::Err("ERR_DBGHELP", "MiniDumpWriteDump not found");
    }

    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, parent_pid);
    if (!hProc) {
        FreeLibrary(hDbgHelp);
        return Result<std::filesystem::path>::Err("ERR_OPEN_PROCESS", "Failed to open parent process for minidump");
    }
    HANDLE hFile = CreateFileW(dump_path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        CloseHandle(hProc);
        FreeLibrary(hDbgHelp);
        return Result<std::filesystem::path>::Err("ERR_DUMP_FILE", "Failed to create minidump file");
    }

    BOOL ok = fn(hProc, parent_pid, hFile, MiniDumpWithThreadInfo, nullptr, nullptr, nullptr);
    CloseHandle(hFile);
    CloseHandle(hProc);
    FreeLibrary(hDbgHelp);
    if (!ok) {
        return Result<std::filesystem::path>::Err("ERR_MINIDUMP", "MiniDumpWriteDump failed");
    }
    return Result<std::filesystem::path>::Ok(dump_path);
#else
    (void)parent_pid;
    (void)dump_dir;
    (void)reason;
    return Result<std::filesystem::path>::Err("ERR_UNSUPPORTED", "Windows minidump only");
#endif
}

bool CrashWatchdog::StartMonitorProcess(
    const std::filesystem::path& exe_path,
    const std::filesystem::path& data_dir) {
#ifdef _WIN32
    DWORD pid = GetCurrentProcessId();
    std::wstring pipe_name = L"\\\\.\\pipe\\RadiumCrash_" + std::to_wstring(pid);
    std::wstring cmd = L"\"" + exe_path.wstring() + L"\" --crash-monitor \"" + pipe_name +
                       L"\" --data \"" + data_dir.wstring() + L"\" --parent " + std::to_wstring(pid);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_FORCEOFFFEEDBACK | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)exe_path;
    (void)data_dir;
    return false;
#endif
}

int CrashWatchdog::RunMonitorLoop(const CrashMonitorArgs& args) {
#ifdef _WIN32
    // Clear any Windows app-starting cursor feedback immediately for this headless monitor process.
    MSG init_msg{};
    PeekMessageW(&init_msg, nullptr, 0, 0, PM_NOREMOVE);
    HANDLE hParent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, args.parent_pid);
    if (!hParent) return 1;
    std::wstring wpipe(args.pipe_name.begin(), args.pipe_name.end());
    HANDLE hPipe = CreateNamedPipeW(
        wpipe.c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
        1, 4096, 4096, 0, nullptr);

    while (WaitForSingleObject(hParent, 200) == WAIT_TIMEOUT) {
        if (hPipe != INVALID_HANDLE_VALUE) {
            ConnectNamedPipe(hPipe, nullptr);
            char buf[256]{};
            DWORD got = 0;
            if (ReadFile(hPipe, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
                std::string msg(buf, got);
                if (msg.find("DUMP") != std::string::npos) {
                    WriteParentMinidump(args.parent_pid, args.data_dir / "crashes", msg);
                }
            }
        }
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(hParent, &exit_code);
    if (exit_code != 0) {
        WriteParentMinidump(args.parent_pid, args.data_dir / "crashes", "abnormal_exit");
    }
    if (hPipe != INVALID_HANDLE_VALUE) CloseHandle(hPipe);
    CloseHandle(hParent);
    return 0;
#else
    (void)args;
    return 1;
#endif
}

} // namespace radium::remote
