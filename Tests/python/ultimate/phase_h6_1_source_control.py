#!/usr/bin/env python3
"""Phase H6.1 — sc.* source-control surface coverage.

The sc.* tools (status/checkout/diff/diff_binary/revert) wrap UE's
ISourceControlModule. Whether or not a provider (Git/Perforce) is
configured for this project, every tool MUST return a graceful structured
response — never crash, never hang. With no provider, the expected outcome
is a clean structured error (or an empty/ok result), not a transport
failure.

Probes (each: graceful structured response):
  P1 — sc.status            (overall / on a known asset path)
  P2 — sc.checkout          (on a /Game asset — may be no-op or provider err)
  P3 — sc.diff              (text diff request)
  P4 — sc.diff_binary       (binary diff request)
  P5 — sc.revert            (revert request — read path; provider-gated)
  P6 — malformed: sc.status with a hostile/overlong path → structured reject

VERDICT per probe:
  ok=true                              → PASS (provider present / no-op)
  structured Bridge error (-327xx)     → PASS (graceful, e.g. no provider)
  -32601 not registered                → SKIP
  transport failure / editor crash     → FAIL

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
    call,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "h6_1"
NAME = "source_control"

KNOWN_ASSET = "/Engine/BasicShapes/Cube.Cube"
GAME_ASSET = "/Game/__sc_probe_nonexistent__"


def _probe(log: TestLogger, name: str, method: str, args: Dict[str, Any],
           timeout: float = 12.0) -> int:
    """Returns fail-delta (0/1). PASS on ok OR any structured error."""
    t0 = time.monotonic()
    try:
        r = call(method, args, timeout=timeout)
    except Exception as e:
        r = {"_err": "exception", "_exc": str(e)}
    dt = (time.monotonic() - t0) * 1000.0

    if not health(timeout=4.0):
        log.case(name, "FAIL", f"EDITOR DIED after {method}", alive=False, duration_ms=dt)
        return 1
    if is_transport_failure(r):
        log.case(name, "FAIL", f"transport: {r.get('_err')}", duration_ms=dt)
        return 1
    if is_ok(r):
        res = r.get("result", {}) or {}
        keys = list(res.keys())[:5] if isinstance(res, dict) else []
        log.case(name, "PASS", f"{method} ok (result keys={keys})", duration_ms=dt)
        return 0
    c = err_code(r)
    if c == -32601:
        log.case(name, "SKIP", f"{method} not registered", duration_ms=dt)
        return 0
    if c is not None and -32700 <= c <= -32000:
        log.case(name, "PASS",
                 f"{method} graceful structured error {c}: {err_message(r)[:50]}",
                 duration_ms=dt, code=c)
        return 0
    log.case(name, "FAIL", f"{method} unknown response code={c}: {err_message(r)[:50]}",
             duration_ms=dt, code=c)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[H6.1] sc.* source-control surface coverage…", flush=True)

    fail_total += _probe(log, "P1_status", "sc.status", {"path": KNOWN_ASSET})
    fail_total += _probe(log, "P1b_status_noargs", "sc.status", {})
    fail_total += _probe(log, "P2_checkout", "sc.checkout", {"path": GAME_ASSET})
    fail_total += _probe(log, "P3_diff", "sc.diff", {"path": GAME_ASSET})
    fail_total += _probe(log, "P4_diff_binary", "sc.diff_binary", {"path": GAME_ASSET})
    fail_total += _probe(log, "P5_revert", "sc.revert", {"path": GAME_ASSET})
    # Malformed/hostile path — must reject gracefully (no FName/path crash).
    fail_total += _probe(log, "P6_hostile_path", "sc.status",
                         {"path": "/Game/" + ("Z" * 1100)})

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H6.1] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} "
          f"SKIP={cc.get('SKIP', 0)} TOTAL={cc['TOTAL']}")
    print(f"       log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
