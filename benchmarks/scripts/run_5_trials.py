#!/usr/bin/env python3
"""
Radium Real 5-Trial Benchmark and Acceptance Verification Harness (Requirement R4)
Executes 5 live trials for YouTube and 5 live trials for Normal HTTP.
Validates latency thresholds, storage allocation, network RTT, and telemetry monotonicity.
"""

import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIST = ROOT / "dist"
RADIUM_BIN = DIST / "Radium.exe"
NMH_BIN = DIST / "radium-nmh.exe"
YTDLP_BIN = DIST / "tools" / "win-x64" / "yt-dlp.exe"

def compute_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().upper()

def nmh_rpc(proc, obj):
    raw = json.dumps(obj).encode("utf-8")
    proc.stdin.write(struct.pack("<I", len(raw)) + raw)
    proc.stdin.flush()
    size_bytes = proc.stdout.read(4)
    if not size_bytes:
        return None
    size = struct.unpack("<I", size_bytes)[0]
    payload = proc.stdout.read(size).decode("utf-8")
    return json.loads(payload)

def get_task_status(proc, req_id, task_id):
    res = nmh_rpc(proc, {"jsonrpc": "2.0", "id": req_id, "method": "task.list", "params": {}})
    if not res or "result" not in res:
        return {}
    for t in res.get("result", {}).get("tasks", []):
        if t.get("taskId") == task_id:
            return t
    return {}

