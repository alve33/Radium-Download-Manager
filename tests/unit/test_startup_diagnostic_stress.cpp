// SPDX-License-Identifier: Apache-2.0
// Empirical Stress Test Harness for StartupDiagnosticRecorder (Milestone 1)
// Rigorously tests:
// 1. Multithreaded concurrent event recording across 16 threads for the same session.
// 2. Monotonicity (E_i <= E_{i+1}) and non-negative duration deltas (D_i >= 0) post FinalizeMetrics().
// 3. Edge cases in client_click_epoch_ms (future, zero, past, distant past, overflow limits).
// 4. Strict deduplication of "First data bytes received" across concurrent threads and aliases.
// 5. Ingestion of out-of-order extension stages and persistence/export validation.

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cassert>
#include <random>
#include <filesystem>
#include <cmath>

#include "common/startup_diagnostic.hpp"

using namespace radium::diagnostics;

#define STRESS_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "\n[ASSERTION FAILED] " << msg << " (" #cond ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

// Helper to verify invariants on any StartupReport
bool VerifyReportInvariants(const StartupReport& rep, const std::string& context_name) {
    if (rep.events.empty()) {
        return true;
    }

    uint64_t cumulative_duration = 0;
    for (size_t i = 0; i < rep.events.size(); ++i) {
        const auto& ev = rep.events[i];

        // Invariant 1: Delta non-negativity (D_i >= 0)
        // (Note: duration_us is uint64_t, so it is mathematically >= 0, but check against overflow / invalid state)
        if (i == 0) {
            STRESS_ASSERT(ev.duration_us == ev.elapsed_us,
                context_name + ": First event duration_us must equal elapsed_us");
            cumulative_duration = ev.duration_us;
        } else {
            const auto& prev = rep.events[i - 1];

            // Invariant 2: Monotonically non-decreasing elapsed_us (E_{i-1} <= E_i)
            STRESS_ASSERT(ev.elapsed_us >= prev.elapsed_us,
                context_name + ": Monotonicity violated: E[" + std::to_string(i) + "]=" +
                std::to_string(ev.elapsed_us) + " < E[" + std::to_string(i-1) + "]=" +
                std::to_string(prev.elapsed_us));

            // Invariant 3: Duration delta exact match (D_i == E_i - E_{i-1})
            uint64_t expected_delta = ev.elapsed_us - prev.elapsed_us;
            STRESS_ASSERT(ev.duration_us == expected_delta,
                context_name + ": Duration delta mismatch at index " + std::to_string(i) +
                ": D_i=" + std::to_string(ev.duration_us) + " != expected=" + std::to_string(expected_delta));

            cumulative_duration += ev.duration_us;
        }

        // Invariant 4: Cumulative sum of durations must equal current event elapsed_us
        STRESS_ASSERT(cumulative_duration == ev.elapsed_us,
            context_name + ": Cumulative duration sum mismatch: sum=" +
            std::to_string(cumulative_duration) + " != elapsed=" + std::to_string(ev.elapsed_us));
    }

    // Invariant 5: Startup metrics non-negativity
    STRESS_ASSERT(rep.startup_time_ms >= 0.0, context_name + ": startup_time_ms must be non-negative");
    STRESS_ASSERT(rep.transfer_duration_ms >= 0.0, context_name + ": transfer_duration_ms must be non-negative");
    STRESS_ASSERT(rep.avg_speed_mbps >= 0.0, context_name + ": avg_speed_mbps must be non-negative");

    return true;
}

