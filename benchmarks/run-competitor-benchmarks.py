#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Radium Download Manager — Competitor Benchmark Runner & Analysis Harness
# Executes controlled, repeatable performance comparisons on Windows runner against:
#   1. Radium Download Manager (8 connections)
#   2. Radium Download Manager (32 connections)
#   3. aria2 1.37.0 (16 connections)
#   4. Internet Download Manager (IDM 6.42 Build 64)
#   5. Free Download Manager (FDM 6.34.4)
#
# Generates:
#   - benchmarks/latest_comparison.json
#   - benchmarks/latest_comparison.csv
#   - benchmarks/LATEST_COMPARISON.md

import argparse
import os
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = ROOT / "scripts"


def main():
    parser = argparse.ArgumentParser(description="Radium Download Manager v1.0.0 Competitor Benchmark Harness")
    parser.add_argument("--out-dir", type=str, default=None, help="Output directory for reports (default: benchmarks/)")
    parser.add_argument("--extended", action="store_true", help="Run 5 trials instead of 3")
    parser.add_argument("--radium-bin", dest="radium_bin", type=str, default=None, help="Path to radium.exe")
    parser.add_argument("--mode", choices=["local", "hosted", "auto"], default="auto",
                        help="Execution mode: 'local' benchmarks installed IDM & FDM; 'hosted' is for GitHub Actions")
    args = parser.parse_args()

    target_script = SCRIPTS_DIR / "run-competitor-benchmarks.py"
    if not target_script.exists():
        print(f"[Radium Benchmark] Error: Harness script not found at {target_script}", file=sys.stderr)
        sys.exit(1)

    cmd = [sys.executable, str(target_script), "--mode", args.mode]
    if args.out_dir:
        cmd.extend(["--out-dir", args.out_dir])
    if args.extended:
        cmd.append("--extended")
    if args.radium_bin:
        cmd.extend(["--radium-bin", args.radium_bin])

    print("[Radium Benchmark] Launching Radium Download Manager v1.0.0 Competitor Benchmark Harness...")
    res = subprocess.run(cmd, cwd=str(ROOT))
    sys.exit(res.returncode)


if __name__ == "__main__":
    main()
