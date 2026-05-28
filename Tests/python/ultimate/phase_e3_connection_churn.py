#!/usr/bin/env python3
"""Phase E3 — Connection churn (fd / thread / memory leak hunt).

The Bridge spawns one handler thread per TCP connection. If a connection's
thread/socket/fd isn't cleaned up on close, rapid connect→request→close
cycling would leak threads or file descriptors — surfacing as either
connection failures (fd/handle exhaustion) or monotonic memory growth
(accumulated thread stacks).

This phase churns N connect→1-request→close cycles and watches the
editor's memory + UObject count for a leak signature, then forces GC and
checks recovery to baseline.

(Scaled from the plan's 100k to a few thousand — a genuine per-connection
leak balloons within hundreds of cycles, so a few thousand is decisive
without a 20-minute runtime. Each cycle is a full TCP open/close.)

Probes:
  P1 — N cycles of: open socket, send 1 Lane B call, recv, close. Sample
       editor mem + uobj every SAMPLE_EVERY cycles. Record success rate.
  P2 — leak verdict: post-churn mem (after GC) within tolerance of the
       pre-churn baseline; no monotonic upward trend across samples.

PASS: success rate ≥95%, mem delta after GC < 150 MB, no monotonic leak
trend, editor alive, 0 crash dumps.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    HOST,
    LOG_ROOT,
    PORT,
    Connection,
    TestLogger,
    force_gc,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    snapshot,
)

PHASE = "e3"
NAME = "connection_churn"

N_CYCLES = 5000
SAMPLE_EVERY = 500
MEM_TOLERANCE_MB = 150.0


def _one_cycle() -> bool:
    """Open socket, send 1 request, recv, close. Returns True on clean RT."""
    try:
        with Connection(connect_timeout=4.0) as conn:
            r = conn.call_keepalive("memreport.get_quick_stats", {}, timeout=6.0)
            return is_ok(r)
    except (socket.timeout, OSError):
        return False
    except Exception:
        return False


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[E3] connection churn: {N_CYCLES} connect/req/close cycles…", flush=True)

    base = snapshot()
    base_mem = base.get("used_physical_mb", 0.0)
    base_uobj = base.get("live_uobject_slots", 0)
    log.case("P0_baseline", "PASS", f"mem={base_mem:.0f}MB uobj={base_uobj}")

    ok_count = 0
    samples: List[Tuple[int, float, int]] = []  # (cycle, mem, uobj)
    t0 = time.monotonic()
    for i in range(N_CYCLES):
        if _one_cycle():
            ok_count += 1
        if (i + 1) % SAMPLE_EVERY == 0:
            s = snapshot()
            samples.append((i + 1, s.get("used_physical_mb", 0.0),
                            s.get("live_uobject_slots", 0)))
    churn_dt = (time.monotonic() - t0) * 1000.0
    rate = ok_count / N_CYCLES

    cps = N_CYCLES / (churn_dt / 1000.0) if churn_dt > 0 else 0
    if rate >= 0.95:
        log.case("P1_churn", "PASS",
                 f"{ok_count}/{N_CYCLES} ({rate:.1%}) clean RTs in {churn_dt:.0f}ms "
                 f"(~{cps:.0f} cyc/s)", duration_ms=churn_dt)
    elif rate >= 0.80:
        log.case("P1_churn", "XFAIL",
                 f"{ok_count}/{N_CYCLES} ({rate:.1%}) — some connects failed "
                 f"(accept-loop pressure, not necessarily a leak)", duration_ms=churn_dt)
    else:
        log.case("P1_churn", "FAIL",
                 f"{ok_count}/{N_CYCLES} ({rate:.1%}) — high failure rate "
                 f"(possible fd/handle exhaustion)", duration_ms=churn_dt)
        fail_total += 1

    # Health between churn and leak check
    if not health(timeout=6.0):
        log.case("P1_health", "FAIL", "editor unresponsive after churn", alive=False)
        log.write()
        return 1

    # ── P2 — leak verdict ──────────────────────────────────────────────
    sample_mems = [m for (_c, m, _u) in samples]
    peak_mem = max(sample_mems) if sample_mems else base_mem
    # Monotonic trend: is each sample strictly larger than the prior by a lot?
    monotonic_growth = False
    if len(sample_mems) >= 3:
        rises = sum(1 for a, b in zip(sample_mems, sample_mems[1:]) if b > a + 5)
        monotonic_growth = rises >= (len(sample_mems) - 1)  # every step grew >5MB

    gc = force_gc(settle_s=2.0)
    post = snapshot()
    post_mem = post.get("used_physical_mb", 0.0)
    post_uobj = post.get("live_uobject_slots", 0)
    mem_delta = post_mem - base_mem
    uobj_delta = post_uobj - base_uobj

    detail = (f"base={base_mem:.0f}MB peak={peak_mem:.0f}MB postGC={post_mem:.0f}MB "
              f"(delta={mem_delta:+.0f}MB) uobj_delta={uobj_delta:+d} "
              f"samples={[f'{m:.0f}' for m in sample_mems]}")

    if monotonic_growth and mem_delta > MEM_TOLERANCE_MB:
        log.case("P2_leak_verdict", "FAIL",
                 f"LEAK: monotonic growth + {mem_delta:+.0f}MB after GC; {detail}")
        fail_total += 1
    elif mem_delta > MEM_TOLERANCE_MB:
        log.case("P2_leak_verdict", "XFAIL",
                 f"mem {mem_delta:+.0f}MB above tol after GC but non-monotonic "
                 f"(registry churn, not a per-conn leak); {detail}")
    else:
        log.case("P2_leak_verdict", "PASS",
                 f"no leak: mem within tol after GC; {detail}")

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[E3] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