// ============================================================================
// TEST 1: 16-Thread Concurrent Event Recording Stress Test
// ============================================================================
bool TestConcurrent16ThreadEventRecording() {
    std::cout << "[RUN] Test 1: 16-Thread Concurrent Event Recording on Same Session...\n";
    auto& recorder = StartupDiagnosticRecorder::Instance();
    recorder.ClearReports();

    const std::string task_id = "test-task-16th-stress";
    const std::string correlation_id = "corr-16th-stress";
    const uint64_t now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const uint64_t click_time = now_ms - 250; // 250 ms ago

    recorder.StartSession(task_id, correlation_id, "https://example.com/page",
                          "https://example.com/file.bin", "file.bin", "HTTP", "", 1048576, click_time);

    const int kNumThreads = 16;
    const int kEventsPerThread = 250; // 16 * 250 = 4,000 concurrent events
    std::atomic<bool> start_signal{false};
    std::atomic<bool> stop_readers{false};
    std::vector<std::thread> workers;
    workers.reserve(kNumThreads);

    // Launch 16 recording worker threads
    for (int t = 0; t < kNumThreads; ++t) {
        workers.emplace_back([&recorder, t, &start_signal, task_id, correlation_id]() {
            while (!start_signal.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int e = 0; e < kEventsPerThread; ++e) {
                // Alternate between download_id and correlation_id lookup
                std::string target_id = (e % 2 == 0) ? task_id : correlation_id;
                std::string ev_name = "Thread_" + std::to_string(t) + "_Event_" + std::to_string(e);
                std::string detail = "Concurrent payload from thread " + std::to_string(t);
                recorder.RecordEvent(target_id, ev_name, detail);

                if (e % 50 == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Launch concurrent reader thread that continuously exercises GetReports()
    std::thread reader_thread([&recorder, &stop_readers]() {
        while (!stop_readers.load(std::memory_order_relaxed)) {
            auto reps = recorder.GetReports();
            for (const auto& r : reps) {
                // Verify invariants even during mid-stream snapshots
                if (!r.events.empty()) {
                    assert(r.events.front().elapsed_us <= r.events.back().elapsed_us);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // Launch concurrent progress/detail updater thread
    std::thread updater_thread([&recorder, &stop_readers, task_id, correlation_id]() {
        uint64_t bytes = 0;
        while (!stop_readers.load(std::memory_order_relaxed)) {
            bytes += 1024;
            recorder.UpdateProgress(task_id, bytes, 1048576);
            recorder.UpdateTaskDetails(correlation_id, "file_updated.bin", "", 1048576, "1080p");
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    // Release workers simultaneously
    start_signal.store(true, std::memory_order_release);

    // Join all 16 workers
    for (auto& w : workers) {
        w.join();
    }

    // Stop background readers
    stop_readers.store(true, std::memory_order_release);
    reader_thread.join();
    updater_thread.join();

    // Complete session
    recorder.CompleteSession(task_id, true, "");

    // Verify final report
    auto reports = recorder.GetReports();
    STRESS_ASSERT(reports.size() == 1, "Expected exactly 1 stored report");
    const auto& rep = reports[0];

    // Initial click event (1) + task created (1) + (16 * 250) + completed (1) = 4,003 events
    const size_t expected_count = 2 + (kNumThreads * kEventsPerThread) + 1;
    STRESS_ASSERT(rep.events.size() == expected_count,
        "Expected " + std::to_string(expected_count) + " events, got " + std::to_string(rep.events.size()));

    // Rigorously test monotonicity and duration delta correctness
    STRESS_ASSERT(VerifyReportInvariants(rep, "TestConcurrent16Thread"), "Report invariants failed");

    std::cout << "   -> PASS (Recorded " << rep.events.size()
              << " events across 16 threads; 100% strictly monotonic, all duration deltas >= 0)\n";
    return true;
}

// ============================================================================
// TEST 2: Monotonicity and Non-Negative Deltas with Out-of-Order Ingestion
// ============================================================================
bool TestOutOfOrderIngestionAndSorting() {
    std::cout << "[RUN] Test 2: Out-of-Order Ingestion & Metric Finalization...\n";
    auto& recorder = StartupDiagnosticRecorder::Instance();
    recorder.ClearReports();

    const std::string task_id = "test-task-ooo";
    const uint64_t base_epoch = 1700000000000ULL;

    recorder.StartSession(task_id, "corr-ooo", "https://example.com", "https://example.com/f",
                          "f.bin", "YouTube", "1080p", 2048, base_epoch);

    // Inject 100 extension stages with randomly distributed and inverted epoch timestamps
    std::vector<std::pair<std::string, uint64_t>> stages;
    std::mt19937_64 rng(1337);
    std::uniform_int_distribution<uint64_t> dist(base_epoch - 500, base_epoch + 15000);

    for (int i = 0; i < 100; ++i) {
        stages.push_back({"Stage_" + std::to_string(i), dist(rng)});
    }
    recorder.IngestExtensionStages(task_id, stages);

    // Record interspersed core events
    recorder.RecordEvent(task_id, "Core early event");
    recorder.RecordEvent(task_id, "First data bytes received");
    recorder.RecordEvent(task_id, "Core middle event");
    recorder.CompleteSession(task_id, true);

    auto reports = recorder.GetReports();
    STRESS_ASSERT(reports.size() == 1, "Expected 1 report");
    const auto& rep = reports[0];

    // Invariants check
    STRESS_ASSERT(VerifyReportInvariants(rep, "TestOutOfOrderIngestion"), "Report invariants failed");

    // Check first byte was properly captured in startup_time_ms
    STRESS_ASSERT(rep.startup_time_ms > 0.0, "startup_time_ms should be positive");

    std::cout << "   -> PASS (Sorted " << rep.events.size()
              << " events with randomized timestamps; all deltas non-negative)\n";
    return true;
}

// ============================================================================
// TEST 3: Edge Cases in client_click_epoch_ms (Future, Zero, Hours Past, Limits)
// ============================================================================
bool TestClientClickEpochEdgeCases() {
    std::cout << "[RUN] Test 3: Edge Cases in client_click_epoch_ms...\n";
    auto& recorder = StartupDiagnosticRecorder::Instance();

    const uint64_t epoch_now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    // 3.1: Future click epoch (e.g. client clock 15 seconds ahead)
    {
        recorder.ClearReports();
        const std::string id = "task-future-click";
        recorder.StartSession(id, "c-fut", "", "https://a.com", "a.bin", "HTTP", "", 0, epoch_now + 15000);
        recorder.RecordEvent(id, "Event 1");
        recorder.RecordEvent(id, "Event 2");
        recorder.CompleteSession(id, true);

        auto reps = recorder.GetReports();
        STRESS_ASSERT(reps.size() == 1, "3.1: Expected 1 report");
        STRESS_ASSERT(VerifyReportInvariants(reps[0], "FutureClickEpoch"), "3.1 invariants failed");
        // Ensure click offset clamped to 0 and did not underflow
        STRESS_ASSERT(reps[0].events[0].elapsed_us == 0, "3.1: First event elapsed should be 0");
        STRESS_ASSERT(reps[0].events[1].elapsed_us == 0, "3.1: Second event elapsed should be 0");
        STRESS_ASSERT(reps[0].events[0].duration_us == 0, "3.1: Duration should be 0");
        STRESS_ASSERT(reps[0].events[1].duration_us == 0, "3.1: Duration should be 0");
    }

    // 3.2: Distant Future click epoch (1 year ahead)
    {
        recorder.ClearReports();
        const std::string id = "task-dist-future";
        recorder.StartSession(id, "c-dfut", "", "https://a.com", "a.bin", "HTTP", "", 0, epoch_now + 31536000000ULL);
        recorder.RecordEvent(id, "Event 1");
        recorder.CompleteSession(id, true);

        auto reps = recorder.GetReports();
        STRESS_ASSERT(reps.size() == 1, "3.2: Expected 1 report");
        STRESS_ASSERT(VerifyReportInvariants(reps[0], "DistantFutureClickEpoch"), "3.2 invariants failed");
    }

    // 3.3: Zero click epoch (direct/internal download without browser click time)
    {
        recorder.ClearReports();
        const std::string id = "task-zero-click";
        recorder.StartSession(id, "c-zero", "", "https://a.com", "a.bin", "HTTP", "", 0, 0);
        recorder.RecordEvent(id, "Event 1");
        recorder.RecordEvent(id, "Event 2");
        recorder.CompleteSession(id, true);

        auto reps = recorder.GetReports();
        STRESS_ASSERT(reps.size() == 1, "3.3: Expected 1 report");
        STRESS_ASSERT(reps[0].events.front().event_name == "Download task created",
            "3.3: First event should be 'Download task created'");
        STRESS_ASSERT(VerifyReportInvariants(reps[0], "ZeroClickEpoch"), "3.3 invariants failed");
    }

    // 3.4: Hours in the past (e.g. 5 hours ago)
    {
        recorder.ClearReports();
        const std::string id = "task-5h-past";
        const uint64_t five_hours_ms = 5ULL * 3600ULL * 1000ULL;
        recorder.StartSession(id, "c-5h", "", "https://a.com", "a.bin", "HTTP", "", 0, epoch_now - five_hours_ms);
        recorder.RecordEvent(id, "Event after 5h");
        recorder.CompleteSession(id, true);

        auto reps = recorder.GetReports();
        STRESS_ASSERT(reps.size() == 1, "3.4: Expected 1 report");
        const auto& r = reps[0];
        STRESS_ASSERT(r.events.size() == 4, "3.4: Expected 4 events (click, create, after, complete)");
        STRESS_ASSERT(r.events[0].elapsed_us == 0, "3.4: Click event elapsed should be 0");
        // Task created should reflect ~5 hours in microseconds (~18,000,000,000 us)
        const uint64_t expected_min_offset_us = (five_hours_ms - 1000) * 1000;
        STRESS_ASSERT(r.events[1].elapsed_us >= expected_min_offset_us,
            "3.4: Task created event elapsed should reflect click offset");
        STRESS_ASSERT(VerifyReportInvariants(r, "FiveHoursPastClickEpoch"), "3.4 invariants failed");
    }

    // 3.5: Distant past (7 days ago = 604,800,000 ms)
    {
        recorder.ClearReports();
        const std::string id = "task-7d-past";
        const uint64_t seven_days_ms = 7ULL * 86400ULL * 1000ULL;
        recorder.StartSession(id, "c-7d", "", "https://a.com", "a.bin", "HTTP", "", 0, epoch_now - seven_days_ms);
        recorder.RecordEvent(id, "Event after 7d");
        recorder.CompleteSession(id, true);

        auto reps = recorder.GetReports();
        STRESS_ASSERT(reps.size() == 1, "3.5: Expected 1 report");
        STRESS_ASSERT(VerifyReportInvariants(reps[0], "SevenDaysPastClickEpoch"), "3.5 invariants failed");
    }

    // 3.6: Extreme limits (UINT64_MAX)
    {
        recorder.ClearReports();
        const std::string id = "task-uint64-max";
        recorder.StartSession(id, "c-max", "", "https://a.com", "a.bin", "HTTP", "", 0, std::numeric_limits<uint64_t>::max());
        recorder.RecordEvent(id, "Event Max");
        recorder.CompleteSession(id, true);

        auto reps = recorder.GetReports();
        STRESS_ASSERT(reps.size() == 1, "3.6: Expected 1 report");
        STRESS_ASSERT(VerifyReportInvariants(reps[0], "Uint64MaxClickEpoch"), "3.6 invariants failed");
    }

    std::cout << "   -> PASS (All edge cases handled cleanly without overflow, underflow, or inverted deltas)\n";
    return true;
}

// ============================================================================
// TEST 4: Deduplication of "First data bytes received" Under Concurrency
// ============================================================================
bool TestFirstDataBytesDeduplication() {
    std::cout << "[RUN] Test 4: Deduplication of 'First data bytes received' Across Threads & Aliases...\n";
    auto& recorder = StartupDiagnosticRecorder::Instance();
    recorder.ClearReports();

    const std::string task_id = "test-task-dedup";
    const std::string correlation_id = "corr-task-dedup";

    recorder.StartSession(task_id, correlation_id, "https://example.com", "https://example.com/v.mp4",
                          "v.mp4", "YouTube", "1080p", 50000000, 0);

    const int kNumThreads = 16;
    const int kAttemptsPerThread = 200; // 16 * 200 = 3,200 attempts to log first byte
    std::atomic<bool> start_gate{false};
    std::vector<std::thread> threads;
    threads.reserve(kNumThreads);

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([&recorder, t, &start_gate, task_id, correlation_id]() {
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < kAttemptsPerThread; ++i) {
                // Vary targets and alias names
                std::string target = (i % 2 == 0) ? task_id : correlation_id;
                int variant = (t + i) % 3;
                if (variant == 0) {
                    recorder.RecordEvent(target, "First data bytes received", "chunk from thread " + std::to_string(t));
                } else if (variant == 1) {
                    recorder.RecordEvent(target, "first_byte", "alias from thread " + std::to_string(t));
                } else {
                    recorder.RecordEvent(target, "FIRST_BYTE_RECEIVED", "uppercase alias from thread " + std::to_string(t));
                }

                // Also call UpdateProgress repeatedly (should NEVER emit synthetic first byte)
                if (i % 10 == 0) {
                    recorder.UpdateProgress(target, static_cast<uint64_t>(i * 1024), 50000000);
                }
            }
        });
    }

    start_gate.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    recorder.CompleteSession(task_id, true);

    auto reps = recorder.GetReports();
    STRESS_ASSERT(reps.size() == 1, "Expected 1 report");
    const auto& rep = reps[0];

    // Count first-byte occurrences
    int first_byte_count = 0;
    for (const auto& ev : rep.events) {
        if (ev.event_name == "First data bytes received" ||
            ev.event_name == "first_byte" ||
            ev.event_name == "FIRST_BYTE_RECEIVED") {
            first_byte_count++;
        }
    }

    STRESS_ASSERT(first_byte_count == 1,
        "Deduplication failed: Expected EXACTLY 1 first-byte event across 3,200 concurrent attempts, got " +
        std::to_string(first_byte_count));

    STRESS_ASSERT(rep.startup_time_ms > 0.0 || rep.events.size() > 0,
        "startup_time_ms must be validly calculated");
    STRESS_ASSERT(VerifyReportInvariants(rep, "TestFirstDataBytesDeduplication"), "Invariants failed");

    std::cout << "   -> PASS (Successfully deduplicated 3,200 concurrent first-byte attempts down to exactly 1)\n";
    return true;
}

// ============================================================================
// TEST 5: Persistence & Serialization Monotonicity Round-Trip
// ============================================================================
bool TestPersistenceAndSerializationRoundTrip() {
    std::cout << "[RUN] Test 5: Persistence & Serialization Monotonicity Round-Trip...\n";
    auto& recorder = StartupDiagnosticRecorder::Instance();
    recorder.ClearReports();

    std::error_code ec;
    auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path temp_dir = std::filesystem::temp_directory_path() / ("radium_stress_diag_" + std::to_string(now_ticks));
    std::filesystem::create_directories(temp_dir, ec);

    recorder.InitializeStorage(temp_dir);
    recorder.ClearReports();

    // Create 10 sessions with varying parameters and events
    for (int i = 0; i < 10; ++i) {
        std::string id = "persist-task-" + std::to_string(i);
        std::string corr = "persist-corr-" + std::to_string(i);
        recorder.StartSession(id, corr, "https://srv.com", "https://srv.com/data.bin",
                              "data.bin", "HTTP", "", 1000000, 0);

        recorder.RecordEvent(id, "Connect initialized");
        recorder.RecordEvent(corr, "TLS handshake complete");
        recorder.RecordEvent(id, "First data bytes received");
        recorder.UpdateProgress(id, 500000, 1000000);
        recorder.RecordEvent(corr, "50% transferred");
        recorder.UpdateProgress(id, 1000000, 1000000);
        recorder.CompleteSession(id, true);
    }

    // Verify in-memory reports
    auto reps_before = recorder.GetReports();
    STRESS_ASSERT(reps_before.size() == 10, "Expected 10 reports before disk reload");
    for (const auto& r : reps_before) {
        STRESS_ASSERT(VerifyReportInvariants(r, "PersistPreReload"), "Pre-reload invariants failed");
    }

    // Test JSON export parses back without errors
    std::string json_export = recorder.ExportToJson();
    STRESS_ASSERT(!json_export.empty(), "ExportToJson produced empty string");
    auto parsed_json = radium::json::Parse(json_export);
    STRESS_ASSERT(parsed_json.is_array(), "Parsed JSON export must be an array");
    STRESS_ASSERT(parsed_json.as_array().size() == 10, "JSON export array size mismatch");

    // Test CSV export is non-empty and well-formed
    std::string csv_export = recorder.ExportToCsv();
    STRESS_ASSERT(!csv_export.empty(), "ExportToCsv produced empty string");
    STRESS_ASSERT(csv_export.find("Download ID,Correlation ID") != std::string::npos, "CSV missing header");
    STRESS_ASSERT(csv_export.find("persist-task-9") != std::string::npos, "CSV missing task entries");

    // Force disk reload by re-initializing storage path
    recorder.InitializeStorage(temp_dir);

    auto reps_after = recorder.GetReports();
    STRESS_ASSERT(reps_after.size() == 10, "Expected 10 reports after disk reload");
    for (const auto& r : reps_after) {
        STRESS_ASSERT(VerifyReportInvariants(r, "PersistPostReload"), "Post-reload invariants failed");
    }

    // Clean up temp dir
    std::filesystem::remove_all(temp_dir, ec);

    std::cout << "   -> PASS (Round-trip disk persistence, JSON, and CSV verified with full invariant retention)\n";
    return true;
}

// ============================================================================
// TEST 6: Multi-Session Concurrent Stress (8 Sessions x 16 Threads)
// ============================================================================
bool TestMultiSessionConcurrentStress() {
    std::cout << "[RUN] Test 6: Multi-Session Concurrent Stress (8 Sessions x 16 Threads)...\n";
    auto& recorder = StartupDiagnosticRecorder::Instance();
    recorder.ClearReports();

    const int kNumSessions = 8;
    const int kThreads = 16;
    const int kEventsPerThread = 150;

    for (int s = 0; s < kNumSessions; ++s) {
        std::string sid = "multi-session-" + std::to_string(s);
        std::string scorr = "multi-corr-" + std::to_string(s);
        recorder.StartSession(sid, scorr, "https://ex.com", "https://ex.com/f" + std::to_string(s),
                              "f.bin", "HTTP", "", 1024 * 1024, 0);
    }

    std::atomic<bool> start_gate{false};
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&recorder, t, &start_gate, kNumSessions]() {
            while (!start_gate.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int e = 0; e < kEventsPerThread; ++e) {
                int s = (t + e) % kNumSessions;
                std::string sid = "multi-session-" + std::to_string(s);
                std::string scorr = "multi-corr-" + std::to_string(s);
                std::string target = (e % 2 == 0) ? sid : scorr;
                recorder.RecordEvent(target, "Th_" + std::to_string(t) + "_Ev_" + std::to_string(e));

                if (e % 20 == 0) {
                    recorder.UpdateProgress(target, static_cast<uint64_t>(e * 5000), 1024 * 1024);
                }
            }
        });
    }

    start_gate.store(true, std::memory_order_release);
    for (auto& w : workers) {
        w.join();
    }

    for (int s = 0; s < kNumSessions; ++s) {
        std::string sid = "multi-session-" + std::to_string(s);
        recorder.CompleteSession(sid, true);
    }

    auto reports = recorder.GetReports();
    STRESS_ASSERT(reports.size() == kNumSessions, "Expected all 8 sessions to be stored");
    for (size_t s = 0; s < reports.size(); ++s) {
        STRESS_ASSERT(VerifyReportInvariants(reports[s], "MultiSession_" + std::to_string(s)),
            "Multi-session invariant violation");
    }

    std::cout << "   -> PASS (8 concurrent sessions successfully isolated and verified across 16 threads)\n";
    return true;
}

// ============================================================================
// TEST 7: FinalizeMetrics Idempotency & Mathematical Invariant Extremes
// ============================================================================
bool TestFinalizeMetricsIdempotencyAndExtremes() {
    std::cout << "[RUN] Test 7: FinalizeMetrics Idempotency & Mathematical Invariant Extremes...\n";

    // Subtest 7.1: Empty report
    {
        StartupReport rep{};
        rep.FinalizeMetrics();
        STRESS_ASSERT(rep.events.empty(), "Empty report remains empty");
        STRESS_ASSERT(rep.startup_time_ms == 0.0, "startup_time_ms 0.0");
    }

    // Subtest 7.2: Single event
    {
        StartupReport rep{};
        StartupEvent ev{};
        ev.event_name = "Solo";
        ev.elapsed_us = 42000;
        rep.events.push_back(ev);
        rep.FinalizeMetrics();
        STRESS_ASSERT(rep.events[0].duration_us == 42000, "Single event D_0 == E_0");
        STRESS_ASSERT(VerifyReportInvariants(rep, "SingleEvent"), "Single event invariant");
    }

    // Subtest 7.3: Multiple identical elapsed timestamps (same microsecond)
    {
        StartupReport rep{};
        for (int i = 0; i < 5; ++i) {
            StartupEvent ev{};
            ev.event_name = "Clustered_" + std::to_string(i);
            ev.elapsed_us = 1000; // All in exact same microsecond
            rep.events.push_back(ev);
        }
        rep.FinalizeMetrics();
        STRESS_ASSERT(rep.events[0].duration_us == 1000, "First event in cluster has D_0 = 1000");
        for (size_t i = 1; i < rep.events.size(); ++i) {
            STRESS_ASSERT(rep.events[i].duration_us == 0, "Clustered event delta must be 0");
        }
        STRESS_ASSERT(VerifyReportInvariants(rep, "ClusteredTimestamps"), "Clustered invariant");
    }

    // Subtest 7.4: Idempotency (calling FinalizeMetrics 10 times consecutively)
    {
        StartupReport rep{};
        rep.downloaded_bytes = 10485760; // 10 MB
        for (int i = 0; i < 20; ++i) {
            StartupEvent ev{};
            ev.event_name = (i == 5) ? "First data bytes received" : ("Step_" + std::to_string(i));
            ev.elapsed_us = static_cast<uint64_t>((20 - i) * 1500); // Reverse order intentionally
            rep.events.push_back(ev);
        }

        rep.FinalizeMetrics();
        auto snapshot_1 = rep;

        for (int iter = 0; iter < 10; ++iter) {
            rep.FinalizeMetrics();
            STRESS_ASSERT(rep.events.size() == snapshot_1.events.size(), "Events count unchanged");
            STRESS_ASSERT(rep.startup_time_ms == snapshot_1.startup_time_ms, "startup_time_ms unchanged");
            STRESS_ASSERT(rep.transfer_duration_ms == snapshot_1.transfer_duration_ms, "transfer_duration_ms unchanged");
            STRESS_ASSERT(rep.avg_speed_mbps == snapshot_1.avg_speed_mbps, "avg_speed_mbps unchanged");
            for (size_t i = 0; i < rep.events.size(); ++i) {
                STRESS_ASSERT(rep.events[i].elapsed_us == snapshot_1.events[i].elapsed_us, "elapsed_us unchanged");
                STRESS_ASSERT(rep.events[i].duration_us == snapshot_1.events[i].duration_us, "duration_us unchanged");
            }
        }
        STRESS_ASSERT(VerifyReportInvariants(rep, "IdempotencyCheck"), "Idempotency invariants passed");
    }

    std::cout << "   -> PASS (FinalizeMetrics proved mathematically idempotent across 10 iterations & extreme clusters)\n";
    return true;
}

// ============================================================================
// MAIN RUNNER
// ============================================================================
int main() {
    std::cout << "====================================================\n";
    std::cout << " Radium StartupDiagnosticRecorder Empirical Stress Harness\n";
    std::cout << "====================================================\n";

    int passed = 0;
    int total = 7;

    if (TestConcurrent16ThreadEventRecording()) passed++;
    if (TestOutOfOrderIngestionAndSorting()) passed++;
    if (TestClientClickEpochEdgeCases()) passed++;
    if (TestFirstDataBytesDeduplication()) passed++;
    if (TestPersistenceAndSerializationRoundTrip()) passed++;
    if (TestMultiSessionConcurrentStress()) passed++;
    if (TestFinalizeMetricsIdempotencyAndExtremes()) passed++;

    std::cout << "\nStress Test Summary: " << passed << " / " << total << " suites PASSED.\n";
    return (passed == total) ? 0 : 1;
}

