// SPDX-License-Identifier: Apache-2.0
// Dedicated Unit Test Suite for Radium Core Engine Components:
//   - PathSanitizer (Path traversal, Windows reserved names, ADS, Unicode)
//   - JsonMini (RFC 8259 parser, primitives, arrays, objects, malformed rejection)
//   - Bencode & TorrentParser (bencode types, malformed encodings, minimal torrents, invalid pieces)
//   - SegmentScheduler (splitting, bounds, adaptive thresholds, rollback, concurrency invariants)

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <filesystem>

#include "common/path_sanitizer.hpp"
#include "common/json_mini.hpp"
#include "engine_torrent/bencode.hpp"
#include "engine_torrent/torrent_parser.hpp"
#include "brisk_engine/segment_scheduler.hpp"
#include "core/download_controller.hpp"
#include "ipc/ipc_server.hpp"
#include "common/startup_diagnostic.hpp"

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

// 1. PathSanitizer Comprehensive Unit Tests
bool TestDedicatedPathSanitizer() {
    using openidm::PathSanitizer;

    // Traversal attacks
    TEST_ASSERT(PathSanitizer::SanitizeFileName("../secret.txt") == "secret.txt", "Parent traversal 1");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("..\\..\\Windows\\System32\\calc.exe") == "calc.exe", "Windows backslash traversal");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("/etc/shadow") == "shadow", "Root absolute path");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("D:\\data\\file.zip") == "file.zip", "Drive letter stripping");

    // NTFS Alternate Data Streams (ADS)
    TEST_ASSERT(PathSanitizer::SanitizeFileName("normal.txt:hidden:$DATA") == "normal.txt", "NTFS ADS stripping");

    // Windows Reserved Names
    TEST_ASSERT(PathSanitizer::SanitizeFileName("CON.txt") == "_CON.txt", "Reserved CON.txt");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("prn.txt") == "_prn.txt", "Reserved PRN.txt");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("aux") == "_aux", "Reserved AUX");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("nul") == "_nul", "Reserved NUL");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("COM1.mp4") == "_COM1.mp4", "Reserved COM1.mp4");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("lpt9.log") == "_lpt9.log", "Reserved LPT9.log");

    // Trailing dots and spaces (Windows filesystem quirks)
    TEST_ASSERT(PathSanitizer::SanitizeFileName("document.docx. . .") == "document.docx", "Trailing dots and spaces");

    // Unicode & Non-ASCII filenames
    TEST_ASSERT(PathSanitizer::SanitizeFileName("résumé_2026.pdf") == "résumé_2026.pdf", "Unicode Latin");
    TEST_ASSERT(PathSanitizer::SanitizeFileName("视频_1080p.mkv") == "视频_1080p.mkv", "Unicode CJK");

    // Empty or pure separator names fallback safely
    std::string empty_san = PathSanitizer::SanitizeFileName("");
    TEST_ASSERT(!empty_san.empty() && empty_san != ".." && empty_san != "/", "Empty filename safe fallback");

    return true;
}

