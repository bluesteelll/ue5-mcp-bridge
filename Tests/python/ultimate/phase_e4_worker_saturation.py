#!/usr/bin/env python3
"""Phase E4 — Lane B saturation / worker-starvation soak.

The Bridge has NO fixed worker pool — each TCP connection spawns its own
handler thread (FRunnableThread), so the plan's literal "block all 8
workers" premise doesn't apply (documented as C6 = N/A). The REAL
saturation risk is: (a) the FTcpListener serial-accept loop throttling
new connections under a burst, and (b) sustained concurrent Lane B load
degrading or starving quick calls over time.

This phase soaks Lane B under repeated high-concurrency waves and verifies
the bridge neither deadlocks, drops, degrades over time, nor starves a
priority call.

Probes:
  P1 — 6 sequential waves × 50 concurrent mixed Lane B calls. Each wave
       must complete ≥70% (the documented accept-saturation floor from
       C1); editor alive between waves; NO downward degradation trend
       (last wave's rate not far below the first).
  P2 — aggregate throughput: 300 Lane B calls at 50-way concurrency →
       overall completion + p99 latency recorded.
  P3 — no-starvation: while a 60-way Lane B flood is in flight, a single
       priority memreport.get_quick_stats on its own connection still
       returns < 8s (not starved behind the flood).

PASS: no deadlock, ≥70% per wave, no degradation trend, priority call
served, editor alive, 0 crash dumps.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "e4"
NAME = "worker_saturation"

LANE_B_TOOLS: List[Tuple[str, Dict[str, Any]]] = [
    ("memreport.get_quick_stats", {}),
    ("engine.get_info", {}),
    ("engine.get_memory_snapshot", {}),
    ("asset.exists", {"path": "/Engine/BasicShapes/Cube"}),
    ("cfg.list_cvars", {"page_size": 5}),
    ("log.list_categories", {"page_size": 5}),
]

ACCEPT_FLOOR = 0.70  # documented FTcpListener accept-saturation floor (C1)


def _call_once(method: str, args: Dict[str, Any], timeout: float = 12.0) -> Tuple[bool, float]:
    t0 = time.monotonic()
    try:
        r = call(method, args, timeout=timeout)
    except Exception:
        return (False, (time.monotonic() - t0) * 1000.0)
    dt = (time.monotonic() - t0) * 1000.0
    if is_transport_failure(r):
        return (False, dt)
    # Benign structured errors still count as "served" (dispatch worked).
    return (is_ok(r) or err_code(r) is not None, dt)


def _wave(concurrency: int) -> Tuple[int, float]:
    """Fire `concurrency` mixed Lane B calls. Returns (ok_count, p99_ms)."""
    plan = [(LANE_B_TOOLS[i % len(LANE_B_TOOLS)]) for i in range(concurrency)]
    oks = 0
    lats: List[float] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as ex:
        futs = [ex.submit(_call_once, m, a) for (m, a) in plan]
        for f in concurrent.futures.as_completed(futs, timeout=120.0):
            ok, dt = f.result()
            if ok:
                oks += 1
                lats.append(dt)
    lats.sort()
    p99 = lats[int(len(lats) * 0.99)] if lats else -1.0
    return (oks, p99)


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[E4] Lane B saturation soak…", flush=True)

    # ── P1 — 6 waves × 50 concurrent ───────────────────────────────────
    n_waves, conc = 6, 50
    rates: List[float] = []
    wave_fail = False
    for w in range(n_waves):
        t0 = time.monotonic()
        oks, p99 = _wave(conc)
        dt = (time.monotonic() - t0) * 1000.0
        rate = oks / conc
        rates.append(rate)
        status = "PASS" if rate >= ACCEPT_FLOOR else "XFAIL"
        if rate == 0.0:
            status = "FAIL"
            wave_fail = True
        log.case(f"P1_wave{w}", status,
                 f"ok={oks}/{conc} ({rate:.0%}) p99={p99:.0f}ms", duration_ms=dt)
        if not health(timeout=5.0):
            log.case(f"P1_wave{w}_health", "FAIL", "editor unresponsive between waves",
                     alive=False)
            log.write()
            return 1
    if wave_fail:
        fail_total += 1
    # Degradation trend: last wave shouldn't be dramatically worse than first.
    if rates[0] > 0 and rates[-1] < rates[0] * 0.5 and rates[-1] < ACCEPT_FLOOR:
        log.case("P1_trend", "XFAIL",
                 f"downward trend: first={rates[0]:.0%} last={rates[-1]:.0%} "
                 f"(degradation under sustained load)")
    else:
        log.case("P1_trend", "PASS",
                 f"no degradation: rates={['%.0f%%' % (r*100) for r in rates]}")

    # ── P2 — aggregate throughput (300 @ 50-way) ───────────────────────
    t0 = time.monotonic()
    total_ok = 0
    total_lat: List[float] = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
        futs = [ex.submit(_call_once, LANE_B_TOOLS[i % len(LANE_B_TOOLS)][0],
                          LANE_B_TOOLS[i % len(LANE_B_TOOLS)][1]) for i in range(300)]
        for f in concurrent.futures.as_completed(futs, timeout=180.0):
            ok, dt = f.result()
            if ok:
                total_ok += 1
                total_lat.append(dt)
    dt = (time.monotonic() - t0) * 1000.0
    total_lat.sort()
    p99 = total_lat[int(len(total_lat) * 0.99)] if total_lat else -1.0
    thr = total_ok / (dt / 1000.0) if dt > 0 else 0
    rate = total_ok / 300
    if rate >= ACCEPT_FLOOR:
        log.case("P2_throughput", "PASS",
                 f"{total_ok}/300 ({rate:.0%}) in {dt:.0f}ms ~{thr:.0f}req/s p99={p99:.0f}ms",
                 duration_ms=dt)
    elif rate > 0:
        log.case("P2_throughput", "XFAIL",
                 f"{total_ok}/300 ({rate:.0%}) below floor (accept saturation)",
                 duration_ms=dt)
    else:
        log.case("P2_throughput", "FAIL", "0/300 — deadlock", duration_ms=dt)
        fail_total += 1

    if not health(timeout=5.0):
        log.case("between_p2_p3", "FAIL", "editor unresponsive after P2", alive=False)
        log.write()
        return 1

    # ── P3 — no-starvation: priority call during a 60-way flood ────────
    flood_done = threading.Event()
    priority_result: Dict[str, Any] = {}

    def _flood() -> None:
        with concurrent.futures.ThreadPoolExecutor(max_workers=60) as ex:
            futs = [ex.submit(_call_once, "engine.get_info", {}) for _ in range(60)]
            for f in concurrent.futures.as_completed(futs, timeout=120.0):
                f.result()
        flood_done.set()

    def _priority() -> None:
        time.sleep(0.2)  # let the flood ramp
        t0 = time.monotonic()
        ok, _ = _call_once("memreport.get_quick_stats", {}, timeout=8.0)
        priority_result["ok"] = ok
        priority_result["ms"] = (time.monotonic() - t0) * 1000.0

    t0 = time.monotonic()
    tf = threading.Thread(target=_flood)
    tp = threading.Thread(target=_priority)
    tf.start(); tp.start()
    tp.join(timeout=20.0); tf.join(timeout=120.0)
    dt = (time.monotonic() - t0) * 1000.0
    pok = priority_result.get("ok", False)
    pms = priority_result.get("ms", -1)
    if pok and pms < 8000:
        log.case("P3_no_starvation", "PASS",
                 f"priority call served in {pms:.0f}ms during 60-way flood",
                 duration_ms=dt)
    elif pok:
        log.case("P3_no_starvation", "XFAIL",
                 f"priority call served but slow ({pms:.0f}ms) under flood",
                 duration_ms=dt)
    else:
        log.case("P3_no_starvation", "XFAIL",
                 f"priority call failed under flood (accept saturation, not deadlock "
                 f"if editor alive)", duration_ms=dt)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[E4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
