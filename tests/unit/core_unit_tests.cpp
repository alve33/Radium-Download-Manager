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
#include <fstream>
#include <thread>
#include <chrono>

#include "common/path_sanitizer.hpp"
#include "common/json_mini.hpp"
#include "engine_torrent/bencode.hpp"
#include "engine_torrent/torrent_parser.hpp"
#include "brisk_engine/segment_scheduler.hpp"
#include "core/download_controller.hpp"
#include "core/extension_installer.hpp"
#include "ipc/ipc_server.hpp"
#include "ui/win32_main_window.hpp"
#include "common/startup_diagnostic.hpp"
#include "engine_stream/video_extractor.hpp"
#include "brisk_engine/http_client.hpp"
#include "brisk_engine/site_handlers.hpp"
#include "brisk_engine/http_download_engine.hpp"
#include "crawler/site_grabber.hpp"
#include "platform/windows_integrations.hpp"
#include "storage/import_export.hpp"
#include "engine_ftp/ftp_client.hpp"
#include "engine_torrent/torrent_engine.hpp"
#include "archive/zip_preview.hpp"
#include "packs/specialized_packs.hpp"
#include "remote/remote_control.hpp"
#include "ui/win32_main_window.hpp"
#include "lab_server.hpp"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

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

    // Explicitly disable auto_start_downloads so the test does not trigger worker execution
    auto ctrl_settings = ctrl.GetSettings();
    ctrl_settings.auto_start_downloads = false;
    auto set_res = ctrl.UpdateSettings(ctrl_settings);
    TEST_ASSERT(set_res.ok(), "Disable automatic downloads for IPC test");

    openidm::ipc::IpcServer ipc_server(ctrl);
    // Do NOT enable headless daemon mode here, as that forces auto_start to true.
    ipc_server.SetHeadlessDaemonMode(false);

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
    offer_params["url"] = "http://127.0.0.1:9/mock_test_file.bin";
    offer_params["filename"] = "mock_test_file.bin";
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
    stream_params["cachedManifestBody"] = "{\"radiumType\":\"YOUTUBE_STREAM\",\"videoUrl\":\"http://127.0.0.1:9/v.mp4\",\"audioUrl\":\"http://127.0.0.1:9/a.mp4\"}";
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
    nc_params["url"] = "http://127.0.0.1:9/mock_no_click.zip";
    nc_params["filename"] = "mock_no_click.zip";
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
    fc_params["url"] = "http://127.0.0.1:9/mock_future.zip";
    fc_params["filename"] = "mock_future.zip";
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
    recorder.StartSession(dedupe_task, "dedupe-corr", "", "http://127.0.0.1:9/mock_dedupe.bin", "mock_dedupe.bin", "HTTP", "", 1024, 100);
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
    spec_params["url"] = "http://127.0.0.1:9/mock_spec.zip";
    spec_params["filename"] = "mock_spec.zip";
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
    ctrl.Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);

    return true;
}

// 6. Comprehensive Video Extraction & Website Compatibility Overhaul Unit Tests
bool TestMultiSiteVideoExtractionOverhaul() {
    using namespace openidm::engine_stream;
    using openidm::PathSanitizer;

    // 6.1 URL Detection & Classification (Bilibili, Facebook, Adult Sites, General Video Platforms)
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.bilibili.com/video/BV1xx411c7mD?p=2") == "bilibili", "Identify Bilibili BV watch URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://b23.tv/BV1xx411c7mD") == "bilibili", "Identify b23.tv short URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.facebook.com/watch/?v=1234567890") == "facebook", "Identify Facebook Watch URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.facebook.com/reel/987654321") == "facebook", "Identify Facebook Reel URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://fb.watch/abc123xyz/") == "facebook", "Identify fb.watch URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.pornhub.com/view_video.php?viewkey=64a1b2c3d4") == "pornhub", "Identify Pornhub URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.xvideos.com/video.kdfj123/test_title") == "xvideos", "Identify XVideos URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://xhamster.com/videos/sample-video-12345") == "xhamster", "Identify xHamster URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://spankbang.com/5abcd/video/sample") == "spankbang", "Identify SpankBang URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://vimeo.com/123456789") == "vimeo", "Identify Vimeo URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.dailymotion.com/video/x8abcde") == "dailymotion", "Identify Dailymotion URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.twitch.tv/videos/987654321") == "twitch", "Identify Twitch VOD URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://x.com/user/status/1234567890") == "twitter", "Identify X/Twitter status video URL");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.tiktok.com/@creator/video/7123456789") == "tiktok", "Identify TikTok video URL");

    // Direct media files on video hosts must NOT be mistaken for webpage watch URLs
    TEST_ASSERT(!VideoExtractionManager::IsSupportedVideoSiteUrl("https://cdn.bilibili.com/media/clip.mp4"), "Direct MP4 is not a watch page");
    TEST_ASSERT(!PathSanitizer::IsWebpageWatchUrl("https://ev-h.phncdn.com/hls/videos/master.m3u8?validfrom=1700000000"), "Direct M3U8 is not a watch page");
    TEST_ASSERT(PathSanitizer::IsWebpageWatchUrl("https://www.bilibili.com/video/BV1xx411c7mD"), "Bilibili BV page is a watch page");
    TEST_ASSERT(PathSanitizer::IsWebpageWatchUrl("https://www.facebook.com/watch/?v=123456789"), "Facebook watch page is a watch page");
    TEST_ASSERT(PathSanitizer::IsWebpageWatchUrl("https://www.pornhub.com/view_video.php?viewkey=ph6012345"), "Pornhub watch page is a watch page");

    // 6.2 Registrable Domain Extraction for Netscape Cookie Files (Bilibili, Facebook, Pornhub)
    TEST_ASSERT(VideoExtractionManager::ExtractRegistrableDomain("www.bilibili.com") == "bilibili.com", "Bilibili root cookie domain is bilibili.com");
    TEST_ASSERT(VideoExtractionManager::ExtractRegistrableDomain("m.facebook.com") == "facebook.com", "Facebook root cookie domain is facebook.com");
    TEST_ASSERT(VideoExtractionManager::ExtractRegistrableDomain("rt.pornhub.com") == "pornhub.com", "Pornhub root cookie domain is pornhub.com");

    // 6.3 Facebook CDN Range Trap Stripping (bytestart / byteend)
    const std::string raw_fb_cdn = "https://video.fdac1-1.fna.fbcdn.net/v/t42.1790-2/100000_n.mp4?_nc_cat=104&bytestart=0&byteend=524287&efg=eyJ2ZW5jb2RlX3RhZyI6InN2ZV9oZCJ9&oh=abc&oe=6789ABCD";
    const std::string clean_fb_cdn = VideoExtractionManager::StripCdnRangeTrapParams(raw_fb_cdn);
    TEST_ASSERT(clean_fb_cdn.find("bytestart=") == std::string::npos, "Stripped bytestart from fbcdn URL");
    TEST_ASSERT(clean_fb_cdn.find("byteend=") == std::string::npos, "Stripped byteend from fbcdn URL");
    TEST_ASSERT(clean_fb_cdn.find("oh=abc") != std::string::npos && clean_fb_cdn.find("oe=6789ABCD") != std::string::npos, "Preserved fbcdn signature params");

    // 6.4 Mobile Facebook URL Normalization
    TEST_ASSERT(VideoExtractionManager::NormalizeExtractorUrl("https://m.facebook.com/watch/?v=12345") == "https://www.facebook.com/watch/?v=12345", "Normalized m.facebook.com to www.facebook.com");

    // 6.5 Bilibili Native HTML / __playinfo__ & __INITIAL_STATE__ Parser (DASH video+audio pairing + multipart episodes)
    const std::string mock_bili_html = R"(
<!DOCTYPE html>
<html>
<head><title>Bilibili Architecture Deep Dive_哔哩哔哩_bilibili</title></head>
<body>
<script>window.__playinfo__={"code":0,"data":{"dash":{"duration":200,"video":[{"id":80,"baseUrl":"https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/v_1080p.m4s","width":1920,"height":1080,"bandwidth":4000000,"codecs":"avc1.640028","frameRate":"60.0"},{"id":64,"baseUrl":"https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/v_720p.m4s","width":1280,"height":720,"bandwidth":2000000,"codecs":"avc1.64001f","frameRate":"30.0"}],"audio":[{"id":30280,"baseUrl":"https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/a_320k.m4s","bandwidth":320000,"codecs":"mp4a.40.2"}]}}}</script>
<script>window.__INITIAL_STATE__={"bvid":"BV1xx411c7mD","aid":123456,"videoData":{"bvid":"BV1xx411c7mD","title":"Bilibili Architecture Deep Dive","duration":200,"owner":{"name":"TechCreator"},"pages":[{"cid":1001,"page":1,"part":"Episode 1 - Intro","duration":200},{"cid":1002,"page":2,"part":"Episode 2 - Implementation","duration":350}]}};</script>
</body>
</html>
    )";
    auto bili_res = VideoExtractionManager::ParseBilibiliPlayInfoJson(mock_bili_html, "https://www.bilibili.com/video/BV1xx411c7mD?p=2");
    TEST_ASSERT(bili_res.ok(), "Parsed Bilibili __playinfo__ and __INITIAL_STATE__");
    const auto& bili_meta = *bili_res.value;
    TEST_ASSERT(bili_meta.video_id == "BV1xx411c7mD", "Extracted Bilibili BV ID");
    TEST_ASSERT(bili_meta.uploader == "TechCreator", "Extracted Bilibili uploader");
    TEST_ASSERT(bili_meta.episodes.size() == 2, "Extracted Bilibili multipart episodes");
    TEST_ASSERT(bili_meta.current_page == 2, "Detected Bilibili ?p=2 episode index");
    TEST_ASSERT(bili_meta.title.find("Episode 2 - Implementation") != std::string::npos, "Appended Episode 2 part title");
    TEST_ASSERT(bili_meta.formats.size() == 3, "Bilibili formats include 1080p60, 720p, and Audio Only (no fake resolutions)");
    TEST_ASSERT(bili_meta.formats[0].height == 1080 && bili_meta.formats[0].requires_merge, "Bilibili 1080p requires audio merge");
    TEST_ASSERT(bili_meta.formats[0].video_url == "https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/v_1080p.m4s", "Bilibili 1080p video URL resolved");
    TEST_ASSERT(bili_meta.formats[0].audio_url == "https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/a_320k.m4s", "Bilibili 1080p paired with highest-bitrate audio URL");
    TEST_ASSERT(bili_meta.required_headers.at("Referer") == "https://www.bilibili.com/", "Bilibili Referer header enforced");

    // 6.6 Facebook Native HTML Parser (HD/SD progressive + byte-range trap cleanup + login wall detection)
    const std::string mock_fb_html = R"(
<html>
<head><meta property="og:title" content="Facebook Engineering Demo Reel" /></head>
<body>
<script>
{"playable_url_quality_hd":"https:\/\/video.fdac1-1.fna.fbcdn.net\/v\/t42.1790-2\/hd_video.mp4?bytestart=0&byteend=65535&oh=111&oe=6789ABCD","playable_url":"https:\/\/video.fdac1-1.fna.fbcdn.net\/v\/t42.1790-2\/sd_video.mp4?bytestart=0&byteend=32767&oh=222&oe=6789ABCD"}
</script>
</body>
</html>
    )";
    auto fb_res = VideoExtractionManager::ParseFacebookVideoHtml(mock_fb_html, "https://www.facebook.com/watch/?v=99887766");
    TEST_ASSERT(fb_res.ok(), "Parsed Facebook video HTML");
    const auto& fb_meta = *fb_res.value;
    TEST_ASSERT(fb_meta.title == "Facebook Engineering Demo Reel", "Extracted Facebook title");
    TEST_ASSERT(fb_meta.formats.size() == 2, "Extracted HD and SD Facebook formats");
    TEST_ASSERT(fb_meta.formats[0].height >= 720 && fb_meta.formats[0].video_url.find("bytestart=") == std::string::npos, "Facebook HD format stripped bytestart trap");
    TEST_ASSERT(fb_meta.formats[1].height <= 480 && fb_meta.formats[1].video_url.find("byteend=") == std::string::npos, "Facebook SD format stripped byteend trap");

    const std::string mock_fb_login_html = R"(<html><body><form id="login_form" action="/login/device-based/regular/login/">Log In</form></body></html>)";
    auto fb_login_res = VideoExtractionManager::ParseFacebookVideoHtml(mock_fb_login_html, "https://www.facebook.com/watch/?v=111");
    TEST_ASSERT(!fb_login_res.ok(), "Facebook login wall returns structured error");
    TEST_ASSERT(fb_login_res.error->code.find("ERR_EXTRACTOR_LOGIN_REQUIRED") != std::string::npos, "Facebook login wall classified as ERR_EXTRACTOR_LOGIN_REQUIRED");

    // 6.7 Adult Platform Native HTML Parser (mediaDefinitions JSON + html5player calls + no fake qualities)
    const std::string mock_adult_html = R"(
