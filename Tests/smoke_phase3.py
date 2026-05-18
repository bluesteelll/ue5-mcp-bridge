#!/usr/bin/env python3
"""Phase 3 smoke wrapper — runs all 4 sub-suites and reports combined result.

Sub-suites (each is a self-contained Python script with its own main()):
  - smoke_phase3_days_1_3.py    — 12 level.* tools
  - smoke_phase3_days_4_8.py    — 20 actor.* tools
  - smoke_phase3_days_9_10.py   — 8 component.* tools
  - smoke_phase3_days_11_14.py  — 5 composite tools (level.full_actor_dump etc.)

Each child inherits the connection args (--host / --port) passed to this wrapper.
The wrapper exits with the first failing sub-suite's non-zero code, or 0 if all
PASS. A short summary line ``[SMOKE_PHASE3_ALL] PASS|FAIL`` follows the last
sub-suite's output for easy log scraping.

Usage:
  python smoke_phase3.py [--host 127.0.0.1] [--port 30020]
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SUB_SUITES = [
    "smoke_phase3_days_1_3.py",
    "smoke_phase3_days_4_8.py",
    "smoke_phase3_days_9_10.py",
    "smoke_phase3_days_11_14.py",
]


def main(argv: list[str]) -> int:
    extra_args = argv[1:]
    overall_rc = 0
    failed: list[str] = []
    for sub in SUB_SUITES:
        path = HERE / sub
        print(f"\n========== {sub} ==========", flush=True)
        proc = subprocess.run([sys.executable, str(path), *extra_args])
        if proc.returncode != 0:
            overall_rc = proc.returncode
            failed.append(sub)
            print(f"[SMOKE_PHASE3_ALL] sub-suite FAIL: {sub} (rc={proc.returncode})", flush=True)

    print()
    if overall_rc == 0:
        print(f"[SMOKE_PHASE3_ALL] PASS — all {len(SUB_SUITES)} sub-suites green", flush=True)
    else:
        print(f"[SMOKE_PHASE3_ALL] FAIL — {len(failed)} sub-suite(s) failed: {', '.join(failed)}", flush=True)
    return overall_rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
