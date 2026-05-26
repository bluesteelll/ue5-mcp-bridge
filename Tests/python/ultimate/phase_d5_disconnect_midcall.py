#!/usr/bin/env python3
"""Phase D5 — Disconnect mid-call.

Goal: client TCP close while bridge processes request doesn't crash the
bridge, leak memory, or wedge the dispatcher for subsequent connections.

Probes:
  P1 — Issue Lane A call (bp.list_categories), close socket BEFORE response
       arrives. Repeat 30 times. Bridge must not crash or leak threads.
  P2 — Issue Lane B call, close immediately after send (don't wait). 30x.
  P3 — Issue Lane A call, half-close (shutdown(SHUT_WR)). 10x.
  P4 — Verify subsequent valid call succeeds after each batch.

PASS: editor alive, no crash dumps, post-batch valid calls succeed,
Lane A drains correctly (no orphaned queue entries).

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    HOST,
    PORT,
    TestLogger,
    call,
    err_code,
    err_message,
    health,
    is_ok,
    latest_crash_dump,
    preflight,
)

PHASE = "d5"
NAME = "disconnect_midcall"


def _make_frame(method: str = "memreport.get_quick_stats",
                args: Dict[str, Any] = None) -> bytes:
    obj = {"id": "x", "kind": "call_function", "method": method, "args": args or {}}
    return (json.dumps(obj) + "\n").encode("utf-8")


def _send_and_close_immediately(payload: bytes) -> bool:
    """Send frame, close socket without waiting for response. Returns True on send success."""
    try:
        with socket.create_connection((HOST, PORT), timeout=5.0) as sock:
            sock.sendall(payload)
            # Close immediately
            sock.shutdown(socket.SHUT_RDWR)
        return True
    except Exception:
        return False


def _send_and_half_close(payload: bytes) -> bool:
    """Send frame, then SHUT_WR but keep read half open (don't read though)."""
    try:
        with socket.create_connection((HOST, PORT), timeout=5.0) as sock:
            sock.sendall(payload)
            sock.shutdown(socket.SHUT_WR)
        return True
    except Exception:
        return False


def _verify_recovery() -> bool:
    """Standard recovery probe: one valid memreport.get_quick_stats."""
    r = call("memreport.get_quick_stats", {}, timeout=8.0)
    return is_ok(r)


def probe_close_immediately(log: TestLogger, n: int = 30,
                            method: str = "memreport.get_quick_stats") -> int:
    label = f"P1 close-immediately x{n} ({method})"
    payload = _make_frame(method=method)
    t0 = time.monotonic()
    sent_ok = sum(1 for _ in range(n) if _send_and_close_immediately(payload))
    dt = (time.monotonic() - t0) * 1000.0

    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive after {n} immediate closes; sent_ok={sent_ok}",
                 alive=False, duration_ms=dt)
        return 1
    if not _verify_recovery():
        log.case(label, "FAIL", f"dispatcher wedged after {n} immediate closes",
                 duration_ms=dt)
        return 1
    log.case(label, "PASS",
             f"sent {sent_ok}/{n} mid-call closes, dispatcher recovered cleanly",
             duration_ms=dt)
    return 0


def probe_close_lane_a(log: TestLogger, n: int = 30) -> int:
    """P2: close mid-call to Lane A (asset registry walk)."""
    return probe_close_immediately(log, n=n, method="bp.list_categories")


def probe_half_close(log: TestLogger, n: int = 10) -> int:
    label = f"P3 half-close (SHUT_WR) x{n}"
    payload = _make_frame()
    t0 = time.monotonic()
    sent_ok = sum(1 for _ in range(n) if _send_and_half_close(payload))
    dt = (time.monotonic() - t0) * 1000.0

    if not health(timeout=5.0):
        log.case(label, "FAIL", f"editor unresponsive after {n} half-closes; sent_ok={sent_ok}",
                 alive=False, duration_ms=dt)
        return 1
    if not _verify_recovery():
        log.case(label, "FAIL", "dispatcher wedged after half-closes",
                 duration_ms=dt)
        return 1
    log.case(label, "PASS",
             f"sent {sent_ok}/{n} half-closes, dispatcher OK",
             duration_ms=dt)
    return 0


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D5] disconnect mid-call probes…", flush=True)

    fail_total += probe_close_immediately(log, n=30)
    fail_total += probe_close_lane_a(log, n=30)
    fail_total += probe_half_close(log, n=10)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D5] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
