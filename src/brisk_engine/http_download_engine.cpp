// SPDX-License-Identifier: Apache-2.0
#include "brisk_engine/http_download_engine.hpp"

#include "brisk_engine/http_client.hpp"
#include "common/crypto_utils.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/startup_diagnostic.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace openidm::brisk {

namespace {

// Cross-platform random-access .openidm.part file wrapper supporting concurrent thread writes at exact byte offsets
class PlatformPartFile {
public:
    PlatformPartFile() = default;
    ~PlatformPartFile() { Close(); }

    bool Open(const std::filesystem::path& path) {
#ifdef _WIN32
        m_handle = CreateFileW(
            path.wstring().c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        return m_handle != INVALID_HANDLE_VALUE;
#else
        m_fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
        return m_fd >= 0;
#endif
    }

    bool Truncate(uint64_t size) {
#ifdef _WIN32
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        // On Windows NTFS, setting EOF on non-sparse files synchronously zero-fills unallocated clusters,
        // which introduces 300ms to 1500ms+ synchronous startup latency for multi-gigabyte downloads.
        // Marking the file sparse enables instant logical EOF expansion (<1ms) without cluster zeroing.
        DWORD bytes_returned = 0;
        DeviceIoControl(m_handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);

        LARGE_INTEGER li{};
        li.QuadPart = static_cast<LONGLONG>(size);
        if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return false;
        return SetEndOfFile(m_handle) != 0;
#else
        if (m_fd < 0) return false;
        return ::ftruncate(m_fd, static_cast<off_t>(size)) == 0;
#endif
    }

    bool PWrite(const uint8_t* data, size_t len, uint64_t offset) {
#ifdef _WIN32
        if (m_handle == INVALID_HANDLE_VALUE) return false;
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        ov.OffsetHigh = static_cast<DWORD>((offset >> 32) & 0xFFFFFFFFULL);
        DWORD written = 0;
        return WriteFile(m_handle, data, static_cast<DWORD>(len), &written, &ov) && written == static_cast<DWORD>(len);
#else
        if (m_fd < 0) return false;
        ssize_t w = ::pwrite(m_fd, data, len, static_cast<off_t>(offset));
        return w == static_cast<ssize_t>(len);
#endif
    }

    void SyncAndClose() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_handle);
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::fsync(m_fd);
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

    void Close() {
#ifdef _WIN32
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_handle{INVALID_HANDLE_VALUE};
#else
    int m_fd{-1};
#endif
};

} // namespace

namespace {
bool HttpRangeTraceEnabled() {
    return std::getenv("RADIUM_HTTP_RANGE_TRACE") != nullptr;
}

uint64_t TraceEpochMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

uint64_t TraceElapsedMicroseconds(const std::chrono::steady_clock::time_point& origin) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - origin).count());
}

void EmitRuntimeTrace(const std::string& event) {
    if (HttpRangeTraceEnabled()) {
        std::string timed_event = event;
        if (!timed_event.empty() && timed_event.back() == '}') {
            timed_event.pop_back();
            const auto steady_tick_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            timed_event += ",\"steady_tick_us\":" + std::to_string(steady_tick_us) + "}";
        }
        OPENIDM_LOG_INFO("HttpRangeTrace", "HTTP_RUNTIME_TRACE " + timed_event);
    }
}

void EmitHttpPhaseTrace(const char* phase, const std::chrono::steady_clock::time_point& origin) {
    if (HttpRangeTraceEnabled()) {
        OPENIDM_LOG_INFO("HttpRangeTrace", std::string("HTTP_PHASE_TRACE phase=") + phase +
            " elapsed_us=" + std::to_string(TraceElapsedMicroseconds(origin)));
    }
}

class RuntimeTimedLock {
public:
    RuntimeTimedLock(std::mutex& mutex, const char* lock_name, const char* operation,
                     uint64_t request_id = 0, uint32_t worker = 0, uint32_t segment = 0)
        : m_lock(mutex, std::defer_lock), m_lock_name(lock_name), m_operation(operation),
          m_request_id(request_id), m_worker(worker), m_segment(segment), m_enabled(HttpRangeTraceEnabled()) {
        const auto wait_start = m_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        m_lock.lock();
        if (m_enabled) {
            m_acquired = std::chrono::steady_clock::now();
            m_wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(m_acquired - wait_start).count());
        }
    }

    void Unlock() {
        if (!m_lock.owns_lock()) return;
        if (m_enabled) {
            const auto released = std::chrono::steady_clock::now();
            const uint64_t hold_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(released - m_acquired).count());
            m_lock.unlock();
            EmitRuntimeTrace("{\"event\":\"lock_timing\",\"lock\":\"" + std::string(m_lock_name) +
                "\",\"operation\":\"" + m_operation + "\",\"request_id\":" + std::to_string(m_request_id) +
                ",\"worker\":" + std::to_string(m_worker) + ",\"segment\":" + std::to_string(m_segment) +
                ",\"wait_us\":" + std::to_string(m_wait_us) + ",\"hold_us\":" + std::to_string(hold_us) + "}");
        } else {
            m_lock.unlock();
        }
    }

    ~RuntimeTimedLock() {
        Unlock();
    }

    RuntimeTimedLock(const RuntimeTimedLock&) = delete;
    RuntimeTimedLock& operator=(const RuntimeTimedLock&) = delete;

private:
    std::unique_lock<std::mutex> m_lock;
    const char* m_lock_name;
    const char* m_operation;
    uint64_t m_request_id{0};
    uint32_t m_worker{0};
    uint32_t m_segment{0};
    bool m_enabled{false};
    uint64_t m_wait_us{0};
    std::chrono::steady_clock::time_point m_acquired{};
};

class RuntimeDurationAccumulator {
public:
    RuntimeDurationAccumulator(uint64_t& total_us, bool enabled)
        : m_total_us(total_us), m_enabled(enabled), m_start(enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~RuntimeDurationAccumulator() {
        if (m_enabled) {
            m_total_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - m_start).count());
        }
    }
    RuntimeDurationAccumulator(const RuntimeDurationAccumulator&) = delete;
    RuntimeDurationAccumulator& operator=(const RuntimeDurationAccumulator&) = delete;
private:
    uint64_t& m_total_us;
    bool m_enabled{false};
    std::chrono::steady_clock::time_point m_start{};
};
} // namespace

HttpDownloadEngine::HttpDownloadEngine(EngineConfig cfg) : m_cfg(cfg) {}
HttpDownloadEngine::~HttpDownloadEngine() = default;

