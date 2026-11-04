import json
from pathlib import Path

report_path = Path(r"benchmarks\real_execution_trials\diagnostics\startup_reports.json")
with open(report_path, "r", encoding="utf-8") as f:
    reps = json.load(f)

# The last 10 sessions correspond to the 5 YouTube trials and 5 HTTP trials from the last run (indices 12 to 21)
print(f"Total sessions: {len(reps)}")
last_10 = reps[-10:]

for idx, r in enumerate(last_10, 1):
    kind = "YouTube Trial" if r.get("download_type") in ("HLS", "YOUTUBE_EXTRACTION") else "Normal HTTP Trial"
    fn = r.get("filename", "").encode("ascii", "replace").decode("ascii")
    print("=" * 80)
    print(f"{kind} [{idx}]: Task ID={r.get('download_id')} | File={fn}")
    print(f"URL: {r.get('download_url')}")
    for ev in r.get("events", []):
        el = ev.get("elapsed_us", 0) / 1000.0
        dur = ev.get("duration_us", 0) / 1000.0
        name = ev.get("event_name")
        detail = ev.get("detail", "").encode("ascii", "replace").decode("ascii")
        print(f"  + {el:7.2f} ms (d={dur:6.2f} ms) [{name}] {detail}")
    print()
