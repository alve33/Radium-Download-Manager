import json
import os
import struct
import subprocess
import time
from pathlib import Path

ROOT = Path(r"c:\Users\sayed\Downloads\Radium")
RADIUM_BIN = ROOT / "dist" / "Radium.exe"
NMH_BIN = ROOT / "dist" / "radium-nmh.exe"

def nmh_rpc(proc, obj):
    raw = json.dumps(obj).encode("utf-8")
    proc.stdin.write(struct.pack("<I", len(raw)) + raw)
    proc.stdin.flush()
    size_bytes = proc.stdout.read(4)
    if not size_bytes:
        return None
    size = struct.unpack("<I", size_bytes)[0]
    return json.loads(proc.stdout.read(size).decode("utf-8"))

def main():
    work_dir = ROOT / "benchmarks" / "real_execution_trials"
    out_dir = work_dir / "downloads"
    out_dir.mkdir(parents=True, exist_ok=True)
    db_file = work_dir / "test_live.db"
    if db_file.exists():
        try: db_file.unlink()
        except Exception: pass

    sock_name = f"radium_live_test_{os.getpid()}"
    pipe_path = f"\\\\.\\pipe\\{sock_name}"

    print(f"Launching Radium Daemon: {RADIUM_BIN}")
    daemon = subprocess.Popen([
        str(RADIUM_BIN),
        "--daemon",
        "--db", str(db_file),
        "--out", str(out_dir),
        "--ipc-socket", pipe_path,
        "--allow-loopback"
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.6)

    env = os.environ.copy()
    env["RADIUM_IPC_SOCKET"] = pipe_path
    env["FLUX_IPC_SOCKET"] = pipe_path
    env["OPENIDM_IPC_SOCKET"] = pipe_path

    nmh = subprocess.Popen([str(NMH_BIN)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=env)

    try:
        # 1. Test Normal HTTP Early GET & Streaming
        http_url = "https://sg-lg.regxa.com/500MB.test"
        click_ms = int(time.time() * 1000)
        corr_id = f"test-http-{click_ms}"
        print(f"\n[HTTP Test] Offering {http_url}...")
        res = nmh_rpc(nmh, {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "download.offer",
            "params": {
                "url": http_url,
                "saveDir": str(out_dir),
                "fileName": "test_500MB.bin",
                "autoStart": True,
                "clientClickTime": click_ms,
                "correlationId": corr_id
            }
        })
        print(f"Offer response: {res}")
        task_id = res["result"]["taskId"]

        # Wait for first data bytes
        t0 = time.time()
        while time.time() - t0 < 8.0:
            time.sleep(0.1)
            st = nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 2, "method": "task.get", "params": {"taskId": task_id}})
            t_obj = st.get("result", {}).get("task", {})
            dl_bytes = t_obj.get("downloadedBytes", 0)
            if dl_bytes > 0:
                print(f"HTTP Task {task_id}: First data received ({dl_bytes} bytes, state={t_obj.get('state')})")
                break

        # Cancel HTTP task
        nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 3, "method": "task.control", "params": {"taskId": task_id, "action": "cancel"}})

        # 2. Test YouTube Stream
        print(f"\n[YouTube Test] Fetching metadata for 7NWN3wivxhA...")
        yt_p = subprocess.run([str(ROOT / "dist" / "tools" / "win-x64" / "yt-dlp.exe"),
                              "-j", "--skip-download", "https://www.youtube.com/watch?v=7NWN3wivxhA"],
                             capture_output=True, text=True, encoding="utf-8")
        yt_info = json.loads(yt_p.stdout)
        v_url, a_url = "", ""
        for f in yt_info.get("formats", []):
            if f.get("format_id") == "137" and f.get("url"): v_url = f.get("url")
            if f.get("format_id") == "140" and f.get("url"): a_url = f.get("url")

        descriptor = {
            "radiumType": "YOUTUBE_STREAM",
            "fluxType": "YOUTUBE_STREAM",
            "sourceType": "YOUTUBE_EXTRACTION",
            "videoId": "7NWN3wivxhA",
            "title": "20 SECOND TIMER ⏲️",
            "watchUrl": "https://www.youtube.com/watch?v=7NWN3wivxhA",
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

        yt_click_ms = int(time.time() * 1000)
        yt_corr_id = f"test-yt-{yt_click_ms}"
        print(f"[YouTube Test] Sending stream.add with cachedManifestBody...")
        yt_res = nmh_rpc(nmh, {
            "jsonrpc": "2.0",
            "id": 10,
            "method": "stream.add",
            "params": {
                "videoId": "7NWN3wivxhA",
                "tabUrl": "https://www.youtube.com/watch?v=7NWN3wivxhA",
                "url": "https://www.youtube.com/watch?v=7NWN3wivxhA",
                "streamType": "YOUTUBE",
                "sourceType": "YOUTUBE_EXTRACTION",
                "qualityId": "best",
                "resolution": "1080p",
                "videoFilter": "best",
                "saveDir": str(out_dir),
                "autoStart": True,
                "clientClickTime": yt_click_ms,
                "conflictPolicy": "OVERWRITE",
                "correlationId": yt_corr_id,
                "cachedManifestBody": json.dumps(descriptor)
            }
        })
        print(f"stream.add response: {yt_res}")
        yt_task_id = yt_res["result"]["taskId"]

        t0 = time.time()
        while time.time() - t0 < 8.0:
            time.sleep(0.1)
            st = nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 11, "method": "task.get", "params": {"taskId": yt_task_id}})
            t_obj = st.get("result", {}).get("task", {})
            dl_bytes = t_obj.get("downloadedBytes", 0)
            if dl_bytes > 0:
                print(f"YouTube Task {yt_task_id}: First data received ({dl_bytes} bytes, state={t_obj.get('state')})")
                break

        nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 12, "method": "task.control", "params": {"taskId": yt_task_id, "action": "cancel"}})

        # 3. Retrieve and inspect diagnostic reports
        diag_res = nmh_rpc(nmh, {"jsonrpc": "2.0", "id": 20, "method": "diagnostics.getReports", "params": {}})
        reports = diag_res.get("result", {}).get("reports", [])
        print(f"\n=== DIAGNOSTIC REPORTS ({len(reports)} total) ===")
        for r in reports:
            print("=" * 80)
            print(f"Task: {r.get('task_id')} | Corr: {r.get('correlation_id')} | Type: {r.get('download_type')}")
            for ev in r.get("events", []):
                el = ev.get("elapsed_us", 0) / 1000.0
                dur = ev.get("duration_us", 0) / 1000.0
                name = ev.get("event_name")
                det = ev.get("detail")
                print(f"  + {el:8.2f} ms (d={dur:7.2f} ms) [{name}] {det}")

    finally:
        nmh.terminate()
        daemon.terminate()
        try: daemon.wait(timeout=2)
        except Exception: daemon.kill()

if __name__ == "__main__":
    main()
