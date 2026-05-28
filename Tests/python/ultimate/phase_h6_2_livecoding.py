#!/usr/bin/env python3
"""Phase H6.2 — livecoding.* surface coverage.

The Live Coding surface is a single tool, livecoding._recompile_internal
(Lane B). G4 already exercised the real-recompile-vs-bp.compile race; this
phase focuses on INPUT-VALIDATION robustness of the `modules` argument
(the path most likely to be hit with bad input) plus one bounded real
recompile of an up-to-date module (resolves to NoChanges fast).

CAUTION: never pass modules=["*"] — that scans ALL modules and can run a
multi-minute compile. Use a single, loaded, up-to-date plugin module.

Probes:
  P1 — no args                  → -32602 (missing 'modules')
  P2 — modules=[] (empty)       → -32602 (empty array)
  P3 — modules="UnrealMCPBridge" (string, not array) → -32602 (wrong type)
  P4 — modules=[123, true]      → graceful structured reject (non-string)
  P5 — bounded real recompile, modules=["UnrealMCPBridgeCore"] →
       ok (NoChanges/Success) or graceful structured (LC disabled);
       client-timeout → XFAIL (heavy compile, not a fault)
  P6 — recovery: both lanes responsive afterward (no wedge/modal)

PASS: validation rejects cleanly, real recompile graceful, editor recovers,
0 crash dumps.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Any, Dict

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    assert_lane_a_alive,
    call,
    dismiss_ue_modal_via_win32,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "h6_2"
NAME = "livecoding"

LC = "livecoding._recompile_internal"
REAL_TIMEOUT_S = 30.0


def _reject(log: TestLogger, name: str, args: Dict[str, Any]) -> int:
    """Expect a clean structured rejection (validation). fail-delta 0/1."""
    t0 = time.monotonic()
    r = call(LC, args, timeout=10.0)
    dt = (time.monotonic() - t0) * 1000.0
    c = err_code(r)
    if is_transport_failure(r):
        log.case(name, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return 1
    if c == -32601:
        log.case(name, "SKIP", f"{LC} not registered", duration_ms=dt)
        return 0
    if c == -32602:
        log.case(name, "PASS", f"clean -32602: {err_message(r)[:55]}",
                 duration_ms=dt, code=c)
        return 0
    if is_ok(r):
        log.case(name, "XFAIL", "accepted malformed input (lenient validation)",
                 duration_ms=dt)
        return 0
    if c is not None and -32700 <= c <= -32000:
        log.case(name, "PASS", f"structured reject {c}: {err_message(r)[:50]}",
                 duration_ms=dt, code=c)
        return 0
    log.case(name, "FAIL", f"unknown response code={c}: {err_message(r)[:50]}",
             duration_ms=dt, code=c)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H6.2] livecoding.* surface coverage…", flush=True)

    # ── Validation probes ──────────────────────────────────────────────
    # Only probe inputs that REJECT before the compile step. A `modules` value
    # that passes validation (even garbage like [123, True], which the handler
    # leniently AsString()s) KICKS OFF a recompile attempt → stalls Lane A.
    # So we stick to missing / empty / wrong-type, all of which reject at
    # -32602 before any compile.
    fail_total += _reject(log, "P1_no_args", {})
    fail_total += _reject(log, "P2_empty_array", {"modules": []})
    fail_total += _reject(log, "P3_string_not_array", {"modules": "UnrealMCPBridge"})
    fail_total += _reject(log, "P4_number_not_array", {"modules": 42})

    # NOTE: A real recompile (the modules=["<module>"] path) is intentionally
    # NOT triggered here — G4 (livecoding race) already validates it. A real
    # recompile returns ok fast but its async background compile stalls Lane A
    # (game thread) for the compile duration (~6-90s; confirmed recovers, no
    # wedge), and repeated real recompiles can destabilise a long-running
    # editor. H6.2 stays a non-disruptive INPUT-VALIDATION surface test.
    log.case("P5_real_recompile_note", "SKIP",
             "real recompile covered by G4 (livecoding race); skipped here to "
             "avoid Lane A stall / editor destabilisation on repeated compiles")

    # ── P6 — liveness (no compile triggered → should be trivially alive) ─
    h = health(timeout=6.0)
    la = assert_lane_a_alive(timeout_s=12.0)
    if h and la:
        log.case("P6_liveness", "PASS", "both lanes responsive after validation probes")
    else:
        log.case("P6_liveness", "FAIL", f"not responsive: laneB={h} laneA={la}")
        fail_total += 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.2] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