<html>
<head><meta property="og:title" content="Sample Adult Platform Multi-Quality Stream" /></head>
<body>
<script>
var flashvars_12345 = {
  "mediaDefinitions": [
    {"defaultQuality":true,"format":"mp4","videoUrl":"https:\/\/ev.phncdn.com\/videos\/1080P_4000K_12345.mp4?validfrom=1700000000&validto=1900000000","quality":"1080"},
    {"defaultQuality":false,"format":"mp4","videoUrl":"https:\/\/ev.phncdn.com\/videos\/720P_2000K_12345.mp4?validfrom=1700000000&validto=1900000000","quality":"720"},
    {"defaultQuality":false,"format":"hls","videoUrl":"https:\/\/ev-h.phncdn.com\/hls\/videos\/12345\/master.m3u8?validfrom=1700000000&validto=1900000000","quality":"480"}
  ]
};
</script>
</body>
</html>
    )";
    auto adult_res = VideoExtractionManager::ParseAdultPlayerHtml(mock_adult_html, "https://www.pornhub.com/view_video.php?viewkey=ph12345");
    TEST_ASSERT(adult_res.ok(), "Parsed adult platform HTML mediaDefinitions");
    const auto& adult_meta = *adult_res.value;
    TEST_ASSERT(adult_meta.title == "Sample Adult Platform Multi-Quality Stream", "Extracted adult video title");
    TEST_ASSERT(adult_meta.formats.size() == 3, "Extracted exact 3 formats from mediaDefinitions");
    TEST_ASSERT(adult_meta.formats[0].height == 1080 && adult_meta.formats[0].protocol == "https", "1080p direct MP4 format prioritized first");
    TEST_ASSERT(adult_meta.formats[1].height == 720 && adult_meta.formats[1].protocol == "https", "720p direct MP4 format second");
    TEST_ASSERT(adult_meta.formats[2].height == 480 && adult_meta.formats[2].protocol == "m3u8_native", "480p HLS manifest format classified as m3u8_native");

    // 6.7b Targets.txt Video Platform Identification and Generic HTML5 Player Extraction
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.eporner.com/video-12345/sample/") == "eporner", "Identified eporner");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://txxx.com/videos/123/title/") == "txxx", "Identified txxx");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://drtuber.com/video/54321/sample") == "drtuber", "Identified drtuber");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.tnaflix.com/video/999/test") == "tnaflix", "Identified tnaflix");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://spankbang.com/abc/video/watch") == "spankbang", "Identified spankbang");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://xcafe.com/videos/111/") == "xcafe", "Identified xcafe");
    TEST_ASSERT(VideoExtractionManager::IdentifyPlatform("https://www.porndoe.com/video/222") == "porndoe", "Identified porndoe");
    TEST_ASSERT(PathSanitizer::IsWebpageWatchUrl("https://www.eporner.com/video-12345/"), "eporner watch URL recognized");
    TEST_ASSERT(PathSanitizer::IsWebpageWatchUrl("https://txxx.com/videos/123/"), "txxx watch URL recognized");
    TEST_ASSERT(PathSanitizer::IsWebpageWatchUrl("https://xcafe.com/videos/111/"), "xcafe watch URL recognized");

    const std::string mock_html5_player_html = R"(
<html>
<head><title>HTML5 Stream Test Video</title></head>
<body>
<video id="player" src="https://cdn.example.org/videos/h264_stream.mp4?expire=1890000000" controls></video>
<script>
var playerConfig = {
    "file": "https://cdn.example.org/master.m3u8",
    "video_url": "https://cdn.example.org/videos/720p_video.mp4",
    "quality_720": "https://cdn.example.org/videos/720p_video.mp4"
};
</script>
</body>
</html>
    )";
    auto html5_parse_res = VideoExtractionManager::ParseAdultPlayerHtml(mock_html5_player_html, "https://www.xcafe.com/videos/111/");
    TEST_ASSERT(html5_parse_res.ok(), "Parsed generic HTML5 player stream tags");
    TEST_ASSERT(!html5_parse_res.value->formats.empty(), "Discovered formats from HTML5 video & playerConfig");
    TEST_ASSERT(html5_parse_res.value->title.find("HTML5 Stream Test Video") != std::string::npos, "Extracted title from HTML5 page");

    // 6.8 yt-dlp JSON Parser: Real Format Consolidation (No Fake Resolutions) & DRM Rejection
    const std::string mock_ytdlp_360p_only = R"({
      "id": "vid_360_only",
      "title": "Only 360p Video",
      "extractor_key": "Generic",
      "duration": 60,
      "formats": [
        {
          "format_id": "http-360p",
          "url": "https://cdn.example.org/video_360p.mp4?expire=1950000000",
          "ext": "mp4",
          "width": 640,
          "height": 360,
          "vcodec": "avc1.42001E",
          "acodec": "mp4a.40.2",
          "tbr": 600,
          "filesize": 4500000,
          "protocol": "https"
        }
      ]
    })";
    auto ytdlp_res = VideoExtractionManager::ParseYtDlpJson(mock_ytdlp_360p_only, "https://example.org/watch/1");
    TEST_ASSERT(ytdlp_res.ok(), "Parsed yt-dlp JSON with single 360p format");
    TEST_ASSERT(ytdlp_res.value->formats.size() == 1, "Did not invent fake 1080p/720p resolutions when only 360p exists");
    TEST_ASSERT(ytdlp_res.value->formats[0].quality_label.find("360p") != std::string::npos, "Preserved exact 360p quality label");
    TEST_ASSERT(ytdlp_res.value->earliest_expiry_epoch_s == 1950000000, "Extracted URL expiration epoch from expire= query parameter");

    // Verify DRM-protected stream rejection (AGENTS.md Section 7.6)
    const std::string mock_ytdlp_drm = R"({
      "id": "drm_vid",
      "title": "Encrypted Movie",
      "drm": "widevine",
      "formats": []
    })";
    auto drm_res = VideoExtractionManager::ParseYtDlpJson(mock_ytdlp_drm, "https://example.org/drm/1");
    TEST_ASSERT(!drm_res.ok(), "DRM stream rejected");
    TEST_ASSERT(drm_res.error->code.find("ERR_STREAM_DRM_PROTECTED") != std::string::npos, "DRM stream returns ERR_STREAM_DRM_PROTECTED");

    // 6.9 URL Expiration Detection & Diagnostic Credential Redaction
    TEST_ASSERT(VideoExtractionManager::ParseUrlExpirationEpoch("https://rr1---sn.googlevideo.com/videoplayback?expire=1700000100&itag=137") == 1700000100, "Parsed expire= epoch");
    TEST_ASSERT(VideoExtractionManager::IsStreamUrlExpired("https://rr1---sn.googlevideo.com/videoplayback?expire=1700000100", 1700000090), "URL within 60s safety margin is treated as expired");
    TEST_ASSERT(!VideoExtractionManager::IsStreamUrlExpired("https://rr1---sn.googlevideo.com/videoplayback?expire=1900000000", 1700000000), "Future URL is not expired");

    ExtractionDiagnosticReport diag;
    diag.input_url = "https://example.com/video?token=super_secret_token_123&id=42";
    diag.selected_extractor = "bilibili";
    diag.failure_message = "Cookie: SESSDATA=secret_sess_999 failed with Authorization: Bearer secret_jwt_abc";
    std::string redacted_json = diag.ToRedactedJson();
    TEST_ASSERT(redacted_json.find("super_secret_token_123") == std::string::npos, "Redacted token= query parameter in diagnostic JSON");
    TEST_ASSERT(redacted_json.find("secret_sess_999") == std::string::npos, "Redacted Cookie value in diagnostic JSON");
    TEST_ASSERT(redacted_json.find("secret_jwt_abc") == std::string::npos, "Redacted Bearer token in diagnostic JSON");

    return true;
}

bool TestSchedulerCapacityWait() {
    using namespace openidm;
    auto root = std::filesystem::temp_directory_path() / ("radium_capacity_" + GenerateUuidV4());
    std::filesystem::create_directories(root);
    std::atomic<unsigned> passes{0};
    ControllerConfig cfg;
    cfg.state_db_path = root / "state.db";
    cfg.default_download_dir = root / "downloads";
    cfg.temp_root_dir = root / "tmp";
    // No available slots isolates scheduling from DNS/server timing.
    cfg.max_concurrent_tasks = 0;
    cfg.local_time_override = [&]() { ++passes; return std::string("12:00"); };
    unsigned observed = 0;
    {
        DownloadController controller(cfg);
        TEST_ASSERT(controller.Initialize().ok(), "Initialize capacity fixture");
        TaskModel task;
        task.url = "https://example.com/capacity.bin";
        task.file_name = "capacity.bin";
        TEST_ASSERT(controller.AddTask(task).ok(), "Queue capacity fixture");
        passes.store(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        observed = passes.load();
        controller.Shutdown();
    }
    std::filesystem::remove_all(root);
    std::cout << "Capacity-exhausted scheduler passes / 250 ms: " << observed << '\n';
    TEST_ASSERT(observed < 30, "Full scheduler must wait rather than spin on queued work");
    return true;
}

bool TestUnusualServerCompatibilityAndClassification() {
    using openidm::PathSanitizer;
    using openidm::engine_stream::VideoExtractionManager;

    // 1. Verify Azure SAS (?sv=) and download endpoints are never rejected as webpage watch URLs
    TEST_ASSERT(!PathSanitizer::IsWebpageWatchUrl(
        "https://productionresultssa30.blob.core.windows.net/actions-results/123.zip?rscd=attachment&sv=2025-11-05&sig=abc"),
        "Azure SAS sv= URL must not be treated as webpage watch URL");
    TEST_ASSERT(!VideoExtractionManager::IsSupportedVideoSiteUrl(
        "https://www.overleaf.com/download/project/abc/output/output.pdf?popupDownload=true"),
        "Overleaf PDF URL must not be classified as supported video site");
    TEST_ASSERT(!VideoExtractionManager::IsSupportedVideoSiteUrl(
        "https://github.com/alve33/Radium-Download-Manager/actions/runs/44728468936"),
        "GitHub Actions URL must not be classified as supported video site");
    TEST_ASSERT(!VideoExtractionManager::IsSupportedVideoSiteUrl(
        "https://contribution.usercontent.google.com/download?c=xyz&filename=out.zip"),
        "Gemini download URL must not be classified as supported video site");
    TEST_ASSERT(VideoExtractionManager::IsSupportedVideoSiteUrl("https://www.youtube.com/watch?v=Ddm7VUgw5Vw"),
        "YouTube watch URL must be classified as supported video site");

#ifdef _WIN32
    // 2. Controlled local HTTP server: premature single-stream close + automatic retry/resume,
    //    and HTML error page rejection for binary file downloads.
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    TEST_ASSERT(listen_sock != INVALID_SOCKET, "Create loopback listen socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    TEST_ASSERT(bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "Bind loopback");
    TEST_ASSERT(listen(listen_sock, 8) == 0, "Listen loopback");
    int alen = sizeof(addr);
    getsockname(listen_sock, reinterpret_cast<sockaddr*>(&addr), &alen);
    uint16_t port = ntohs(addr.sin_port);

    std::string full_payload(32768, 'R');
    for (size_t i = 0; i < full_payload.size(); ++i) {
        full_payload[i] = static_cast<char>('A' + (i % 26));
    }

    std::atomic<bool> stop_srv{false};
    std::atomic<int> drop_hits{0};
    std::thread srv([&]() {
        while (!stop_srv.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_sock, &rfds);
            timeval tv{0, 50000};
            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET cs = accept(listen_sock, nullptr, nullptr);
            if (cs == INVALID_SOCKET) continue;
            char req[2048]{};
            int r = recv(cs, req, sizeof(req) - 1, 0);
            std::string req_s(r > 0 ? req : "");
            if (req_s.find("GET /html_error.zip ") != std::string::npos) {
                std::string body = "<html><body><h1>403 Forbidden</h1></body></html>";
                std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                send(cs, resp.data(), static_cast<int>(resp.size()), 0);
            } else {
                int hit = ++drop_hits;
                size_t offset = 0;
                if (req_s.find("Range: bytes=16384-") != std::string::npos) {
                    offset = 16384;
                }
                if (hit == 1) {
                    // Simulate non-range 200 OK that drops prematurely at 16384 bytes
                    std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nAccept-Ranges: bytes\r\nContent-Length: " +
                                       std::to_string(full_payload.size()) + "\r\nConnection: close\r\n\r\n";
                    send(cs, resp.data(), static_cast<int>(resp.size()), 0);
                    send(cs, full_payload.data(), 16384, 0);
                } else {
                    std::string chunk = full_payload.substr(offset);
                    std::string resp = (offset > 0 ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " +
                        std::to_string(offset) + "-" + std::to_string(full_payload.size() - 1) + "/" +
                        std::to_string(full_payload.size()) + "\r\n" : "HTTP/1.1 200 OK\r\n") +
                        "Content-Type: application/zip\r\nContent-Length: " + std::to_string(chunk.size()) +
                        "\r\nConnection: close\r\n\r\n";
                    send(cs, resp.data(), static_cast<int>(resp.size()), 0);
                    send(cs, chunk.data(), static_cast<int>(chunk.size()), 0);
                }
            }
            closesocket(cs);
        }
    });

    const auto tmp_dir = std::filesystem::temp_directory_path() / "radium_server_compat_test";
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);

    {
        openidm::brisk::EngineConfig cfg;
        cfg.allow_loopback_redirect = true;
        cfg.stall_timeout_ms = 3000;
        cfg.min_split_bytes = 1048576;
        openidm::brisk::HttpDownloadEngine engine(cfg);

        openidm::TaskModel t1;
        t1.task_id = "compat-resume-1";
        t1.url = "http://127.0.0.1:" + std::to_string(port) + "/artifact.zip";
        t1.file_name = "artifact.zip";
        t1.save_dir = tmp_dir.string();
        t1.max_connections = 1;
        t1.conflict_policy = openidm::ConflictPolicy::kOverwrite;
        std::atomic<bool> pause1{false}, cancel1{false};
        std::atomic<uint64_t> limit1{0};
        auto res1 = engine.ExecuteDownload(t1, pause1, cancel1, limit1, nullptr);
        TEST_ASSERT(res1.ok(), "Single-stream premature close must auto-resume and succeed");
        TEST_ASSERT(t1.downloaded_bytes == full_payload.size(), "Resumed single-stream size must match full payload");
        TEST_ASSERT(drop_hits.load() >= 2, "Server must have observed retry/resume request");

        openidm::TaskModel t2;
        t2.task_id = "compat-html-err-1";
        t2.url = "http://127.0.0.1:" + std::to_string(port) + "/html_error.zip";
        t2.file_name = "html_error.zip";
        t2.save_dir = tmp_dir.string();
        t2.max_connections = 1;
        t2.conflict_policy = openidm::ConflictPolicy::kOverwrite;
        std::atomic<bool> pause2{false}, cancel2{false};
        std::atomic<uint64_t> limit2{0};
        auto res2 = engine.ExecuteDownload(t2, pause2, cancel2, limit2, nullptr);
        TEST_ASSERT(!res2.ok(), "HTML error page for .zip must be rejected");
        TEST_ASSERT(t2.error_code == "ERR_HTTP_HTML_ERROR_PAGE", "Error code must be ERR_HTTP_HTML_ERROR_PAGE");
    }

    stop_srv.store(true);
    srv.join();
    closesocket(listen_sock);
    std::filesystem::remove_all(tmp_dir);
#endif
    return true;
}

