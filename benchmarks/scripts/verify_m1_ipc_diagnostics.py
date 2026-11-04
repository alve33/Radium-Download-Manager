import json
import os
import struct
import subprocess
import time
from pathlib import Path

ROOT = Path(r"c:\Users\sayed\Downloads\Radium")
RADIUM_BIN = ROOT / "build" / "radium-x64" / "Release" / "radium.exe"
NMH_BIN = ROOT / "build" / "radium-x64" / "Release" / "radium-nmh.exe"

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
    work_dir = ROOT / "benchmarks" / "emp_ipc_test"
    work_dir.mkdir(parents=True, exist_ok=True)
    out_dir = work_dir / "downloads"
    out_dir.mkdir(parents=True, exist_ok=True)
    db_file = work_dir / "state.db"
    if db_file.exists():
        try:
            db_file.unlink()
        except Exception:
            pass

    sock_name = f"radium_emp_test_{os.getpid()}"
    pipe_path = f"\\\\.\\pipe\\{sock_name}"

    print(f"[TEST] Starting Radium Daemon: {RADIUM_BIN}")
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
    env["FLUX_IPC_SOCKET"] = pipe_path
    env["OPENIDM_IPC_SOCKET"] = pipe_path

    print(f"[TEST] Launching NMH: {NMH_BIN}")
    nmh = subprocess.Popen([str(NMH_BIN)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, env=env)

    try:
        # 1. system.handshake
        hs_resp = nmh_rpc(nmh, {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "system.handshake",
            "params": {"protocolVersion": "1.0"}
        })
        print(f"[TEST] Handshake response: {hs_resp}")
        assert hs_resp.get("result", {}).get("protocolVersion") == "1.0", "Protocol version mismatch"

        # 2. download.offer with custom correlationId & clientClickTime
        offer_corr = "empirical-nmh-offer-uuid-42"
        offer_click_epoch = int(time.time() * 1000) - 150
        offer_resp = nmh_rpc(nmh, {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "download.offer",
            "params": {
                "url": "https://sg-lg.regxa.com/500MB.test",
                "filename": "500MB.test",
                "correlationId": offer_corr,
                "clientClickTime": offer_click_epoch,
                "autoStart": False
            }
        })
        print(f"[TEST] download.offer response: {offer_resp}")
        res = offer_resp.get("result", {})
        assert res.get("accepted") is True, "download.offer not accepted"
        assert res.get("correlationId") == offer_corr, f"correlationId mismatch: {res.get('correlationId')} != {offer_corr}"
        offer_task_id = res.get("taskId")
        assert offer_task_id, "taskId empty in download.offer response"

        # 3. stream.add with custom correlationId & clientClickTime
        stream_corr = "empirical-nmh-stream-uuid-99"
        stream_click_epoch = int(time.time() * 1000) - 300
        stream_resp = nmh_rpc(nmh, {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "stream.add",
            "params": {
                "url": "https://www.youtube.com/watch?v=7NWN3wivxhA",
                "sourceType": "YOUTUBE_EXTRACTION",
                "streamType": "YOUTUBE",
                "correlationId": stream_corr,
                "clientClickTime": stream_click_epoch,
                "autoStart": False,
                "filename": "youtube_live_test.mp4",
                "qualityId": "1080p"
            }
        })
        print(f"[TEST] stream.add response: {stream_resp}")
        s_res = stream_resp.get("result", {})
        assert s_res.get("accepted") is True, "stream.add not accepted"
        stream_task_id = s_res.get("taskId")
        assert stream_task_id, "taskId empty in stream.add response"

        # 4. task.list
        list_resp = nmh_rpc(nmh, {
            "jsonrpc": "2.0",
            "id": 4,
            "method": "task.list",
            "params": {}
        })
        tasks = list_resp.get("result", {}).get("tasks", [])
        print(f"[TEST] task.list returned {len(tasks)} tasks")
        found_offer = False
        found_stream = False
        for t in tasks:
            if t.get("taskId") == offer_task_id:
                assert t.get("correlationId") == offer_corr, f"offer task correlationId mismatch: {t.get('correlationId')}"
                found_offer = True
            if t.get("taskId") == stream_task_id:
                assert t.get("correlationId") == stream_corr, f"stream task correlationId mismatch: {t.get('correlationId')}"
                found_stream = True

        assert found_offer, "offer task missing from task.list"
        assert found_stream, "stream task missing from task.list"
        print("[TEST] Verified task.list correlation IDs match exactly via NMH bridge!")

    finally:
        nmh.terminate()
        daemon.terminate()
        try:
            daemon.wait(timeout=2.0)
        except Exception:
            daemon.kill()
        print("[TEST] Processes cleaned up.")

    print("\n>>> ALL NMH TO CORE IPC VERIFICATION CHECKS PASSED SUCCESSFULLY! <<<\n")

if __name__ == "__main__":
    main()