Result<std::filesystem::path> HttpDownloadEngine::ExecuteDownload(
    TaskModel& task,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    std::atomic<uint64_t>& task_speed_limit_bps,
    ProgressCallback on_progress,
    std::shared_ptr<TokenBucketLimiter> global_limiter,
    std::atomic<uint64_t>* global_speed_limit_bps) {

    const bool http_trace_enabled = HttpRangeTraceEnabled();
    const auto engine_start_tp = std::chrono::steady_clock::now();
    std::atomic<int64_t> t_probe_ms{0};
    std::atomic<int64_t> t_connect_ms{0};
    std::atomic<int64_t> t_disk_alloc_ms{0};
    std::atomic<int64_t> t_first_byte_ms{0};
    std::atomic<uint64_t> next_trace_request_id{0};
    std::atomic<uint32_t> active_http_requests{0};
    std::atomic<bool> first_request_recorded{false};
    std::atomic<bool> first_byte_recorded{false};
    std::atomic<bool> first_write_recorded{false};
    EmitHttpPhaseTrace("initialization_start", engine_start_tp);

    auto task_limiter = std::make_shared<TokenBucketLimiter>();

    auto consume_both_limiters = [&](size_t bytes, std::atomic<bool>& cancel_flag) {
        if (bytes == 0) return;
        if (task_speed_limit_bps.load(std::memory_order_relaxed) > 0) {
            task_limiter->Consume(bytes, task_speed_limit_bps, cancel_flag);
        }
        if (global_limiter && global_speed_limit_bps && global_speed_limit_bps->load(std::memory_order_relaxed) > 0) {
            global_limiter->Consume(bytes, *global_speed_limit_bps, cancel_flag);
        }
    };

    std::map<std::string, std::string> req_headers = task.headers;
    for (const auto& [k, v] : task.auth_headers) {
        req_headers[k] = v;
    }

    // -------------------------------------------------------------------------
    // Early GET & Streaming Response Reuse Pipeline (Requirement R2)
    // Eliminates preliminary probe roundtrip by dispatching Worker 0 directly
    // with the actual GET request. Pre-allocates sparse file and initializes
    // SegmentScheduler upon response headers, streaming first segment bytes
    // without destroying the TCP/TLS connection.
    // -------------------------------------------------------------------------
    bool is_resume = !task.segments.empty() && task.downloaded_bytes > 0;
    uint64_t resume_start = 0;
    std::optional<uint64_t> resume_end = std::nullopt;

    if (is_resume) {
        for (const auto& s : task.segments) {
            if (!s.completed && s.current_offset <= s.end_offset) {
                resume_start = s.current_offset;
                resume_end = s.end_offset;
                break;
            }
        }
        if (!task.etag.empty() && task.etag.rfind("W/", 0) != 0) {
            req_headers["If-Range"] = task.etag;
        } else if (!task.last_modified.empty()) {
            req_headers["If-Range"] = task.last_modified;
        }
    }

    // Shared state between Worker 0 and secondary workers
    PlatformPartFile part_file;
    std::filesystem::path part_path;
    std::unique_ptr<SegmentScheduler> scheduler;
    std::shared_ptr<LiveSegment> seg0;
    bool is_segmented = false;
    std::atomic<bool> fatal_abort{false};
    std::atomic<bool> disable_splitting{false};
    std::atomic<uint32_t> running_workers{0};
    std::mutex err_mu;
    std::optional<Error> fatal_error;
    std::vector<std::thread> secondary_workers;
    uint64_t single_stream_written = 0;
    uint64_t single_stream_speed = 0;
    auto transfer_start_tp = std::chrono::steady_clock::now();
    auto last_progress_tp = transfer_start_tp;
    uint64_t last_progress_bytes = 0;
    std::mutex progress_mu;
    auto seg_last_progress_tp = transfer_start_tp;
    uint64_t seg_last_progress_bytes = 0;
    bool seg_first_progress_emitted = false;
    uint32_t max_conns = std::clamp(task.max_connections > 0 ? task.max_connections : 8u, 1u, 32u);

    auto emit_segmented_progress_if_due = [&](bool force_first) {
        if (!on_progress || !scheduler) return;
        std::unique_lock<std::mutex> plk(progress_mu, std::try_to_lock);
        if (!plk.owns_lock()) return;
        auto now = std::chrono::steady_clock::now();
        auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - seg_last_progress_tp).count();
        if (!force_first && seg_first_progress_emitted && diff_ms < 80) {
            return;
        }
        uint64_t dl = scheduler->TotalDownloadedBytes();
        if (dl == 0) return;
        seg_first_progress_emitted = true;
        uint64_t delta_bytes = (dl >= seg_last_progress_bytes) ? (dl - seg_last_progress_bytes) : 0;
        double dt_sec = std::max<double>(0.02, static_cast<double>(std::max<int64_t>(1, diff_ms)) / 1000.0);
        uint64_t physical_bps = static_cast<uint64_t>(static_cast<double>(delta_bytes) / dt_sec);
        seg_last_progress_bytes = dl;
        seg_last_progress_tp = now;

        EngineProgress ep;
        ep.task_id = task.task_id;
        ep.state = TaskState::kDownloading;
        ep.downloaded_bytes = dl;
        ep.total_bytes = task.total_bytes;
        ep.speed_bps = physical_bps;
        ep.range_supported = true;
        ep.resolved_filename = task.file_name;
        ep.etag = task.etag;
        ep.last_modified = task.last_modified;
        ep.status_detail = "Downloading (" + std::to_string(std::max<size_t>(1, scheduler->ActiveWorkerCount())) + " active connections)";
        ep.segments = scheduler->GetSnapshots();
        plk.unlock();
        on_progress(ep);
    };

    // Secondary worker function (Workers 1..N-1)
    auto worker_fn = [&](uint32_t worker_idx) {
        running_workers.fetch_add(1, std::memory_order_relaxed);
        if (http_trace_enabled) {
            EmitRuntimeTrace("{\"event\":\"worker_start\",\"worker\":" + std::to_string(worker_idx) +
                ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                ",\"active_workers\":" + std::to_string(running_workers.load()) + "}");
        }
        uint64_t idle_wait_us = 0;
        uint64_t queue_wait_us = 0;
        uint64_t queue_wait_count = 0;
        auto stop_worker = [&]() {
            const uint32_t active_workers = running_workers.fetch_sub(1, std::memory_order_relaxed) - 1;
            if (http_trace_enabled) {
                EmitRuntimeTrace("{\"event\":\"worker_stop\",\"worker\":" + std::to_string(worker_idx) +
                    ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                    ",\"active_workers\":" + std::to_string(active_workers) +
                    ",\"idle_wait_us\":" + std::to_string(idle_wait_us) +
                    ",\"queue_wait_us\":" + std::to_string(queue_wait_us) +
                    ",\"queue_wait_count\":" + std::to_string(queue_wait_count) + "}");
            }
        };

        while (!fatal_abort.load() && !pause_requested.load() && !cancel_requested.load() && scheduler) {
            const auto wait_started = std::chrono::steady_clock::now();
            auto seg = scheduler->AcquireNextOrStealSegment(!disable_splitting.load());
            const bool initially_waiting_for_segment = !seg;
            if (!seg) {
                if (scheduler->AllCompleted()) {
                    break;
                }
                bool acquired = false;
                for (int idle_retry = 0; idle_retry < 600; ++idle_retry) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (fatal_abort.load() || pause_requested.load() || cancel_requested.load() || scheduler->AllCompleted()) {
                        break;
                    }
                    if (scheduler->CanSplitAnySegment() || scheduler->HasIncompleteUnassignedSegment()) {
                        seg = scheduler->AcquireNextOrStealSegment(!disable_splitting.load());
                        if (seg) {
                            acquired = true;
                            break;
                        }
                    }
                }
                if (!acquired && !seg) {
                    const uint64_t wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - wait_started).count());
                    idle_wait_us += wait_us;
                    queue_wait_us += wait_us;
                    ++queue_wait_count;
                    break;
                }
            }
            const uint64_t acquisition_wait_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - wait_started).count());
            idle_wait_us += acquisition_wait_us;
            if (initially_waiting_for_segment) {
                queue_wait_us += acquisition_wait_us;
                ++queue_wait_count;
            }

            int retries = 0;
            uint64_t last_progress_offset = seg->current_offset.load();
            while (!fatal_abort.load() && !pause_requested.load() && !cancel_requested.load()) {
                uint64_t curr = seg->current_offset.load();
                uint64_t end = seg->end_offset.load();
                if (curr > end) {
                    seg->completed.store(true);
                    seg->active_worker.store(false);
                    break;
                }

                std::atomic<bool> req_cancel{false};
                bool server_ignored_range = false;
                int captured_retry_after_sec_w = 0;
                const uint64_t trace_request_id = http_trace_enabled
                    ? next_trace_request_id.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
                const auto request_start_tp = std::chrono::steady_clock::now();
                const uint32_t active_at_start = active_http_requests.fetch_add(1, std::memory_order_relaxed) + 1;
                uint64_t client_bytes_received = 0;
                uint64_t file_bytes_written = 0;
                uint64_t file_write_duration_us = 0;
                uint64_t file_write_max_duration_us = 0;
                uint64_t file_write_count = 0;
                uint64_t callback_processing_us = 0;
                uint64_t limiter_wait_us = 0;
                uint64_t response_content_length = 0;
                uint64_t response_range_start = 0;
                uint64_t response_range_end = 0;
                int response_status = 0;
                std::string cancel_reason;

                if (http_trace_enabled) {
                    EmitRuntimeTrace("{\"event\":\"request_start\",\"request_id\":" + std::to_string(trace_request_id) +
                        ",\"worker\":" + std::to_string(worker_idx) + ",\"segment\":" + std::to_string(seg->segment_id) +
                        ",\"range_start\":" + std::to_string(curr) + ",\"range_end\":" + std::to_string(end) +
                        ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                        ",\"active_requests\":" + std::to_string(active_at_start) + "}");
                }

                auto res = HttpClient::PerformGet(
                    task.url,
                    req_headers,
                    curr,
                    end,
                    m_cfg.stall_timeout_ms,
                    m_cfg.allow_loopback_redirect,
                    req_cancel,
                    [&](const HttpResponseMeta& meta) -> HeaderAction {
                        response_status = meta.status_code;
                        response_content_length = meta.content_length;
                        response_range_start = meta.range_start;
                        response_range_end = meta.range_end;
                        captured_retry_after_sec_w = meta.retry_after_sec;
                        if (meta.status_code == 429 || meta.status_code >= 500) {
                            return HeaderAction::kAbort;
                        }
                        if (meta.status_code == 416) {
                            if (curr == 0 && task.total_bytes == 0 && meta.is_unsatisfied_range && meta.has_range_total && meta.range_total == 0) {
                                seg->completed.store(true);
                                return HeaderAction::kStopSuccess;
                            }
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_HTTP_RANGE_NOT_SATISFIABLE", "HTTP 416 Range Not Satisfiable on active segment", false, true};
                            fatal_abort.store(true);
                            return HeaderAction::kAbort;
                        }
                        if (meta.status_code == 200) {
                            server_ignored_range = true;
                            disable_splitting.store(true);
                            OPENIDM_LOG_WARN("BriskEngine", "Server returned 200 OK to range request on worker (expected " +
                                std::to_string(curr) + "-" + std::to_string(end) + "); aborting secondary stream");
                            return HeaderAction::kAbort;
                        }
                        if (meta.status_code != 206 || !meta.is_partial_content || meta.content_range_malformed) {
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL", "Server returned invalid or malformed Content-Range header", false, false};
                            fatal_abort.store(true);
                            return HeaderAction::kAbort;
                        }
                        if (meta.range_start != curr) {
                            server_ignored_range = true;
                            disable_splitting.store(true);
                            OPENIDM_LOG_WARN("BriskEngine", "Server range_start mismatch (got " +
                                std::to_string(meta.range_start) + ", expected " + std::to_string(curr) + "); aborting secondary stream");
                            return HeaderAction::kAbort;
                        }
                        if (meta.range_end < meta.range_start || (task.total_bytes > 0 && meta.range_end >= task.total_bytes)) {
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL", "Server returned impossible range bounds (end < start or end >= total)", false, false};
                            fatal_abort.store(true);
                            return HeaderAction::kAbort;
                        }
                        if (task.total_bytes > 0 && meta.range_total > 0 && meta.range_total != task.total_bytes) {
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_HTTP_VALIDATOR_MISMATCH",
                                "Remote entity length changed mid-download (expected " +
                                std::to_string(task.total_bytes) + ", got " + std::to_string(meta.range_total) + ")", false, false};
                            fatal_abort.store(true);
                            return HeaderAction::kAbort;
                        }
                        uint64_t expected_span = meta.range_end - meta.range_start + 1;
                        if (meta.has_content_length && meta.content_length != expected_span) {
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL",
                                "Content-Length (" + std::to_string(meta.content_length) +
                                ") does not match Content-Range span (" + std::to_string(expected_span) + ")", false, false};
                            fatal_abort.store(true);
                            return HeaderAction::kAbort;
                        }
                        {
                            RuntimeTimedLock split_lock(seg->write_mu, "segment_write", "header_split_check",
                                trace_request_id, worker_idx, seg->segment_id);
                            if (seg->end_offset.load() == end) {
                                seg->split_pending.store(false);
                            }
                        }
                        return HeaderAction::kContinue;
                    },
                    [&](const uint8_t* data, size_t len) -> bool {
                        RuntimeDurationAccumulator callback_timer(callback_processing_us, http_trace_enabled);
                        client_bytes_received += static_cast<uint64_t>(len);
                        if (fatal_abort.load() || pause_requested.load() || cancel_requested.load()) {
                            cancel_reason = pause_requested.load() ? "user_pause" :
                                (cancel_requested.load() ? "user_cancel" : "fatal_abort");
                            req_cancel.store(true);
                            return false;
                        }

                        const auto limiter_started = std::chrono::steady_clock::now();
                        consume_both_limiters(len, req_cancel);
                        limiter_wait_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - limiter_started).count());

                        RuntimeTimedLock write_lk(seg->write_mu, "segment_write", "body_write_callback",
                            trace_request_id, worker_idx, seg->segment_id);
                        uint64_t write_pos = seg->current_offset.load();
                        uint64_t current_end = seg->end_offset.load();
                        if (write_pos > current_end) {
                            seg->completed.store(true);
                            cancel_reason = current_end < end ? "range_split" : "segment_complete";
                            req_cancel.store(true);
                            return false;
                        }

                        uint64_t max_writable = (current_end >= write_pos) ? (current_end - write_pos + 1) : 0;
                        if (max_writable == 0) {
                            seg->completed.store(true);
                            cancel_reason = current_end < end ? "range_split" : "segment_complete";
                            req_cancel.store(true);
                            return false;
                        }
                        size_t to_write = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(len), max_writable));

                        const auto file_write_start = http_trace_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        const bool file_write_ok = part_file.PWrite(data, to_write, write_pos);
                        if (file_write_ok && !first_write_recorded.exchange(true)) {
                            diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                                task.task_id, "First bytes written to disk",
                                "Segment worker " + std::to_string(worker_idx) + " committed " + std::to_string(to_write) +
                                " bytes at offset " + std::to_string(write_pos));
                        }
                        if (http_trace_enabled) {
                            const auto file_write_end = std::chrono::steady_clock::now();
                            const uint64_t write_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                file_write_end - file_write_start).count());
                            file_write_duration_us += write_us;
                            file_write_max_duration_us = std::max(file_write_max_duration_us, write_us);
                            ++file_write_count;
                        }
                        if (!file_write_ok) {
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_DISK_FULL", "Disk write failed on segment", false, true};
                            fatal_abort.store(true);
                            req_cancel.store(true);
                            cancel_reason = "disk_write_error";
                            return false;
                        }

                        seg->current_offset.fetch_add(to_write);
                        file_bytes_written += static_cast<uint64_t>(to_write);

                        const bool should_terminate_chunk = (to_write < len || seg->current_offset.load() > seg->end_offset.load());
                        if (should_terminate_chunk) {
                            cancel_reason = seg->end_offset.load() < end ? "range_split" : "segment_complete";
                            if (seg->current_offset.load() > seg->end_offset.load()) {
                                seg->completed.store(true);
                            }
                            req_cancel.store(true);
                        }

                        // Unlock segment write lock before metrics/return
                        write_lk.Unlock();
                        if (!should_terminate_chunk && client_bytes_received > 0) {
                            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - request_start_tp).count();
                            // Recycle trickle-throttled connections (< 48 KiB/s after 5s) when segment still has > 2 MiB left
                            if (elapsed_ms >= 5000 &&
                                (client_bytes_received * 1000ULL / static_cast<uint64_t>(elapsed_ms)) < 49152ULL &&
                                seg->end_offset.load() > seg->current_offset.load() + (2ULL << 20)) {
                                cancel_reason = "trickle_recycle";
                                req_cancel.store(true);
                                return false;
                            }
                        }
                        return !should_terminate_chunk;
                    },
                    m_cfg.net_opts);

                const uint32_t active_after_request = active_http_requests.fetch_sub(1, std::memory_order_relaxed) - 1;

                if (http_trace_enabled) {
                    const auto request_end_tp = std::chrono::steady_clock::now();
                    const bool body_complete = response_status > 0 && response_content_length > 0 &&
                        client_bytes_received >= response_content_length;
                    const std::string outcome = body_complete ? "complete" :
                        (!cancel_reason.empty() ? "cancelled" : "failed");
                    EmitRuntimeTrace("{\"event\":\"request_end\",\"request_id\":" + std::to_string(trace_request_id) +
                        ",\"worker\":" + std::to_string(worker_idx) + ",\"status\":" + std::to_string(response_status) +
                        ",\"response_start\":" + std::to_string(response_range_start) +
                        ",\"response_end\":" + std::to_string(response_range_end) +
                        ",\"response_content_length\":" + std::to_string(response_content_length) +
                        ",\"client_bytes_received\":" + std::to_string(client_bytes_received) +
                        ",\"file_bytes_written\":" + std::to_string(file_bytes_written) +
                        ",\"file_write_duration_us\":" + std::to_string(file_write_duration_us) +
                        ",\"file_write_max_duration_us\":" + std::to_string(file_write_max_duration_us) +
                        ",\"file_write_count\":" + std::to_string(file_write_count) +
                        ",\"callback_processing_us\":" + std::to_string(callback_processing_us) +
                        ",\"limiter_wait_us\":" + std::to_string(limiter_wait_us) +
                        ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) +
                        ",\"duration_us\":" + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(request_end_tp - request_start_tp).count()) +
                        ",\"active_requests\":" + std::to_string(active_after_request) +
                        ",\"outcome\":\"" + outcome + "\",\"cancel_reason\":\"" + cancel_reason + "\"}");
                }

                if (seg->current_offset.load() > seg->end_offset.load()) {
                    seg->completed.store(true);
                    seg->active_worker.store(false);
                    break;
                }

                if (cancel_reason == "range_split") {
                    seg->split_pending.store(false);
                }

                if (pause_requested.load() || cancel_requested.load() || fatal_abort.load()) {
                    seg->active_worker.store(false);
                    break;
                }

                if (server_ignored_range) {
                    if (!scheduler->RollbackUnstartedStolenSegment(seg->segment_id)) {
                        std::lock_guard<std::mutex> lk(err_mu);
                        fatal_error = Error{"ERR_HTTP_RANGE_PROTOCOL", "Server failed range check on active segment", false, false};
                        fatal_abort.store(true);
                    }
                    break;
                }

                uint64_t cur_pos = seg->current_offset.load();
                if (cur_pos > last_progress_offset) {
                    last_progress_offset = cur_pos;
                    retries = 0;
                    scheduler->RecordSuccess();
                    scheduler->TryRampUpConnections(max_conns);
                }

                if (cancel_reason == "trickle_recycle") {
                    // Immediately reconnect on a fresh TCP socket without penalty
                    continue;
                }

                if (!res.ok()) {
                    uint32_t fail_streak = scheduler->IncrementConsecutiveFailures();
                    int retry_after_ms = 0;

                    bool is_server_overloaded = (res.error->code == "ERR_HTTP_429_RATE_LIMITED" ||
                                                 (response_status == 503 && res.error->code == "ERR_HTTP_5XX_TRANSIENT"));

                    if (is_server_overloaded) {
                        disable_splitting.store(true);
                        scheduler->ReduceConnectionsStepped();
                        if (captured_retry_after_sec_w > 15) {
                            seg->active_worker.store(false);
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{
                                (response_status == 429) ? "ERR_HTTP_429_RATE_LIMITED" : "ERR_HTTP_503",
                                "Server requested Retry-After delay of " + std::to_string(captured_retry_after_sec_w) + "s",
                                true, true
                            };
                            fatal_abort.store(true);
                            stop_worker();
                            return;
                        }

                        retry_after_ms = (captured_retry_after_sec_w > 0)
                                            ? (captured_retry_after_sec_w * 1000)
                                            : 2000;
                        OPENIDM_LOG_WARN("BriskEngine", "Server overload/rate limit (HTTP " +
                            std::to_string(response_status) + ") on segment #" +
                            std::to_string(seg->segment_id) + "; reduced target concurrency to " +
                            std::to_string(scheduler->GetMaxConnections()) + ", waiting " +
                            std::to_string(retry_after_ms) + " ms");

                        uint32_t current_max = scheduler->GetMaxConnections();
                        if (running_workers.load() > current_max && running_workers.load() > 1) {
                            seg->active_worker.store(false);
                            stop_worker();
                            return;
                        }
                    } else if (res.error->code == "ERR_NET_CONNECTION_RESET" ||
                               res.error->code == "ERR_NET_CONNECT" ||
                               res.error->code == "ERR_NET_TIMEOUT" ||
                               res.error->code == "ERR_NET_SEND") {
                        // Do not permanently halve connections on transient TCP/stall timeouts;
                        // only back off if we see a severe streak (>= 8) and let TryRampUpConnections recover.
                        if (fail_streak >= 8) {
                            scheduler->ReduceConnectionsStepped();
                        }
                    }

                    int max_retries = is_server_overloaded ? 8 : m_cfg.max_retries_per_worker;
                    uint32_t max_streak = is_server_overloaded ? 16 : 12;

                    if (!res.error->retryable || retries >= max_retries || fail_streak > max_streak) {
                        seg->active_worker.store(false);
                        std::lock_guard<std::mutex> lk(err_mu);
                        if (!fatal_error.has_value()) {
                            fatal_error = res.error;
                        }
                        fatal_abort.store(true);
                        break;
                    }
                    ++retries;
                    int exp_backoff = (100 * (1 << std::min(retries, 6))) + (std::rand() % 150);
                    int backoff_ms = (retry_after_ms > 0) ? retry_after_ms : std::min(exp_backoff, 15000);
                    const auto backoff_started = std::chrono::steady_clock::now();

                    for (int slept = 0; slept < backoff_ms; slept += 50) {
                        if (fatal_abort.load() || pause_requested.load() || cancel_requested.load()) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, backoff_ms - slept)));
                    }
                    if (http_trace_enabled) {
                        const auto actual_wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - backoff_started).count();
                        EmitRuntimeTrace("{\"event\":\"retry_wait\",\"worker\":" + std::to_string(worker_idx) +
                            ",\"segment\":" + std::to_string(seg->segment_id) + ",\"retry\":" + std::to_string(retries) +
                            ",\"requested_wait_ms\":" + std::to_string(backoff_ms) +
                            ",\"actual_wait_us\":" + std::to_string(actual_wait_us) +
                            ",\"elapsed_us\":" + std::to_string(TraceElapsedMicroseconds(engine_start_tp)) + "}");
                    }
                } else {
                    scheduler->ResetConsecutiveFailures();
                }
            }
        }
        stop_worker();
    };

    // Dispatch Worker 0 directly with the actual GET request
    if (!first_request_recorded.exchange(true)) {
        diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
            task.task_id, "First HTTP request dispatched",
            "Worker 0 requesting " + (is_resume ? ("resume bytes " + std::to_string(resume_start) + "-") : "bytes 0-") + " to " + task.url);
    }

    constexpr int kMaxInitialAttempts = 3;
    Result<HttpResponseMeta> initial_res = Result<HttpResponseMeta>::Ok({});
    int captured_retry_after_sec = 0;

    for (int initial_attempt = 1; initial_attempt <= kMaxInitialAttempts; ++initial_attempt) {
        captured_retry_after_sec = 0;
        std::atomic<bool> req_cancel{false};

        std::optional<uint64_t> req_start = is_resume ? std::optional<uint64_t>(resume_start) : std::optional<uint64_t>(0);
        std::optional<uint64_t> req_end = is_resume ? resume_end : std::nullopt;

        initial_res = HttpClient::PerformGet(
            task.url,
            req_headers,
            req_start,
            req_end,
            m_cfg.stall_timeout_ms,
            m_cfg.allow_loopback_redirect,
            req_cancel,
            [&](const HttpResponseMeta& meta) -> HeaderAction {
                if (meta.status_code == 429 || meta.status_code == 503) {
                    captured_retry_after_sec = meta.retry_after_sec;
                    return HeaderAction::kAbort;
                }

                if (meta.status_code == 416) {
                    if (meta.is_unsatisfied_range && meta.has_range_total && meta.range_total == 0) {
                        task.total_bytes = 0;
                        task.range_supported = false;
                        return HeaderAction::kStopSuccess;
                    }
                    std::lock_guard<std::mutex> lk(err_mu);
                    fatal_error = Error{"ERR_HTTP_RANGE_NOT_SATISFIABLE", "HTTP 416 Range Not Satisfiable on initial request", false, true};
                    fatal_abort.store(true);
                    return HeaderAction::kAbort;
                }

                if (meta.status_code >= 400) {
                    return HeaderAction::kAbort;
                }

                if (!meta.final_url.empty() && meta.final_url != task.url) {
                    auto orig_u = HttpClient::ParseUrl(task.url);
                    auto final_u = HttpClient::ParseUrl(meta.final_url);
                    if (orig_u.ok() && final_u.ok()) {
                        bool cross_origin = (orig_u.value->scheme != final_u.value->scheme ||
                                             orig_u.value->host != final_u.value->host ||
                                             orig_u.value->port != final_u.value->port);
                        if (cross_origin) {
                            OPENIDM_LOG_INFO("HttpEngine", "Cross-origin redirect detected (" + task.url + " -> " + meta.final_url + "); scrubbing credentials");
                            static const char* kSensitiveHeaders[] = {
                                "cookie", "cookie2", "authorization", "proxy-authorization",
                                "Cookie", "Cookie2", "Authorization", "Proxy-Authorization"
                            };
                            for (const char* h : kSensitiveHeaders) {
                                req_headers.erase(h);
                                task.headers.erase(h);
                                task.auth_headers.erase(h);
                            }
                        }
                    }
                    task.url = meta.final_url;
                }

                bool range_supported = (meta.status_code == 206 && meta.is_partial_content);
                uint64_t total_bytes = 0;
                if (range_supported && meta.range_total > 0) {
                    total_bytes = meta.range_total;
                } else if (meta.has_content_length) {
                    total_bytes = meta.content_length;
                }
                if (!range_supported && !meta.accept_ranges_none &&
                    (meta.accept_ranges_bytes || task.range_supported)) {
                    range_supported = true;
                }

                task.range_supported = range_supported;
                task.total_bytes = total_bytes;
                if (!meta.etag.empty()) task.etag = meta.etag;
                if (!meta.last_modified.empty()) task.last_modified = meta.last_modified;

                t_connect_ms.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - engine_start_tp).count(), std::memory_order_relaxed);
                diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                    task.task_id, "Response headers received",
                    "HTTP " + std::to_string(meta.status_code) + " Content-Length=" + std::to_string(total_bytes) +
                    (range_supported ? " (Range supported)" : " (No Range)"));

                if (!meta.content_disposition.empty()) {
                    std::string cd_name = PathSanitizer::ExtractFromContentDisposition(meta.content_disposition);
                    if (!cd_name.empty()) task.file_name = cd_name;
                }
                if (task.file_name.empty() || task.file_name == "download" || task.file_name == "index.html" || task.file_name == "file") {
                    std::string url_name = PathSanitizer::ExtractFromUrl(task.url);
                    if (!url_name.empty() && url_name != "download") task.file_name = url_name;
                }
                if (task.file_name.empty()) task.file_name = "download.bin";
                task.file_name = PathSanitizer::SanitizeFileName(task.file_name);

                if (task.conflict_policy == ConflictPolicy::kPrompt || task.conflict_policy == ConflictPolicy::kFailIfExists) {
                    auto pre_dest = PathSanitizer::ResolveSafeOutputPath(task.save_dir, task.file_name, task.conflict_policy);
                    if (!pre_dest.ok() && pre_dest.error->code == "ERR_FILE_EXISTS") {
                        std::lock_guard<std::mutex> lk(err_mu);
                        fatal_error = Error{"ERR_FILE_EXISTS", "Destination file already exists: " + PathSanitizer::PathToUtf8(std::filesystem::path(task.save_dir) / task.file_name), false, false};
                        fatal_abort.store(true);
                        return HeaderAction::kAbort;
                    }
                }

                if (task.category.empty() || task.category == "General") {
                    std::string fn_cat = InferCategoryFromFilename(task.file_name);
                    if (fn_cat != "General") {
                        task.category = fn_cat;
                    } else if (!meta.content_type.empty()) {
                        std::string mime_cat = InferCategoryFromMime(meta.content_type);
                        if (mime_cat != "General") task.category = mime_cat;
                    }
                }

                uint64_t remaining_to_download = (total_bytes > task.downloaded_bytes) ? (total_bytes - task.downloaded_bytes) : total_bytes;
                auto space_res = PathSanitizer::CheckAvailableDiskSpace(task.save_dir, remaining_to_download);
                if (!space_res.ok()) {
                    std::lock_guard<std::mutex> lk(err_mu);
                    fatal_error = *space_res.error;
                    fatal_abort.store(true);
                    return HeaderAction::kAbort;
                }

                std::error_code ec;
                std::filesystem::create_directories(task.save_dir, ec);

                if (!task.part_path.empty() && std::filesystem::exists(task.part_path, ec) &&
                    task.part_path.find(task.task_id) != std::string::npos) {
                    part_path = task.part_path;
                } else {
                    part_path = std::filesystem::path(task.save_dir) / (task.file_name + "." + task.task_id + ".part");
                    task.part_path = part_path.string();
                }

                if (is_resume) {
                    bool validator_mismatch = false;
                    std::string mismatch_reason;
                    if (!task.etag.empty() && !meta.etag.empty() &&
                        meta.etag.rfind("W/", 0) != 0 && task.etag != meta.etag) {
                        validator_mismatch = true;
                        mismatch_reason = "Remote ETag changed from " + task.etag + " to " + meta.etag;
                    } else if (!task.last_modified.empty() && !meta.last_modified.empty() &&
                               task.last_modified != meta.last_modified) {
                        validator_mismatch = true;
                        mismatch_reason = "Remote Last-Modified changed from " + task.last_modified + " to " + meta.last_modified;
                    } else if (task.total_bytes > 0 && total_bytes > 0 && task.total_bytes != total_bytes) {
                        validator_mismatch = true;
                        mismatch_reason = "Remote file size changed on resume (expected " +
                                          std::to_string(task.total_bytes) + ", got " + std::to_string(total_bytes) + ")";
                    } else if (meta.status_code == 200) {
                        validator_mismatch = true;
                        mismatch_reason = "Server returned 200 OK instead of 206 Partial Content on resume";
                    }

                    if (validator_mismatch) {
                        OPENIDM_LOG_WARN("HttpEngine", "Validator mismatch detected (" + mismatch_reason + "); discarding stale partial segments and restarting from byte 0");
                        task.segments.clear();
                        task.downloaded_bytes = 0;
                        is_resume = false;
                        if (!part_path.empty()) {
                            std::error_code ec_remove;
                            std::filesystem::remove(part_path, ec_remove);
                        }
                    }
                }

                if (!part_file.Open(part_path)) {
                    std::lock_guard<std::mutex> lk(err_mu);
                    fatal_error = Error{"ERR_STORAGE_OPEN", "Failed to open part file: " + part_path.string(), false, false};
                    fatal_abort.store(true);
                    return HeaderAction::kAbort;
                }

                if (total_bytes > 0) {
                    diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                        task.task_id, "Storage allocation begins", "Allocating " + std::to_string(total_bytes) + " bytes using sparse file optimization");
                    auto alloc_t0 = std::chrono::steady_clock::now();
                    if (!is_resume) {
                        if (!part_file.Truncate(total_bytes)) {
                            part_file.Close();
                            std::lock_guard<std::mutex> lk(err_mu);
                            fatal_error = Error{"ERR_DISK_FULL", "Failed to pre-allocate disk space", false, true};
                            fatal_abort.store(true);
                            return HeaderAction::kAbort;
                        }
                    } else {
                        uint64_t actual_part_sz = std::filesystem::file_size(part_path, ec);
                        if (actual_part_sz != total_bytes) {
                            part_file.Truncate(total_bytes);
                        }
                    }
                    auto alloc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - alloc_t0).count();
                    t_disk_alloc_ms.store(alloc_ms, std::memory_order_relaxed);
                    diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                        task.task_id, "Storage allocation completed", "Completed in " + std::to_string(alloc_ms) + " ms");
                    OPENIDM_LOG_INFO("HttpEngine", "Task " + task.task_id + ": Storage allocation completed in " + std::to_string(alloc_ms) + " ms");
                }

                task.state = TaskState::kDownloading;

                if (range_supported && total_bytes > m_cfg.min_split_bytes && max_conns > 1) {
                    is_segmented = true;
                    scheduler = std::make_unique<SegmentScheduler>(total_bytes, max_conns, m_cfg.min_split_bytes);
                    if (is_resume && !task.segments.empty()) {
                        scheduler->Initialize(task.segments);
                    } else {
                        scheduler->InitializeEquipartition(max_conns);
                    }
                    seg0 = scheduler->AcquireNextOrStealSegment(false);
                    if (seg0) {
                        seg0->active_worker.store(true);
                    }
                    task.status_detail = "Downloading (" + std::to_string(max_conns) + " connections)";
                    task.segments = scheduler->GetSnapshots();
                    if (on_progress) {
                        EngineProgress ep;
                        ep.task_id = task.task_id;
                        ep.state = TaskState::kDownloading;
                        ep.downloaded_bytes = scheduler->TotalDownloadedBytes();
                        ep.total_bytes = task.total_bytes;
                        ep.speed_bps = 0;
                        ep.range_supported = true;
                        ep.resolved_filename = task.file_name;
                        ep.etag = task.etag;
                        ep.last_modified = task.last_modified;
                        ep.status_detail = task.status_detail;
                        ep.segments = task.segments;
                        on_progress(ep);
                    }
                    diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                        task.task_id, "Worker initialization begins", "Spawning " + std::to_string(max_conns) + " download worker connections");

                    // Spawn secondary workers 1..max_conns-1
                    for (uint32_t i = 1; i < max_conns; ++i) {
                        secondary_workers.emplace_back(worker_fn, i);
                    }
                } else {
                    is_segmented = false;
                    task.status_detail = "Downloading (1 connection)";
                    if (on_progress) {
                        EngineProgress ep;
                        ep.task_id = task.task_id;
                        ep.state = TaskState::kDownloading;
                        ep.downloaded_bytes = 0;
                        ep.total_bytes = task.total_bytes;
                        ep.speed_bps = 0;
                        ep.range_supported = false;
                        ep.resolved_filename = task.file_name;
                        ep.etag = task.etag;
                        ep.last_modified = task.last_modified;
                        ep.status_detail = task.status_detail;
                        on_progress(ep);
                    }
                }

                return HeaderAction::kContinue;
            },
            [&](const uint8_t* data, size_t len) -> bool {
                if (fatal_abort.load() || pause_requested.load() || cancel_requested.load()) {
                    req_cancel.store(true);
                    return false;
                }
                if (!first_byte_recorded.exchange(true)) {
                    diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                        task.task_id, "First data bytes received",
                        "Initial transfer received payload (" + std::to_string(len) + " bytes)");
                }
                consume_both_limiters(len, req_cancel);

                if (is_segmented && seg0) {
                    RuntimeTimedLock write_lk(seg0->write_mu, "segment_write", "worker0_body_write");
                    uint64_t write_pos = seg0->current_offset.load();
                    uint64_t current_end = seg0->end_offset.load();
                    if (write_pos > current_end) {
                        seg0->completed.store(true);
                        seg0->active_worker.store(false);
                        req_cancel.store(true);
                        return false;
                    }
                    uint64_t max_writable = (current_end >= write_pos) ? (current_end - write_pos + 1) : 0;
                    if (max_writable == 0) {
                        seg0->completed.store(true);
                        seg0->active_worker.store(false);
                        req_cancel.store(true);
                        return false;
                    }
                    size_t to_write = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(len), max_writable));
                    if (!part_file.PWrite(data, to_write, write_pos)) {
                        std::lock_guard<std::mutex> lk(err_mu);
                        fatal_error = Error{"ERR_DISK_FULL", "Disk write failed on Worker 0", false, true};
                        fatal_abort.store(true);
                        req_cancel.store(true);
                        return false;
                    }
                    bool is_first_w0 = false;
                    if (!first_write_recorded.exchange(true)) {
                        is_first_w0 = true;
                        const auto first_byte_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - engine_start_tp).count();
                        t_first_byte_ms.store(first_byte_ms, std::memory_order_relaxed);
                        OPENIDM_LOG_INFO("HttpStartupProfile",
                            "task=" + task.task_id +
                            " t_probe_ms=" + std::to_string(t_probe_ms.load(std::memory_order_relaxed)) +
                            " t_disk_alloc_ms=" + std::to_string(t_disk_alloc_ms.load(std::memory_order_relaxed)) +
                            " t_connect_ms=" + std::to_string(t_connect_ms.load(std::memory_order_relaxed)) +
                            " t_first_byte_ms=" + std::to_string(first_byte_ms));
                        diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                            task.task_id, "First bytes written to disk",
                            "Worker 0 committed " + std::to_string(to_write) + " bytes at offset " + std::to_string(write_pos) +
                            " (t_first_byte_ms=" + std::to_string(first_byte_ms) + ")");
                    }
                    seg0->current_offset.fetch_add(to_write);
                    const bool seg0_done = (seg0->current_offset.load() > seg0->end_offset.load());
                    if (seg0_done) {
                        seg0->completed.store(true);
                        seg0->active_worker.store(false);
                        req_cancel.store(true);
                    }
                    write_lk.Unlock();
                    emit_segmented_progress_if_due(is_first_w0);
                    if (seg0_done) {
                        return false;
                    }
                } else {
                    bool is_first_single = false;
                    if (!part_file.PWrite(data, len, single_stream_written)) {
                        std::lock_guard<std::mutex> lk(err_mu);
                        fatal_error = Error{"ERR_DISK_FULL", "Disk write failed in single stream", false, true};
                        fatal_abort.store(true);
                        req_cancel.store(true);
                        return false;
                    }
                    if (!first_write_recorded.exchange(true)) {
                        is_first_single = true;
                        const auto first_byte_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - engine_start_tp).count();
                        t_first_byte_ms.store(first_byte_ms, std::memory_order_relaxed);
                        OPENIDM_LOG_INFO("HttpStartupProfile",
                            "task=" + task.task_id +
                            " t_probe_ms=" + std::to_string(t_probe_ms.load(std::memory_order_relaxed)) +
                            " t_disk_alloc_ms=" + std::to_string(t_disk_alloc_ms.load(std::memory_order_relaxed)) +
                            " t_connect_ms=" + std::to_string(t_connect_ms.load(std::memory_order_relaxed)) +
                            " t_first_byte_ms=" + std::to_string(first_byte_ms));
                        diagnostics::StartupDiagnosticRecorder::Instance().RecordEvent(
                            task.task_id, "First bytes written to disk",
                            "Single stream committed " + std::to_string(len) + " bytes at offset " + std::to_string(single_stream_written) +
                            " (t_first_byte_ms=" + std::to_string(first_byte_ms) + ")");
                    }
                    single_stream_written += len;
                    task.downloaded_bytes = single_stream_written;

                    auto now = std::chrono::steady_clock::now();
                    auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_tp).count();
                    if ((is_first_single || diff_ms >= 80) && on_progress) {
                        uint64_t delta_bytes = (single_stream_written >= last_progress_bytes) ? (single_stream_written - last_progress_bytes) : 0;
                        double dt_sec = std::max<double>(0.02, static_cast<double>(std::max<int64_t>(1, diff_ms)) / 1000.0);
                        uint64_t physical_bps = static_cast<uint64_t>(static_cast<double>(delta_bytes) / dt_sec);
                        single_stream_speed = physical_bps;
                        last_progress_bytes = single_stream_written;
                        last_progress_tp = now;

                        EngineProgress ep;
                        ep.task_id = task.task_id;
                        ep.state = TaskState::kDownloading;
                        ep.downloaded_bytes = single_stream_written;
                        ep.total_bytes = task.total_bytes;
                        ep.speed_bps = single_stream_speed;
                        ep.range_supported = false;
                        ep.resolved_filename = task.file_name;
                        ep.etag = task.etag;
                        ep.last_modified = task.last_modified;
                        on_progress(ep);
                    }
                }
                return true;
            },
            m_cfg.net_opts);

        if (captured_retry_after_sec > 0 ||
            (!initial_res.ok() && initial_res.error &&
             (initial_res.error->code == "ERR_HTTP_429_RATE_LIMITED" || initial_res.error->code == "ERR_HTTP_5XX_TRANSIENT"))) {
            if (captured_retry_after_sec > 30) {
                OPENIDM_LOG_WARN("HttpEngine", "Task " + task.task_id + ": Initial request received HTTP " +
                    (initial_res.error ? initial_res.error->code : "429") + " with Retry-After: " + std::to_string(captured_retry_after_sec) + "s > 30s");
                task.state = TaskState::kPaused;
                task.pause_reason = PauseReason::kError;
                task.error_code = (initial_res.error ? initial_res.error->code : "ERR_HTTP_429_RATE_LIMITED");
                task.error_message = "Server requested wait of " + std::to_string(captured_retry_after_sec) + "s";
                task.status_detail = task.error_message;
                return Result<std::filesystem::path>::Err(task.error_code, task.error_message, true, true);
            }
            int wait_sec = (captured_retry_after_sec > 0) ? captured_retry_after_sec : (initial_attempt * 2);
            OPENIDM_LOG_WARN("HttpEngine", "Task " + task.task_id + ": Initial request rate limited; waiting " + std::to_string(wait_sec) + "s before retry...");
            for (int slept = 0; slept < wait_sec * 1000; slept += 50) {
                if (pause_requested.load() || cancel_requested.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50, wait_sec * 1000 - slept)));
            }
            if (pause_requested.load() || cancel_requested.load()) break;
            continue;
        }
        break;
    }

    if (is_segmented && scheduler) {
        if (seg0) {
            if (seg0->current_offset.load() > seg0->end_offset.load()) {
                seg0->completed.store(true);
            }
            seg0->active_worker.store(false);
        }
        uint64_t initial_bytes = scheduler->TotalDownloadedBytes();
        auto last_sp_tp = std::chrono::steady_clock::now();
        uint64_t last_sp_bytes = initial_bytes;
        uint64_t current_speed = 0;
        uint32_t spawned = max_conns;

        while (!scheduler->AllCompleted() && !fatal_abort.load() && !pause_requested.load() && !cancel_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            auto now = std::chrono::steady_clock::now();
            uint32_t dynamic_max = std::min(max_conns, scheduler->GetMaxConnections());
            if (running_workers.load() < dynamic_max && spawned < 256 &&
                (scheduler->HasIncompleteUnassignedSegment() || (!disable_splitting.load() && scheduler->CanSplitAnySegment()))) {
                secondary_workers.emplace_back(worker_fn, spawned++);
            }

            auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_sp_tp).count();
            if (diff_ms >= 100) {
                uint64_t dl = scheduler->TotalDownloadedBytes();
                task.downloaded_bytes = dl;
                task.segments = scheduler->GetSnapshots();
                task.status_detail = "Downloading (" + std::to_string(scheduler->ActiveWorkerCount()) + " active connections)";
                uint64_t delta_bytes = (dl >= last_sp_bytes) ? (dl - last_sp_bytes) : 0;
                double dt_sec = std::max<double>(0.05, static_cast<double>(diff_ms) / 1000.0);
                uint64_t physical_bps = static_cast<uint64_t>(static_cast<double>(delta_bytes) / dt_sec);
                current_speed = physical_bps;
                last_sp_bytes = dl;
                last_sp_tp = now;

                if (on_progress) {
                    EngineProgress ep;
                    ep.task_id = task.task_id;
                    ep.state = TaskState::kDownloading;
                    ep.downloaded_bytes = dl;
                    ep.total_bytes = task.total_bytes;
                    ep.speed_bps = current_speed;
                    ep.range_supported = true;
                    ep.resolved_filename = task.file_name;
                    ep.etag = task.etag;
                    ep.last_modified = task.last_modified;
                    ep.status_detail = task.status_detail;
                    ep.segments = task.segments;
                    on_progress(ep);
                }
            }
        }

        for (auto& t : secondary_workers) {
            if (t.joinable()) t.join();
        }

        part_file.SyncAndClose();
        task.downloaded_bytes = scheduler->TotalDownloadedBytes();
        task.segments = scheduler->GetSnapshots();
    } else {
        part_file.SyncAndClose();
        if (task.total_bytes > 0 && single_stream_written < task.total_bytes && !pause_requested.load() && !cancel_requested.load()) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_CONNECTION_CLOSED_PREMATURELY";
            task.error_message = "Single-stream transfer closed before receiving advertised content length";
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(task.error_code, task.error_message);
        }
        task.total_bytes = single_stream_written;
        task.downloaded_bytes = single_stream_written;
    }

    if (cancel_requested.load()) {
        std::error_code ec;
        std::filesystem::remove(part_path, ec);
        task.state = TaskState::kCanceled;
        task.status_detail = "Canceled";
        return Result<std::filesystem::path>::Err("ERR_CANCELED", "Task canceled by user");
    }
    if (pause_requested.load()) {
        task.state = TaskState::kPaused;
        task.status_detail = "Paused";
        return Result<std::filesystem::path>::Err("ERR_PAUSED", "Task paused by user");
    }
    if (fatal_abort.load() || (is_segmented && scheduler && !scheduler->AllCompleted())) {
        std::lock_guard<std::mutex> lk(err_mu);
        Error err = fatal_error.value_or(Error{"ERR_NET_INCOMPLETE", "Segments did not complete", true, true});
        task.state = err.user_action_resumable ? TaskState::kPaused : TaskState::kFailed;
        task.error_code = err.code;
        task.error_message = err.message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(err.code, err.message, err.retryable, err.user_action_resumable);
    }
    if (!initial_res.ok() && fatal_error.has_value()) {
        task.state = fatal_error->user_action_resumable ? TaskState::kPaused : TaskState::kFailed;
        task.error_code = fatal_error->code;
        task.error_message = fatal_error->message;
        task.status_detail = task.error_message;
        return Result<std::filesystem::path>::Err(fatal_error->code, fatal_error->message, fatal_error->retryable, fatal_error->user_action_resumable);
    }

    // -------------------------------------------------------------------------
    // Phase 3: File Verification & Atomic Rename (FR-007, FR-026)
    // -------------------------------------------------------------------------
    EmitHttpPhaseTrace("finalization_start", engine_start_tp);
    task.state = TaskState::kVerifying;
    task.status_detail = "Verifying file integrity...";
    if (!task.expected_sha256.empty()) {
        auto sha_res = CryptoUtils::ComputeFileSha256Hex(part_path);
        if (!sha_res.ok()) {
            task.state = TaskState::kFailed;
            task.error_code = sha_res.error->code;
            task.error_message = sha_res.error->message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(sha_res.error->code, sha_res.error->message);
        }
        std::string actual_sha = *sha_res.value;
        std::string expected_lower = task.expected_sha256;
        for (char& c : expected_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (actual_sha != expected_lower) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_STORAGE_PART_CORRUPTED";
            task.error_message = "SHA-256 mismatch: expected " + expected_lower + ", got " + actual_sha;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err("ERR_STORAGE_PART_CORRUPTED", task.error_message, false, true);
        }
    }
    if (!task.expected_md5.empty()) {
        auto md5_res = CryptoUtils::ComputeFileMd5Hex(part_path);
        if (!md5_res.ok()) {
            task.state = TaskState::kFailed;
            task.error_code = md5_res.error->code;
            task.error_message = md5_res.error->message;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err(md5_res.error->code, md5_res.error->message);
        }
        std::string actual_md5 = *md5_res.value;
        std::string expected_md5_lower = task.expected_md5;
        for (char& c : expected_md5_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (actual_md5 != expected_md5_lower) {
            task.state = TaskState::kFailed;
            task.error_code = "ERR_STORAGE_PART_CORRUPTED";
            task.error_message = "MD5 mismatch: expected " + expected_md5_lower + ", got " + actual_md5;
            task.status_detail = task.error_message;
            return Result<std::filesystem::path>::Err("ERR_STORAGE_PART_CORRUPTED", task.error_message, false, true);
        }
    }

    auto commit_res = PathSanitizer::CommitPartFile(part_path, task.save_dir, task.file_name, task.conflict_policy, task.total_bytes);
    if (!commit_res.ok()) {
        task.state = TaskState::kFailed;
        task.error_code = commit_res.error->code;
        task.error_message = commit_res.error->message;
        task.status_detail = task.error_message;
        return commit_res;
    }

    std::filesystem::path final_path = *commit_res.value;
    task.file_name = PathSanitizer::PathToUtf8(final_path.filename());
    task.part_path.clear();
    task.state = TaskState::kCompleted;
    task.status_detail = "Completed";
    task.completed_at_ms = NowEpochMs();
    task.auth_headers.clear();
    EmitHttpPhaseTrace("finalization_end", engine_start_tp);
    return Result<std::filesystem::path>::Ok(final_path);
}

} // namespace openidm::brisk
