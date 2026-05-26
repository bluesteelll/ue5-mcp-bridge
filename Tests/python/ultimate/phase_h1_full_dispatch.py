#!/usr/bin/env python3
"""Phase H1 — Canonical 426-tool dispatch sweep.

Goal: every registered method dispatches successfully (no -32601
MethodNotFound). This is the CANONICAL coverage gate for the plugin —
identical in goal to A1 but treated as a separate phase so it can be
run periodically as a regression gate.

Approach:
  1. Discover all registered tool names via mcp.list_methods (Lane B).
  2. For each method, call with empty args ({}).
  3. PASS if response is one of: ok=true OR any structured Bridge error
     in [-32700, -32000] range. FAIL only on -32601 (method not found)
     or transport / editor death.

Editor state expected: PIE OFF (PIE-required tools will XFAIL with
-32038 — that's fine, it's a structured error, not a dispatch failure).

Exit codes: 0=PASS (all dispatched), 1=FAIL (any -32601 / crash),
            2=preflight.
"""

from __future__ import annotations

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
    discover_methods,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
)

PHASE = "h1"
NAME = "full_dispatch"


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    methods = discover_methods()
    if not methods:
        log.case("discover", "FAIL", "no methods discovered")
        log.write()
        return 1

    print(f"[H1] dispatching {len(methods)} canonical tools…", flush=True)

    not_found = 0
    transport_fail = 0
    ok_count = 0
    structured_err_count = 0

    for i, method in enumerate(methods):
        t0 = time.monotonic()
        try:
            r = call(method, {}, timeout=6.0)
        except Exception as e:
            log.case(method, "FAIL", f"exception: {e}",
                     duration_ms=(time.monotonic() - t0) * 1000.0)
            fail_total += 1
            transport_fail += 1
            continue
        dt = (time.monotonic() - t0) * 1000.0

        if is_transport_failure(r):
            transport_fail += 1
            # Editor alive check between transport failures
            if not health(timeout=3.0):
                log.case(method, "FAIL", f"editor died on {method}",
                         alive=False, duration_ms=dt)
                log.write()
                return 1
            log.case(method, "FAIL", f"transport: {r.get('_err')}",
                     duration_ms=dt)
            fail_total += 1
            continue

        c = err_code(r)
        if c == -32601:
            not_found += 1
            log.case(method, "FAIL",
                     f"-32601 METHOD NOT FOUND — dispatch missing",
                     duration_ms=dt, code=c)
            fail_total += 1
            continue
        if is_ok(r):
            ok_count += 1
            log.case(method, "PASS", "ok=true", duration_ms=dt)
        elif c is not None and -32700 <= c <= -32000:
            structured_err_count += 1
            log.case(method, "PASS",
                     f"structured error {c}: {err_message(r)[:40]}",
                     duration_ms=dt, code=c)
        else:
            log.case(method, "FAIL",
                     f"unexpected response: code={c}: {err_message(r)[:40]}",
                     duration_ms=dt, code=c)
            fail_total += 1

        # Periodic crash check
        if i > 0 and (i % 50) == 0:
            crash = latest_crash_dump(since=crash_baseline)
            if crash:
                log.case("interim_crash", "FAIL", f"CRASH DUMP: {crash}")
                log.write()
                return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[H1] total={len(methods)} dispatched={ok_count + structured_err_count} "
          f"(ok={ok_count}, structured_err={structured_err_count}) "
          f"NOT_FOUND={not_found} transport_fail={transport_fail}")
    print(f"     PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