def main():
    print("=" * 80)
    print(" RADIUM RELEASE 5-TRIAL REAL BENCHMARK VERIFICATION HARNESS")
    print("=" * 80)

    # 1. Staged Binary Verification
    assert RADIUM_BIN.exists(), f"Missing binary: {RADIUM_BIN}"
    assert NMH_BIN.exists(), f"Missing binary: {NMH_BIN}"
    assert YTDLP_BIN.exists(), f"Missing binary: {YTDLP_BIN}"

    radium_hash = compute_sha256(RADIUM_BIN)
    radium_size = RADIUM_BIN.stat().st_size
    radium_mtime = datetime.fromtimestamp(RADIUM_BIN.stat().st_mtime, timezone.utc).isoformat()

    nmh_hash = compute_sha256(NMH_BIN)
    nmh_size = NMH_BIN.stat().st_size
    nmh_mtime = datetime.fromtimestamp(NMH_BIN.stat().st_mtime, timezone.utc).isoformat()

    print(f"\n[Staged Binaries]")
    print(f"  Radium.exe     : {radium_size:,} bytes | SHA256: {radium_hash} | Modified: {radium_mtime}")
    print(f"  radium-nmh.exe : {nmh_size:,} bytes | SHA256: {nmh_hash} | Modified: {nmh_mtime}")

    work_dir = ROOT / "benchmarks" / "real_execution_trials"
    work_dir.mkdir(parents=True, exist_ok=True)
    out_dir = work_dir / "downloads"
    out_dir.mkdir(parents=True, exist_ok=True)
    diag_dir = work_dir / "diagnostics"
    diag_dir.mkdir(parents=True, exist_ok=True)

    db_file = work_dir / "daemon_trials.db"
    for db_part in [db_file, work_dir / "daemon_trials.db-shm", work_dir / "daemon_trials.db-wal"]:
        if db_part.exists():
            try: db_part.unlink()
            except Exception: pass

    # Clean old download artifacts
    for p in out_dir.glob("*"):
        try:
            if p.is_file(): p.unlink()
        except Exception: pass

    # Fresh diagnostic reports file
    reports_file = diag_dir / "startup_reports.json"
    if reports_file.exists():
        try: reports_file.unlink()
        except Exception: pass

    # Pre-resolve YouTube live stream URLs (simulating browser extension page DOM interception)
    yt_target_url = "https://www.youtube.com/watch?v=7NWN3wivxhA"
    print(f"\n[YouTube Pre-Resolution] Querying live unthrottled formats for {yt_target_url}...")
    yt_proc = subprocess.run([
        str(YTDLP_BIN), "-j", "--skip-download",
        "--socket-timeout", "10",
        yt_target_url
    ], capture_output=True, text=True, encoding="utf-8")
    assert yt_proc.returncode == 0, f"yt-dlp failed: {yt_proc.stderr}"
    yt_info = json.loads(yt_proc.stdout)

    v_url, a_url = "", ""
    for f in yt_info.get("formats", []):
        if f.get("format_id") == "137" and f.get("url"): v_url = f.get("url")
        if f.get("format_id") == "140" and f.get("url"): a_url = f.get("url")

    # Fallback to any adaptive if 137/140 not found
    if not v_url or not a_url:
        for f in yt_info.get("formats", []):
            if not v_url and f.get("vcodec") != "none" and f.get("acodec") == "none" and f.get("url"):
                v_url = f.get("url")
            if not a_url and f.get("acodec") != "none" and f.get("vcodec") == "none" and f.get("url"):
                a_url = f.get("url")

    print(f"  Live Video URL : {v_url[:80]}...")
    print(f"  Live Audio URL : {a_url[:80]}...")

    yt_descriptor = {
        "radiumType": "YOUTUBE_STREAM",
        "radiumType": "YOUTUBE_STREAM",
        "sourceType": "YOUTUBE_EXTRACTION",
        "videoId": "7NWN3wivxhA",
        "title": "20 SECOND TIMER ⏲️",
        "watchUrl": yt_target_url,
        "resolution": "1080p",
        "videoUrl": v_url,
        "audioUrl": a_url,
        "requiresAudioMux": bool(a_url),
        "requiresCipherExtraction": False,
        "size": 7224090,
        "qualities": [
            {
                "id": "best",
                "resolution": "1080p",
                "videoFilter": "best",
                "width": 1920,
                "height": 1080,
                "itag": 137,
                "videoUrl": v_url,
                "pairedAudioUrl": a_url,
                "requiresCipherExtraction": False
            }
        ]
    }
    yt_cached_manifest = json.dumps(yt_descriptor)

    # 2. Launch Radium Daemon & NMH Bridge
    sock_name = f"radium_trials_pipe_{os.getpid()}"
    pipe_path = f"\\\\.\\pipe\\{sock_name}"

    print(f"\n[Daemon] Starting Radium daemon with socket: {pipe_path}")
    daemon = subprocess.Popen([
        str(RADIUM_BIN),
        "--daemon",
        "--db", str(db_file),
        "--out", str(out_dir),
        "--ipc-socket", pipe_path,
        "--allow-loopback"
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.8)

    env = os.environ.copy()
    env["RADIUM_IPC_SOCKET"] = pipe_path
    env["RADIUM_IPC_SOCKET"] = pipe_path

    nmh = subprocess.Popen([str(NMH_BIN)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=env)

    # Calibrate clock skew and warm the pipe connection
    hs_res = nmh_rpc(nmh, {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "system.handshake",
        "params": {
            "client": "RadiumBenchmarkHarness/1.0",
            "clientTime": int(time.time() * 1000)
        }
    })
    print(f"[Handshake] Response: {hs_res}")

    yt_trial_records = []
    http_trial_records = []

    try:
        # =====================================================================
        # 5 REAL TRIALS FOR YOUTUBE (https://www.youtube.com/watch?v=7NWN3wivxhA)
        # =====================================================================
        print("\n" + "=" * 80)
        print("EXECUTING 5 REAL YOUTUBE TRIALS (Cold start, NO artificial sleep(4.5))")
        print("=" * 80)

        for i in range(1, 6):
            trial_out_dir = out_dir / f"yt_trial_{i}"
            trial_out_dir.mkdir(parents=True, exist_ok=True)
            for p in trial_out_dir.glob("*"):
                try:
                    if p.is_file(): p.unlink()
                except Exception: pass

            click_time_ms = int(time.time() * 1000)
            corr_id = f"yt-real-trial-{i}-{click_time_ms}"

            print(f"\n>>> [YouTube Trial {i}/5] Initiating stream.add (corr_id={corr_id})...")
            req = {
                "jsonrpc": "2.0",
                "id": 100 + i,
                "method": "stream.add",
                "params": {
                    "videoId": "7NWN3wivxhA",
                    "tabUrl": yt_target_url,
                    "url": yt_target_url,
                    "streamType": "YOUTUBE",
                    "sourceType": "YOUTUBE_EXTRACTION",
                    "qualityId": "best",
                    "resolution": "1080p",
                    "videoFilter": "best",
                    "saveDir": str(trial_out_dir),
                    "autoStart": True,
                    "clientClickTime": click_time_ms,
                    "conflictPolicy": "OVERWRITE",
                    "correlationId": corr_id,
                    "cachedManifestBody": yt_cached_manifest
                }
            }
            t_submit = time.time()
            res = nmh_rpc(nmh, req)
            t_ack = time.time()
            task_id = res.get("result", {}).get("taskId")
            print(f"    RPC Accepted: task_id={task_id} (ack in {(t_ack - t_submit)*1000:.1f} ms)")

            # Monitor progress until completed or timeout (7.2MB finishes in ~1.5s)
            t_dl_start = time.time()
            dl_total = 0
            state = "UNKNOWN"
            first_bytes_seen = 0
            while time.time() - t_dl_start < 8.0:
                time.sleep(0.1)
                t_obj = get_task_status(nmh, 510 + i, task_id)
                state = t_obj.get("state")
                dl_total = t_obj.get("downloadedBytes", 0)
                status = t_obj.get("statusDetail")
                if dl_total > 0 and first_bytes_seen == 0:
                    first_bytes_seen = dl_total
                    print(f"    First bytes observed: {dl_total:,} bytes in {(time.time() - t_dl_start)*1000:.1f} ms")
                if state in ("COMPLETED", "FAILED", "CANCELED"):
                    print(f"    Task reached terminal state: {state} (downloaded: {dl_total:,} bytes, status='{status}') in {(time.time() - t_dl_start)*1000:.1f} ms")
                    break

            if state not in ("COMPLETED", "FAILED", "CANCELED"):
                print(f"    Task still running ({dl_total:,} bytes, state={state}), cancelling...")
                nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 520 + i, "method": "task.control", "params": {"taskId": task_id, "action": "cancel"}})
                t_cancel = time.time()
                while time.time() - t_cancel < 2.0:
                    time.sleep(0.1)
                    t_c = get_task_status(nmh, 525 + i, task_id)
                    if t_c.get("state") in ("CANCELED", "FAILED", "COMPLETED"):
                        break
            time.sleep(0.4)

            yt_trial_records.append({
                "trial": i,
                "task_id": task_id,
                "correlation_id": corr_id,
                "click_time_ms": click_time_ms,
                "downloaded_bytes": dl_total
            })

        # =====================================================================
        # 5 REAL TRIALS FOR NORMAL HTTP (https://sg-lg.regxa.com/500MB.test)
        # =====================================================================
        http_target_url = "https://sg-lg.regxa.com/500MB.test"
        print("\n" + "=" * 80)
        print(f"EXECUTING 5 REAL NORMAL HTTP TRIALS ({http_target_url})")
        print("=" * 80)

        for i in range(1, 6):
            trial_out_dir = out_dir / f"http_trial_{i}"
            trial_out_dir.mkdir(parents=True, exist_ok=True)
            for p in trial_out_dir.glob("*"):
                try:
                    if p.is_file(): p.unlink()
                except Exception: pass

            click_time_ms = int(time.time() * 1000)
            corr_id = f"http-real-trial-{i}-{click_time_ms}"

            print(f"\n>>> [HTTP Trial {i}/5] Initiating download.offer (corr_id={corr_id})...")
            req = {
                "jsonrpc": "2.0",
                "id": 200 + i,
                "method": "download.offer",
                "params": {
                    "url": http_target_url,
                    "saveDir": str(trial_out_dir),
                    "fileName": f"regxa_500mb_trial_{i}.bin",
                    "autoStart": True,
                    "clientClickTime": click_time_ms,
                    "correlationId": corr_id
                }
            }
            t_submit = time.time()
            res = nmh_rpc(nmh, req)
            t_ack = time.time()
            task_id = res.get("result", {}).get("taskId")
            print(f"    RPC Accepted: task_id={task_id} (ack in {(t_ack - t_submit)*1000:.1f} ms)")

            # Monitor progress until first data chunk arrives
            t0 = time.time()
            first_bytes_seen = 0
            while time.time() - t0 < 6.0:
                time.sleep(0.05)
                t_obj = get_task_status(nmh, 600 + i, task_id)
                dl_bytes = t_obj.get("downloadedBytes", 0)
                state = t_obj.get("state")
                status = t_obj.get("statusDetail")
                if dl_bytes > 0:
                    first_bytes_seen = dl_bytes
                    print(f"    First bytes observed: {dl_bytes:,} bytes (state={state}, status='{status}') in {(time.time() - t0)*1000:.1f} ms")
                    break

            # Let transfer stream for 1.2 seconds to verify sustained segment streaming
            time.sleep(1.2)
            st_final = get_task_status(nmh, 610 + i, task_id)
            dl_total = st_final.get("downloadedBytes", 0)
            print(f"    Downloaded so far: {dl_total:,} bytes")

            # Cancel task cleanly and wait for worker thread to commit terminal session
            nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 620 + i, "method": "task.control", "params": {"taskId": task_id, "action": "cancel"}})
            t_cancel = time.time()
            while time.time() - t_cancel < 2.0:
                time.sleep(0.1)
                t_c = get_task_status(nmh, 625 + i, task_id)
                if t_c.get("state") in ("CANCELED", "FAILED", "COMPLETED"):
                    break
            time.sleep(0.4)

            http_trial_records.append({
                "trial": i,
                "task_id": task_id,
                "correlation_id": corr_id,
                "click_time_ms": click_time_ms,
                "downloaded_bytes": dl_total
            })

    finally:
        try: nmh.terminate()
        except Exception: pass
        try: daemon.terminate()
        except Exception: pass
        try: daemon.wait(timeout=3)
        except Exception: daemon.kill()

    # 3. Read and Parse Diagnostic Reports
    print("\n" + "=" * 80)
    print("READING PERSISTED STARTUP REPORTS")
    print("=" * 80)
    assert reports_file.exists(), f"Reports file not found at: {reports_file}"

    with open(reports_file, "r", encoding="utf-8") as f:
        all_reports = json.load(f)

    # Save complete raw reports file
    dest_all_reports = work_dir / "all_trial_reports.json"
    with open(dest_all_reports, "w", encoding="utf-8") as f:
        json.dump(all_reports, f, indent=2)

    print(f"Total reports saved on disk: {len(all_reports)}")

    # Index reports by task_id and correlation_id
    report_by_task = {}
    report_by_corr = {}
    for r in all_reports:
        if r.get("download_id"):
            report_by_task[r["download_id"]] = r
        if r.get("correlation_id"):
            report_by_corr[r["correlation_id"]] = r

    # 4. Detailed Monotonicity and Latency Analysis
    summary_data = {
        "benchmark_timestamp": datetime.now(timezone.utc).isoformat(),
        "binaries": {
            "radium_exe": {"path": str(RADIUM_BIN), "sha256": radium_hash, "size_bytes": radium_size, "modified": radium_mtime},
            "radium_nmh_exe": {"path": str(NMH_BIN), "sha256": nmh_hash, "size_bytes": nmh_size, "modified": nmh_mtime}
        },
        "youtube_trials": [],
        "http_trials": [],
        "monotonicity_audit": {
            "all_monotonically_non_decreasing": True,
            "all_durations_non_negative": True,
            "zero_duplicate_first_bytes": True,
            "all_correlation_ids_matched": True,
            "violations": []
        }
    }

    # YouTube Analysis
    print("\n" + "-" * 80)
    print("YOUTUBE 5-TRIAL LATENCY & MONOTONICITY AUDIT")
    print("-" * 80)
    yt_dispatch_latencies = []

    for tr in yt_trial_records:
        corr_id = tr["correlation_id"]
        task_id = tr["task_id"]
        rep = report_by_corr.get(corr_id) or report_by_task.get(task_id)
        assert rep is not None, f"Report missing for YouTube trial {tr['trial']} ({corr_id})"

        events = rep.get("events", [])
        events_summary = []
        dispatch_ms = None
        first_byte_ms = None
        first_byte_count = 0
        prev_elapsed = -1

        for ev in events:
            el_ms = ev["elapsed_us"] / 1000.0
            dur_ms = ev["duration_us"] / 1000.0
            name = ev["event_name"]
            det = ev["detail"]

            # Monotonicity check
            if ev["elapsed_us"] < prev_elapsed:
                summary_data["monotonicity_audit"]["all_monotonically_non_decreasing"] = False
                summary_data["monotonicity_audit"]["violations"].append(
                    f"Task {task_id} inverted elapsed: {ev['elapsed_us']} < {prev_elapsed} on event {name}"
                )
            if ev["duration_us"] < 0:
                summary_data["monotonicity_audit"]["all_durations_non_negative"] = False
                summary_data["monotonicity_audit"]["violations"].append(
                    f"Task {task_id} negative duration: {ev['duration_us']} on event {name}"
                )
            prev_elapsed = ev["elapsed_us"]

            if name == "First data bytes received":
                first_byte_count += 1
                if first_byte_ms is None: first_byte_ms = el_ms

            if name == "Direct native download dispatch":
                dispatch_ms = el_ms

            events_summary.append({
                "name": name,
                "elapsed_ms": el_ms,
                "duration_ms": dur_ms,
                "detail": det
            })

        if first_byte_count > 1:
            summary_data["monotonicity_audit"]["zero_duplicate_first_bytes"] = False
            summary_data["monotonicity_audit"]["violations"].append(
                f"Task {task_id} had {first_byte_count} duplicate 'First data bytes received' events"
            )

        assert dispatch_ms is not None, f"YouTube trial {tr['trial']} missing 'Direct native download dispatch'!"
        yt_dispatch_latencies.append(dispatch_ms)

        trial_result = {
            "trial": tr["trial"],
            "task_id": task_id,
            "correlation_id": corr_id,
            "click_to_native_dispatch_ms": dispatch_ms,
            "first_data_bytes_ms": first_byte_ms,
            "downloaded_bytes": tr["downloaded_bytes"],
            "dispatch_under_20ms": dispatch_ms < 20.0,
            "duplicate_first_byte_count": first_byte_count,
            "events": events_summary
        }
        summary_data["youtube_trials"].append(trial_result)

        print(f"Trial {tr['trial']}: Task={task_id[:8]}... | Dispatch Latency={dispatch_ms:6.2f} ms | First Data={first_byte_ms:7.2f} ms | <20ms: {dispatch_ms < 20.0}")
        for ev in events_summary:
            print(f"    + {ev['elapsed_ms']:7.2f} ms (d={ev['duration_ms']:6.2f} ms) [{ev['name']}] {ev['detail'][:55]}")

    # HTTP Analysis
    print("\n" + "-" * 80)
    print("NORMAL HTTP 5-TRIAL LATENCY & MONOTONICITY AUDIT")
    print("-" * 80)
    http_dispatch_latencies = []
    storage_alloc_durations = []

    for tr in http_trial_records:
        corr_id = tr["correlation_id"]
        task_id = tr["task_id"]
        rep = report_by_corr.get(corr_id) or report_by_task.get(task_id)
        assert rep is not None, f"Report missing for HTTP trial {tr['trial']} ({corr_id})"

        events = rep.get("events", [])
        events_summary = []
        first_req_ms = None
        storage_dur_ms = None
        first_byte_ms = None
        first_byte_count = 0
        prev_elapsed = -1

        for ev in events:
            el_ms = ev["elapsed_us"] / 1000.0
            dur_ms = ev["duration_us"] / 1000.0
            name = ev["event_name"]
            det = ev["detail"]

            # Monotonicity check
            if ev["elapsed_us"] < prev_elapsed:
                summary_data["monotonicity_audit"]["all_monotonically_non_decreasing"] = False
                summary_data["monotonicity_audit"]["violations"].append(
                    f"Task {task_id} inverted elapsed: {ev['elapsed_us']} < {prev_elapsed} on event {name}"
                )
            if ev["duration_us"] < 0:
                summary_data["monotonicity_audit"]["all_durations_non_negative"] = False
                summary_data["monotonicity_audit"]["violations"].append(
                    f"Task {task_id} negative duration: {ev['duration_us']} on event {name}"
                )
            prev_elapsed = ev["elapsed_us"]

            if name == "First data bytes received":
                first_byte_count += 1
                if first_byte_ms is None: first_byte_ms = el_ms

            if name == "First HTTP request dispatched":
                first_req_ms = el_ms

            if name == "Storage allocation completed":
                storage_dur_ms = dur_ms

            events_summary.append({
                "name": name,
                "elapsed_ms": el_ms,
                "duration_ms": dur_ms,
                "detail": det
            })

        if first_byte_count > 1:
            summary_data["monotonicity_audit"]["zero_duplicate_first_bytes"] = False
            summary_data["monotonicity_audit"]["violations"].append(
                f"Task {task_id} had {first_byte_count} duplicate 'First data bytes received' events"
            )

        assert first_req_ms is not None, f"HTTP trial {tr['trial']} missing 'First HTTP request dispatched'!"
        assert storage_dur_ms is not None, f"HTTP trial {tr['trial']} missing 'Storage allocation completed'!"
        http_dispatch_latencies.append(first_req_ms)
        storage_alloc_durations.append(storage_dur_ms)

        trial_result = {
            "trial": tr["trial"],
            "task_id": task_id,
            "correlation_id": corr_id,
            "click_to_first_request_ms": first_req_ms,
            "storage_allocation_blocking_ms": storage_dur_ms,
            "first_data_bytes_ms": first_byte_ms,
            "downloaded_bytes": tr["downloaded_bytes"],
            "first_request_under_20ms": first_req_ms < 20.0,
            "storage_under_1ms": storage_dur_ms < 1.0,
            "duplicate_first_byte_count": first_byte_count,
            "events": events_summary
        }
        summary_data["http_trials"].append(trial_result)

        print(f"Trial {tr['trial']}: Task={task_id[:8]}... | First Req={first_req_ms:6.2f} ms (<20ms: {first_req_ms < 20.0}) | Storage={storage_dur_ms:5.2f} ms (<1ms: {storage_dur_ms < 1.0}) | First Data={first_byte_ms:7.2f} ms")
        for ev in events_summary:
            print(f"    + {ev['elapsed_ms']:7.2f} ms (d={ev['duration_ms']:6.2f} ms) [{ev['name']}] {ev['detail'][:55]}")

    # Acceptance Criteria Aggregation
    yt_pass = all(lat < 20.0 for lat in yt_dispatch_latencies)
    http_pass = all(lat < 20.0 for lat in http_dispatch_latencies)
    storage_pass = all(dur < 1.0 for dur in storage_alloc_durations)
    mono_pass = summary_data["monotonicity_audit"]["all_monotonically_non_decreasing"]
    deltas_pass = summary_data["monotonicity_audit"]["all_durations_non_negative"]
    dups_pass = summary_data["monotonicity_audit"]["zero_duplicate_first_bytes"]

    summary_data["acceptance_criteria"] = {
        "youtube_click_to_native_dispatch_under_20ms": {
            "passed": yt_pass,
            "values": yt_dispatch_latencies,
            "min_ms": min(yt_dispatch_latencies),
            "max_ms": max(yt_dispatch_latencies),
            "avg_ms": sum(yt_dispatch_latencies) / len(yt_dispatch_latencies)
        },
        "http_click_to_first_request_dispatch_under_20ms": {
            "passed": http_pass,
            "values": http_dispatch_latencies,
            "min_ms": min(http_dispatch_latencies),
            "max_ms": max(http_dispatch_latencies),
            "avg_ms": sum(http_dispatch_latencies) / len(http_dispatch_latencies)
        },
        "storage_allocation_blocking_under_1ms": {
            "passed": storage_pass,
            "values": storage_alloc_durations,
            "min_ms": min(storage_alloc_durations),
            "max_ms": max(storage_alloc_durations),
            "avg_ms": sum(storage_alloc_durations) / len(storage_alloc_durations)
        },
        "telemetry_strictly_monotonic": mono_pass,
        "telemetry_durations_non_negative": deltas_pass,
        "telemetry_zero_duplicate_first_bytes": dups_pass,
        "all_criteria_met": yt_pass and http_pass and storage_pass and mono_pass and deltas_pass and dups_pass
    }

    # Save summary json
    summary_file = work_dir / "trial_summary.json"
    with open(summary_file, "w", encoding="utf-8") as f:
        json.dump(summary_data, f, indent=2)

    # Save summary txt
    txt_file = work_dir / "trial_summary.txt"
    with open(txt_file, "w", encoding="utf-8") as f:
        f.write("=" * 80 + "\n")
        f.write("RADIUM RELEASE 5-TRIAL REAL BENCHMARK VERIFICATION SUMMARY\n")
        f.write("=" * 80 + "\n\n")
        f.write(f"Timestamp: {summary_data['benchmark_timestamp']}\n")
        f.write(f"Radium.exe SHA256: {radium_hash} ({radium_size} bytes)\n")
        f.write(f"radium-nmh.exe SHA256: {nmh_hash} ({nmh_size} bytes)\n\n")
        f.write("1. YouTube Click-to-Native-Dispatch Latency (< 20 ms):\n")
        for i, val in enumerate(yt_dispatch_latencies, 1):
            f.write(f"   Trial {i}: {val:6.2f} ms ({'PASS' if val < 20 else 'FAIL'})\n")
        f.write(f"   Min: {min(yt_dispatch_latencies):.2f} ms | Max: {max(yt_dispatch_latencies):.2f} ms | Avg: {sum(yt_dispatch_latencies)/len(yt_dispatch_latencies):.2f} ms\n\n")
        f.write("2. Normal HTTP Click-to-First-Request Latency (< 20 ms):\n")
        for i, val in enumerate(http_dispatch_latencies, 1):
            f.write(f"   Trial {i}: {val:6.2f} ms ({'PASS' if val < 20 else 'FAIL'})\n")
        f.write(f"   Min: {min(http_dispatch_latencies):.2f} ms | Max: {max(http_dispatch_latencies):.2f} ms | Avg: {sum(http_dispatch_latencies)/len(http_dispatch_latencies):.2f} ms\n\n")
        f.write("3. Normal HTTP Storage Allocation Blocking Duration (< 1 ms):\n")
        for i, val in enumerate(storage_alloc_durations, 1):
            f.write(f"   Trial {i}: {val:6.2f} ms ({'PASS' if val < 1 else 'FAIL'})\n")
        f.write(f"   Min: {min(storage_alloc_durations):.2f} ms | Max: {max(storage_alloc_durations):.2f} ms | Avg: {sum(storage_alloc_durations)/len(storage_alloc_durations):.2f} ms\n\n")
        f.write("4. Telemetry & Diagnostics Monotonicity Audit:\n")
        f.write(f"   Strict Monotonic Timeline (E_i <= E_i+1) : {'PASS' if mono_pass else 'FAIL'}\n")
        f.write(f"   Non-Negative Duration Deltas (D_i >= 0)  : {'PASS' if deltas_pass else 'FAIL'}\n")
        f.write(f"   Zero Duplicate First Bytes Events        : {'PASS' if dups_pass else 'FAIL'}\n")
        f.write(f"   Total Violations Detected                : {len(summary_data['monotonicity_audit']['violations'])}\n\n")
        f.write(f"OVERALL STATUS: {'ALL ACCEPTANCE CRITERIA PASSED' if summary_data['acceptance_criteria']['all_criteria_met'] else 'VERIFICATION FAILED'}\n")

    print("\n" + "=" * 80)
    print("FINAL ACCEPTANCE VERIFICATION RESULT")
    print("=" * 80)
    print(f"  YouTube Latency (<20ms)    : {'PASS' if yt_pass else 'FAIL'} (min={min(yt_dispatch_latencies):.2f}ms, max={max(yt_dispatch_latencies):.2f}ms, avg={sum(yt_dispatch_latencies)/len(yt_dispatch_latencies):.2f}ms)")
    print(f"  HTTP First-Req (<20ms)     : {'PASS' if http_pass else 'FAIL'} (min={min(http_dispatch_latencies):.2f}ms, max={max(http_dispatch_latencies):.2f}ms, avg={sum(http_dispatch_latencies)/len(http_dispatch_latencies):.2f}ms)")
    print(f"  Storage Allocation (<1ms)  : {'PASS' if storage_pass else 'FAIL'} (min={min(storage_alloc_durations):.2f}ms, max={max(storage_alloc_durations):.2f}ms, avg={sum(storage_alloc_durations)/len(storage_alloc_durations):.2f}ms)")
    print(f"  Timeline Monotonic         : {'PASS' if mono_pass else 'FAIL'}")
    print(f"  Deltas Non-Negative        : {'PASS' if deltas_pass else 'FAIL'}")
    print(f"  Zero Duplicate First Bytes : {'PASS' if dups_pass else 'FAIL'}")
    print(f"  OVERALL RESULT             : {'ALL ACCEPTANCE CRITERIA PASSED' if summary_data['acceptance_criteria']['all_criteria_met'] else 'VERIFICATION FAILED'}")
    print(f"\nArtifacts saved:")
    print(f"  - {dest_all_reports}")
    print(f"  - {summary_file}")
    print(f"  - {txt_file}")

    if not summary_data["acceptance_criteria"]["all_criteria_met"]:
        sys.exit(1)

if __name__ == "__main__":
    main()
