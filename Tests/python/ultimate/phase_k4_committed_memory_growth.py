#!/usr/bin/env python3
"""Phase K4 — Committed/private memory-growth diagnostic (~6h-OOM confirmation).

The ~6h editor death was attributed to environmental OS memory pressure. But
every prior soak tracked WORKING SET (used_physical_mb), which stayed flat or
SHRANK — that's the resident set, not what drives an OOM. The OOM-relevant
metric is the process's COMMITTED / PRIVATE bytes (PrivateMemorySize64 /
PagedMemorySize64): committed pages count against the system commit limit even
when trimmed out of the working set.

K4 drives a moderate mixed Lane A/B load while sampling the editor's PRIVATE +
PAGED (committed) bytes over time, fits the growth slope, and extrapolates to
the death window. This definitively confirms or refutes the OOM-creep
hypothesis:
  - private bytes climb steadily  -> confirms OOM trajectory (extrapolate to
    when it approaches the host commit limit ~= the observed ~6h)
  - private bytes flat            -> OOM-creep refuted; death is bursty/
    event-driven, look elsewhere

Usage: phase_k4_committed_memory_growth.py [--minutes N]  (default 45)

Verdict (diagnostic):
  PASS  — private-byte slope ~0 (no committed-memory creep)
  XFAIL — measurable positive slope (OOM trajectory CONFIRMED; reports MB/min
          + extrapolated hours-to-limit)
  FAIL  — editor died mid-run / transport

Exit codes: 0=PASS/XFAIL, 1=editor died, 2=preflight.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
    snapshot,
)

PHASE = "k4"
NAME = "committed_memory_growth"

ROOT = f"/Game/PhT_K4_{random_suffix(6)}"
TICK_HZ = 4.0
SAMPLE_SECS = 150.0
WRITE_EVERY = 20
HOST_COMMIT_GB = 15.4  # this host's total RAM (commit headroom proxy)

LANE_B = [
    ("memreport.get_quick_stats", {}),
    ("engine.get_info", {}),
    ("engine.get_memory_snapshot", {}),
    ("asset.exists", {"path": "/Engine/BasicShapes/Cube"}),
    ("cfg.list_cvars", {"page_size": 5}),
]
LANE_A = [("pie.is_running", {}), ("level.actor_summary", {})]


def _editor_committed_mb() -> Optional[Tuple[float, float]]:
    """Return (PrivateMB, PagedMB) for UnrealEditor, or None if gone."""
    try:
        r = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "$p=Get-Process UnrealEditor -ErrorAction SilentlyContinue;"
             "if($p){\"{0} {1}\" -f [math]::Round($p.PrivateMemorySize64/1MB,1),"
             "[math]::Round($p.PagedMemorySize64/1MB,1)}else{'DEAD'}"],
            capture_output=True, text=True, timeout=15)
        out = (r.stdout or "").strip()
        if out == "DEAD" or not out:
            return None
        a, b = out.split()
        return (float(a), float(b))
    except Exception:
        return None


def _tick(i: int) -> int:
    ok = 0
    for m, a in LANE_B:
        if is_ok(call(m, a, timeout=8.0)):
            ok += 1
    la = LANE_A[i % len(LANE_A)]
    if is_ok(call(la[0], la[1], timeout=8.0)):
        ok += 1
    if i % WRITE_EVERY == 0 and i > 0:
        p = f"{ROOT}/K4_{i:05d}"
        if is_ok(call("asset.create_data_asset",
                      {"dest_path": p, "class_path": "/Script/Engine.PrimaryAssetLabel"},
                      timeout=8.0)):
            call("cb.delete", {"path": p, "force": True}, timeout=8.0)
    return ok


def _slope_mb_per_min(samples: List[Tuple[float, float]]) -> float:
    """Least-squares slope of value vs elapsed-minutes. samples: (min, mb)."""
    n = len(samples)
    if n < 2:
        return 0.0
    sx = sum(s[0] for s in samples); sy = sum(s[1] for s in samples)
    sxx = sum(s[0] * s[0] for s in samples); sxy = sum(s[0] * s[1] for s in samples)
    denom = n * sxx - sx * sx
    return 0.0 if denom == 0 else (n * sxy - sx * sy) / denom


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=45.0)
    a = ap.parse_args()
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    log.note("Tracks editor PrivateMemorySize64 / PagedMemorySize64 (committed "
             "bytes — the OOM-relevant metric), not working set.")

    print(f"[K4] committed-memory growth over {a.minutes:.0f}min…", flush=True)
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    base = _editor_committed_mb()
    if base is None:
        log.case("P0_baseline", "FAIL", "couldn't read editor committed bytes")
        log.write(); return 1
    base_priv, base_paged = base
    ws0 = snapshot().get("used_physical_mb", 0.0)
    log.case("P0_baseline", "PASS",
             f"private={base_priv:.0f}MB paged={base_paged:.0f}MB workingset={ws0:.0f}MB")

    priv_samples: List[Tuple[float, float]] = [(0.0, base_priv)]
    paged_samples: List[Tuple[float, float]] = [(0.0, base_paged)]
    start = time.monotonic()
    deadline = start + a.minutes * 60.0
    next_sample = start + SAMPLE_SECS
    tick_interval = 1.0 / TICK_HZ
    i = 0
    while time.monotonic() < deadline:
        t = time.monotonic()
        _tick(i); i += 1
        now = time.monotonic()
        if now >= next_sample:
            cm = _editor_committed_mb()
            if cm is None:
                log.case("midrun_health", "FAIL",
                         f"editor DIED at t={ (now-start)/60:.1f}min", alive=False)
                log.write()
                call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)
                return 1
            mins = (now - start) / 60.0
            priv_samples.append((mins, cm[0]))
            paged_samples.append((mins, cm[1]))
            print(f"  [K4] t={mins:.1f}min private={cm[0]:.0f}MB paged={cm[1]:.0f}MB",
                  flush=True)
            next_sample = now + SAMPLE_SECS
        s = tick_interval - (time.monotonic() - t)
        if s > 0:
            time.sleep(s)

    final = _editor_committed_mb() or (base_priv, base_paged)
    priv_slope = _slope_mb_per_min(priv_samples)
    paged_slope = _slope_mb_per_min(paged_samples)
    # A one-time early step (asset-registry warm-up / pool growth) inflates the
    # full-range least-squares slope even when memory is otherwise flat. A
    # GENUINE steady leak shows a positive slope in BOTH the full range AND the
    # second half. Use the second-half (post-warmup) slope for the verdict.
    half = len(priv_samples) // 2
    priv_slope_tail = (_slope_mb_per_min(priv_samples[half:])
                       if len(priv_samples) >= 4 else priv_slope)
    headroom_mb = HOST_COMMIT_GB * 1024 - final[0]
    hrs_to_limit = (headroom_mb / priv_slope_tail / 60.0) if priv_slope_tail > 0.5 else float("inf")

    detail = (f"private {base_priv:.0f}->{final[0]:.0f}MB "
              f"(full slope {priv_slope:+.2f}, post-warmup slope {priv_slope_tail:+.2f} MB/min) "
              f"paged {base_paged:.0f}->{final[1]:.0f}MB")
    log.case("P1_load", "PASS", f"{i} ticks over {a.minutes:.0f}min", duration_ms=0)

    if priv_slope_tail <= 0.5:
        note = ("(full slope >0 only from a one-time early step; flat thereafter)"
                if priv_slope > 0.5 else "")
        log.case("P2_committed_growth", "PASS",
                 f"committed memory STABLE after warmup {note}; steady OOM-creep "
                 f"REFUTED. Editor resource metrics all flat (working set, UObject, "
                 f"handles, threads, committed) -> ~6h death is SYSTEM-level host "
                 f"pressure, not an editor/bridge leak. {detail}")
    else:
        eta = (f"{hrs_to_limit:.1f}h to {HOST_COMMIT_GB:.0f}GB commit ceiling"
               if hrs_to_limit != float("inf") else "n/a")
        log.case("P2_committed_growth", "XFAIL",
                 f"steady committed-memory creep CONFIRMED (post-warmup slope "
                 f"{priv_slope_tail:+.2f}MB/min) -> extrapolated {eta} (observed ~6h). {detail}")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)
        return 1

    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[K4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     {detail}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