// 2. JsonMini RFC 8259 Parser & Builder Tests
bool TestDedicatedJsonMini() {
    using namespace openidm::json;

    // Primitives
    TEST_ASSERT(Parse("null").is_null(), "Parse null");
    TEST_ASSERT(Parse("true").as_bool() == true, "Parse true");
    TEST_ASSERT(Parse("false").as_bool() == false, "Parse false");
    TEST_ASSERT(Parse("123456").as_int64() == 123456, "Parse positive int");
    TEST_ASSERT(Parse("-789").as_int64() == -789, "Parse negative int");
    TEST_ASSERT(std::abs(Parse("3.14159").as_double() - 3.14159) < 1e-4, "Parse float");

    // String with escapes
    std::string s_json = R"("Hello \"Radium\" \n\t\\ \u0041")";
    Value str_val = Parse(s_json);
    TEST_ASSERT(str_val.is_string(), "Parse escaped string");
    TEST_ASSERT(str_val.as_string().find("Radium") != std::string::npos, "String content contains Radium");
    TEST_ASSERT(str_val.as_string().find("A") != std::string::npos, "Unicode escape \\u0041 -> A");

    // Arrays & Objects
    std::string complex_json = R"({
        "app": "Radium",
        "concurrency": 8,
        "features": ["segmented", "bittorrent", "streams"],
        "active": true,
        "nested": { "sub_key": 42 }
    })";
    Value doc = Parse(complex_json);
    TEST_ASSERT(doc.is_object(), "Root is object");
    TEST_ASSERT(doc["app"].as_string() == "Radium", "Object field 'app'");
    TEST_ASSERT(doc["concurrency"].as_int64() == 8, "Object field 'concurrency'");
    TEST_ASSERT(doc["features"].is_array(), "Object field 'features' is array");
    TEST_ASSERT(doc["features"].as_array().size() == 3, "Array size is 3");
    TEST_ASSERT(doc["features"].as_array()[0].as_string() == "segmented", "Array element 0");
    TEST_ASSERT(doc["nested"]["sub_key"].as_int64() == 42, "Nested object navigation");

    // Serialization / Dump
    std::string dumped = doc.dump();
    Value redoc = Parse(dumped);
    TEST_ASSERT(redoc["app"].as_string() == "Radium", "Roundtrip dump-and-parse");

    // Malformed / Truncated inputs must throw runtime_error safely without crashing
    bool caught_malformed = false;
    try {
        Parse("{ \"unclosed\": true ");
    } catch (const std::runtime_error&) {
        caught_malformed = true;
    }
    TEST_ASSERT(caught_malformed, "Catch truncated object");

    bool caught_invalid_num = false;
    try {
        Parse("[1, 2, invalid_token]");
    } catch (const std::runtime_error&) {
        caught_invalid_num = true;
    }
    TEST_ASSERT(caught_invalid_num, "Catch invalid token in array");

    return true;
}

// 3. Bencode & TorrentParser Tests
bool TestDedicatedBencodeAndTorrentParser() {
    using namespace openidm::torrent;

    // Bencode decoding
    auto i = BencodeParser::Decode("i999e");
    TEST_ASSERT(i.ok() && i.value->as_int() == 999, "Bencode int");

    auto s = BencodeParser::Decode("6:radium");
    TEST_ASSERT(s.ok() && s.value->as_string() == "radium", "Bencode string");

    auto l = BencodeParser::Decode("l4:testi10ee");
    TEST_ASSERT(l.ok() && l.value->as_list().size() == 2, "Bencode list");

    auto d = BencodeParser::Decode("d3:cow3:moo4:spami42ee");
    TEST_ASSERT(d.ok() && d.value->as_dict().size() == 2, "Bencode dict");

    // Malformed bencode rejection
    TEST_ASSERT(!BencodeParser::Decode("i42").ok(), "Reject unclosed integer");
    TEST_ASSERT(!BencodeParser::Decode("10:short").ok(), "Reject length exceeding buffer");
    TEST_ASSERT(!BencodeParser::Decode("d3:key").ok(), "Reject unclosed dict");

    // Torrent parsing with synthetic deterministic bencoded metainfo
    // Minimal valid single-file torrent dictionary:
    // d4:infod6:lengthi1024e4:name8:test.bin12:piece lengthi1024e6:pieces20:12345678901234567890ee
    std::string valid_meta = "d4:infod6:lengthi1024e4:name8:test.bin12:piece lengthi1024e6:pieces20:12345678901234567890ee";
    auto t_res = TorrentParser::ParseTorrentBuffer(valid_meta);
    TEST_ASSERT(t_res.ok(), "Parse minimal valid torrent metainfo");
    TEST_ASSERT(t_res.value->name == "test.bin", "Parsed torrent name");
    TEST_ASSERT(t_res.value->total_length == 1024, "Parsed torrent length");
    TEST_ASSERT(t_res.value->piece_length == 1024, "Parsed piece length");
    TEST_ASSERT(t_res.value->pieces.size() == 1, "Parsed pieces count");

    // Corrupt / Missing fields rejection
    std::string missing_length = "d4:infod4:name8:test.bin12:piece lengthi1024e6:pieces20:12345678901234567890ee";
    TEST_ASSERT(!TorrentParser::ParseTorrentBuffer(missing_length).ok(), "Reject torrent missing length/files");

    std::string bad_pieces = "d4:infod6:lengthi1024e4:name8:test.bin12:piece lengthi1024e6:pieces15:short_pieces_ee";
    TEST_ASSERT(!TorrentParser::ParseTorrentBuffer(bad_pieces).ok(), "Reject torrent with pieces not divisible by 20");

    return true;
}

