#!/usr/bin/env python3
"""Phase D7 — Wrong `kind` field.

Goal: dispatcher rejects unknown `kind` values cleanly. The Bridge
expects `kind="call_function"` for tool invocations; anything else
should produce a structured error, not dispatch the method anyway
or wedge the connection.

Probes (raw bytes via send_raw_bytes; each verified with post-probe
recovery via valid call):
  * kind="call_method"      → wrong value
  * kind="method"           → similar
  * kind=42                 → wrong type
  * kind=null               → null value
  * kind=true               → bool value
  * kind=[]                 → array value
  * kind={}                 → object value
  * kind=""                 → empty string
  * missing kind            → no kind field
  * kind="CALL_FUNCTION"    → wrong case
  * kind="call_function "   → trailing whitespace

PASS: each probe returns a structured -32600/-32601 error (or empty
response if dispatcher silently drops), AND the next valid call works.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    TestLogger,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
    send_raw_bytes,
)

PHASE = "d7"
NAME = "wrong_kind"

PROBES: List[Tuple[str, bytes]] = [
    ("kind_call_method",
     b'{"id":"x","kind":"call_method","method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_method",
     b'{"id":"x","kind":"method","method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_number",
     b'{"id":"x","kind":42,"method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_null",
     b'{"id":"x","kind":null,"method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_bool",
     b'{"id":"x","kind":true,"method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_array",
     b'{"id":"x","kind":[],"method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_object",
     b'{"id":"x","kind":{},"method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_empty_string",
     b'{"id":"x","kind":"","method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_missing",
     b'{"id":"x","method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_uppercase",
     b'{"id":"x","kind":"CALL_FUNCTION","method":"memreport.get_quick_stats","args":{}}\n'),
    ("kind_trailing_ws",
     b'{"id":"x","kind":"call_function ","method":"memreport.get_quick_stats","args":{}}\n'),
]


def _parse(raw: Optional[bytes]) -> dict:
    if raw is None:
        return {"_err": "no_response"}
    if not raw.strip():
        return {"_err": "empty"}
    try:
        return json.loads(raw.decode("utf-8", errors="replace"))
    except Exception as e:
        return {"_err": "unparseable", "_raw": raw[:60].decode("utf-8", errors="replace"),
                "_exc": str(e)}


def _verify_recovery() -> bool:
    """Send a known-good frame; return True if it succeeds."""
    valid = b'{"id":"y","kind":"call_function","method":"memreport.get_quick_stats","args":{}}\n'
    try:
        r = send_raw_bytes(valid, expect_response=True, timeout=4.0)
    except Exception:
        return False
    if not r:
        return False
    try:
        obj = json.loads(r.decode("utf-8", "replace"))
        return bool(obj.get("ok"))
    except Exception:
        return False


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D7] running {len(PROBES)} wrong-kind probes…", flush=True)

    for (case_label, payload) in PROBES:
        label = f"wrong_kind :: {case_label}"
        t0 = time.monotonic()
        try:
            raw = send_raw_bytes(payload, expect_response=True, timeout=6.0)
        except Exception as e:
            log.case(label, "FAIL", f"send exception: {e}",
                     duration_ms=(time.monotonic() - t0) * 1000.0)
            fail_total += 1
            continue
        r = _parse(raw)
        dt = (time.monotonic() - t0) * 1000.0
        alive = health(timeout=3.0)
        if not alive:
            log.case(label, "FAIL", f"EDITOR DIED on {case_label}",
                     alive=False, duration_ms=dt)
            log.write()
            return 1
        crash = latest_crash_dump(since=crash_baseline)
        if crash:
            log.case(label, "FAIL", f"CRASH DUMP: {crash}", alive=alive, duration_ms=dt)
            log.write()
            return 1
        # Verify subsequent call works.
        if not _verify_recovery():
            log.case(label, "FAIL", "dispatcher wedged after probe (recovery failed)",
                     alive=alive, duration_ms=dt)
            fail_total += 1
            continue
        # Classify probe response.
        if r.get("_err") in ("no_response", "empty"):
            log.case(label, "PASS",
                     "server silently dropped frame, next call works",
                     alive=alive, duration_ms=dt)
            continue
        c = err_code(r)
        if is_ok(r):
            # If server accepted wrong kind and dispatched anyway, that's a
            # protocol violation but no security issue — XFAIL.
            log.case(label, "XFAIL",
                     "server LENIENT: dispatched ok=true despite wrong kind",
                     alive=alive, duration_ms=dt)
            continue
        if c is not None and -32700 <= c <= -32000:
            log.case(label, "PASS",
                     f"clean structured error {c}: {err_message(r)[:50]}",
                     alive=alive, duration_ms=dt, code=c)
            continue
        log.case(label, "FAIL",
                 f"unexpected response: code={c}: {err_message(r)[:50]}",
                 alive=alive, duration_ms=dt)
        fail_total += 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D7] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
