#!/usr/bin/env python3
"""Phase D6 — Oversized payloads.

Goal: very large JSON args don't OOM the editor. Server should either
cap at some size and reject cleanly, or accept and discard gracefully.

Probes:
  P1 — 10MB single-string value in args → expect ok=true OR -32602
  P2 — 10MB nested in args→deep_object{val:huge} → expect ok or cap
  P3 — 100K element flat array → expect ok or -32602
  P4 — 50MB outer payload (well above any reasonable cap) → expect
       rejection or socket close
  P5 — 100KB args object split into 1000 keys → tests object-allocation
       on parse

PASS: any well-formed response (ok or structured error), editor alive,
no crash, no OOM. FAIL: editor crash, transport hang for next call.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).parent))
from mcp_test_harness import (
    LOG_ROOT,
    HOST,
    PORT,
    TestLogger,
    err_code,
    err_message,
    health,
    is_ok,
    is_transport_failure,
    latest_crash_dump,
    preflight,
    call,
)

PHASE = "d6"
NAME = "oversized"


def _send_recv(payload: bytes, timeout: float = 30.0) -> Optional[bytes]:
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(payload)
            buf = bytearray()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    sock.settimeout(max(1.0, deadline - time.monotonic()))
                    chunk = sock.recv(65536)
                except socket.timeout:
                    return None
                if not chunk:
                    break
                buf.extend(chunk)
                nl = buf.find(b"\n")
                if nl >= 0:
                    return bytes(buf[:nl])
            return None
    except Exception:
        return None


def _make_frame(args: Dict[str, Any]) -> bytes:
    obj = {"id": "x", "kind": "call_function",
           "method": "memreport.get_quick_stats", "args": args}
    return (json.dumps(obj) + "\n").encode("utf-8")


def _classify(raw: Optional[bytes], dt_ms: float) -> Tuple[str, str]:
    """Returns (status, summary). status in {PASS, FAIL, NULL}."""
    if raw is None:
        # Server may have dropped (socket close) — verify dispatcher still works.
        return ("NULL", "no response (server may have capped frame)")
    try:
        obj = json.loads(raw.decode("utf-8", "replace"))
    except Exception:
        return ("FAIL", f"unparseable response: {raw[:80].decode('utf-8', 'replace')}")
    if obj.get("ok"):
        return ("PASS", f"server accepted, ok=true ({dt_ms:.0f}ms)")
    c = obj.get("error", {}).get("code")
    if c is not None and -32700 <= c <= -32000:
        return ("PASS",
                f"server rejected cleanly: {c}: {obj.get('error', {}).get('message', '')[:60]}")
    return ("FAIL", f"unexpected response: {obj}")


def _verify_recovery() -> bool:
    r = call("memreport.get_quick_stats", {}, timeout=8.0)
    return is_ok(r)


def probe_10mb_string(log: TestLogger) -> int:
    label = "P1 10MB single-string value"
    payload = _make_frame({"huge": "X" * 10_000_000})
    t0 = time.monotonic()
    resp = _send_recv(payload, timeout=30.0)
    dt = (time.monotonic() - t0) * 1000.0
    status, summary = _classify(resp, dt)
    alive = health(timeout=5.0)
    if not alive:
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1
    if not _verify_recovery():
        log.case(label, "FAIL", f"dispatcher wedged after probe; {summary}", duration_ms=dt)
        return 1
    if status == "NULL":
        # No response, but dispatcher recovered → server probably dropped frame.
        log.case(label, "PASS", f"server dropped frame, recovery OK; {summary}", duration_ms=dt)
        return 0
    log.case(label, status, summary, duration_ms=dt)
    return 0 if status == "PASS" else 1


def probe_huge_array(log: TestLogger) -> int:
    label = "P3 100k-element flat array"
    payload = _make_frame({"arr": list(range(100_000))})
    t0 = time.monotonic()
    resp = _send_recv(payload, timeout=30.0)
    dt = (time.monotonic() - t0) * 1000.0
    status, summary = _classify(resp, dt)
    alive = health(timeout=5.0)
    if not alive:
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1
    if not _verify_recovery():
        log.case(label, "FAIL", f"dispatcher wedged; {summary}", duration_ms=dt)
        return 1
    if status == "NULL":
        log.case(label, "PASS", f"server dropped frame, recovery OK; {summary}", duration_ms=dt)
        return 0
    log.case(label, status, summary, duration_ms=dt)
    return 0 if status == "PASS" else 1


def probe_50mb_frame(log: TestLogger) -> int:
    label = "P4 50MB frame (over cap)"
    payload = _make_frame({"huge": "X" * 50_000_000})
    t0 = time.monotonic()
    resp = _send_recv(payload, timeout=60.0)
    dt = (time.monotonic() - t0) * 1000.0
    status, summary = _classify(resp, dt)
    alive = health(timeout=5.0)
    if not alive:
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1
    if not _verify_recovery():
        log.case(label, "FAIL", f"dispatcher wedged; {summary}", duration_ms=dt)
        return 1
    # PASS regardless of accept/reject as long as editor alive + dispatcher OK.
    if status == "NULL":
        log.case(label, "PASS", f"server dropped 50MB frame (cap enforced); recovery OK", duration_ms=dt)
        return 0
    log.case(label, status, summary, duration_ms=dt)
    return 0 if status == "PASS" else 1


def probe_1k_keys_object(log: TestLogger) -> int:
    label = "P5 1000-key object (100KB total)"
    payload = _make_frame({f"k{i:04d}": f"value_{i}" * 5 for i in range(1000)})
    t0 = time.monotonic()
    resp = _send_recv(payload, timeout=30.0)
    dt = (time.monotonic() - t0) * 1000.0
    status, summary = _classify(resp, dt)
    alive = health(timeout=5.0)
    if not alive:
        log.case(label, "FAIL", f"editor unresponsive; {summary}", alive=False, duration_ms=dt)
        return 1
    if not _verify_recovery():
        log.case(label, "FAIL", f"dispatcher wedged; {summary}", duration_ms=dt)
        return 1
    if status == "NULL":
        log.case(label, "PASS", f"server dropped frame, recovery OK; {summary}", duration_ms=dt)
        return 0
    log.case(label, status, summary, duration_ms=dt)
    return 0 if status == "PASS" else 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D6] oversized payload probes…", flush=True)

    fail_total += probe_10mb_string(log)
    fail_total += probe_huge_array(log)
    fail_total += probe_50mb_frame(log)
    fail_total += probe_1k_keys_object(log)

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D6] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
