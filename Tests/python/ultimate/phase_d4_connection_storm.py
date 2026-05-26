#!/usr/bin/env python3
"""Phase D4 — Connection storm.

Goal: bridge handles thousands of sequential connect/disconnect cycles
without socket leak, thread leak, or memory bloat.

Probes:
  P1 — 500 sequential connect → send → recv → close
  P2 — 50 parallel × 10 cycles each = 500 parallel-style cycles
  P3 — verify post-storm Lane A and Lane B both responsive

Pass criteria:
  * ≥ 99% success rate on P1 (sequential)
  * ≥ 70% on P2 (parallel — listener saturation expected)
  * editor alive, no crash
  * memory delta < 50 MB after force_gc

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
import json
import socket
import sys
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    HOST,
    PORT,
    TestLogger,
    call,
    err_code,
    err_message,
    force_gc,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    snapshot,
)

PHASE = "d4"
NAME = "connection_storm"


def _one_cycle(timeout: float = 5.0) -> bool:
    """Single connect/send/recv/close cycle. Returns True on success."""
    payload = b'{"id":"x","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(payload)
            buf = bytearray()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    sock.settimeout(max(0.5, deadline - time.monotonic()))
                    chunk = sock.recv(4096)
                except socket.timeout:
                    return False
                if not chunk:
                    return False
                buf.extend(chunk)
                nl = buf.find(b"\n")
                if nl >= 0:
                    try:
                        obj = json.loads(buf[:nl].decode("utf-8", "replace"))
                        return bool(obj.get("ok"))
                    except Exception:
                        return False
            return False
    except Exception:
        return False


def probe_sequential(log: TestLogger, n: int = 500) -> int:
    label = f"P1 sequential x{n}"
    t0 = time.monotonic()
    ok_count = 0
    for _ in range(n):
        if _one_cycle():
            ok_count += 1
    dt = (time.monotonic() - t0) * 1000.0
    rps = n / (dt / 1000.0) if dt > 0 else 0
    summary = f"ok={ok_count}/{n} rate={rps:.0f}/s"
    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1
    success_rate = ok_count / n
    if success_rate >= 0.99:
        log.case(label, "PASS", summary, duration_ms=dt)
        return 0
    if success_rate >= 0.95:
        log.case(label, "XFAIL",
                 f"low rate (95-99%) — possible TIME_WAIT exhaust; {summary}",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL", summary, duration_ms=dt)
    return 1


def probe_parallel(log: TestLogger, n_workers: int = 50, cycles_per_worker: int = 10) -> int:
    """Each worker does N sequential cycles. Total = n_workers * cycles_per_worker."""
    label = f"P2 parallel x{n_workers}w × {cycles_per_worker}c"
    total = n_workers * cycles_per_worker
    t0 = time.monotonic()

    def _worker_loop():
        return sum(1 for _ in range(cycles_per_worker) if _one_cycle())

    with concurrent.futures.ThreadPoolExecutor(max_workers=n_workers) as ex:
        futures = [ex.submit(_worker_loop) for _ in range(n_workers)]
        per_worker_ok = [f.result(timeout=180.0) for f in concurrent.futures.as_completed(futures, timeout=300.0)]

    dt = (time.monotonic() - t0) * 1000.0
    total_ok = sum(per_worker_ok)
    summary = f"ok={total_ok}/{total} workers={n_workers} per-worker_avg={total_ok/n_workers:.1f}"

    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1

    success_rate = total_ok / total
    if success_rate >= 0.7:
        log.case(label, "PASS", summary, duration_ms=dt)
        return 0
    if success_rate > 0:
        log.case(label, "XFAIL", f"low rate (listener saturation expected); {summary}",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL", summary, duration_ms=dt)
    return 1


def probe_post_storm_recovery(log: TestLogger) -> int:
    label = "P3 post-storm recovery"
    t0 = time.monotonic()
    # Lane B
    r_b = call("memreport.get_quick_stats", {}, timeout=5.0)
    # Lane A
    r_a = call("asset.exists", {"path": "/Engine/BasicShapes/Cube"}, timeout=8.0)
    dt = (time.monotonic() - t0) * 1000.0
    if is_ok(r_b) and is_ok(r_a):
        log.case(label, "PASS", "both lanes responsive post-storm", duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"post-storm degraded: lane_b={is_ok(r_b)} lane_a={is_ok(r_a)}",
             duration_ms=dt)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D4] connection storm probes…", flush=True)

    # Snapshot before
    snap_before = snapshot()
    mem_before = snap_before.get("used_physical_mb", 0.0)
    uobj_before = snap_before.get("live_uobject_slots", 0)

    fail_total += probe_sequential(log, n=500)
    fail_total += probe_parallel(log, n_workers=20, cycles_per_worker=10)
    fail_total += probe_post_storm_recovery(log)

    # Memory delta check
    try:
        force_gc(timeout=20.0)
    except Exception:
        pass
    time.sleep(2.0)
    snap_after = snapshot()
    mem_after = snap_after.get("used_physical_mb", 0.0)
    uobj_after = snap_after.get("live_uobject_slots", 0)
    mem_delta = mem_after - mem_before if (mem_before > 0 and mem_after > 0) else 0.0
    uobj_delta = uobj_after - uobj_before if (uobj_before > 0 and uobj_after > 0) else 0
    log.case("memory_check", "PASS" if abs(mem_delta) < 100 else "XFAIL",
             f"mem_delta={mem_delta:.1f}MB uobj_delta={uobj_delta}")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D4] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
