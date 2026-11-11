// SPDX-License-Identifier: Apache-2.0
// OpenIDM / Radium -- YouTube Stream Background Pre-warmer
// Resolves deciphered googlevideo media URLs in the background while user browses,
// enabling zero-wait (<10ms) direct native download dispatch on click.
#pragma once

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace openidm::stream {

struct PrewarmedQuality {
    std::string format_id;
    std::string resolution;     // "1080p", "720p", "480p", "360p", "240p", "144p"
    int height{0};
    int width{0};
    std::string ext;           // "mp4", "webm"
    std::string video_url;
    std::string audio_url;
    std::map<std::string, std::string> http_headers;
    bool is_audio_only{false};
    uint64_t expire_epoch{0};
};

struct PrewarmedVideoEntry {
    std::string video_id;
    std::string watch_url;
    std::string title;
    std::string cookies;
    std::map<std::string, PrewarmedQuality> qualities_by_res; // "1080p", "720p", etc.
    std::map<std::string, PrewarmedQuality> qualities_by_fmt; // "137", "398", etc.
    std::string best_video_url;
    std::string best_audio_url;
    std::map<std::string, std::string> best_video_headers;
    std::map<std::string, std::string> best_audio_headers;
    std::string best_video_res{"1080p"};
    std::chrono::steady_clock::time_point fetched_tp;
    bool is_ready{false};
    bool is_fetching{false};
};

class YouTubeStreamPrewarmer {
public:
    static YouTubeStreamPrewarmer& Instance() {
        static YouTubeStreamPrewarmer s_instance;
        return s_instance;
    }

    void PrewarmAsync(const std::string& video_id_in, const std::string& watch_url_in, const std::string& cookie_str = "") {
        std::string vid = video_id_in;
        std::string url = watch_url_in;
        if (vid.empty() && !url.empty()) {
            vid = ExtractVideoId(url);
        }
        if (vid.empty()) return;
        if (url.empty()) {
            url = "https://www.youtube.com/watch?v=" + vid;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto now = std::chrono::steady_clock::now();
            auto it = m_cache.find(vid);
            if (it != m_cache.end()) {
                if (it->second.is_fetching) return; // already in flight
                if (it->second.is_ready) {
                    auto age_sec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.fetched_tp).count();
                    if (age_sec < 7200) return; // cache valid for 2 hours
                }
            }

            auto& entry = m_cache[vid];
            entry.video_id = vid;
            entry.watch_url = url;
            if (!cookie_str.empty()) entry.cookies = cookie_str;
            entry.is_fetching = true;
            entry.is_ready = false;
        }

        OPENIDM_LOG_INFO("Prewarmer", "Starting background stream resolution for YouTube videoId: " + vid);
        std::thread([this, vid, url, cookie_str]() {
            try {
                ExecuteResolution(vid, url, cookie_str);
            } catch (const std::exception& e) {
                OPENIDM_LOG_ERROR("Prewarmer", "Exception in background prewarm for " + vid + ": " + e.what());
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_cache[vid].is_fetching = false;
                }
                m_cv.notify_all();
            } catch (...) {
                OPENIDM_LOG_ERROR("Prewarmer", "Unknown exception in background prewarm for " + vid);
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_cache[vid].is_fetching = false;
                }
                m_cv.notify_all();
            }
        }).detach();
    }

    std::string GetTitle(const std::string& video_id_in) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string vid = video_id_in;
        if (vid.empty()) return "";
        auto it = m_cache.find(vid);
        if (it != m_cache.end() && it->second.is_ready) {
            return it->second.title;
        }
        return "";
    }

    bool ResolveSync(const std::string& video_id_in, const std::string& watch_url_in,
                     const std::string& resolution_or_filter,
                     std::string& out_video_url, std::string& out_audio_url, std::string& out_resolution,
                     std::string* out_cookies = nullptr,
                     std::map<std::string, std::string>* out_v_headers = nullptr,
                     std::map<std::string, std::string>* out_a_headers = nullptr,
                     std::chrono::milliseconds wait_for_inflight = std::chrono::milliseconds(0)) {
        std::string vid = video_id_in;
        std::string url = watch_url_in;
        if (vid.empty() && !url.empty()) vid = ExtractVideoId(url);
        if (vid.empty()) return false;
        if (url.empty()) url = "https://www.youtube.com/watch?v=" + vid;

        // 1. Immediate non-blocking cache lookup (<0.1ms)
        if (GetDirectUrls(vid, resolution_or_filter, out_video_url, out_audio_url, out_resolution, out_cookies, out_v_headers, out_a_headers)) {
            return true;
        }

        // Optionally let a navigation-started prewarm finish when the caller
        // has already observed that its cached signed URL is expired. This is
        // bounded by the caller and is never applied to an ordinary cold path.
        bool prewarm_was_in_flight = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            auto it = m_cache.find(vid);
            prewarm_was_in_flight = (it != m_cache.end() && it->second.is_fetching);
            if (prewarm_was_in_flight && wait_for_inflight > std::chrono::milliseconds::zero()) {
                m_cv.wait_for(lock, wait_for_inflight, [&]() {
                    auto current = m_cache.find(vid);
                    return current == m_cache.end() || !current->second.is_fetching;
                });
            }
        }

        if (GetDirectUrls(vid, resolution_or_filter, out_video_url, out_audio_url, out_resolution,
                          out_cookies, out_v_headers, out_a_headers)) {
            return true;
        }

        // 3. If prewarming was not in flight, trigger async preparation in background.
        // Return false immediately so caller can proceed without freezing the worker thread.
        if (!prewarm_was_in_flight) {
            PrewarmAsync(vid, url, (out_cookies && !out_cookies->empty()) ? *out_cookies : "");
        }
        return false;
    }

    bool GetDirectUrls(const std::string& video_id_in, const std::string& resolution_or_filter,
                       std::string& out_video_url, std::string& out_audio_url, std::string& out_resolution,
                       std::string* out_cookies = nullptr,
                       std::map<std::string, std::string>* out_v_headers = nullptr,
                       std::map<std::string, std::string>* out_a_headers = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string vid = video_id_in;
        if (vid.empty()) return false;

        auto it = m_cache.find(vid);
        if (it == m_cache.end() || !it->second.is_ready) return false;

        const auto& entry = it->second;
        if (out_cookies && out_cookies->empty() && !entry.cookies.empty()) {
            *out_cookies = entry.cookies;
        }
        if (out_a_headers && !entry.best_audio_headers.empty()) {
            *out_a_headers = entry.best_audio_headers;
        }

        std::string target = resolution_or_filter;
        bool wants_audio = (target == "audio" || target == "audio-only" || target == "Audio" || target == "m4a" || target == "mp3");
        if (wants_audio) {
            out_video_url = entry.best_audio_url;
            out_audio_url.clear();
            out_resolution = "audio";
            if (out_v_headers) *out_v_headers = entry.best_audio_headers;
            return !out_video_url.empty();
        }

        if (target.empty() || target == "best" || target == "Auto") {
            out_video_url = entry.best_video_url;
            out_audio_url = entry.best_audio_url;
            out_resolution = entry.best_video_res;
            if (out_v_headers) *out_v_headers = entry.best_video_headers;
            return !out_video_url.empty();
        }

        // Try format ID match (e.g. itag "137", "248", "18", etc.)
        auto fmt_it = entry.qualities_by_fmt.find(target);
        if (fmt_it != entry.qualities_by_fmt.end() && !fmt_it->second.video_url.empty()) {
            out_video_url = fmt_it->second.video_url;
            out_audio_url = fmt_it->second.audio_url.empty() ? entry.best_audio_url : fmt_it->second.audio_url;
            out_resolution = fmt_it->second.resolution;
            if (out_v_headers) *out_v_headers = fmt_it->second.http_headers;
            return true;
        }

        // Try exact resolution match
        auto res_it = entry.qualities_by_res.find(target);
        if (res_it != entry.qualities_by_res.end() && !res_it->second.video_url.empty()) {
            out_video_url = res_it->second.video_url;
            out_audio_url = res_it->second.audio_url.empty() ? entry.best_audio_url : res_it->second.audio_url;
            out_resolution = res_it->second.resolution;
            if (out_v_headers) *out_v_headers = res_it->second.http_headers;
            return true;
        }

        // Try normalized height regex match (e.g. '720p', '720', '720p (MP4)', '240p WebM')
        static const std::regex kHeightRe(R"((\d{3,4})p?)");
        std::smatch hm;
        if (std::regex_search(target, hm, kHeightRe)) {
            std::string normalized_res = hm[1].str() + "p";
            int target_h = std::stoi(hm[1].str());
            std::string target_low = target;
            for (char& c : target_low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            bool wants_webm = (target_low.find("webm") != std::string::npos);
            bool wants_mp4 = (target_low.find("mp4") != std::string::npos);

            // Check if there is an exact format matching height/width and container
            for (const auto& [fid, q] : entry.qualities_by_fmt) {
                if ((q.height == target_h || q.width == target_h) && !q.video_url.empty()) {
                    if ((wants_webm && q.ext == "webm") || (wants_mp4 && q.ext == "mp4")) {
                        out_video_url = q.video_url;
                        out_audio_url = q.audio_url.empty() ? entry.best_audio_url : q.audio_url;
                        out_resolution = q.resolution;
                        if (out_v_headers) *out_v_headers = q.http_headers;
                        return true;
                    }
                }
            }

            auto n_it = entry.qualities_by_res.find(normalized_res);
            if (n_it != entry.qualities_by_res.end() && !n_it->second.video_url.empty()) {
                out_video_url = n_it->second.video_url;
                out_audio_url = n_it->second.audio_url.empty() ? entry.best_audio_url : n_it->second.audio_url;
                out_resolution = n_it->second.resolution;
                if (out_v_headers) *out_v_headers = n_it->second.http_headers;
                return true;
            }

            // Fallback for vertical videos or approximate resolution match
            for (const auto& [fid, q] : entry.qualities_by_fmt) {
                if ((q.height == target_h || q.width == target_h) && !q.video_url.empty()) {
                    out_video_url = q.video_url;
                    out_audio_url = q.audio_url.empty() ? entry.best_audio_url : q.audio_url;
                    out_resolution = q.resolution;
                    if (out_v_headers) *out_v_headers = q.http_headers;
                    return true;
                }
            }
        }

        // Fall back to best
        if (!entry.best_video_url.empty()) {
            out_video_url = entry.best_video_url;
            out_audio_url = entry.best_audio_url;
            out_resolution = entry.best_video_res;
            if (out_v_headers) *out_v_headers = entry.best_video_headers;
            return true;
        }

        return false;
    }

private:
    YouTubeStreamPrewarmer() = default;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::map<std::string, PrewarmedVideoEntry> m_cache;

    static std::string ExtractVideoId(const std::string& url) {
        static const std::regex kRe(R"((?:v=|\/shorts\/|\/embed\/|youtu\.be\/)([a-zA-Z0-9_-]{11}))");
        std::smatch m;
        if (std::regex_search(url, m, kRe)) {
            return m[1].str();
        }
        return "";
    }

    void ExecuteResolution(const std::string& vid, const std::string& url, const std::string& cookies = "") {
        std::error_code ec;
        std::filesystem::path ytdlp = PathSanitizer::FindWorkerBinary("yt-dlp", PathSanitizer::GetExecutableDir());
        if (ytdlp.empty() || !std::filesystem::exists(ytdlp, ec)) {
            OPENIDM_LOG_ERROR("Prewarmer", "yt-dlp binary not found in tools or PATH");
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[vid].is_fetching = false;
            m_cv.notify_all();
            return;
        }

#ifdef _WIN32
        auto run_ytdlp_json = [&](const std::string& ck_str, std::string& out_json,
                                  std::string& out_err, DWORD& out_exit) -> bool {
            out_json.clear();
            out_err.clear();
            out_exit = 1;

            std::filesystem::path cache_dir = PathSanitizer::GetDefaultAppDataDir() / "cache" / "yt-dlp";
            std::filesystem::create_directories(cache_dir, ec);
            std::wstring wcmd = L"\"" + ytdlp.wstring() +
                L"\" -J --skip-download --no-warnings --no-playlist --socket-timeout 10 --cache-dir \"" +
                cache_dir.wstring() + L"\"";
            std::filesystem::path node_bin = PathSanitizer::FindWorkerBinary("node", PathSanitizer::GetExecutableDir());
            if (std::filesystem::exists(node_bin, ec)) {
                wcmd += L" --js-runtimes \"node:" + node_bin.wstring() + L"\"";
            }
            std::filesystem::path tmp_cookies = std::filesystem::temp_directory_path(ec) / ("ytdlp_prewarm_cookies_" + vid + ".txt");
            if (!ck_str.empty()) {
                std::ofstream cfs(tmp_cookies, std::ios::binary | std::ios::trunc);
                if (cfs.is_open()) {
                    cfs << "# Netscape HTTP Cookie File\n";
                    std::string_view sv = ck_str;
                    while (!sv.empty()) {
                        auto semi = sv.find(';');
                        auto token = sv.substr(0, semi);
                        sv = (semi == std::string_view::npos) ? "" : sv.substr(semi + 1);
                        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
                        while (!token.empty() && (token.back() == ' ' || token.back() == '\t' || token.back() == '\r' || token.back() == '\n')) token.remove_suffix(1);
                        auto eq = token.find('=');
                        if (eq != std::string_view::npos && eq > 0) {
                            std::string name(token.substr(0, eq));
                            std::string val(token.substr(eq + 1));
                            cfs << ".youtube.com\tTRUE\t/\tTRUE\t2147483647\t" << name << "\t" << val << "\n";
                            cfs << ".google.com\tTRUE\t/\tTRUE\t2147483647\t" << name << "\t" << val << "\n";
                        }
                    }
                    cfs.close();
                    wcmd += L" --cookies \"" + tmp_cookies.wstring() + L"\"";
                }
            }
            int url_wlen = MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), nullptr, 0);
            std::wstring w_url(url_wlen, 0);
            if (url_wlen > 0) {
                MultiByteToWideChar(CP_UTF8, 0, url.data(), static_cast<int>(url.size()), w_url.data(), url_wlen);
            }
            wcmd += L" \"" + w_url + L"\"";

            SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE hOutRead = nullptr, hOutWrite = nullptr;
            HANDLE hErrRead = nullptr, hErrWrite = nullptr;
            if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0) || !CreatePipe(&hErrRead, &hErrWrite, &sa, 0)) {
                if (hOutRead) CloseHandle(hOutRead);
                if (hOutWrite) CloseHandle(hOutWrite);
                if (hErrRead) CloseHandle(hErrRead);
                if (hErrWrite) CloseHandle(hErrWrite);
                if (std::filesystem::exists(tmp_cookies, ec)) std::filesystem::remove(tmp_cookies, ec);
                return false;
            }
            SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(hErrRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdOutput = hOutWrite;
            si.hStdError = hErrWrite;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

            PROCESS_INFORMATION pi{};
            OPENIDM_LOG_INFO("Prewarmer", "Executing command: " + PathSanitizer::PathToUtf8(wcmd));
            BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            CloseHandle(hOutWrite);
            CloseHandle(hErrWrite);

            if (!ok) {
                DWORD err = GetLastError();
                OPENIDM_LOG_ERROR("Prewarmer", "CreateProcessW failed for yt-dlp prewarm: " + std::to_string(err));
                CloseHandle(hOutRead);
                CloseHandle(hErrRead);
                if (std::filesystem::exists(tmp_cookies, ec)) std::filesystem::remove(tmp_cookies, ec);
                return false;
            }

            char buf[8192];
            DWORD got = 0;
            DWORD bytes_avail = 0;
            auto wait_start = std::chrono::steady_clock::now();
            bool proc_done = false;
            while (!proc_done) {
                DWORD wait_res = WaitForSingleObject(pi.hProcess, 40);
                if (wait_res == WAIT_OBJECT_0) {
                    proc_done = true;
                } else if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - wait_start).count() > 25) {
                    TerminateProcess(pi.hProcess, 1);
                    break;
                }
                while (PeekNamedPipe(hOutRead, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
                    DWORD to_read = std::min<DWORD>(sizeof(buf), bytes_avail);
                    if (ReadFile(hOutRead, buf, to_read, &got, nullptr) && got > 0) {
                        out_json.append(buf, got);
                    } else break;
                }
                while (PeekNamedPipe(hErrRead, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
                    DWORD to_read = std::min<DWORD>(sizeof(buf), bytes_avail);
                    if (ReadFile(hErrRead, buf, to_read, &got, nullptr) && got > 0) {
                        out_err.append(buf, got);
                    } else break;
                }
            }
            while (PeekNamedPipe(hOutRead, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
                DWORD to_read = std::min<DWORD>(sizeof(buf), bytes_avail);
                if (ReadFile(hOutRead, buf, to_read, &got, nullptr) && got > 0) {
                    out_json.append(buf, got);
                } else break;
            }
            while (PeekNamedPipe(hErrRead, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
                DWORD to_read = std::min<DWORD>(sizeof(buf), bytes_avail);
                if (ReadFile(hErrRead, buf, to_read, &got, nullptr) && got > 0) {
                    out_err.append(buf, got);
                } else break;
            }
            CloseHandle(hOutRead);
            CloseHandle(hErrRead);

            GetExitCodeProcess(pi.hProcess, &out_exit);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (std::filesystem::exists(tmp_cookies, ec)) std::filesystem::remove(tmp_cookies, ec);
            return (out_exit == 0 && !out_json.empty());
        };

        std::string json_str;
        std::string err_str;
        DWORD exitCode = 1;
        bool ok = run_ytdlp_json(cookies, json_str, err_str, exitCode);
        if (!ok && !cookies.empty()) {
            OPENIDM_LOG_WARN("Prewarmer", "yt-dlp prewarm failed with cookies (exit=" + std::to_string(exitCode) +
                             ", err=" + err_str + "), retrying without cookies...");
            ok = run_ytdlp_json("", json_str, err_str, exitCode);
        }
        OPENIDM_LOG_INFO("Prewarmer", "yt-dlp exited with code " + std::to_string(exitCode) + " json bytes=" + std::to_string(json_str.size()) + (err_str.empty() ? "" : " stderr: " + err_str.substr(0, 200)));
#endif

        if (json_str.empty()) {
            OPENIDM_LOG_ERROR("Prewarmer", "yt-dlp output empty JSON for vid=" + vid + (!err_str.empty() ? " (stderr: " + err_str + ")" : ""));
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[vid].is_fetching = false;
            m_cv.notify_all();
            return;
        }

        try {
            auto root = json::Parse(json_str);
            if (!root.is_object() || !root["formats"].is_array()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cache[vid].is_fetching = false;
                m_cv.notify_all();
                return;
            }

            PrewarmedVideoEntry entry;
            entry.video_id = vid;
            entry.watch_url = url;
            entry.title = root["title"].as_string();
            entry.fetched_tp = std::chrono::steady_clock::now();

            std::string best_audio_url;
            std::map<std::string, std::string> best_audio_headers;
            uint64_t best_audio_bw = 0;
            int max_v_height = 0;

            for (const auto& f : root["formats"].as_array()) {
                std::string f_url = f["url"].as_string();
                if (f_url.empty() || f_url.find("googlevideo.com") == std::string::npos || f_url.find("videoplayback") == std::string::npos) continue;

                std::string fid = f["format_id"].as_string();
                std::string vcodec = f["vcodec"].as_string();
                std::string acodec = f["acodec"].as_string();
                int h = static_cast<int>(f["height"].as_int64(0));
                int w = static_cast<int>(f["width"].as_int64(0));
                uint64_t tbr = static_cast<uint64_t>(f["tbr"].as_int64(f["bitrate"].as_int64(0)));
                std::string ext = f["ext"].as_string("mp4");

                std::map<std::string, std::string> f_headers;
                if (f.contains("http_headers") && f["http_headers"].is_object()) {
                    for (const auto& [hk, hv] : f["http_headers"].as_object()) {
                        f_headers[hk] = hv.as_string();
                    }
                }

                bool is_audio = (vcodec == "none" || vcodec.empty()) && (acodec != "none" && !acodec.empty());
                if (is_audio) {
                    if (tbr >= best_audio_bw || best_audio_url.empty()) {
                        best_audio_bw = tbr;
                        best_audio_url = f_url;
                        best_audio_headers = f_headers;
                    }
                    continue;
                }

                if (h > 0) {
                    PrewarmedQuality q;
                    q.format_id = fid;
                    q.height = h;
                    q.width = w;
                    q.resolution = std::to_string(h) + "p";
                    q.ext = ext;
                    q.video_url = f_url;
                    q.http_headers = f_headers;
                    q.is_audio_only = false;

                    entry.qualities_by_fmt[fid] = q;
                    if (entry.qualities_by_res.find(q.resolution) == entry.qualities_by_res.end() || h >= max_v_height) {
                        entry.qualities_by_res[q.resolution] = q;
                    }

                    if (h >= max_v_height) {
                        max_v_height = h;
                        entry.best_video_url = f_url;
                        entry.best_video_res = q.resolution;
                        entry.best_video_headers = f_headers;
                    }
                }
            }

            entry.best_audio_url = best_audio_url;
            entry.best_audio_headers = best_audio_headers;
            // Attach best audio to all video entries
            for (auto& [res, q] : entry.qualities_by_res) {
                q.audio_url = best_audio_url;
            }
            for (auto& [fid, q] : entry.qualities_by_fmt) {
                q.audio_url = best_audio_url;
            }

            entry.is_ready = !entry.best_video_url.empty();
            entry.is_fetching = false;

            std::string best_res = entry.best_video_res;
            size_t n_qualities = entry.qualities_by_res.size();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_cache[vid] = std::move(entry);
            }
            m_cv.notify_all();
            OPENIDM_LOG_INFO("Prewarmer", "Background stream pre-warm complete for vid=" + vid +
                             ": resolved " + std::to_string(n_qualities) +
                             " qualities (best=" + best_res + ")");
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_cache[vid].is_fetching = false;
            m_cv.notify_all();
        }
    }
};

} // namespace openidm::stream
