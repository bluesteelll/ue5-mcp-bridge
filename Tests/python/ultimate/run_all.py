#!/usr/bin/env python3
"""run_all.py — ULTIMATE full-suite regression runner.

Executes every phase_*.py script in sequence (each does its own preflight,
so the editor is auto-relaunched if it dies), captures exit code + the
script's final summary line, continues past failures, then runs the J2
aggregator last and prints grand totals.

Excluded by default:
  - mcp_test_harness.py             (library, not a phase)
  - phase_e1_sustained_soak.py      (long soak — run separately)
  - phase_j2_final_report.py        (aggregator — always run LAST)

Repeated invocation = a cumulative multi-hour stability soak: the editor
processes the entire surface over and over, surfacing cumulative-state /
slow-leak / long-uptime bugs that single passes miss.

Usage:
  run_all.py [--only SUBSTR] [--exclude SUBSTR] [--per-timeout SECS]
             [--include-e1]

Exit code: number of phase scripts that exited non-zero (0 = all green).
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Tuple

HERE = Path(__file__).parent
PY = sys.executable

DEFAULT_EXCLUDE = {"mcp_test_harness.py", "phase_e1_sustained_soak.py",
                   "phase_j2_final_report.py", "run_all.py"}

# The four "sweep every one of the ~431 tools with live probes" phases each
# run for MANY minutes (A1 alone ~141s; A2/A3/A4 longer with chain discovery).
# They're validated individually and are too heavy to re-run on every
# regression/soak pass — they dominate runtime and previously stalled a pass.
# Skipped by default; run with --include-slow (or invoke them directly).
DEFAULT_SLOW = {"phase_a1_inventory.py", "phase_a2_required_args.py",
                "phase_a3_optional_defaults.py", "phase_a4_type_coercion.py"}


def _discover(only: str, exclude: str, include_e1: bool, include_slow: bool) -> List[Path]:
    out: List[Path] = []
    for p in sorted(HERE.glob("phase_*.py")):
        if p.name in DEFAULT_EXCLUDE and not (include_e1 and p.name == "phase_e1_sustained_soak.py"):
            continue
        if p.name in DEFAULT_SLOW and not include_slow:
            continue
        if only and only not in p.name:
            continue
        if exclude and exclude in p.name:
            continue
        out.append(p)
    return out


def _run_one(script: Path, timeout: float) -> Tuple[str, int, float, str]:
    """Returns (name, exit_code, secs, last_summary_line)."""
    t0 = time.monotonic()
    try:
        r = subprocess.run([PY, str(script)], cwd=str(HERE),
                           capture_output=True, text=True, timeout=timeout,
                           encoding="utf-8", errors="replace")
        dt = time.monotonic() - t0
        # Find the most informative summary line (PASS=.. FAIL=..).
        summary = ""
        for line in (r.stdout or "").splitlines():
            if "PASS=" in line and "FAIL=" in line:
                summary = line.strip()
        if not summary:
            tail = [l for l in (r.stdout or "").splitlines() if l.strip()]
            summary = tail[-1].strip() if tail else "(no output)"
        return (script.name, r.returncode, dt, summary[:160])
    except subprocess.TimeoutExpired:
        return (script.name, 99, time.monotonic() - t0, f"TIMEOUT after {timeout:.0f}s")
    except Exception as e:
        return (script.name, 98, time.monotonic() - t0, f"runner exception: {e}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--exclude", default="")
    ap.add_argument("--per-timeout", type=float, default=240.0)
    ap.add_argument("--include-e1", action="store_true")
    ap.add_argument("--include-slow", action="store_true",
                    help="include the heavy A1-A4 sweep-all-tools phases")
    a = ap.parse_args()

    scripts = _discover(a.only, a.exclude, a.include_e1, a.include_slow)
    print(f"[run_all] {len(scripts)} phase scripts; per-timeout={a.per_timeout:.0f}s",
          flush=True)
    print(f"[run_all] start {time.strftime('%H:%M:%S')}", flush=True)

    results: List[Tuple[str, int, float, str]] = []
    t_start = time.monotonic()
    for i, s in enumerate(scripts):
        name, code, dt, summary = _run_one(s, a.per_timeout)
        results.append((name, code, dt, summary))
        flag = "OK " if code == 0 else f"X{code}"
        print(f"[{i+1:>2}/{len(scripts)}] {flag} {name:<42} {dt:>5.0f}s | {summary}",
              flush=True)

    # J2 aggregator last.
    j2 = HERE / "phase_j2_final_report.py"
    if j2.exists():
        name, code, dt, summary = _run_one(j2, a.per_timeout)
        print(f"[J2] {'OK ' if code==0 else 'X'+str(code)} {name} {dt:.0f}s | {summary}",
              flush=True)

    total_dt = time.monotonic() - t_start
    nonzero = [r for r in results if r[1] != 0]
    print(f"\n[run_all] DONE in {total_dt/60:.1f}min — "
          f"{len(results)-len(nonzero)}/{len(results)} green", flush=True)
    if nonzero:
        print("[run_all] non-zero exits:", flush=True)
        for name, code, dt, summary in nonzero:
            print(f"    X{code} {name}: {summary}", flush=True)
    return len(nonzero)


if __name__ == "__main__":
    sys.exit(main())
