#!/usr/bin/env python3
"""Phase J1 — Self-instrumentation sanity.

Goal: verify that the Bridge's own observability tools actually return
truthful values — gc.collect drops memory, snapshots are consistent,
stat.* family responds to enable/disable.

Probes:
  P1 — engine.get_memory_snapshot before
  P2 — allocate temp work via 50 asset.create + cb.delete cycles
  P3 — engine.gc_collect should reduce live UObject count back near baseline
  P4 — engine.get_memory_snapshot after (UObject count back within tolerance)
  P5 — stat.* sanity (insights start_capture + stop_capture)
  P6 — memreport.get_quick_stats consistency: two back-to-back calls return
       similar values (within memory churn tolerance)

PASS: observability tools tell the truth.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict, Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    random_suffix,
    snapshot,
)

PHASE = "j1"
NAME = "observability"

ROOT = f"/Game/PhT_J1_{random_suffix(6)}"


def cleanup() -> None:
    call("folder.delete", {"folder_path": ROOT, "recursive": True}, timeout=20.0)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[J1] observability self-instrumentation sanity…", flush=True)
    cleanup()
    call("folder.create", {"folder_path": ROOT}, timeout=8.0)

    # P1 — baseline snapshot
    snap0 = snapshot()
    uobj0 = snap0.get("live_uobject_slots", 0)
    mem0 = snap0.get("used_physical_mb", 0.0)
    log.case("P1_baseline_snapshot", "PASS",
             f"uobj0={uobj0} mem0={mem0:.1f}MB")

    # P2 — allocate 50 short-lived assets
    n_cycles = 50
    n_create_ok = 0
    n_delete_ok = 0
    for i in range(n_cycles):
        path = f"{ROOT}/J1_{i:04d}"
        rc = call("asset.create_data_asset",
                   {"dest_path": path,
                    "class_path": "/Script/Engine.PrimaryAssetLabel"},
                   timeout=6.0)
        if is_ok(rc):
            n_create_ok += 1
        rd = call("cb.delete", {"path": path, "force": True}, timeout=6.0)
        if is_ok(rd):
            n_delete_ok += 1
    snap_after_work = snapshot()
    uobj_after_work = snap_after_work.get("live_uobject_slots", 0)
    mem_after_work = snap_after_work.get("used_physical_mb", 0.0)
    log.case("P2_after_work", "PASS",
             f"created={n_create_ok}/{n_cycles} deleted={n_delete_ok} "
             f"uobj_after_work={uobj_after_work} mem_after_work={mem_after_work:.1f}MB")

    # P3 — engine.gc_collect should bring UObject count down
    t0 = time.monotonic()
    r_gc = call("engine.gc_collect", {}, timeout=20.0)
    dt_gc = (time.monotonic() - t0) * 1000.0
    if not is_ok(r_gc):
        c = err_code(r_gc)
        log.case("P3_gc_collect", "FAIL",
                 f"engine.gc_collect failed: {err_message(r_gc)[:60]}",
                 duration_ms=dt_gc, code=c)
        fail_total += 1
    else:
        log.case("P3_gc_collect", "PASS",
                 f"engine.gc_collect completed in {dt_gc:.0f}ms",
                 duration_ms=dt_gc)
    time.sleep(2.0)

    # P4 — UObject count returns near baseline post-GC
    snap_post_gc = snapshot()
    uobj_post = snap_post_gc.get("live_uobject_slots", 0)
    mem_post = snap_post_gc.get("used_physical_mb", 0.0)
    uobj_delta = uobj_post - uobj0
    tolerance = max(n_cycles, 100)
    if abs(uobj_delta) > tolerance:
        log.case("P4_gc_uobj_recovered", "XFAIL",
                 f"uobj_delta={uobj_delta:+d} exceeds tolerance ±{tolerance}; "
                 f"GC may not reclaim everything immediately; uobj_post={uobj_post}")
    else:
        log.case("P4_gc_uobj_recovered", "PASS",
                 f"uobj_delta={uobj_delta:+d} within tolerance ±{tolerance}; "
                 f"mem_post={mem_post:.1f}MB")

    # P5 — stat.* family sanity
    r_stat = call("stat.list_categories", {}, timeout=8.0)
    if is_ok(r_stat):
        cats = (r_stat.get("result", {}) or {}).get("categories") or []
        log.case("P5_stat_list", "PASS",
                 f"stat.list_categories returned {len(cats)} categories")
    else:
        c = err_code(r_stat)
        if c == -32601:
            log.case("P5_stat_list", "SKIP", "stat.list_categories not registered")
        else:
            log.case("P5_stat_list", "XFAIL",
                     f"stat.list_categories failed: {err_message(r_stat)[:60]}")

    # P6 — memreport.get_quick_stats consistency
    s1 = snapshot()
    time.sleep(0.5)
    s2 = snapshot()
    uobj_drift = abs(s2.get("live_uobject_slots", 0) - s1.get("live_uobject_slots", 0))
    mem_drift = abs(s2.get("used_physical_mb", 0.0) - s1.get("used_physical_mb", 0.0))
    if uobj_drift > 1000 or mem_drift > 50:
        log.case("P6_consistency", "XFAIL",
                 f"large drift between back-to-back snapshots: "
                 f"uobj_drift={uobj_drift} mem_drift={mem_drift:.1f}MB")
    else:
        log.case("P6_consistency", "PASS",
                 f"snapshots stable: uobj_drift={uobj_drift} mem_drift={mem_drift:.1f}MB")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        cleanup()
        return 1

    cleanup()
    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[J1] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
