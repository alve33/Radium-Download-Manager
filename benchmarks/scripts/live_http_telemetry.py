import json
import os
import subprocess
import time
from pathlib import Path

ROOT = Path(r"c:\Users\sayed\Downloads\Radium")
RADIUM_BIN = ROOT / "dist" / "Radium.exe"

def run_test():
    work_dir = ROOT / "benchmarks" / "live_startup_test"
    work_dir.mkdir(parents=True, exist_ok=True)
    out_dir = work_dir / "dl"
    out_dir.mkdir(parents=True, exist_ok=True)
    db_file = work_dir / "state.db"
    if db_file.exists():
        db_file.unlink()
    
    sock_name = f"radium_live_test_{os.getpid()}"
    pipe_path = f"\\\\.\\pipe\\{sock_name}"

    print(f"Starting Radium Daemon: {RADIUM_BIN}")
    proc = subprocess.Popen([
        str(RADIUM_BIN),
        "--daemon",
        "--db", str(db_file),
        "--out", str(out_dir),
        "--ipc-socket", pipe_path,
        "--allow-loopback"
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.6)

    try:
        # Open pipe via python
        with open(pipe_path, "r+b", buffering=0) as pipe:
            click_epoch_ms = int(time.time() * 1000)
            rpc = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "download.offer",
                "params": {
                    "url": "https://fsn1-speed.hetzner.com/100MB.bin",
                    "saveDir": str(out_dir),
                    "fileName": "100MB.bin",
                    "autoStart": True,
                    "clientClickTime": click_epoch_ms
                }
            }
            raw = json.dumps(rpc).encode("utf-8") + b"\n"
            pipe.write(raw)
            resp_line = pipe.readline().decode("utf-8")
            print("download.offer response:", resp_line.strip())
            
            # Let it run for 3 seconds to gather telemetry
            time.sleep(3.0)

            # Query startup report
            rpc_diag = {
                "jsonrpc": "2.0",
                "id": 2,
                "method": "diagnostics.getReports",
                "params": {}
            }
            pipe.write(json.dumps(rpc_diag).encode("utf-8") + b"\n")
            diag_resp = pipe.readline().decode("utf-8")
            print("diagnostics response received")

            # App shutdown
            pipe.write(json.dumps({"jsonrpc": "2.0", "id": 3, "method": "app.shutdown"}).encode("utf-8") + b"\n")
            pipe.readline()

            return json.loads(diag_resp)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except Exception:
            proc.kill()

if __name__ == "__main__":
    res = run_test()
    reports = res.get("result", {}).get("reports", [])
    print(f"Total reports: {len(reports)}")
    for r in reports:
        print(f"Download: {r.get('filename')} ({r.get('download_type')}) - Startup Time: {r.get('startup_time_ms')} ms")
        for ev in r.get("events", []):
            print(f"  [{ev.get('elapsed_ms')} ms] (delta {ev.get('duration_ms')} ms) {ev.get('event_name')}: {ev.get('detail')}")
