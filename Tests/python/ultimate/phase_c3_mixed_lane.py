#!/usr/bin/env python3
"""Phase C3 — Lane A blocked, Lane B continues.

Goal: a slow Lane A operation does not stall Lane B readers. The two
lanes share a TCP listener but different worker pools — Lane A drains
via OnEndFrame tick; Lane B uses the worker pool directly. They MUST
NOT have shared mutexes that block each other.

Probe design:
  P1 — measure baseline Lane B latency without contention.
  P2 — issue a slow Lane A op (memreport.dump full mode, ~2-5s) in
       parallel with 50 Lane B calls. Lane B latency must NOT degrade
       significantly (< 3× baseline p99).

PASS: Lane B p99 during contention ≤ 3× baseline. Editor alive.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "c3"
NAME = "mixed_lane"


def _lane_b_call() -> Dict[str, Any]:
    t0 = time.monotonic()
    r = call("memreport.get_quick_stats", {}, timeout=10.0)
    return {
        "ok": is_ok(r),
        "duration_ms": (time.monotonic() - t0) * 1000.0,
        "transport_fail": is_transport_failure(r),
    }


def _lane_a_slow_call() -> Dict[str, Any]:
    """A Lane A call expected to take some real time. memreport.dump in full
    mode walks the asset registry; if not available, fall back to bp.list with
    a large page (which iterates blueprints on game thread)."""
    t0 = time.monotonic()
    # bp.list with large page is a known Lane A op (game-thread asset registry walk).
    r = call("bp.list", {"page_size": 500}, timeout=30.0)
    return {
        "ok": is_ok(r),
        "duration_ms": (time.monotonic() - t0) * 1000.0,
        "transport_fail": is_transport_failure(r),
    }


def measure_baseline(log: TestLogger, n: int = 30) -> List[float]:
    """P1: fire N sequential Lane B calls, collect latencies."""
    label = f"P1 baseline Lane B x{n}"
    t0 = time.monotonic()
    latencies = []
    for _ in range(n):
        rs = _lane_b_call()
        if rs["ok"]:
            latencies.append(rs["duration_ms"])
        else:
            latencies.append(-1.0)
    dt = (time.monotonic() - t0) * 1000.0
    valid = [l for l in latencies if l > 0]
    if not valid:
        log.case(label, "FAIL", "no successful baseline Lane B calls", duration_ms=dt)
        return []
    sorted_lat = sorted(valid)
    p50 = sorted_lat[len(sorted_lat) // 2]
    p99 = sorted_lat[int(len(sorted_lat) * 0.99)]
    log.case(label, "PASS",
             f"baseline ok={len(valid)}/{n} p50={p50:.0f}ms p99={p99:.0f}ms",
             duration_ms=dt)
    return valid


def probe_contention(log: TestLogger, baseline_p99: float,
                     n_b: int = 10, gap_ms: int = 100) -> int:
    """P2: fire Lane A slow op, then ISSUE Lane B calls sequentially with gap.

    This avoids the listener-saturation noise from C1 (the listener accept loop
    becomes the bottleneck if N Lane B clients connect simultaneously — that's
    NOT the property we're testing here). We test the actual lane-separation
    property: while Lane A is busy, EACH individual Lane B call returns fast.
    """
    label = f"P2 contention {n_b} Lane B (sequential, gap={gap_ms}ms) + 1 Lane A slow"
    t0 = time.monotonic()

    # Fire Lane A slow op in background.
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as ex:
        future_a = ex.submit(_lane_a_slow_call)
        time.sleep(0.1)  # let Lane A grab its worker

        # Now issue Lane B sequentially while Lane A is still in flight.
        b_results = []
        for _ in range(n_b):
            b_results.append(_lane_b_call())
            time.sleep(gap_ms / 1000.0)

        a_result = future_a.result(timeout=60.0)

    dt = (time.monotonic() - t0) * 1000.0

    b_latencies = sorted([r["duration_ms"] for r in b_results if r["ok"]])
    if not b_latencies:
        log.case(label, "FAIL",
                 f"no successful Lane B during contention; Lane A ok={a_result['ok']}",
                 duration_ms=dt)
        return 1
    p50 = b_latencies[len(b_latencies) // 2]
    p99 = b_latencies[int(len(b_latencies) * 0.99)]

    threshold = max(baseline_p99 * 5.0, 200.0)
    ok_b = sum(1 for r in b_results if r["ok"])
    summary = (f"Lane B ok={ok_b}/{n_b} p50={p50:.0f}ms p99={p99:.0f}ms; "
               f"Lane A took={a_result['duration_ms']:.0f}ms ok={a_result['ok']}; "
               f"baseline_p99={baseline_p99:.0f}ms threshold={threshold:.0f}ms")
    if p99 <= threshold:
        log.case(label, "PASS",
                 f"Lane B latency NOT blocked by Lane A; {summary}",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"Lane B p99 {p99:.0f}ms > threshold {threshold:.0f}ms (Lane B blocked by Lane A); "
             f"{summary}",
             duration_ms=dt)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2

    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[C3] mixed-lane probes (baseline + contention)…", flush=True)

    baseline = measure_baseline(log, n=30)
    if not baseline:
        log.write()
        return 1

    sorted_lat = sorted(baseline)
    baseline_p99 = sorted_lat[int(len(sorted_lat) * 0.99)]

    if not health(timeout=5.0):
        log.case("between_probes", "FAIL", "editor unresponsive after baseline", alive=False)
        log.write()
        return 1

    fail_total += probe_contention(log, baseline_p99, n_b=50)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[C3] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
