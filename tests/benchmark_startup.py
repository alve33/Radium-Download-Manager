"""Controlled CLI startup measurement; no internet and SHA-256 verified output."""
import argparse
import hashlib
import http.server
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    payload = bytes(range(256)) * 4096
    requests = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            requests.append(time.perf_counter())
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            try:
                self.wfile.write(payload)
            except (ConnectionError, OSError):
                pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    results = []
    try:
        for trial in range(5):
            with tempfile.TemporaryDirectory(prefix="radium_startup_") as temp:
                root = Path(temp)
                requests.clear()
                start = time.perf_counter()
                result = subprocess.run(
                    [str(Path(args.binary).resolve()), "--db", str(root / "state.db"),
                     "--out", str(root), "--allow-loopback", "--download",
                     f"http://127.0.0.1:{server.server_port}/payload.bin"],
                    capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr.decode(errors="replace")
                assert hashlib.sha256((root / "payload.bin").read_bytes()).digest() == hashlib.sha256(payload).digest()
                reports = json.loads((root / "diagnostics" / "startup_reports.json").read_text())
                events = {e["event_name"]: e["elapsed_us"] / 1000 for e in reports[-1]["events"]}
                results.append({"trial": trial, "launch_to_server_request_ms": (requests[0] - start) * 1000,
                                "events_ms": events, "sha256_verified": True})
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    Path(args.output).write_text(json.dumps(results, indent=2))
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
