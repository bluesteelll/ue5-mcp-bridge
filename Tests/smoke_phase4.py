#!/usr/bin/env python3
"""Phase 4 smoke wrapper — runs all sub-suites and reports combined result.

Sub-suites (each is a self-contained Python script with its own main()):
  - smoke_phase4_days_1_5.py    — 6 bp.* read tools (Day 1-5)
  - smoke_phase4_days_6_10.py   — 6 bp.* writes + bp.compile + bp.compile_all_dirty (Day 6-10)

Days 11-15 will add a smoke_phase4_days_11_15.py for the material.* surface and the wrapper
will pick it up automatically (just append to SUB_SUITES below).

Each child inherits the connection args (--host / --port) passed to this wrapper. The wrapper
exits with the first failing sub-suite's non-zero code, or 0 if all PASS. A short summary line
``[SMOKE_PHASE4_ALL] PASS|FAIL`` follows the last sub-suite's output for easy log scraping.

Usage:
  python smoke_phase4.py [--host 127.0.0.1] [--port 30020]
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SUB_SUITES = [
    "smoke_phase4_days_1_5.py",
    "smoke_phase4_days_6_10.py",
]


def main(argv: list[str]) -> int:
    extra_args = argv[1:]
    overall_rc = 0
    failed: list[str] = []
    for sub in SUB_SUITES:
        path = HERE / sub
        if not path.exists():
            print(f"[SMOKE_PHASE4_ALL] sub-suite SKIP (missing file): {sub}", flush=True)
            continue
        print(f"\n========== {sub} ==========", flush=True)
        proc = subprocess.run([sys.executable, str(path), *extra_args])
        if proc.returncode != 0:
            overall_rc = proc.returncode
            failed.append(sub)
            print(f"[SMOKE_PHASE4_ALL] sub-suite FAIL: {sub} (rc={proc.returncode})", flush=True)

    print()
    if overall_rc == 0:
        print(f"[SMOKE_PHASE4_ALL] PASS — all {len(SUB_SUITES)} sub-suites green", flush=True)
    else:
        print(f"[SMOKE_PHASE4_ALL] FAIL — {len(failed)} sub-suite(s) failed: {', '.join(failed)}",
              flush=True)
    return overall_rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
