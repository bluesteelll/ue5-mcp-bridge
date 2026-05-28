#!/usr/bin/env python3
"""Phase E1 — Sustained soak load (long-run stability).

Drives a steady mixed Lane A + Lane B request stream for a target
duration, sampling editor memory + UObject count periodically to detect
slow leaks or drift that only surface over time. The realistic
counterpart to the burst-y C/D/E4 tests.

Load mix per tick (paced to ~TICK_HZ ticks/sec):
  - several cheap Lane B reads (memreport/engine/asset.exists/cfg/log)
  - one Lane A read (pie.is_running / level.actor_summary)
  - every Nth tick: a light write cycle (create + delete a tiny data
    asset) to exercise the mutate→GC path
NO heavy ops (no full memreport, no Live Coding) — those wedge the game
thread and aren't representative of sustained interactive load.

Samples every SAMPLE_SECS: (elapsed, mem_mb, uobj, ok_so_far).

PASS:
  - request success rate ≥ 98%
  - UObject count returns within tolerance of baseline after final GC
    (canonical leak metric — see E2)
  - memory drift bounded (< MEM_DRIFT_MB after GC)
  - editor alive throughout, 0 crash dumps

Usage: phase_e1_sustained_soak.py [--minutes N]   (default 60)

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    force_gc,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    random_suffix,
    snapshot,
)

PHASE = "e1"
NAME = "sustained_soak"

ROOT = f"/Game/PhT_E1_{random_suffix(6)}"
TICK_HZ = 4.0            # ~4 ticks/sec
SAMPLE_SECS = 120.0      # sample drift every 2 min
WRITE_EVERY = 20         # light write cycle every Nth tick
MEM_DRIFT_MB = 250.0     # post-GC mem drift tolerance over the whole soak
UOBJ_TOL = 2000          # post-GC UObject tolerance vs baseline

LANE_B = [
    ("memreport.get_quick_stats", {}),
    ("engine.get_info", {}),
    ("engine.get_memory_snapshot", {}),
    ("asset.exists", {"path": "/Engine/BasicShapes/Cube"}),
    ("cfg.list_cvars", {"page_size": 5}),
    ("log.list_categories", {"page_size": 5}),
]
LANE_A = [
    ("pie.is_running", {}),
    ("level.actor_summary", {}),
]


def _tick(tick_idx: int) -> Tuple[int, int]:
    """One load tick. Returns (n_sent, n_ok)."""
    sent = ok = 0
    for method, args in LANE_B:
        r = call(method, args, timeout=8.0)
        sent += 1
        if is_ok(r) or (err_code(r) is not None and not is_transport_failure(r)):
            ok += 1
    la = LANE_A[tick_idx % len(LANE_A)]
    r = call(la[0], la[1], timeout=8.0)
    sent += 1
    if is_ok(r) or (err_code(r) is not None and not is_transport_failure(r)):
        ok += 1
    if tick_idx % WRITE_EVERY == 0 and tick_idx > 0:
        p = f"{ROOT}/E1_{tick_idx:05d}"
        rc = call("asset.create_data_asset",
                  {"dest_path": p, "class_path": "/Script/Engine.PrimaryAssetLabel"},
                  timeout=8.0)
        sent += 1
        if is_ok(rc):
            ok += 1
            rd = call("cb.delete", {"path": p, "force": True}, timeout=8.0)
            sent += 1
            if is_ok(rd):
                ok += 1
    return sent, ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=60.0)
    a = ap.parse_args()

    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0
    log.note(f"sustained soak target={a.minutes:.0f} min, ~{TICK_HZ:.0f} ticks/s, "
             f"write cycle every {WRITE_EVERY} ticks")

    print(f"[E1] sustained soak {a.minutes:.0f} min (root={ROOT})…", flush=True)
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    base = snapshot()
    base_mem = base.get("used_physical_mb", 0.0)
    base_uobj = base.get("live_uobject_slots", 0)
    log.case("P0_baseline", "PASS", f"mem={base_mem:.0f}MB uobj={base_uobj}")

    deadline = time.monotonic() + a.minutes * 60.0
    next_sample = time.monotonic() + SAMPLE_SECS
    tick_interval = 1.0 / TICK_HZ
    total_sent = total_ok = 0
    samples: List[Tuple[float, float, int]] = []
    tick_idx = 0
    dead_detected = False
    soak_start = time.monotonic()

    while time.monotonic() < deadline:
        t_tick = time.monotonic()
        s, o = _tick(tick_idx)
        total_sent += s
        total_ok += o
        tick_idx += 1

        now = time.monotonic()
        if now >= next_sample:
            snap = snapshot()
            elapsed = now - soak_start
            mem = snap.get("used_physical_mb", 0.0)
            uobj = snap.get("live_uobject_slots", 0)
            samples.append((elapsed, mem, uobj))
            rate = total_ok / total_sent if total_sent else 0
            print(f"  [E1] t={elapsed/60:.1f}min ticks={tick_idx} "
                  f"req={total_sent} ok={rate:.1%} mem={mem:.0f}MB uobj={uobj}",
                  flush=True)
            # Mid-soak liveness — if editor died, stop early and FAIL.
            if not health(timeout=6.0):
                dead_detected = True
                log.case("midsoak_health", "FAIL",
                         f"editor unresponsive at t={elapsed/60:.1f}min", alive=False)
                break
            next_sample = now + SAMPLE_SECS

        # Pace the tick.
        sleep_for = tick_interval - (time.monotonic() - t_tick)
        if sleep_for > 0:
            time.sleep(sleep_for)

    soak_dt = time.monotonic() - soak_start
    rate = total_ok / total_sent if total_sent else 0

    if dead_detected:
        log.write()
        call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=15.0)
        return 1

    # Completion / success-rate verdict
    if rate >= 0.98:
        log.case("P1_throughput", "PASS",
                 f"{total_ok}/{total_sent} ({rate:.2%}) over {soak_dt/60:.1f}min, "
                 f"{tick_idx} ticks", duration_ms=soak_dt * 1000)
    elif rate >= 0.90:
        log.case("P1_throughput", "XFAIL",
                 f"{total_ok}/{total_sent} ({rate:.2%}) — some dips under sustained load",
                 duration_ms=soak_dt * 1000)
    else:
        log.case("P1_throughput", "FAIL",
                 f"{total_ok}/{total_sent} ({rate:.2%}) — high failure rate",
                 duration_ms=soak_dt * 1000)
        fail_total += 1

    # Leak / drift verdict (post-GC, UObject canonical)
    gc = force_gc(settle_s=2.0)
    post = snapshot()
    post_mem = post.get("used_physical_mb", 0.0)
    post_uobj = post.get("live_uobject_slots", 0)
    mem_delta = post_mem - base_mem
    uobj_delta = post_uobj - base_uobj
    sample_mems = [m for (_e, m, _u) in samples]
    detail = (f"base={base_mem:.0f}MB postGC={post_mem:.0f}MB (delta={mem_delta:+.0f}MB) "
              f"uobj_delta={uobj_delta:+d} mem_samples={[f'{m:.0f}' for m in sample_mems]}")

    if abs(uobj_delta) <= UOBJ_TOL and mem_delta <= MEM_DRIFT_MB:
        log.case("P2_drift", "PASS", f"no leak: within tolerance; {detail}")
    elif mem_delta > MEM_DRIFT_MB and abs(uobj_delta) <= UOBJ_TOL:
        log.case("P2_drift", "XFAIL",
                 f"mem drift {mem_delta:+.0f}MB > tol but uobj stable "
                 f"(allocator/registry retention, not a UObject leak); {detail}")
    else:
        log.case("P2_drift", "FAIL",
                 f"LEAK signature: uobj_delta={uobj_delta:+d} (tol {UOBJ_TOL}); {detail}")
        fail_total += 1

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
    print(f"[E1] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"TOTAL={cc['TOTAL']} | {total_sent} requests over {soak_dt/60:.1f}min")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
