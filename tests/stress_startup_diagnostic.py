#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Empirical Verification & Stress Test Runner for StartupDiagnosticRecorder (Milestone 1)
Executes native C++20 multithreaded stress harness and verifies telemetry invariants:
1. Multithreaded concurrent event recording across 16 threads for same session.
2. Invariant verification: E_i <= E_{i+1} (monotonicity) and D_i >= 0 (non-negative duration deltas).
3. Edge case testing: client_click_epoch_ms future, 0, hours past, limits.
4. Strict deduplication of 'First data bytes received' events.
5. IngestExtensionStages out-of-order reordering and serialization round-trip.
"""

import os
import sys
import subprocess
import time
import json

def run_stress_harness(trials=5):
    exe_path = os.path.join(os.getcwd(), "build", "test_startup_diagnostic_stress.exe")
    if not os.path.exists(exe_path):
        print(f"[FAIL] Test executable not found at: {exe_path}", file=sys.stderr)
        return False

    print(f"============================================================")
    print(f" StartupDiagnosticRecorder Stress Verification ({trials} Trials)")
    print(f" Executable: {exe_path}")
    print(f"============================================================")

    all_passed = True
    total_duration_ms = 0.0

    for trial in range(1, trials + 1):
        t0 = time.perf_counter()
        proc = subprocess.run([exe_path], capture_output=True, text=True)
        t1 = time.perf_counter()
        dur_ms = (t1 - t0) * 1000.0
        total_duration_ms += dur_ms

        if proc.returncode != 0:
            print(f"[TRIAL {trial}] FAILED with exit code {proc.returncode}")
            print(f"STDOUT:\n{proc.stdout}")
            print(f"STDERR:\n{proc.stderr}")
            all_passed = False
            break
        else:
            # Check stdout for all suites pass message
            if "7 / 7 suites PASSED" in proc.stdout:
                print(f"[TRIAL {trial}/{trials}] PASS ({dur_ms:.1f} ms) — 7/7 stress suites verified")
            else:
                print(f"[TRIAL {trial}/{trials}] FAILED — unexpected output:\n{proc.stdout}")
                all_passed = False
                break

    avg_ms = total_duration_ms / trials if trials > 0 else 0
    print(f"============================================================")
    print(f" Stress Test Verdict: {'PASS' if all_passed else 'FAIL'}")
    print(f" Average Trial Execution Time: {avg_ms:.2f} ms")
    print(f" Total Trials Run: {trials}")
    print(f"============================================================")
    return all_passed

if __name__ == "__main__":
    success = run_stress_harness(trials=5)
    sys.exit(0 if success else 1)