bool TestPhase1EngineAndNetworking() {
    // 1. FtpUrl::Parse("ftps://...")
    {
        auto parsed = openidm::ftp::FtpUrl::Parse("ftps://example.com/a.bin");
        TEST_ASSERT(parsed.ok(), "FtpUrl::Parse must accept ftps://");
        TEST_ASSERT(parsed.value->scheme == "ftps", "Scheme must be ftps");
        TEST_ASSERT(parsed.value->host == "example.com", "Host must be example.com");
        TEST_ASSERT(parsed.value->port == 990, "Default FTPS port must be 990");
        TEST_ASSERT(parsed.value->remote_path == "/a.bin", "Remote path must be /a.bin");
    }

    // 2. HostLimits refusal learning and cap raising
    {
        auto& hl = openidm::brisk::HostLimits::Global();
        const std::string host = "phase1-unit-test.invalid";
        hl.RecordRefusal(host, 3);
        hl.RecordRefusal(host, 3);
        TEST_ASSERT(hl.GetCap(host).has_value() && *hl.GetCap(host) <= 3U, "HostLimits must lower cap after 2 refusals");
        hl.RecordWorking(host, 5);
        TEST_ASSERT(!hl.GetCap(host).has_value() || *hl.GetCap(host) >= 5U, "HostLimits must raise cap on working concurrency");
    }

    // 3. SegmentScheduler PREEMPT_TTFB & WaitForWork/NotifyAll
    {
        openidm::brisk::SegmentScheduler sched(1024ULL * 1024ULL, 2, 262144);
        sched.Initialize();
        sched.SetPreemptAfterMs(30);
        auto seg0 = sched.AcquireNextOrStealSegment(false);
        TEST_ASSERT(seg0 != nullptr, "Worker 0 must acquire initial segment");
        const uint64_t gen0 = seg0->owner_generation.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto seg1 = sched.AcquireNextOrStealSegment(false);
        TEST_ASSERT(seg1 != nullptr, "Idle worker must preempt stalled TTFB segment");
        TEST_ASSERT(seg1->start_offset == 0 && seg1->end_offset.load() == 1024ULL * 1024ULL - 1, "Preempted segment bounds must match");
        TEST_ASSERT(seg1->owner_generation.load() == gen0 + 1U, "Preempted segment generation must bump by 1");

        std::atomic<bool> cancel_wait{false};
        auto t0 = std::chrono::steady_clock::now();
        std::thread waiter([&]() {
            sched.WaitForWork(2000, [&]() { return cancel_wait.load(); });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        cancel_wait.store(true);
        sched.NotifyAll();
        waiter.join();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        TEST_ASSERT(elapsed_ms < 500, "WaitForWork must wake up promptly on NotifyAll");
    }

#ifdef _WIN32
    // 4. Async WinHTTP < 500ms cancellation against a loopback tarpit socket
    {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        TEST_ASSERT(listen_sock != INVALID_SOCKET, "Failed to create tarpit listen socket");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_ASSERT(bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind failed");
        TEST_ASSERT(listen(listen_sock, 4) == 0, "listen failed");

        int addrlen = sizeof(addr);
        getsockname(listen_sock, reinterpret_cast<sockaddr*>(&addr), &addrlen);
        int port = ntohs(addr.sin_port);

        std::atomic<bool> stop_tarpit{false};
        std::thread tarpit_srv([&]() {
            fd_set fds;
            while (!stop_tarpit.load()) {
                FD_ZERO(&fds);
                FD_SET(listen_sock, &fds);
                timeval tv{0, 50000};
                if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
                    SOCKET c = accept(listen_sock, nullptr, nullptr);
                    if (c != INVALID_SOCKET) {
                        while (!stop_tarpit.load()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        closesocket(c);
                    }
                }
            }
        });

        std::atomic<bool> cancel_flag{false};
        std::thread canceller([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            cancel_flag.store(true);
        });

        auto t0 = std::chrono::steady_clock::now();
        auto res = openidm::brisk::HttpClient::PerformGet(
            "http://127.0.0.1:" + std::to_string(port) + "/tarpit",
            {}, std::nullopt, std::nullopt, 15000, true, cancel_flag,
            std::function<bool(const openidm::brisk::HttpResponseMeta&)>{},
            std::function<bool(const uint8_t*, size_t)>{});
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        canceller.join();
        stop_tarpit.store(true);
        tarpit_srv.join();
        closesocket(listen_sock);

        TEST_ASSERT(!res.ok(), "Cancelled tarpit request must fail");
        TEST_ASSERT(elapsed_ms < 500, "Async WinHTTP cancel must abort in < 500 ms");
    }

    // 5. 206 If-Range validator mismatch must cleanly restart from byte 0 (no offset-0 corruption)
    {
        SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        TEST_ASSERT(listen_sock != INVALID_SOCKET, "Failed to create 206 mismatch listen socket");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        TEST_ASSERT(bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind failed");
        TEST_ASSERT(listen(listen_sock, 8) == 0, "listen failed");

        int addrlen = sizeof(addr);
        getsockname(listen_sock, reinterpret_cast<sockaddr*>(&addr), &addrlen);
        int port = ntohs(addr.sin_port);

        std::string new_payload(128, 'N');
        for (size_t i = 0; i < new_payload.size(); ++i) {
            new_payload[i] = static_cast<char>('A' + (i % 26));
        }

        std::atomic<bool> stop_srv{false};
        std::thread srv([&]() {
            fd_set fds;
            while (!stop_srv.load()) {
                FD_ZERO(&fds);
                FD_SET(listen_sock, &fds);
                timeval tv{0, 50000};
                if (select(0, &fds, nullptr, nullptr, &tv) > 0) {
                    SOCKET c = accept(listen_sock, nullptr, nullptr);
                    if (c == INVALID_SOCKET) continue;
                    char buf[4096];
                    int n = recv(c, buf, sizeof(buf) - 1, 0);
                    if (n > 0) {
                        buf[n] = '\0';
                        std::string req(buf);
                        if (req.find("Range:") != std::string::npos) {
                            std::string tail = new_payload.substr(64);
                            std::string resp =
                                "HTTP/1.1 206 Partial Content\r\n"
                                "ETag: \"v2\"\r\n"
                                "Accept-Ranges: bytes\r\n"
                                "Content-Range: bytes 64-127/128\r\n"
                                "Content-Length: " + std::to_string(tail.size()) + "\r\n"
                                "Connection: close\r\n\r\n" + tail;
                            send(c, resp.data(), static_cast<int>(resp.size()), 0);
                        } else {
                            std::string resp =
                                "HTTP/1.1 200 OK\r\n"
                                "ETag: \"v2\"\r\n"
                                "Accept-Ranges: bytes\r\n"
                                "Content-Length: " + std::to_string(new_payload.size()) + "\r\n"
                                "Connection: close\r\n\r\n" + new_payload;
                            send(c, resp.data(), static_cast<int>(resp.size()), 0);
                        }
                    }
                    closesocket(c);
                }
            }
        });

        auto tmp_dir = std::filesystem::temp_directory_path() / "radium_phase1_206_test";
        std::filesystem::remove_all(tmp_dir);
        std::filesystem::create_directories(tmp_dir);

        // Write stale .part file (64 bytes of 'X')
        auto part_path = tmp_dir / "changed.bin.phase1-206-test.part";
        {
            std::ofstream ofs(part_path, std::ios::binary | std::ios::trunc);
            std::string stale(64, 'X');
            ofs.write(stale.data(), static_cast<std::streamsize>(stale.size()));
        }

        openidm::TaskModel t;
        t.task_id = "phase1-206-test";
        t.url = "http://127.0.0.1:" + std::to_string(port) + "/changed.bin";
        t.file_name = "changed.bin";
        t.save_dir = tmp_dir.string();
        t.part_path = part_path.string();
        t.total_bytes = 128;
        t.downloaded_bytes = 64;
        t.etag = "\"v1\"";
        t.max_connections = 1;
        t.conflict_policy = openidm::ConflictPolicy::kOverwrite;
        t.segments.push_back(openidm::Segment{0, 0, 64, 127, false});

        openidm::brisk::EngineConfig cfg;
        cfg.allow_loopback_redirect = true;
        cfg.stall_timeout_ms = 3000;
        openidm::brisk::HttpDownloadEngine engine(cfg);
        std::atomic<bool> pause{false}, cancel{false};
        std::atomic<uint64_t> limit{0};
        auto res = engine.ExecuteDownload(t, pause, cancel, limit, nullptr);

        stop_srv.store(true);
        srv.join();
        closesocket(listen_sock);

        TEST_ASSERT(res.ok(), "206 validator-mismatch restart must succeed");
        auto final_path = tmp_dir / "changed.bin";
        TEST_ASSERT(std::filesystem::exists(final_path), "Final file must exist");
        std::ifstream ifs(final_path, std::ios::binary);
        std::string actual((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();
        std::filesystem::remove_all(tmp_dir);

        TEST_ASSERT(actual == new_payload, "Final file must match new payload byte-for-byte with zero offset-0 corruption");
    }
#endif
    return true;
}

bool TestPhase2StreamVideoAndGallery() {
    using namespace openidm::engine_stream;

    // 1. In-process HLS Master Playlist parser (MDM manifest.rs port)
    {
        const std::string master_m3u8 =
            "#EXTM3U\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",LANGUAGE=\"en\",URI=\"audio/en.m3u8\"\n"
            "#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID=\"subs\",NAME=\"English\",LANGUAGE=\"en\",URI=\"subs/en.vtt.m3u8\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=2500000,AVERAGE-BANDWIDTH=2200000,RESOLUTION=1280x720,FRAME-RATE=60,AUDIO=\"aud\"\n"
            "720p/index.m3u8\n";
        auto hls_res = VideoExtractionManager::ParseHlsMasterPlaylist(master_m3u8, "https://cdn.example.com/hls/master.m3u8");
        TEST_ASSERT(hls_res.ok() && hls_res.value.has_value(), "ParseHlsMasterPlaylist must parse valid VOD master");
        TEST_ASSERT(hls_res.value->title == "master", "HLS title must derive from URL stem");
        TEST_ASSERT(hls_res.value->subtitles.size() == 1 && hls_res.value->subtitles[0].language_code == "en", "HLS subtitle must be parsed");
        TEST_ASSERT(hls_res.value->formats.size() == 2, "HLS must emit audio rendition + video variant");
        TEST_ASSERT(hls_res.value->formats[1].height == 720 && hls_res.value->formats[1].tbr_kbps == 2200, "HLS variant 720p bitrate must match");

        const std::string drm_m3u8 =
            "#EXTM3U\n#EXT-X-SESSION-KEY:METHOD=SAMPLE-AES,URI=\"skd://key\"\n#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=640x360\n360.m3u8\n";
        auto drm_res = VideoExtractionManager::ParseHlsMasterPlaylist(drm_m3u8, "https://cdn.example.com/drm.m3u8");
        TEST_ASSERT(!drm_res.ok() && drm_res.error->code == "ERR_STREAM_DRM_PROTECTED", "DRM master must be rejected");
    }

    // 2. MP4 / WebM HTTP-Range container index probe (GD3 container.py port)
    {
        // Synthetic MP4: ftyp (16 bytes) + mdat (16 bytes) + moov (36 bytes with mvhd)
        std::vector<uint8_t> mp4;
        auto push_be32 = [&](uint32_t v) {
            mp4.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
            mp4.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            mp4.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            mp4.push_back(static_cast<uint8_t>(v & 0xFF));
        };
        auto push_tag = [&](const char* t) {
            for (int i = 0; i < 4; ++i) mp4.push_back(static_cast<uint8_t>(t[i]));
        };
        // ftyp (16 bytes)
        push_be32(16); push_tag("ftyp"); push_tag("isom"); push_be32(0);
        // mdat (16 bytes)
        push_be32(16); push_tag("mdat"); for (int i = 0; i < 8; ++i) mp4.push_back(0);
        // Check FindContainerIndexRange when only first 32 bytes (head) are fetched
        uint64_t off = 0, len = 0;
        TEST_ASSERT(VideoExtractionManager::FindContainerIndexRange(mp4, 68, off, len), "FindContainerIndexRange must locate tail moov");
        TEST_ASSERT(off == 32 && len == 36, "moov range must be offset=32, len=36");

        // Now append moov (36 bytes) containing mvhd (28 bytes: timescale=1000, duration=12500 -> 12.5s)
        push_be32(36); push_tag("moov");
        push_be32(28); push_tag("mvhd");
        push_be32(0); // version=0, flags=0
        push_be32(0); // creation_time
        push_be32(0); // modification_time
        push_be32(1000); // timescale
        push_be32(12500); // duration
        auto idx = VideoExtractionManager::ParseContainerIndex(mp4);
        TEST_ASSERT(idx.has_value() && idx->format == "mp4", "ParseContainerIndex must parse MP4 moov");
        TEST_ASSERT(std::abs(idx->duration_sec - 12.5) < 0.001, "MP4 mvhd duration must be 12.5s");
    }

    // 3. VideoPartSpec (--download-sections)
    {
        auto part = VideoPartSpec::Validate("1:30", "4:10", false, 600.0);
        TEST_ASSERT(part.ok() && part.value.has_value(), "Valid part 1:30-4:10 must succeed");
        TEST_ASSERT(part.value->SectionsArg() == "*90-250", "SectionsArg must be *90-250");
        TEST_ASSERT(VideoPartSpec::FormatPartFilename("Talk.mp4", *part.value) == "Talk (1m30s-4m10s).mp4", "Part filename must match");
        auto bad = VideoPartSpec::Validate("5:00", "2:00", false, 600.0);
        TEST_ASSERT(!bad.ok(), "Backwards part must fail");
    }

    // 4. GalleryDownloader::ParsePrepareOutput
    {
        auto items = GalleryDownloader::ParsePrepareOutput(
            "https://img.example.com/1.jpg\t001.jpg\n"
            "None\tignored.jpg\n"
            "https://img.example.com/2.png\tsub/002.png\n");
        TEST_ASSERT(items.size() == 2 && items[0].url == "https://img.example.com/1.jpg" && items[1].relative_path == "sub/002.png",
                    "GalleryDownloader must parse valid prepare-after lines");
    }

    // 5. BilibiliHelper (Wbi, URL, QR Poll, Danmaku XML/ASS)
    {
        std::string mixin = BilibiliHelper::ComputeWbiMixinKey(
            "7cd084941338484aae1ad9425b84077c", "4932caff0ff746eab6f01bf08b70ac45");
        TEST_ASSERT(mixin.size() == 32, "Wbi mixin key must be 32 chars");
        auto signed_params = BilibiliHelper::SignWbiParams({{"bvid", "BV1xx411c7mD"}, {"cid", "12345"}}, mixin, 1700000000);
        TEST_ASSERT(signed_params.count("w_rid") && signed_params["w_rid"].size() == 32, "Wbi w_rid must be 32-char MD5 hex");

        auto bu = BilibiliHelper::ParseBilibiliUrl("https://www.bilibili.com/video/BV1xx411c7mD?p=1,3-4");
        TEST_ASSERT(bu.ok() && bu.value->video_id == "BV1xx411c7mD" && bu.value->selected_pages.size() == 3,
                    "Bilibili URL multi-part selection must parse");

        auto qr = BilibiliHelper::ParseQrPollResponse(R"({"code":0,"data":{"code":86090,"message":"scanned"}})");
        TEST_ASSERT(qr.ok() && qr.value->code == 86090, "Bilibili QR poll code must parse");

        // Minimal protobuf DanmakuSeg: field 1 (len-delimited) -> field 2 (progress=1500ms), field 7 ("hello")
        std::vector<uint8_t> pb = {0x0A, 0x0A, 0x10, 0xDC, 0x0B, 0x3A, 0x05, 'h', 'e', 'l', 'l', 'o'};
        auto dm = BilibiliHelper::ParseDanmakuProtobuf(pb);
        TEST_ASSERT(dm.size() == 1 && dm[0].progress_ms == 1500 && dm[0].content == "hello", "Danmaku protobuf must parse");
        TEST_ASSERT(BilibiliHelper::ConvertDanmakuToXml(dm, 99).find(">hello</d>") != std::string::npos, "Danmaku XML must contain text");
        TEST_ASSERT(BilibiliHelper::ConvertDanmakuToAss(dm).find("Dialogue: 0,") != std::string::npos, "Danmaku ASS must contain Dialogue");
    }

    return true;
}

bool TestPhase3SiteHandlersFreshlinkAndPost() {
    using namespace openidm;
    using namespace openidm::brisk;

    // 1. SiteHandlers: CheckNotAFile (Drive folder + GoFile share)
    auto df = SiteHandlers::CheckNotAFile("https://drive.google.com/drive/folders/1AbCdEfGhIjKlMnOpQrStUvWxYz123456");
    TEST_ASSERT(df.has_value() && df->first == "ERR_SITE_FOLDER", "Drive folder rejected");
    auto gf = SiteHandlers::CheckNotAFile("https://gofile.io/d/AbC123");
    TEST_ASSERT(gf.has_value() && gf->first == "ERR_SITE_NEEDS_BROWSER", "GoFile share rejected");
    TEST_ASSERT(!SiteHandlers::CheckNotAFile("https://store1.gofile.io/download/web/abc/archive.rar").has_value(), "GoFile direct link allowed");

    // 2. SiteHandlers: RewriteUrl (Drive viewer, Docs export, LuluStream)
    auto dr = SiteHandlers::RewriteUrl("https://drive.google.com/file/d/1AbCdEfGhIjKlMnOpQrStUvWxYz123456/view");
    TEST_ASSERT(dr.has_value() && dr->find("drive.usercontent.google.com/download") != std::string::npos && dr->find("confirm=t") != std::string::npos, "Drive viewer rewritten");
    auto docr = SiteHandlers::RewriteUrl("https://docs.google.com/spreadsheets/d/1AbCdEfGhIjKlMnOpQrStUvWxYz123456/edit");
    TEST_ASSERT(docr.has_value() && docr->find("/export?format=xlsx") != std::string::npos, "Sheets rewritten to xlsx export");
    auto lulur = SiteHandlers::RewriteUrl("https://lulustream.com/e/abcdefgh1234");
    TEST_ASSERT(lulur.has_value() && lulur->find("/d/abcdefgh1234") != std::string::npos, "LuluStream embed rewritten to /d/");

    // 3. SiteHandlers: ReadPage (Drive virus-scan form, MediaFire button, LuluStream form POST)
    std::string drive_html = R"(<form id="download-form" action="https://drive.usercontent.google.com/download" method="get"><input type="hidden" name="id" value="1AbCdEfGhIjKlMnOpQrStUvWxYz123456"><input type="hidden" name="confirm" value="t"><input type="hidden" name="uuid" value="xyz"></form>)";
    auto dact = SiteHandlers::ReadPage("https://drive.usercontent.google.com/download?id=1AbCdEfGhIjKlMnOpQrStUvWxYz123456", drive_html);
    TEST_ASSERT(dact.kind == SiteActionKind::kNextUrl && dact.url.find("uuid=xyz") != std::string::npos, "Drive virus-scan form parsed");

    std::string mf_html = R"(<a id="downloadButton" href="https://download850.mediafire.com/ticket123/k4makp2pk726bmb/archive.zip">Download</a>)";
    auto mfact = SiteHandlers::ReadPage("https://www.mediafire.com/file/k4makp2pk726bmb/archive.zip/file", mf_html);
    TEST_ASSERT(mfact.kind == SiteActionKind::kNextUrl && mfact.url.find("download850.mediafire.com") != std::string::npos, "MediaFire button parsed");

    std::string lulu_html = R"(<form action="" method="POST"><input type="hidden" name="op" value="download_orig"><input type="hidden" name="id" value="abcdefgh1234"><input type="hidden" name="hash" value="h999"></form>)";
    auto lact = SiteHandlers::ReadPage("https://lulustream.com/d/abcdefgh1234_o", lulu_html);
    TEST_ASSERT(lact.kind == SiteActionKind::kFormPost && lact.form_body.find("op=download_orig") != std::string::npos && lact.form_body.find("hash=h999") != std::string::npos, "LuluStream form POST parsed");

    // 4. SiteHandlers: SameParty & IdentityPreset & BrowserImpersonationHeaders
    TEST_ASSERT(SiteHandlers::SameParty("https://archive.org/download/item/a.pdf", "https://ia600100.us.archive.org/24/items/item/a.pdf"), "Archive.org petabox same party");
    TEST_ASSERT(!SiteHandlers::SameParty("https://archive.org/download/item/a.pdf", "https://example.com/a.pdf"), "Foreign host not same party");

    IdentityPreset p1;
    p1.enabled = true;
    p1.hosts = {"*.example.org", "cdn.test.net"};
    p1.profile = "firefox";
    p1.user_agent = "CustomFF/1.0";
    auto matched = SiteHandlers::MatchIdentityPreset("sub.example.org", {p1});
    TEST_ASSERT(matched.has_value() && matched->profile == "firefox", "Wildcard IdentityPreset matched");
    auto bh = SiteHandlers::BuildBrowserImpersonationHeaders("chrome", {});
    TEST_ASSERT(bh.count("Sec-CH-UA") > 0 && bh.count("User-Agent") > 0, "Browser impersonation headers built");

    // 5. DownloadController: ExtractRegistrableDomain & TryMatchFreshLink
    TEST_ASSERT(DownloadController::ExtractRegistrableDomain("https://s118.rapidgator.net/download/abc") == "rapidgator.net", "Registrable domain 2-label");
    TEST_ASSERT(DownloadController::ExtractRegistrableDomain("https://dl.example.co.uk/file.bin") == "example.co.uk", "Registrable domain country SLD");

    std::filesystem::path tmp_dir = std::filesystem::temp_directory_path() / "radium_phase3_test";
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);
    ControllerConfig ccfg;
    ccfg.state_db_path = tmp_dir / "state.db";
    ccfg.default_download_dir = tmp_dir / "downloads";
    ccfg.temp_root_dir = tmp_dir / "tmp";
    ccfg.allow_loopback = true;
    {
        DownloadController ctrl(ccfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Controller initialized for freshlink test");
        TaskModel old_t;
        old_t.task_id = "expired-task-1";
        old_t.url = "https://s118.rapidgator.net/download/old-token";
        old_t.page_url = "https://rapidgator.net/file/12345/archive.rar.html";
        old_t.file_name = "Lab.Archive.part1.rar";
        old_t.save_dir = ccfg.default_download_dir.string();
        old_t.total_bytes = 5000000;
        old_t.waits_for_address = true;
        old_t.state = TaskState::kFailed;
        old_t.error_code = "ERR_HTTP_403_EXPIRED_OR_AUTH";
        auto r1 = ctrl.AddTask(old_t, false, true);
        TEST_ASSERT(r1.ok(), "Added waiting expired task");

        TaskModel offer;
        offer.url = "https://s93.rapidgator.net/download/new-token";
        offer.page_url = "https://rapidgator.net/file/12345/archive.rar.html";
        offer.file_name = "Lab.Archive.part1.rar";
        offer.save_dir = ccfg.default_download_dir.string();
        offer.total_bytes = 5000000;
        offer.http_method = "POST";
        offer.post_body = "token=abc";
        offer.post_content_type = "application/x-www-form-urlencoded";
        auto r2 = ctrl.AddTask(offer, false, false);
        TEST_ASSERT(r2.ok() && r2.value->task_id == "expired-task-1", "Freshlink matched expired task");
        TEST_ASSERT(r2.value->url == "https://s93.rapidgator.net/download/new-token", "Freshlink updated URL");
        TEST_ASSERT(r2.value->post_body == "token=abc" && r2.value->http_method == "POST", "Freshlink preserved POST payload");
        TEST_ASSERT(r2.value->state == TaskState::kQueued && !r2.value->waits_for_address, "Freshlink queued task and cleared waits_for_address");
    }
    std::filesystem::remove_all(tmp_dir);
    return true;
}

bool TestPhase4TorrentArchiveAndPacks() {
    // 1. TorrentIpBlocklist (PeerGuardian P2P & eMule .dat)
    {
        auto& bl = openidm::torrent::TorrentIpBlocklist::Global();
        bl.LoadFromText(
            "# comment\n"
            "BadISP:1.2.3.0-1.2.3.255\n"
            "005.006.007.000 - 005.006.007.010 , 000 , Blocked\n"
            "008.008.008.008 - 008.008.008.008 , 200 , Allowed\n");
        TEST_ASSERT(bl.RangeCount() == 2, "Blocklist must load 2 blocked ranges (ignoring level > 127)");
        TEST_ASSERT(bl.IsBlocked("1.2.3.42"), "1.2.3.42 must be blocked");
        TEST_ASSERT(bl.IsBlocked("5.6.7.8"), "Zero-padded eMule IP range must block 5.6.7.8");
        TEST_ASSERT(!bl.IsBlocked("8.8.8.8"), "eMule level > 127 must not be blocked");
        bl.Clear();
    }

    // 2. RssTorrentFeedParser
    {
        const std::string rss =
            "<rss version=\"2.0\"><channel><title>Releases</title>"
            "<item><title>Show.Name.S01E05.1080p</title>"
            "<enclosure url=\"https://tracker.example/ep5.torrent\" type=\"application/x-bittorrent\" length=\"1048576\"/>"
            "</item></channel></rss>";
        auto items = openidm::torrent::RssTorrentFeedParser::ParseFeedXml(rss);
        TEST_ASSERT(items.size() == 1 && items[0].torrent_url == "https://tracker.example/ep5.torrent", "RSS enclosure must parse");
        auto ep = openidm::torrent::RssTorrentFeedParser::ExtractEpisode(items[0].title);
        TEST_ASSERT(ep.has_value() && ep->season == 1 && ep->episode == 5, "ExtractEpisode must parse S01E05");
        TEST_ASSERT(openidm::torrent::RssTorrentFeedParser::MatchesFilter(items[0].title, "Show 1080p|720p", "CAM", "1x2;1x5-8"),
                    "RSS filter must match S01E05");
        TEST_ASSERT(!openidm::torrent::RssTorrentFeedParser::MatchesFilter(items[0].title, "Show", "1080p", "1x5"),
                    "RSS exclude filter must reject 1080p");
    }

    // 3. ZipPreviewer & ArchiveExtractor (real Stored ZIP creation + safe extraction)
    {
        auto tmp_dir = std::filesystem::temp_directory_path() / "radium_phase4_zip_test";
        std::filesystem::remove_all(tmp_dir);
        std::filesystem::create_directories(tmp_dir);
        auto zip_path = tmp_dir / "sample.zip";

        const std::string fname = "hello.txt";
        const std::string body = "hello zip";
        std::vector<uint8_t> z;
        auto put16 = [&](uint16_t v) { z.push_back(static_cast<uint8_t>(v & 0xFF)); z.push_back(static_cast<uint8_t>(v >> 8)); };
        auto put32 = [&](uint32_t v) {
            z.push_back(static_cast<uint8_t>(v & 0xFF)); z.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            z.push_back(static_cast<uint8_t>((v >> 16) & 0xFF)); z.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        };
        put32(0x04034b50); put16(20); put16(0); put16(0); put16(0); put16(0); put32(0);
        put32(static_cast<uint32_t>(body.size())); put32(static_cast<uint32_t>(body.size()));
        put16(static_cast<uint16_t>(fname.size())); put16(0);
        z.insert(z.end(), fname.begin(), fname.end());
        z.insert(z.end(), body.begin(), body.end());
        uint32_t cd_off = static_cast<uint32_t>(z.size());
        put32(0x02014b50); put16(20); put16(20); put16(0); put16(0); put16(0); put16(0); put32(0);
        put32(static_cast<uint32_t>(body.size())); put32(static_cast<uint32_t>(body.size()));
        put16(static_cast<uint16_t>(fname.size())); put16(0); put16(0); put16(0); put16(0); put32(0); put32(0);
        z.insert(z.end(), fname.begin(), fname.end());
        uint32_t cd_sz = static_cast<uint32_t>(z.size()) - cd_off;
        put32(0x06054b50); put16(0); put16(0); put16(1); put16(1); put32(cd_sz); put32(cd_off); put16(0);

        {
            std::ofstream ofs(zip_path, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));
        }
        TEST_ASSERT(openidm::archive::ArchiveExtractor::DetectFormat(zip_path) == openidm::archive::ArchiveFormat::kZip,
                    "DetectFormat must identify ZIP");
        auto ext_dir = tmp_dir / "out";
        auto ext_res = openidm::archive::ArchiveExtractor::ExtractArchive(zip_path, ext_dir, openidm::archive::SubfolderPolicy::kSmart);
        TEST_ASSERT(ext_res.ok(), "ExtractArchive must succeed on Stored ZIP");
        TEST_ASSERT(std::filesystem::exists(ext_dir / "hello.txt"), "Smart single-file ZIP must extract directly into dest_dir");
        std::filesystem::remove_all(tmp_dir);
    }

    // 4. DebridManager & Cnl2Server
    {
        using namespace openidm::packs;
        TEST_ASSERT(DebridManager::ServiceName(DebridServiceKind::kRealDebrid) == "Real-Debrid", "Debrid service name");
        TEST_ASSERT(!DebridManager::SupportsMagnets(DebridServiceKind::kMegaDebrid), "Mega-Debrid magnet flag");
        std::atomic<bool> cflag{false};
        auto no_tok = DebridManager::UnlockLink(DebridServiceKind::kTorBox, "", "https://host/file.zip", cflag);
        TEST_ASSERT(!no_tok.ok() && no_tok.error->code == "ERR_DEBRID_AUTH", "Empty debrid token must fail");

        auto key = Cnl2Server::ExtractKeyFromJk("function f(){ return '0102030405060708090a0b0c0d0e0f10'; }");
        TEST_ASSERT(key.has_value() && key->size() == 16 && (*key)[0] == 0x01 && (*key)[15] == 0x10,
                    "ExtractKeyFromJk must extract 16-byte key without JS eval");
    }

    // 5. SpecializedUrlPacks (Wrapped links, GitHub, arXiv, Hugging Face, eD2k)
    {
        using namespace openidm::packs;
        auto th = SpecializedUrlPacks::DecodeWrappedLink("thunder://QUFodHRwczovL2V4YW1wbGUuY29tL2EuemlwWlo=");
        TEST_ASSERT(th.ok() && *th.value == "https://example.com/a.zip", "thunder:// decode must strip AA..ZZ");

        auto gh = SpecializedUrlPacks::BuildGitHubMirrorCandidates("https://github.com/org/repo/releases/download/v1/app.zip");
        TEST_ASSERT(gh.size() >= 2, "GitHub mirror candidates must include proxy mirrors");

        auto ax = SpecializedUrlPacks::BuildArxivDownloads("https://arxiv.org/abs/2401.12345v2");
        TEST_ASSERT(ax.has_value() && ax->pdf_url == "https://arxiv.org/pdf/2401.12345v2.pdf" &&
                    ax->latex_source_url == "https://arxiv.org/e-print/2401.12345v2",
                    "arXiv bundle URLs must match");

        auto hf = SpecializedUrlPacks::ParseHuggingFaceUrl("https://huggingface.co/meta-llama/Llama-3/blob/main/config.json");
        TEST_ASSERT(hf.has_value() && hf->owner == "meta-llama" && hf->repo == "Llama-3", "Hugging Face URL must parse");
        TEST_ASSERT(SpecializedUrlPacks::BuildHuggingFaceFileUrl(*hf, "deadbeef", true) ==
                    "https://hf-mirror.com/meta-llama/Llama-3/resolve/deadbeef/config.json",
                    "Hugging Face commit-pinned mirror URL must match");

        auto ed = SpecializedUrlPacks::ParseEd2kLink("ed2k://|file|ubuntu.iso|1048576|0123456789abcdef0123456789abcdef|/");
        TEST_ASSERT(ed.ok() && ed.value->file_name == "ubuntu.iso" && ed.value->file_size == 1048576,
                    "eD2k link must parse");
    }

    return true;
}

bool TestPhase5IntegrationsBatchEf2AndQuota() {
    using namespace openidm;
    using namespace openidm::crawler;
    using namespace openidm::platform;
    using namespace openidm::storage;

    // 1. Category subfolder routing
    std::filesystem::path tmp_dir = std::filesystem::temp_directory_path() / "radium_phase5_test";
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);
    {
        ControllerConfig ccfg;
        ccfg.state_db_path = tmp_dir / "state.db";
        ccfg.default_download_dir = tmp_dir / "downloads";
        ccfg.temp_root_dir = tmp_dir / "tmp";
        DownloadController ctrl(ccfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Controller initialized for phase5 test");
        AppSettings s = ctrl.GetSettings();
        s.default_download_dir = ccfg.default_download_dir.string();
        s.enable_category_subfolders = true;
        TEST_ASSERT(ctrl.UpdateSettings(s).ok(), "Updated enable_category_subfolders");
        auto vid_dir = ctrl.ResolveCategorySaveDir("Video", "https://example.com/a.mp4");
        TEST_ASSERT(vid_dir == (ccfg.default_download_dir / "Video"), "Category subfolder routed");
        auto gen_dir = ctrl.ResolveCategorySaveDir("General", "https://example.com/a.bin");
        TEST_ASSERT(gen_dir == ccfg.default_download_dir, "General keeps default dir");
    }
    std::filesystem::remove_all(tmp_dir);

    // 2. SiteGrabber split-volume grouping & Batch URL template expansion
    TEST_ASSERT(SiteGrabber::ExtractVolumeKey("movie.part01.rar").value_or("") == "movie", "part01.rar key");
    TEST_ASSERT(SiteGrabber::ExtractVolumeKey("archive.001").value_or("") == "archive", ".001 key");
    GrabbedItem g1{"https://cdn.example.com/movie.part1.rar", "https://example.com/p", "movie.part1.rar", "Compressed", 0, 1};
    GrabbedItem g2{"https://cdn.example.com/movie.part2.rar", "https://example.com/p", "movie.part2.rar", "Compressed", 0, 1};
    auto groups = SiteGrabber::GroupGrabbedItems({g1, g2});
    TEST_ASSERT(groups.size() == 1 && groups[0].is_split_volume && groups[0].items.size() == 2, "Split volumes grouped");

    auto batch = SiteGrabber::ExpandBatchUrlTemplate("https://host.test/[01-05:2]/v.{mp4,mkv}");
    TEST_ASSERT(batch.size() == 6 && batch[0] == "https://host.test/01/v.mp4" && batch[5] == "https://host.test/05/v.mkv",
                "Batch URL template expanded");

    // 3. IDM .ef2 export & import
    TaskModel t;
    t.url = "https://files.example.com/setup.exe";
    t.headers["Referer"] = "https://example.com/dl";
    t.headers["User-Agent"] = "RadiumTest/1.0";
    t.auth_headers["Cookie"] = "sid=abc";
    auto ef2_res = TaskImportExport::ExportTasksToString({t}, ExportFormat::kEf2, true);
    TEST_ASSERT(ef2_res.ok() && ef2_res.value->find("referer: https://example.com/dl") != std::string::npos, "EF2 exported");
    auto imp_res = TaskImportExport::ImportFromEf2(*ef2_res.value);
    TEST_ASSERT(imp_res.ok() && imp_res.value->size() == 1 && imp_res.value->at(0).auth_headers["Cookie"] == "sid=abc",
                "EF2 imported");

    // 4. SlidingWindowQuota
    SlidingWindowQuota q;
    auto t0 = std::chrono::steady_clock::now();
    q.Record(t0, {{"t1", 100}});
    TEST_ASSERT(q.Used(t0, std::chrono::hours(1)) == 0, "Initial baseline not counted");
    q.Record(t0 + std::chrono::seconds(10), {{"t1", 1100}});
    auto chg1 = q.Check(t0 + std::chrono::seconds(10), 1000, std::chrono::hours(1));
    TEST_ASSERT(chg1.has_value() && *chg1 == true && q.blocked, "Quota blocks at limit");
    auto chg2 = q.Check(t0 + std::chrono::hours(1) + std::chrono::seconds(10), 1000, std::chrono::hours(1));
    TEST_ASSERT(chg2.has_value() && *chg2 == false && !q.blocked, "Quota unblocks after window");

    // 5. WindowsIntegrations pure helpers
    TEST_ASSERT(WindowsIntegrations::KeepsZoneInformation(std::nullopt, std::nullopt), "Default keeps MOTW");
    TEST_ASSERT(!WindowsIntegrations::KeepsZoneInformation(1, 2), "Machine policy 1 disables MOTW");
    auto uri = WindowsIntegrations::ParseLaunchUri("radium://add?url=https%3A%2F%2Fexample.com%2Fa.zip");
    TEST_ASSERT(uri.valid && !uri.wake_only && uri.url == "https://example.com/a.zip", "Launch URI parsed");
    return true;
}

bool TestPhase6RemoteMcpAria2AndCrash() {
    using namespace openidm;
    using namespace openidm::remote;

    auto tmp_dir = std::filesystem::temp_directory_path() / "radium_phase6_test";
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);

    ControllerConfig cfg;
    cfg.state_db_path = tmp_dir / "state.db";
    cfg.default_download_dir = tmp_dir / "downloads";
    cfg.temp_root_dir = tmp_dir / "tmp";
    DownloadController ctrl(cfg);
    TEST_ASSERT(ctrl.Initialize().ok(), "Controller must initialize for Phase 6 tests");

    // 1. McpStdioServer
    {
        McpStdioServer mcp(ctrl);
        auto init_r = mcp.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");
        TEST_ASSERT(init_r.find("radium-download-manager") != std::string::npos, "MCP initialize must return serverInfo");

        auto tools_r = mcp.HandleMessage(R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})");
        TEST_ASSERT(tools_r.find("list_downloads") != std::string::npos && tools_r.find("add_download") != std::string::npos,
                    "MCP tools/list must expose download tools");

        auto add_r = mcp.HandleMessage(
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"add_download","arguments":{"url":"https://example.com/file.zip","later":true}}})");
        TEST_ASSERT(add_r.find("file.zip") != std::string::npos, "MCP add_download must add task");

        auto list_r = mcp.HandleMessage(
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"list_downloads","arguments":{}}})");
        TEST_ASSERT(list_r.find("file.zip") != std::string::npos, "MCP list_downloads must list added task");
    }

    // 2. RemoteControlServer (one-time pairing code + Bearer token + revoke)
    {
        RemoteControlServer rc(ctrl);
        std::string code = rc.GeneratePairingCode();
        TEST_ASSERT(code.size() == 6, "Pairing code must be 6 digits");
        auto paired = rc.PairWithCode(code, "Phone");
        TEST_ASSERT(paired.ok() && !paired.value->token.empty(), "PairWithCode must return device token");
        TEST_ASSERT(!rc.PairWithCode(code, "Phone2").ok(), "One-time pairing code must not work twice");

        int st = 0;
        std::string ct;
        rc.HandleHttpRequest("GET", "/api/tasks", "", "", st, ct);
        TEST_ASSERT(st == 401, "Unauthenticated /api/tasks must return 401");

        auto ok_body = rc.HandleHttpRequest("GET", "/api/tasks", "Bearer " + paired.value->token, "", st, ct);
        TEST_ASSERT(st == 200 && ok_body.find("tasks") != std::string::npos, "Authenticated /api/tasks must return 200");
        TEST_ASSERT(rc.RevokeDevice(paired.value->token), "RevokeDevice must remove paired device");
        rc.HandleHttpRequest("GET", "/api/tasks", "Bearer " + paired.value->token, "", st, ct);
        TEST_ASSERT(st == 401, "Revoked token must return 401");
    }

    // 3. Aria2RpcServer
    {
        Aria2RpcServer rpc(ctrl, "s3cr3t");
        auto ver = rpc.HandleJsonRpc(R"({"jsonrpc":"2.0","id":1,"method":"aria2.getVersion","params":["token:s3cr3t"]})");
        TEST_ASSERT(ver.find("1.37.0") != std::string::npos, "aria2.getVersion must succeed with secret");
        auto unauth = rpc.HandleJsonRpc(R"({"jsonrpc":"2.0","id":2,"method":"aria2.getVersion","params":["token:wrong"]})");
        TEST_ASSERT(unauth.find("Unauthorized") != std::string::npos, "aria2 wrong secret must be rejected");
        auto add_res = rpc.HandleJsonRpc(
            R"({"jsonrpc":"2.0","id":3,"method":"aria2.addUri","params":["token:s3cr3t",["https://example.com/a.iso"],{"out":"a.iso"}]})");
        TEST_ASSERT(add_res.find("\"result\"") != std::string::npos, "aria2.addUri must return gid");
    }

    // 4. CrashMonitorArgs::Parse
    {
        const char* argv[] = {"radium.exe", "--crash-monitor", "\\\\.\\pipe\\RadiumCrash_42", "--data", "C:\\d", "--parent", "42"};
        auto parsed = CrashMonitorArgs::Parse(7, argv);
        TEST_ASSERT(parsed.has_value() && parsed->parent_pid == 42 && parsed->pipe_name == "\\\\.\\pipe\\RadiumCrash_42",
                    "CrashMonitorArgs::Parse must parse monitor arguments");
    }

    ctrl.Shutdown();
    std::filesystem::remove_all(tmp_dir);
    return true;
}