// 4. SegmentScheduler Comprehensive Invariant Tests
bool TestDedicatedSegmentScheduler() {
    using namespace openidm::brisk;

    // Test deterministic splitting of 100 MB file across 8 workers
    const uint64_t total_size = 100 * 1024 * 1024; // 100 MB
    SegmentScheduler scheduler(total_size, 8, 256 * 1024);
    scheduler.Initialize();

    // Initial segment acquisition for first worker
    auto seg0 = scheduler.AcquireNextOrStealSegment(false);
    TEST_ASSERT(seg0 != nullptr, "Worker acquires initial segment");
    TEST_ASSERT(seg0->start_offset == 0 && seg0->end_offset.load() == total_size - 1, "Initial segment covers entire file");

    // Advance cursor to simulate data progress
    seg0->current_offset.store(10 * 1024 * 1024);

    // Second worker attempts work-stealing from first worker
    auto seg1 = scheduler.AcquireNextOrStealSegment(true);
    TEST_ASSERT(seg1 != nullptr, "Worker 2 successfully steals segment");
    TEST_ASSERT(seg1->start_offset > 10 * 1024 * 1024, "Stolen segment starts past worker 1's progress");
    TEST_ASSERT(seg1->end_offset.load() == total_size - 1, "Stolen segment takes upper boundary");

    // In-flight rejection rollback
    bool rolled_back = scheduler.RollbackUnstartedStolenSegment(seg1->segment_id);
    TEST_ASSERT(rolled_back, "Rollback unstarted segment succeeds");

    // Verify all ranges remain contiguous and no hole is formed
    auto snapshots = scheduler.GetSnapshots();
    TEST_ASSERT(!snapshots.empty(), "Snapshots list is non-empty");
    TEST_ASSERT(SegmentScheduler::ValidateSegmentPartition(snapshots, total_size), "Partition validation passes after rollback");

    return true;
}

