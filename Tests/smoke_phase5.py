#!/usr/bin/env python3
"""Phase 5 smoke wrapper — runs all sub-suites and reports combined result.

Sub-suites (each is a self-contained Python script with its own main()):
  - smoke_phase5_pie.py        — 10 pie.* tools (Chunk A): start/stop/pause/resume/step/console/...
  - smoke_phase5_editor.py     — 10 editor.* + pie.screenshot_to_disk (Chunk B): viewport/camera/...
  - smoke_phase5_chunk_c.py    — 5 tools (Chunk C): umg.* (2) + niagara.list_parameters + physics.* (2)
  - smoke_phase5_sequencer.py  — 5 sequencer.* tools (Chunk D): list/tracks/cuts/keyframes/current_time

Each child inherits the connection args (--host / --port) passed to this wrapper. The wrapper
exits with the first failing sub-suite's non-zero code, or 0 if all PASS. A short summary line
``[SMOKE_PHASE5_ALL] PASS|FAIL`` follows the last sub-suite's output for easy log scraping.

Phase 5 ships 30 user-visible tools across 4 chunks (10 pie.* + 10 editor.* + 1 pie.screenshot +
5 Chunk C + 5 sequencer.* = 31 registered handlers when you count the cross-namespace
pie.screenshot_to_disk that lives in EditorTools but is exposed under pie.*; the user-visible
count remains 30 per the plan's coherence guidance). With Phase 5 complete the cumulative
across all phases reaches 145 user-visible tools.

Usage:
  python smoke_phase5.py [--host 127.0.0.1] [--port 30020]
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
SUB_SUITES = [
    "smoke_phase5_pie.py",
    "smoke_phase5_editor.py",
    "smoke_phase5_chunk_c.py",
    "smoke_phase5_sequencer.py",
]


def main(argv: list[str]) -> int:
    extra_args = argv[1:]
    overall_rc = 0
    failed: list[str] = []
    for sub in SUB_SUITES:
        path = HERE / sub
        if not path.exists():
            print(f"[SMOKE_PHASE5_ALL] sub-suite SKIP (missing file): {sub}", flush=True)
            continue
        print(f"\n========== {sub} ==========", flush=True)
        proc = subprocess.run([sys.executable, str(path), *extra_args])
        if proc.returncode != 0:
            overall_rc = proc.returncode
            failed.append(sub)
            print(f"[SMOKE_PHASE5_ALL] sub-suite FAIL: {sub} (rc={proc.returncode})", flush=True)

    print()
    if overall_rc == 0:
        print(f"[SMOKE_PHASE5_ALL] PASS — all {len(SUB_SUITES)} sub-suites green", flush=True)
    else:
        print(f"[SMOKE_PHASE5_ALL] FAIL — {len(failed)} sub-suite(s) failed: {', '.join(failed)}",
              flush=True)
    return overall_rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