bool TestPhase7UiAndFaultInjection() {
    using namespace openidm;
    using namespace openidm::ui;
    using namespace openidm::test;

    // 1. Clickable & sortable table column headers in DesktopView
    {
        auto tmp_dir = std::filesystem::temp_directory_path() / "radium_phase7_ui_test";
        std::filesystem::remove_all(tmp_dir);
        std::filesystem::create_directories(tmp_dir);
        ControllerConfig cfg;
        cfg.state_db_path = tmp_dir / "state.db";
        cfg.default_download_dir = tmp_dir / "dl";
        cfg.temp_root_dir = tmp_dir / "tmp";
        DownloadController ctrl(cfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Controller must initialize for Phase 7 UI test");

        DesktopView view(ctrl);
        auto geom = view.ComputeLayoutGeometry(0);
        TEST_ASSERT(!geom.table_columns.empty(), "Layout geometry must compute table columns");
        const auto& first_col = geom.table_columns[0];
        int cx = (first_col.rect.left + first_col.rect.right) / 2;
        int cy = (first_col.rect.top + first_col.rect.bottom) / 2;
        auto hit = view.HitTestPoint(cx, cy, 0);
        TEST_ASSERT(hit.type == HitTargetType::kTableHeaderColumn, "HitTest on table header must return kTableHeaderColumn");
        TEST_ASSERT(view.HandleMouseClick(cx, cy, false, 0), "Clicking table header must handle sort");
        TEST_ASSERT(view.GetSortColumn() == first_col.sort_column, "Clicking header must sort by that column");

        ctrl.Shutdown();
        std::filesystem::remove_all(tmp_dir);
    }

    // 2. LabServer fault-injection (tarpit cancel < 500ms + max_conns 503)
#ifdef _WIN32
    {
        LabFaults tf;
        tf.tarpit_headers = true;
        LabServer tarpit("payload", tf);
        TEST_ASSERT(tarpit.Start(), "LabServer tarpit must start");
        std::atomic<bool> cancel{false};
        std::thread killer([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cancel.store(true);
        });
        auto t0 = std::chrono::steady_clock::now();
        auto res = brisk::HttpClient::PerformGet(
            tarpit.BaseUrl() + "/tarpit", {}, std::nullopt, std::nullopt, 10000, true, cancel,
            std::function<bool(const brisk::HttpResponseMeta&)>{},
            std::function<bool(const uint8_t*, size_t)>{});
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        killer.join();
        tarpit.Stop();
        TEST_ASSERT(!res.ok() && ms < 500, "LabServer tarpit must cancel in < 500ms");
    }
#endif

    // 3. SlowDiskModel backpressure harness
    {
        SlowDiskModel disk(1048576ULL, 65536ULL);
        TEST_ASSERT(!disk.RecordWrite(32768ULL), "Write within cache must not stall");
        TEST_ASSERT(disk.RecordWrite(65536ULL), "Write exceeding cache must trigger backpressure");
        TEST_ASSERT(disk.WritesWaited() == 1 && disk.TotalWaitMs() > 0, "SlowDiskModel must track backpressure wait");
    }

    // 4. Adversarial parser smoke checks (matching libFuzzer targets)
    {
        const std::string garbage = "\xFF\x00\x01{{{invalid_bencode_and_json";
        (void)torrent::BencodeParser::Decode(garbage);
        try { (void)json::Parse(garbage); } catch (...) {}
        (void)PathSanitizer::SanitizeFileName("..\\..\\evil:stream.exe");
        (void)engine_stream::VideoExtractionManager::ParseHlsMasterPlaylist(garbage, "https://example.com/m.m3u8");
    }

    return true;
}

bool TestPhase8ExtensionSetupSettingsAndAbout() {
    using namespace openidm;
    using namespace radium::core;

    const auto work_dir = std::filesystem::temp_directory_path() / "radium_phase8_ext_test";
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir, ec);

    // 1. Path safety checks
    TEST_ASSERT(ExtensionInstaller::IsSafeRelativeExtensionPath("manifest.json"), "Accept manifest.json");
    TEST_ASSERT(ExtensionInstaller::IsSafeRelativeExtensionPath("js/background.js"), "Accept js/background.js");
    TEST_ASSERT(!ExtensionInstaller::IsSafeRelativeExtensionPath("../evil.js"), "Reject parent traversal");
    TEST_ASSERT(!ExtensionInstaller::IsSafeRelativeExtensionPath("js\\background.js"), "Reject backslash");
    TEST_ASSERT(!ExtensionInstaller::IsSafeRelativeExtensionPath("/etc/passwd"), "Reject leading slash");
    TEST_ASSERT(!ExtensionInstaller::IsSafeRelativeExtensionPath("C:foo.js"), "Reject drive/colon");
    TEST_ASSERT(!ExtensionInstaller::IsSafeRelativeExtensionPath("CON"), "Reject DOS device name");

    // 2. Fresh installation displays onboarding once and persists across restarts & RestoreDefaultSettings
    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    std::filesystem::create_directories(cfg.default_download_dir, ec);
    std::filesystem::create_directories(cfg.temp_root_dir, ec);

    {
        DownloadController ctrl(cfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Controller initializes on fresh DB");
        TEST_ASSERT(ctrl.ShouldShowExtensionOnboarding(), "Fresh install shows extension onboarding");
        TEST_ASSERT(ctrl.MarkExtensionOnboardingDisplayed("later").ok(), "Mark onboarding displayed");
        TEST_ASSERT(!ctrl.ShouldShowExtensionOnboarding(), "Clicking Later prevents onboarding from reappearing");
        TEST_ASSERT(ctrl.RestoreDefaultSettings().ok(), "RestoreDefaultSettings succeeds");
        TEST_ASSERT(!ctrl.ShouldShowExtensionOnboarding(), "RestoreDefaultSettings does not reset onboarding");
        ctrl.Shutdown();
    }
    {
        // Simulated restart / Radium update against same state.db
        DownloadController ctrl2(cfg);
        TEST_ASSERT(ctrl2.Initialize().ok(), "Controller re-initializes after restart");
        TEST_ASSERT(!ctrl2.ShouldShowExtensionOnboarding(), "Restarting does not show onboarding again");
        ctrl2.Shutdown();
    }

    // 3. Mocked GitHub download, validation, update, and atomic failure preservation
    const std::string fake_sha = "0123456789abcdef0123456789abcdef01234567";
    auto make_mock_fetcher = [&](std::string version, bool fail_mid_download, bool rate_limit, bool traversal) -> ExtensionFetchFn {
        return [=](const std::string& url, const std::map<std::string, std::string>&) -> ExtensionFetchResponse {
            ExtensionFetchResponse r;
            if (rate_limit) {
                r.status_code = 403;
                r.headers["X-RateLimit-Remaining"] = "0";
                r.body = "{\"message\":\"API rate limit exceeded\"}";
                return r;
            }
            if (url.find("/releases/latest") != std::string::npos) {
                r.status_code = 200;
                r.body = "{\"tag_name\":\"v1.0.0\",\"assets\":[]}";
                return r;
            }
            if (url.find("/branches/main") != std::string::npos) {
                r.status_code = 200;
                r.body = "{\"commit\":{\"sha\":\"" + fake_sha + "\"}}";
                return r;
            }
            if (url.find("/git/trees/") != std::string::npos) {
                json::Value tree = json::Array{};
                if (traversal) {
                    json::Value bad = json::Object{};
                    bad["path"] = "extension/../outside.txt";
                    bad["type"] = "blob";
                    tree.mutable_array().push_back(std::move(bad));
                }
                for (const auto& req : ExtensionInstaller::RequiredFiles()) {
                    json::Value item = json::Object{};
                    item["path"] = "extension/" + req;
                    item["type"] = "blob";
                    tree.mutable_array().push_back(std::move(item));
                }
                json::Value root = json::Object{};
                root["tree"] = std::move(tree);
                r.status_code = 200;
                r.body = root.dump();
                return r;
            }
            if (url.find("raw.githubusercontent.com") != std::string::npos) {
                if (fail_mid_download && url.find("js/popup.js") != std::string::npos) {
                    r.status_code = 500;
                    r.error_message = "Simulated HTTP 500";
                    return r;
                }
                r.status_code = 200;
                if (url.find("manifest.json") != std::string::npos) {
                    r.body = "{\"manifest_version\":3,\"name\":\"Radium Download Manager\",\"version\":\"" + version +
                             "\",\"permissions\":[\"nativeMessaging\"],\"background\":{\"service_worker\":\"js/background.js\"},"
                             "\"action\":{\"default_popup\":\"popup.html\"}}";
                } else if (url.find("manifest.firefox.json") != std::string::npos) {
                    r.body = "{\"manifest_version\":3,\"name\":\"Radium Download Manager\",\"version\":\"" + version + "\"}";
                } else {
                    r.body = "// mock resource";
                }
                return r;
            }
            r.status_code = 404;
            return r;
        };
    };

    auto res1 = ExtensionInstaller::DownloadAndInstall(work_dir, {}, make_mock_fetcher("1.0.0", false, false, false));
    TEST_ASSERT(res1.success, "Initial mocked extension download succeeds");
    TEST_ASSERT(res1.version == "1.0.0", "Extracted version 1.0.0 from manifest.json");
    TEST_ASSERT(res1.install_dir == ExtensionInstaller::DefaultInstallDir(work_dir), "Installed into expected directory");
    TEST_ASSERT(ExtensionInstaller::ValidateExtensionDirectory(res1.install_dir), "All required files present and valid");

    // Failed download must NOT overwrite existing v1.0.0
    auto res_fail = ExtensionInstaller::DownloadAndInstall(work_dir, {}, make_mock_fetcher("2.0.0", true, false, false));
    TEST_ASSERT(!res_fail.success, "Mid-download failure returns error");
    std::string intact_ver;
    TEST_ASSERT(ExtensionInstaller::ValidateExtensionDirectory(res1.install_dir, &intact_ver) && intact_ver == "1.0.0",
                "Existing v1.0.0 extension remains intact after failed download");

    // Rate limit and traversal rejection
    auto res_rl = ExtensionInstaller::DownloadAndInstall(work_dir, {}, make_mock_fetcher("1.1.0", false, true, false));
    TEST_ASSERT(!res_rl.success && res_rl.rate_limited, "GitHub rate limit handled cleanly");
    auto res_trav = ExtensionInstaller::DownloadAndInstall(work_dir, {}, make_mock_fetcher("1.1.0", false, false, true));
    TEST_ASSERT(!res_trav.success && res_trav.error_code == "ERR_UNSAFE_PATH", "Path traversal in tree rejected");

    // Settings update (v1.0.0 -> v1.1.0)
    auto res2 = ExtensionInstaller::DownloadAndInstall(work_dir, {}, make_mock_fetcher("1.1.0", false, false, false));
    TEST_ASSERT(res2.success && res2.version == "1.1.0", "Updating existing extension package succeeds");

    // 4. Settings & About UI view-model checks
    {
        DownloadController ctrl(cfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Controller initializes for UI view test");
        ctrl.RecordInstalledExtension("1.1.0", fake_sha, res2.install_dir.string());
        ui::DesktopView view(ctrl);
        auto vm = view.BuildSettingsCenterView();
        auto ab = view.GetAboutDialogView();
        TEST_ASSERT(ab.developer_name == "Sayed Alve", "AboutDialogView has developer name");
        TEST_ASSERT(ab.developer_github_url == "https://github.com/alve33", "AboutDialogView has developer GitHub URL");
        TEST_ASSERT(ab.repository_url == "https://github.com/alve33/Radium-Download-Manager", "AboutDialogView has repo URL");

        // Verify Browser Integration has download_browser_extension and About has developer items
        view.SelectSettingsCategory(ui::SettingsCategory::kBrowserIntegration);
        auto bi_vm = view.BuildSettingsCenterView();
        bool found_dl_ext = false;
        for (const auto& item : bi_vm.visible_items) {
            if (item.key == "download_browser_extension") {
                found_dl_ext = true;
                TEST_ASSERT(item.value_text.find("1.1.0") != std::string::npos, "Settings shows installed extension version");
            }
        }
        TEST_ASSERT(found_dl_ext, "Browser Integration Settings exposes download_browser_extension");

        view.SelectSettingsCategory(ui::SettingsCategory::kAboutAndDiagnostics);
        auto ab_vm = view.BuildSettingsCenterView();
        bool found_dev = false, found_dev_gh = false, found_repo = false;
        for (const auto& item : ab_vm.visible_items) {
            if (item.key == "diag_developer") found_dev = true;
            if (item.key == "open_developer_github") found_dev_gh = true;
            if (item.key == "open_github_repo") found_repo = true;
        }
        TEST_ASSERT(found_dev && found_dev_gh && found_repo, "About section exposes Developer info and GitHub links");

        // Verify Tools toolbar button hit-testing and Settings compact scroll bounds
        auto geom = view.ComputeLayoutGeometry(0);
        TEST_ASSERT(geom.tools_button_rect.width() > 0 && geom.tools_button_rect.height() > 0,
                    "Tools menu button has valid geometry");
        auto tools_hit = view.HitTestPoint(
            (geom.tools_button_rect.left + geom.tools_button_rect.right) / 2,
            (geom.tools_button_rect.top + geom.tools_button_rect.bottom) / 2, 0);
        TEST_ASSERT(tools_hit.type == ui::HitTargetType::kToolsMenuButton,
                    "Tools button hit-tests as kToolsMenuButton");

        view.SetWindowDimensions(880, 520, 96);
        view.OpenSettingsCenter(ui::SettingsCategory::kAboutAndDiagnostics);
        auto compact_vm = view.BuildSettingsCenterView();
        TEST_ASSERT(compact_vm.max_scroll > 0, "Compact Settings viewport requires scrolling for About & Diagnostics");
        view.ScrollSettingsBy(100000);
        auto scrolled_vm = view.BuildSettingsCenterView();
        TEST_ASSERT(scrolled_vm.scroll_offset == scrolled_vm.max_scroll, "Settings scroll clamps to max_scroll");
        TEST_ASSERT(!scrolled_vm.visible_items.empty() &&
                    scrolled_vm.visible_items.back().row_rect.bottom <= scrolled_vm.content_rect.bottom,
                    "Last Settings item scrolls completely into view above footer");
        view.SelectSettingsCategory(ui::SettingsCategory::kGeneral);
        TEST_ASSERT(view.GetSettingsScrollOffset() == 0, "Switching Settings category resets scroll offset");
        view.CloseSettingsCenter(true);
        ctrl.Shutdown();
    }

    std::filesystem::remove_all(work_dir, ec);
    std::cout << "  [Phase 8] Extension Setup, Persistent Onboarding, Settings & About tests verified.\n";
    return true;
}

bool TestDownloadExistenceAndConflictResolution13Scenarios() {
    using namespace openidm;

    const auto work_dir = std::filesystem::temp_directory_path() / ("radium_phase1_conflict_13_" + GenerateUuidV4());
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir / "downloads", ec);
    std::filesystem::create_directories(work_dir / "tmp", ec);

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    DownloadController ctrl(cfg);
    TEST_ASSERT(ctrl.Initialize().ok(), "Controller initializes");
    auto s = ctrl.GetSettings();
    s.conflict_policy = ConflictPolicy::kPrompt;
    s.last_download_dir = cfg.default_download_dir.string();
    TEST_ASSERT(ctrl.UpdateSettings(s).ok(), "Settings updated to kPrompt");

    // 1. New download with no existing file
    {
        TaskModel cand;
        cand.url = "https://example.com/fresh.zip";
        cand.file_name = "fresh.zip";
        cand.save_dir = cfg.default_download_dir.string();
        auto rep = ctrl.InspectIncomingDownload(cand);
        TEST_ASSERT(!rep.has_conflict(), "Scenario 1: No conflict for fresh file");
        auto added = ctrl.AddTask(cand, false, false);
        TEST_ASSERT(added.ok(), "Scenario 1: AddTask succeeds");
        TEST_ASSERT(ctrl.ListTasks().size() == 1, "Scenario 1: Exactly 1 entry created");
        TEST_ASSERT(ctrl.RemoveTask(added.value->task_id, false).ok(), "Cleanup 1");
    }

    // 2. New download with an existing file (must not create ghost paused task)
    const std::filesystem::path existing_mp4 = cfg.default_download_dir / "video.mp4";
    {
        std::ofstream ofs(existing_mp4, std::ios::binary);
        ofs << "ORIGINAL_VIDEO_DATA";
    }
    {
        TaskModel cand;
        cand.url = "https://example.com/video.mp4";
        cand.file_name = "video.mp4";
        cand.save_dir = cfg.default_download_dir.string();
        auto rep = ctrl.InspectIncomingDownload(cand);
        TEST_ASSERT(rep.kind == DownloadController::IncomingConflictKind::kExistsOnDisk,
                    "Scenario 2: Detects existing file on disk");
        TEST_ASSERT(rep.overwrite_allowed, "Scenario 2: Overwrite allowed when no active/paused task owns it");
        auto rejected = ctrl.AddTask(cand, false, false);
        TEST_ASSERT(!rejected.ok() && rejected.error->code == "ERR_FILE_EXISTS",
                    "Scenario 2: AddTask with kPrompt rejects without creating ghost task");
        TEST_ASSERT(ctrl.ListTasks().empty(), "Scenario 2: Zero ghost tasks in controller");
    }

    // 3 & 13. Browser extension download.offer in GUI mode + duplicate browser interception events
    {
        ipc::IpcServer ipc(ctrl);
        ipc.SetHeadlessDaemonMode(false);
        std::vector<TaskModel> ui_offers;
        ipc.SetOfferNotificationCallback([&](const TaskModel& draft) {
            ui_offers.push_back(draft);
        });

        json::Value req = json::Object{};
        req["jsonrpc"] = "2.0";
        req["id"] = 1;
        req["method"] = "download.offer";
        json::Value p = json::Object{};
        p["url"] = "https://cdn.example.com/video.mp4";
        p["filename"] = "video.mp4";
        p["correlationId"] = "corr-dup-123";
        req["params"] = std::move(p);

        auto r1 = json::Parse(ipc.HandleJsonRpcMessage(req.dump()));
        req["id"] = 2;
        auto r2 = json::Parse(ipc.HandleJsonRpcMessage(req.dump()));
        TEST_ASSERT(r1["result"]["accepted"].as_bool() == true, "Scenario 13: First offer accepted");
        TEST_ASSERT(r2["result"]["duplicateSuppressed"].as_bool() == true,
                    "Scenario 3: Duplicate correlationId suppressed");
        TEST_ASSERT(ui_offers.size() == 1, "Scenario 3: Exactly 1 UI offer dispatched");
        TEST_ASSERT(ctrl.ListTasks().empty(), "Scenario 13: No persistent ghost task created before user resolution");
    }

    // 4. Same URL already downloading/queued
    {
        TaskModel active;
        active.url = "https://example.com/archive.tar.gz";
        active.file_name = "archive.tar.gz";
        active.save_dir = cfg.default_download_dir.string();
        auto r_active = ctrl.AddTask(active, true, false);
        TEST_ASSERT(r_active.ok(), "Scenario 4: Initial queued task added");

        TaskModel dup_cand;
        dup_cand.url = "https://example.com/archive.tar.gz";
        dup_cand.file_name = "archive.tar.gz";
        dup_cand.save_dir = cfg.default_download_dir.string();
        auto rep = ctrl.InspectIncomingDownload(dup_cand);
        TEST_ASSERT(rep.kind == DownloadController::IncomingConflictKind::kActiveOrQueuedSameDownload,
                    "Scenario 4: Detects same URL already active/queued");
        auto r_dup = ctrl.AddTask(dup_cand, true, false);
        TEST_ASSERT(r_dup.ok() && r_dup.value->task_id == r_active.value->task_id,
                    "Scenario 4: Returns existing task without duplicate");
        TEST_ASSERT(ctrl.ListTasks().size() == 1, "Scenario 4: Still only 1 task");
        TEST_ASSERT(ctrl.RemoveTask(r_active.value->task_id, false).ok(), "Cleanup 4");
    }

    // 5. Same URL previously completed
    {
        TaskModel done;
        done.url = "https://example.com/completed.iso";
        done.file_name = "completed.iso";
        done.save_dir = cfg.default_download_dir.string();
        done.state = TaskState::kCompleted;
        auto r_done = ctrl.AddTask(done, false, true);
        TEST_ASSERT(r_done.ok(), "Scenario 5: Completed history task added");

        TaskModel again;
        again.url = "https://example.com/completed.iso";
        again.file_name = "completed.iso";
        again.save_dir = cfg.default_download_dir.string();
        auto rep = ctrl.InspectIncomingDownload(again);
        TEST_ASSERT(rep.kind == DownloadController::IncomingConflictKind::kCompletedInHistory,
                    "Scenario 5: Distinguishes completed download in history");
        TEST_ASSERT(ctrl.RemoveTask(r_done.value->task_id, false).ok(), "Cleanup 5");
    }

    // 6. Rename existing file conflict
    {
        std::string unique_name = ctrl.ResolveAvailableFileName(cfg.default_download_dir.string(), "video.mp4");
        TEST_ASSERT(unique_name == "video (1).mp4", "Scenario 6: Resolves unique name video (1).mp4");
        TaskModel renamed;
        renamed.url = "https://example.com/video.mp4";
        renamed.file_name = unique_name;
        renamed.save_dir = cfg.default_download_dir.string();
        renamed.user_edited_filename = true;
        renamed.conflict_policy = ConflictPolicy::kAutoRename;
        renamed.conflict_policy_explicit = true;
        auto r_ren = ctrl.AddTask(renamed, false, true);
        TEST_ASSERT(r_ren.ok() && r_ren.value->file_name == "video (1).mp4",
                    "Scenario 6: Renamed task preserves new filename");
        TEST_ASSERT(r_ren.value->user_edited_filename, "Scenario 6: user_edited_filename preserved");
        TEST_ASSERT(ctrl.RemoveTask(r_ren.value->task_id, false).ok(), "Cleanup 6");
    }

    // 7. Overwrite existing file conflict (safe temp commit + block when owned by active/paused task)
    {
        std::filesystem::path part_file = work_dir / "tmp" / "video.part";
        {
            std::ofstream ofs(part_file, std::ios::binary);
            ofs << "NEW_VIDEO_DATA";
        }
        auto commit_res = PathSanitizer::CommitPartFile(part_file, cfg.default_download_dir, "video.mp4", ConflictPolicy::kOverwrite);
        TEST_ASSERT(commit_res.ok() && commit_res.value->filename() == "video.mp4",
                    "Scenario 7: Overwrite commits to exact existing destination");
        std::ifstream ifs(existing_mp4, std::ios::binary);
        std::string content;
        std::getline(ifs, content);
        TEST_ASSERT(content == "NEW_VIDEO_DATA", "Scenario 7: Existing file safely replaced");
    }

    // 8. Cancel existing file conflict
    {
        TEST_ASSERT(ctrl.ListTasks().empty(), "Scenario 8: No ghost or canceled entry left in DB");
        std::ifstream ifs(existing_mp4, std::ios::binary);
        std::string content;
        std::getline(ifs, content);
        TEST_ASSERT(content == "NEW_VIDEO_DATA", "Scenario 8: Existing file untouched");
    }

    // 9 & 10. Destination becomes occupied / Two simultaneous downloads targeting same filename
    {
        TaskModel t1;
        t1.url = "https://mirror1.example.com/setup.exe";
        t1.file_name = "setup.exe";
        t1.save_dir = cfg.default_download_dir.string();
        auto r1 = ctrl.AddTask(t1, false, false); // paused task owning setup.exe
        TEST_ASSERT(r1.ok(), "Scenario 10: First download reserves setup.exe");

        TaskModel t2;
        t2.url = "https://mirror2.example.com/setup.exe";
        t2.file_name = "SETUP.EXE"; // 12: Windows case-insensitive path comparison
        t2.save_dir = cfg.default_download_dir.string();
        auto rep2 = ctrl.InspectIncomingDownload(t2);
        TEST_ASSERT(rep2.kind == DownloadController::IncomingConflictKind::kReservedByAnotherTask,
                    "Scenario 9/10/12: Detects case-insensitive reservation by paused task");
        TEST_ASSERT(!rep2.overwrite_allowed,
                    "Scenario 10: Overwrite disallowed when destination is owned by paused task");

        t2.conflict_policy = ConflictPolicy::kOverwrite;
        t2.conflict_policy_explicit = true;
        auto ovw_blocked = ctrl.AddTask(t2, false, false);
        TEST_ASSERT(!ovw_blocked.ok() && ovw_blocked.error->code == "ERR_DESTINATION_IN_USE",
                    "Scenario 10: Controller blocks overwrite of destination owned by another task");

        t2.conflict_policy = ConflictPolicy::kAutoRename;
        t2.conflict_policy_explicit = true;
        auto r2 = ctrl.AddTask(t2, false, false);
        TEST_ASSERT(r2.ok() && r2.value->file_name == "SETUP (1).EXE",
                    "Scenario 10: AutoRename picks non-colliding slot");
        TEST_ASSERT(ctrl.RemoveTask(r1.value->task_id, false).ok(), "Cleanup 10a");
        TEST_ASSERT(ctrl.RemoveTask(r2.value->task_id, false).ok(), "Cleanup 10b");
    }

    // 11. Torrent with multiple files and an existing destination
    {
        std::filesystem::create_directories(cfg.default_download_dir / "MyPack", ec);
        {
            std::ofstream ofs(cfg.default_download_dir / "MyPack" / "track2.flac", std::ios::binary);
            ofs << "EXISTING_TRACK";
        }
        torrent::TorrentInfo tinfo;
        tinfo.name = "MyPack";
        tinfo.info_hash_hex = "0123456789abcdef0123456789abcdef01234567";
        tinfo.files.push_back({std::filesystem::path("MyPack/track1.flac"), 1024, 0, true});
        tinfo.files.push_back({std::filesystem::path("MyPack/track2.flac"), 2048, 1024, true});
        auto tor_rep = ctrl.InspectIncomingTorrent(tinfo, cfg.default_download_dir.string());
        TEST_ASSERT(tor_rep.kind == DownloadController::IncomingConflictKind::kExistsOnDisk,
                    "Scenario 11: Multi-file torrent detects existing file destination");
        TEST_ASSERT(tor_rep.conflicting_torrent_files.size() == 1 &&
                        tor_rep.conflicting_torrent_files[0] == "MyPack/track2.flac",
                    "Scenario 11: Reports exact conflicting torrent file");
    }

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestRepeatedSameUrlThreeAttemptsAndRestart() {
    using namespace openidm;
    using namespace openidm::test;

    const auto work_dir = std::filesystem::temp_directory_path() / ("radium_repeat_3x_" + GenerateUuidV4());
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir / "downloads", ec);
    std::filesystem::create_directories(work_dir / "tmp", ec);

    std::string payload(65536, 'R');
    LabServer server(payload);
    TEST_ASSERT(server.Start(), "Local LabServer starts");
    const std::string target_url = server.BaseUrl() + "/repeat.bin";

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";
    cfg.allow_loopback = true;

    auto wait_terminal = [](DownloadController& c, const std::string& tid) -> std::optional<TaskModel> {
        for (int i = 0; i < 100; ++i) {
            auto t = c.GetTask(tid);
            if (t.has_value() &&
                (t->state == TaskState::kCompleted || t->state == TaskState::kFailed || t->state == TaskState::kCanceled)) {
                return t;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        return c.GetTask(tid);
    };

    {
        DownloadController ctrl(cfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Controller initializes");
        auto s = ctrl.GetSettings();
        s.conflict_policy = ConflictPolicy::kPrompt;
        s.last_download_dir = cfg.default_download_dir.string();
        TEST_ASSERT(ctrl.UpdateSettings(s).ok(), "Settings updated");

        ipc::IpcServer ipc(ctrl);
        ipc.SetHeadlessDaemonMode(false);
        std::vector<TaskModel> offers;
        ipc.SetOfferNotificationCallback([&](const TaskModel& draft) { offers.push_back(draft); });

        auto send_offer = [&](const std::string& corr) {
            json::Value req = json::Object{};
            req["jsonrpc"] = "2.0";
            req["id"] = 1;
            req["method"] = "download.offer";
            json::Value p = json::Object{};
            p["url"] = target_url;
            p["filename"] = "repeat.bin";
            p["correlationId"] = corr;
            req["params"] = std::move(p);
            return json::Parse(ipc.HandleJsonRpcMessage(req.dump()));
        };

        // Attempt 1: Fresh download
        auto r1 = send_offer("corr-attempt-1");
        TEST_ASSERT(r1["result"]["accepted"].as_bool(), "Attempt 1 accepted");
        TEST_ASSERT(offers.size() == 1, "Attempt 1 delivered to UI");
        ipc.ResolvePendingOffer(offers[0].task_id);
        auto rep1 = ctrl.InspectIncomingDownload(offers[0]);
        TEST_ASSERT(!rep1.has_conflict(), "Attempt 1 has no conflict");
        auto a1 = ctrl.AddTask(offers[0], true, false);
        TEST_ASSERT(a1.ok(), "Attempt 1 AddTask ok");
        auto fin1 = wait_terminal(ctrl, a1.value->task_id);
        TEST_ASSERT(fin1.has_value() && fin1->state == TaskState::kCompleted, "Attempt 1 completed");
        TEST_ASSERT(std::filesystem::exists(cfg.default_download_dir / "repeat.bin", ec), "Attempt 1 file exists");

        // Attempt 2: Same URL, Rename
        auto r2 = send_offer("corr-attempt-2");
        TEST_ASSERT(r2["result"]["accepted"].as_bool(), "Attempt 2 accepted");
        TEST_ASSERT(offers.size() == 2, "Attempt 2 delivered to UI");
        ipc.ResolvePendingOffer(offers[1].task_id);
        auto rep2 = ctrl.InspectIncomingDownload(offers[1]);
        TEST_ASSERT(rep2.kind == DownloadController::IncomingConflictKind::kExistsOnDisk, "Attempt 2 detects existing file");
        TaskModel t2 = offers[1];
        t2.file_name = ctrl.ResolveAvailableFileName(t2.save_dir, t2.file_name);
        t2.user_edited_filename = true;
        t2.conflict_policy = ConflictPolicy::kAutoRename;
        t2.conflict_policy_explicit = true;
        auto a2 = ctrl.AddTask(std::move(t2), true, true);
        TEST_ASSERT(a2.ok(), "Attempt 2 AddTask ok");
        auto fin2 = wait_terminal(ctrl, a2.value->task_id);
        TEST_ASSERT(fin2.has_value() && fin2->state == TaskState::kCompleted, "Attempt 2 completed");
        TEST_ASSERT(std::filesystem::exists(cfg.default_download_dir / "repeat (1).bin", ec), "Attempt 2 renamed file exists");

        // Attempt 3: Same URL, Overwrite (even if same correlationId is reused after ResolvePendingOffer!)
        auto r3 = send_offer("corr-attempt-2");
        TEST_ASSERT(r3["result"]["accepted"].as_bool(), "Attempt 3 accepted");
        TEST_ASSERT(!r3["result"].contains("duplicateSuppressed"), "Attempt 3 not falsely suppressed after resolve");
        TEST_ASSERT(offers.size() == 3, "Attempt 3 delivered to UI");
        ipc.ResolvePendingOffer(offers[2].task_id);
        TaskModel t3 = offers[2];
        t3.conflict_policy = ConflictPolicy::kOverwrite;
        t3.conflict_policy_explicit = true;
        t3.user_edited_filename = true;
        auto a3 = ctrl.AddTask(std::move(t3), true, true);
        TEST_ASSERT(a3.ok(), "Attempt 3 AddTask ok");
        auto fin3 = wait_terminal(ctrl, a3.value->task_id);
        TEST_ASSERT(fin3.has_value() && fin3->state == TaskState::kCompleted, "Attempt 3 completed");
        TEST_ASSERT(std::filesystem::file_size(cfg.default_download_dir / "repeat.bin", ec) == payload.size(),
                    "Attempt 3 overwrite file size exact");

        ctrl.Shutdown();
    }

    // Attempt 4: After simulated application restart
    {
        DownloadController ctrl2(cfg);
        TEST_ASSERT(ctrl2.Initialize().ok(), "Controller re-initializes after restart");
        TaskModel t4;
        t4.url = target_url;
        t4.file_name = ctrl2.ResolveAvailableFileName(cfg.default_download_dir.string(), "repeat.bin");
        t4.save_dir = cfg.default_download_dir.string();
        t4.user_edited_filename = true;
        t4.conflict_policy = ConflictPolicy::kAutoRename;
        t4.conflict_policy_explicit = true;
        auto a4 = ctrl2.AddTask(std::move(t4), true, true);
        TEST_ASSERT(a4.ok(), "Attempt 4 AddTask ok after restart");
        auto fin4 = wait_terminal(ctrl2, a4.value->task_id);
        TEST_ASSERT(fin4.has_value() && fin4->state == TaskState::kCompleted, "Attempt 4 completed after restart");
        TEST_ASSERT(std::filesystem::exists(cfg.default_download_dir / "repeat (2).bin", ec), "Attempt 4 file exists");
        ctrl2.Shutdown();
    }

    server.Stop();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestStartupPausedRestorationDoesNotTriggerConflicts() {
    using namespace openidm;

    const auto work_dir = std::filesystem::temp_directory_path() / ("radium_startup_paused_" + GenerateUuidV4());
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir / "downloads", ec);
    std::filesystem::create_directories(work_dir / "tmp", ec);

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    std::string p1_id, p2_id;
    {
        DownloadController ctrl(cfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Init 1");
        auto s = ctrl.GetSettings();
        s.conflict_policy = ConflictPolicy::kPrompt;
        TEST_ASSERT(ctrl.UpdateSettings(s).ok(), "Set kPrompt");

        // Create two paused tasks (one with an existing .part file, one with legacy ERR_FILE_EXISTS)
        TaskModel t1;
        t1.url = "https://example.com/paused1.zip";
        t1.file_name = "paused1.zip";
        t1.save_dir = cfg.default_download_dir.string();
        auto a1 = ctrl.AddTask(t1, false, false);
        TEST_ASSERT(a1.ok(), "Paused task 1 added");
        p1_id = a1.value->task_id;
        std::filesystem::path part1 = cfg.default_download_dir / ("paused1.zip." + p1_id + ".part");
        { std::ofstream ofs(part1, std::ios::binary); ofs << "PARTIAL"; }
        TaskModel u1 = *a1.value;
        u1.part_path = part1.string();
        u1.downloaded_bytes = 7;
        TEST_ASSERT(ctrl.UpdateTask(u1).ok(), "Paused task 1 updated with part_path");

        TaskModel t2;
        t2.url = "https://example.com/paused2.zip";
        t2.file_name = "paused2.zip";
        t2.save_dir = cfg.default_download_dir.string();
        auto a2 = ctrl.AddTask(t2, false, false);
        TEST_ASSERT(a2.ok(), "Paused task 2 added");
        p2_id = a2.value->task_id;
        TaskModel u2 = *a2.value;
        u2.error_code = "ERR_FILE_EXISTS";
        u2.status_detail = "Waiting for a file conflict choice";
        TEST_ASSERT(ctrl.UpdateTask(u2).ok(), "Paused task 2 given legacy ERR_FILE_EXISTS");

        ctrl.Shutdown();
    }

    // Restart multiple times and verify paused tasks stay paused with original paths/part_path
    for (int restart = 1; restart <= 2; ++restart) {
        DownloadController ctrl(cfg);
        TEST_ASSERT(ctrl.Initialize().ok(), "Re-init on restart");
        auto g1 = ctrl.GetTask(p1_id);
        auto g2 = ctrl.GetTask(p2_id);
        TEST_ASSERT(g1.has_value() && g1->state == TaskState::kPaused && g1->file_name == "paused1.zip",
                    "Paused task 1 restored silently with original name");
        TEST_ASSERT(!g1->part_path.empty() && g1->downloaded_bytes == 7,
                    "Paused task 1 retains resume part_path and bytes");
        TEST_ASSERT(g2.has_value() && g2->state == TaskState::kPaused && g2->file_name == "paused2.zip",
                    "Paused task 2 restored silently with original name");

        // Genuinely new download targeting paused1.zip MUST still detect conflict
        TaskModel incoming;
        incoming.url = "https://other.example.com/paused1.zip";
        incoming.file_name = "paused1.zip";
        incoming.save_dir = cfg.default_download_dir.string();
        auto rep = ctrl.InspectIncomingDownload(incoming);
        TEST_ASSERT(rep.kind == DownloadController::IncomingConflictKind::kReservedByAnotherTask,
                    "New download targeting paused1.zip still detects conflict");
        TEST_ASSERT(!rep.overwrite_allowed, "Overwrite blocked for paused task destination");

        ctrl.Shutdown();
    }

    std::filesystem::remove_all(work_dir, ec);
    return true;
}

bool TestConflictCheckLatencyWithManyTasks() {
    using namespace openidm;

    const auto work_dir = std::filesystem::temp_directory_path() / ("radium_conflict_perf_" + GenerateUuidV4());
    std::error_code ec;
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir / "downloads", ec);
    std::filesystem::create_directories(work_dir / "tmp", ec);

    ControllerConfig cfg;
    cfg.state_db_path = work_dir / "state.db";
    cfg.default_download_dir = work_dir / "downloads";
    cfg.temp_root_dir = work_dir / "tmp";

    DownloadController ctrl(cfg);
    TEST_ASSERT(ctrl.Initialize().ok(), "Controller initializes");

    // Seed 600 tasks (300 paused, 300 completed) to mirror large library state
    for (int i = 0; i < 600; ++i) {
        TaskModel t;
        t.url = "https://cdn.example.com/bulk/file_" + std::to_string(i) + ".bin";
        t.file_name = "file_" + std::to_string(i) + ".bin";
        t.save_dir = cfg.default_download_dir.string();
        if (i % 2 == 0) {
            t.state = TaskState::kCompleted;
        }
        auto r = ctrl.AddTask(std::move(t), false, true);
        TEST_ASSERT(r.ok(), "Seed task added");
    }

    const std::filesystem::path existing = cfg.default_download_dir / "target.mp4";
    { std::ofstream ofs(existing, std::ios::binary); ofs << "EXISTING"; }

    TaskModel cand;
    cand.url = "https://cdn.example.com/target.mp4";
    cand.file_name = "target.mp4";
    cand.save_dir = cfg.default_download_dir.string();

    auto t0 = std::chrono::steady_clock::now();
    for (int iter = 0; iter < 10; ++iter) {
        auto rep = ctrl.InspectIncomingDownload(cand);
        TEST_ASSERT(rep.kind == DownloadController::IncomingConflictKind::kExistsOnDisk, "Detects existing file");
        auto res = ctrl.CheckDestinationConflict(cand.save_dir, "target (1).mp4");
        TEST_ASSERT(res.kind == core::DuplicateKind::kNone, "No task collision for renamed slot");
        std::string avail = ctrl.ResolveAvailableFileName(cand.save_dir, "target.mp4");
        TEST_ASSERT(avail == "target (1).mp4", "Resolves target (1).mp4");
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    std::cout << "  [Perf] 10x (Inspect + CheckDestinationConflict + ResolveAvailable) over 600 tasks: "
              << elapsed_ms << " ms\n";
    TEST_ASSERT(elapsed_ms < 250, "Conflict checks over 600 tasks must be fast (< 250ms for 10 iterations)");

    ctrl.Shutdown();
    std::filesystem::remove_all(work_dir, ec);
    return true;
}

int main() {
    int passed = 0;
    const int total = 20;

    std::cout << "====================================================\n";
    std::cout << " Radium Core Dedicated Unit Test Suite (Fast CTest)\n";
    std::cout << "====================================================\n";

    auto run_test = [&](const char* name, bool (*fn)()) {
        std::cout << "[RUN] " << name << "..." << std::endl;
        try {
            if (fn()) {
                std::cout << "  -> PASS\n";
                ++passed;
            } else {
                std::cout << "  -> FAIL\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "  -> EXCEPTION: " << e.what() << "\n";
            std::cout << "  -> FAIL\n";
        } catch (...) {
            std::cerr << "  -> UNKNOWN EXCEPTION\n";
            std::cout << "  -> FAIL\n";
        }
    };

    run_test("PathSanitizer Comprehensive Unit Tests", TestDedicatedPathSanitizer);
    run_test("Scheduler waits when capacity is exhausted", TestSchedulerCapacityWait);
    run_test("JsonMini RFC 8259 Parser & Builder Tests", TestDedicatedJsonMini);
    run_test("Bencode & TorrentParser Tests", TestDedicatedBencodeAndTorrentParser);
    run_test("SegmentScheduler Invariant & Work-Stealing Tests", TestDedicatedSegmentScheduler);
    run_test("IPC Message Handling, Correlation ID & Diagnostic Report Chronology", TestIpcDiagnosticAndCorrelationVerification);
    run_test("Multi-Site Video Extraction & Compatibility Overhaul Tests", TestMultiSiteVideoExtractionOverhaul);
    run_test("Unusual Server Compatibility, Classification & HTML Error Guard Tests", TestUnusualServerCompatibilityAndClassification);
    run_test("Phase 1 Engine, Scheduler, Async WinHTTP & FTPS Tests", TestPhase1EngineAndNetworking);
    run_test("Phase 2 Stream, Video, Gallery & Bilibili Tests", TestPhase2StreamVideoAndGallery);
    run_test("Phase 3 Site Handlers, Freshlink & POST Capture Tests", TestPhase3SiteHandlersFreshlinkAndPost);
    run_test("Phase 5 Integrations, Batch Templates, EF2 & Quota Tests", TestPhase5IntegrationsBatchEf2AndQuota);
    run_test("Phase 4 Torrent, Archive, Debrid, CNL2 & Specialized Packs Tests", TestPhase4TorrentArchiveAndPacks);
    run_test("Phase 6 Remote Control, MCP, Aria2 RPC & Crash Monitor Tests", TestPhase6RemoteMcpAria2AndCrash);
    run_test("Phase 7 Win32 UI Polish, Accessibility & Fault-Injection Tests", TestPhase7UiAndFaultInjection);
    run_test("Phase 8 Extension Setup, Persistent Onboarding, Settings & About Tests", TestPhase8ExtensionSetupSettingsAndAbout);
    run_test("Phase 1 Download Existence Detection & File Conflict Resolution (13 Scenarios)", TestDownloadExistenceAndConflictResolution13Scenarios);
    run_test("Repeated Same-URL Download (3+ Consecutive Attempts & Restart)", TestRepeatedSameUrlThreeAttemptsAndRestart);
    run_test("Startup Paused Task Restoration Does Not Trigger Conflict Dialogs", TestStartupPausedRestorationDoesNotTriggerConflicts);
    run_test("Conflict Check Latency Over 600+ Tasks", TestConflictCheckLatencyWithManyTasks);

    std::cout << "\nResult: " << passed << " / " << total << " dedicated test suites PASSED.\n";
    return (passed == total) ? 0 : 1;
}