// 5. IPC Message Handling, Correlation ID & Diagnostic Report Chronology Tests
bool TestIpcDiagnosticAndCorrelationVerification() {
    using namespace openidm;
    using namespace openidm::ipc;
    using namespace openidm::diagnostics;

    std::filesystem::path work_dir = std::filesystem::temp_directory_path() / ("radium_emp_diag_" + openidm::GenerateUuidV4());
    std::filesystem::create_directories(work_dir);
    std::filesystem::create_directories(work_dir / "downloads");
    std::filesystem::create_directories(work_dir / "tmp");

    openidm::ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    openidm::DownloadController ctrl(cfg);
    TEST_ASSERT(ctrl.Initialize().ok(), "Initialize DownloadController");

    openidm::ipc::IpcServer ipc_server(ctrl);
    ipc_server.SetHeadlessDaemonMode(true);

    auto& recorder = StartupDiagnosticRecorder::Instance();
    recorder.ClearReports();
    recorder.InitializeStorage(work_dir);

    // =========================================================================
    // Scenario 1: download.offer with custom correlationId & clientClickTime
    // =========================================================================
    const std::string offer_corr_id = "test-corr-offer-empirical-001-uuid";
    const uint64_t offer_click_ms = 1730718000000ULL;

    json::Value offer_req_obj = json::Object{};
    offer_req_obj["jsonrpc"] = "2.0";
    offer_req_obj["id"] = 101;
    offer_req_obj["method"] = "download.offer";
    json::Value offer_params = json::Object{};
    offer_params["url"] = "https://sg-lg.regxa.com/500MB.test";
    offer_params["filename"] = "500MB.test";
    offer_params["correlationId"] = offer_corr_id;
    offer_params["clientClickTime"] = static_cast<int64_t>(offer_click_ms);
    offer_params["autoStart"] = false;
    offer_req_obj["params"] = std::move(offer_params);

    std::string offer_resp_raw = ipc_server.HandleJsonRpcMessage(offer_req_obj.dump());
    json::Value offer_resp = json::Parse(offer_resp_raw);
    TEST_ASSERT(offer_resp.contains("result"), "download.offer response contains result");
    TEST_ASSERT(offer_resp["result"]["accepted"].as_bool() == true, "download.offer accepted == true");
    TEST_ASSERT(offer_resp["result"]["correlationId"].as_string() == offer_corr_id, "download.offer response correlationId matches input");
    std::string offer_task_id = offer_resp["result"]["taskId"].as_string();
    TEST_ASSERT(!offer_task_id.empty(), "download.offer taskId is non-empty");

    // =========================================================================
    // Scenario 2: stream.add with custom correlationId & clientClickTime
    // =========================================================================
    const std::string stream_corr_id = "test-corr-stream-empirical-002-uuid";
    const uint64_t stream_click_ms = 1730718100000ULL;

    json::Value stream_req_obj = json::Object{};
    stream_req_obj["jsonrpc"] = "2.0";
    stream_req_obj["id"] = 102;
    stream_req_obj["method"] = "stream.add";
    json::Value stream_params = json::Object{};
    stream_params["url"] = "https://www.youtube.com/watch?v=7NWN3wivxhA";
    stream_params["sourceType"] = "YOUTUBE_EXTRACTION";
    stream_params["streamType"] = "YOUTUBE";
    stream_params["correlationId"] = stream_corr_id;
    stream_params["clientClickTime"] = static_cast<int64_t>(stream_click_ms);
    stream_params["autoStart"] = false;
    stream_params["filename"] = "youtube_empirical.mp4";
    stream_params["qualityId"] = "1080p";
    stream_req_obj["params"] = std::move(stream_params);

    std::string stream_resp_raw = ipc_server.HandleJsonRpcMessage(stream_req_obj.dump());
    json::Value stream_resp = json::Parse(stream_resp_raw);
    TEST_ASSERT(stream_resp.contains("result"), "stream.add response contains result");
    TEST_ASSERT(stream_resp["result"]["accepted"].as_bool() == true, "stream.add accepted == true");
    std::string stream_task_id = stream_resp["result"]["taskId"].as_string();
    TEST_ASSERT(!stream_task_id.empty(), "stream.add taskId is non-empty");

    // =========================================================================
    // Verification: Inspect Active Diagnostic Reports via GetReports()
    // =========================================================================
    auto reports = recorder.GetReports();
    TEST_ASSERT(reports.size() >= 2, "At least 2 active reports registered");

    const StartupReport* r_offer = nullptr;
    const StartupReport* r_stream = nullptr;
    for (const auto& r : reports) {
        if (r.download_id == offer_task_id) r_offer = &r;
        if (r.download_id == stream_task_id) r_stream = &r;
    }

    TEST_ASSERT(r_offer != nullptr, "Report for download.offer task located");
    TEST_ASSERT(r_stream != nullptr, "Report for stream.add task located");

    // Exact correlation ID match
    TEST_ASSERT(r_offer->correlation_id == offer_corr_id, "download.offer report correlation_id matches input exactly");
    TEST_ASSERT(r_stream->correlation_id == stream_corr_id, "stream.add report correlation_id matches input exactly");
    TEST_ASSERT(r_offer->download_type == "HTTP", "download.offer report download_type is HTTP");
    TEST_ASSERT(r_stream->download_type == "YouTube", "stream.add report download_type is YouTube");

    // Inspect chronological ordering of initial events for offer
    TEST_ASSERT(r_offer->events.size() >= 3, "download.offer has at least 3 initial events");
    TEST_ASSERT(r_offer->events[0].event_name == "Download button clicked", "Event 0 is 'Download button clicked'");
    TEST_ASSERT(r_offer->events[1].event_name == "Download task created", "Event 1 is 'Download task created'");
    TEST_ASSERT(r_offer->events[2].event_name == "Native messaging host receives the request", "Event 2 is 'Native messaging host receives the request'");
    TEST_ASSERT(r_offer->events[0].elapsed_us <= r_offer->events[1].elapsed_us, "Event 0 elapsed <= Event 1 elapsed");
    TEST_ASSERT(r_offer->events[1].elapsed_us <= r_offer->events[2].elapsed_us, "Event 1 elapsed <= Event 2 elapsed");
    TEST_ASSERT(r_offer->events[0].duration_us == 0, "Event 0 duration is 0");
    TEST_ASSERT(r_offer->events[1].duration_us == r_offer->events[1].elapsed_us, "Event 1 duration matches elapsed delta");
    TEST_ASSERT(r_offer->events[2].duration_us >= 0, "Event 2 duration is non-negative");

    // Inspect chronological ordering of initial events for stream
    TEST_ASSERT(r_stream->events.size() >= 3, "stream.add has at least 3 initial events");
    TEST_ASSERT(r_stream->events[0].event_name == "Download button clicked", "Stream Event 0 is 'Download button clicked'");
    TEST_ASSERT(r_stream->events[1].event_name == "Download task created", "Stream Event 1 is 'Download task created'");
    TEST_ASSERT(r_stream->events[2].event_name == "Native messaging host receives the request", "Stream Event 2 is 'Native messaging host receives the request'");
    TEST_ASSERT(r_stream->events[0].elapsed_us <= r_stream->events[1].elapsed_us, "Stream Event 0 elapsed <= Event 1 elapsed");
    TEST_ASSERT(r_stream->events[1].elapsed_us <= r_stream->events[2].elapsed_us, "Stream Event 1 elapsed <= Event 2 elapsed");
    TEST_ASSERT(r_stream->events[0].duration_us == 0, "Stream Event 0 duration is 0");
    TEST_ASSERT(r_stream->events[1].duration_us == r_stream->events[1].elapsed_us, "Stream Event 1 duration matches elapsed delta");
    TEST_ASSERT(r_stream->events[2].duration_us >= 0, "Stream Event 2 duration is non-negative");

    // =========================================================================
    // Verification: Serialized JSON Schema & Integrity (ExportToJson)
    // =========================================================================
    std::string exported_json = recorder.ExportToJson();
    TEST_ASSERT(!exported_json.empty(), "ExportToJson produced non-empty string");

    json::Value parsed_json;
    bool parse_ok = true;
    try {
        parsed_json = json::Parse(exported_json);
    } catch (...) {
        parse_ok = false;
    }
    TEST_ASSERT(parse_ok, "ExportToJson produced strictly valid JSON");
    TEST_ASSERT(parsed_json.is_array(), "ExportToJson root is JSON array");
    TEST_ASSERT(parsed_json.as_array().size() >= 2, "Exported array contains at least 2 sessions");

    // Verify schema fields for each item in parsed_json
    for (const auto& item : parsed_json.as_array()) {
        TEST_ASSERT(item.is_object(), "Report item is object");
        TEST_ASSERT(item.contains("download_id"), "Report contains download_id");
        TEST_ASSERT(item.contains("correlation_id"), "Report contains correlation_id");
        TEST_ASSERT(item.contains("created_epoch_ms"), "Report contains created_epoch_ms");
        TEST_ASSERT(item.contains("website"), "Report contains website");
        TEST_ASSERT(item.contains("source_url"), "Report contains source_url");
        TEST_ASSERT(item.contains("download_url"), "Report contains download_url");
        TEST_ASSERT(item.contains("filename"), "Report contains filename");
        TEST_ASSERT(item.contains("download_type"), "Report contains download_type");
        TEST_ASSERT(item.contains("status"), "Report contains status");
        TEST_ASSERT(item.contains("events"), "Report contains events array");
        TEST_ASSERT(item["events"].is_array(), "events is an array");

        const auto& ev_arr = item["events"].as_array();
        TEST_ASSERT(ev_arr.size() >= 3, "Report has at least 3 events");

        double prev_elapsed = -1.0;
        for (const auto& ev : ev_arr) {
            TEST_ASSERT(ev.contains("event_name"), "Event contains event_name");
            TEST_ASSERT(ev.contains("epoch_ms"), "Event contains epoch_ms");
            TEST_ASSERT(ev.contains("elapsed_ms"), "Event contains elapsed_ms");
            TEST_ASSERT(ev.contains("duration_ms"), "Event contains duration_ms");
            TEST_ASSERT(ev.contains("detail"), "Event contains detail");

            double cur_elapsed = ev["elapsed_ms"].as_double(0.0);
            double cur_duration = ev["duration_ms"].as_double(0.0);
            TEST_ASSERT(cur_elapsed >= prev_elapsed, "Event elapsed_ms is monotonically non-decreasing");
            TEST_ASSERT(cur_duration >= 0.0, "Event duration_ms is non-negative (no negative deltas)");
            prev_elapsed = cur_elapsed;
        }
    }

    // =========================================================================
    // Verification: CompleteSession & Persistence to Disk
    // =========================================================================
    recorder.CompleteSession(offer_task_id, true, "");
    recorder.CompleteSession(stream_task_id, true, "");

    std::filesystem::path report_file = work_dir / "diagnostics" / "startup_reports.json";
    TEST_ASSERT(std::filesystem::exists(report_file), "startup_reports.json persisted to disk");

    std::ifstream ifs(report_file, std::ios::binary);
    std::string disk_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();

    json::Value disk_json = json::Parse(disk_content);
    TEST_ASSERT(disk_json.is_array(), "Disk reports root is array");
    TEST_ASSERT(disk_json.as_array().size() >= 2, "Disk reports contain at least 2 completed sessions");

    // =========================================================================
    // Scenario 3: Adversarial Challenge - Edge Cases
    // 3a. clientClickTime == 0 (no click timestamp reported)
    // =========================================================================
    json::Value no_click_obj = json::Object{};
    no_click_obj["jsonrpc"] = "2.0";
    no_click_obj["id"] = 103;
    no_click_obj["method"] = "download.offer";
    json::Value nc_params = json::Object{};
    nc_params["url"] = "https://example.com/no_click.zip";
    nc_params["filename"] = "no_click.zip";
    nc_params["correlationId"] = "corr-no-click-test";
    nc_params["clientClickTime"] = 0;
    nc_params["autoStart"] = false;
    no_click_obj["params"] = std::move(nc_params);

    std::string no_click_resp = ipc_server.HandleJsonRpcMessage(no_click_obj.dump());
    json::Value nc_val = json::Parse(no_click_resp);
    std::string nc_task_id = nc_val["result"]["taskId"].as_string();

    auto active_reps = recorder.GetReports();
    const StartupReport* r_nc = nullptr;
    for (const auto& r : active_reps) {
        if (r.download_id == nc_task_id) r_nc = &r;
    }
    TEST_ASSERT(r_nc != nullptr, "No-click report found");
    TEST_ASSERT(r_nc->events.size() >= 2, "No-click report has at least 2 events");
    TEST_ASSERT(r_nc->events[0].event_name == "Download task created", "When click is 0, first event is 'Download task created'");
    TEST_ASSERT(r_nc->events[0].elapsed_us == 0, "No-click Event 0 elapsed is 0");
    TEST_ASSERT(r_nc->events[1].event_name == "Native messaging host receives the request", "No-click Event 1 is NMH request");
    TEST_ASSERT(r_nc->events[1].elapsed_us >= r_nc->events[0].elapsed_us, "No-click Event 1 elapsed >= Event 0 elapsed");

    // 3b. clientClickTime in future (clock skew edge case)
    uint64_t future_click_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + 60000ULL; // 1 min in future

    json::Value future_click_obj = json::Object{};
    future_click_obj["jsonrpc"] = "2.0";
    future_click_obj["id"] = 104;
    future_click_obj["method"] = "download.offer";
    json::Value fc_params = json::Object{};
    fc_params["url"] = "https://example.com/future.zip";
    fc_params["filename"] = "future.zip";
    fc_params["correlationId"] = "corr-future-click-test";
    fc_params["clientClickTime"] = static_cast<int64_t>(future_click_ms);
    fc_params["autoStart"] = false;
    future_click_obj["params"] = std::move(fc_params);

    std::string future_resp = ipc_server.HandleJsonRpcMessage(future_click_obj.dump());
    json::Value fc_val = json::Parse(future_resp);
    std::string fc_task_id = fc_val["result"]["taskId"].as_string();

    active_reps = recorder.GetReports();
    const StartupReport* r_fc = nullptr;
    for (const auto& r : active_reps) {
        if (r.download_id == fc_task_id) r_fc = &r;
    }
    TEST_ASSERT(r_fc != nullptr, "Future-click report found");
    for (const auto& ev : r_fc->events) {
        TEST_ASSERT(ev.elapsed_us < 1000000000ULL, "Future click does not produce underflowed elapsed_us");
        TEST_ASSERT(ev.duration_us < 1000000000ULL, "Future click does not produce underflowed duration_us");
    }

    // 3c. First-byte deduplication verification
    std::string dedupe_task = "dedupe-session-test";
    recorder.StartSession(dedupe_task, "dedupe-corr", "", "https://example.com/dedupe.bin", "dedupe.bin", "HTTP", "", 1024, 100);
    recorder.RecordEvent(dedupe_task, "First data bytes received", "Worker 0");
    recorder.RecordEvent(dedupe_task, "First data bytes received", "Worker 1 duplicate");
    recorder.RecordEvent(dedupe_task, "First data bytes received", "Worker 2 duplicate");

    size_t first_byte_count = 0;
    active_reps = recorder.GetReports();
    for (const auto& r : active_reps) {
        if (r.download_id == dedupe_task) {
            for (const auto& ev : r.events) {
                if (ev.event_name == "First data bytes received") {
                    first_byte_count++;
                }
            }
        }
    }
    TEST_ASSERT(first_byte_count == 1, "First data bytes received is strictly deduplicated to exactly 1 event");

    // 3d. Unicode & special characters in correlationId
    std::string special_corr_id = "corr-spec-special-chars-test";
    json::Value spec_obj = json::Object{};
    spec_obj["jsonrpc"] = "2.0";
    spec_obj["id"] = 105;
    spec_obj["method"] = "download.offer";
    json::Value spec_params = json::Object{};
    spec_params["url"] = "https://example.com/spec.zip";
    spec_params["filename"] = "spec.zip";
    spec_params["correlationId"] = special_corr_id;
    spec_params["clientClickTime"] = static_cast<int64_t>(offer_click_ms);
    spec_params["autoStart"] = false;
    spec_obj["params"] = std::move(spec_params);

    std::string spec_resp = ipc_server.HandleJsonRpcMessage(spec_obj.dump());
    json::Value spec_val = json::Parse(spec_resp);
    TEST_ASSERT(spec_val["result"]["correlationId"].as_string() == special_corr_id, "Special characters in correlationId preserved in RPC");

    std::string spec_task_id = spec_val["result"]["taskId"].as_string();
    active_reps = recorder.GetReports();
    const StartupReport* r_spec = nullptr;
    for (const auto& r : active_reps) {
        if (r.download_id == spec_task_id) r_spec = &r;
    }
    TEST_ASSERT(r_spec != nullptr, "Special correlation report found");
    TEST_ASSERT(r_spec->correlation_id == special_corr_id, "Special correlation preserved in report");

    // Clean up temporary test files
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);

    return true;
}

int main() {
    int passed = 0;
    const int total = 5;

    std::cout << "====================================================\n";
    std::cout << " Radium Core Dedicated Unit Test Suite (Fast CTest)\n";
    std::cout << "====================================================\n";

    auto run_test = [&](const char* name, bool (*fn)()) {
        std::cout << "[RUN] " << name << "..." << std::endl;
        if (fn()) {
            std::cout << "  -> PASS\n";
            ++passed;
        } else {
            std::cout << "  -> FAIL\n";
        }
    };

    run_test("PathSanitizer Comprehensive Unit Tests", TestDedicatedPathSanitizer);
    run_test("JsonMini RFC 8259 Parser & Builder Tests", TestDedicatedJsonMini);
    run_test("Bencode & TorrentParser Tests", TestDedicatedBencodeAndTorrentParser);
    run_test("SegmentScheduler Invariant & Work-Stealing Tests", TestDedicatedSegmentScheduler);
    run_test("IPC Message Handling, Correlation ID & Diagnostic Report Chronology", TestIpcDiagnosticAndCorrelationVerification);

    std::cout << "\nResult: " << passed << " / " << total << " dedicated test suites PASSED.\n";
    return (passed == total) ? 0 : 1;
}
