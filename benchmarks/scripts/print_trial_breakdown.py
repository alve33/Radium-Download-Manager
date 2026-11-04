import json
from pathlib import Path

report_path = Path(r"benchmarks\real_execution_trials\diagnostics\startup_reports.json")
if not report_path.exists():
    print("Report file not found")
    exit(1)

with open(report_path, "r", encoding="utf-8") as f:
    reps = json.load(f)

print(f"Total recorded sessions: {len(reps)}\n")
for i, r in enumerate(reps, 1):
    dt = r.get("download_type", "")
    fn = r.get("filename", "").encode("ascii", "replace").decode("ascii")
    st = r.get("startup_time_ms", 0.0)
    status = r.get("status", "")
    print("=" * 80)
    print(f"[{i}] {dt} -> {fn} | Startup Time: {st:.2f} ms | Status: {status}")
    print(f"    URL: {r.get('download_url')}")
    print("  Timeline:")
    for ev in r.get("events", []):
        el_ms = ev.get("elapsed_us", 0) / 1000.0
        dur_ms = ev.get("duration_us", 0) / 1000.0
        name = ev.get("event_name", "")
        detail = ev.get("detail", "").encode("ascii", "replace").decode("ascii")
        print(f"    + {el_ms:8.2f} ms (delta: {dur_ms:7.2f} ms) [{name}] {detail}")
    print()
