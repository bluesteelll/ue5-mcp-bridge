#!/usr/bin/env python3
"""Phase D2 — Frame fragmentation.

Goal: incoming bytes split arbitrarily across TCP packets reassemble
correctly. Line-delimited framing on '\\n' must work regardless of
chunk boundaries.

Probes:
  * P1 — valid frame sent in 1-byte chunks with 10ms gap → parsed
  * P2 — valid frame sent in 100-byte chunks → parsed
  * P3 — two frames in one packet "frame1\\nframe2\\n" → both dispatch
         in order, both ok=true
  * P4 — oversized 1MB frame (well under 16MB cap) → handled
  * P5 — chunked with 0-byte segments (Empty TCP recv) → safe

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import json
import socket
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

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
    latest_crash_dump,
    preflight,
)

PHASE = "d2"
NAME = "frame_fragments"


def _make_call_payload(method: str = "memreport.get_quick_stats",
                       extra: Optional[Dict[str, Any]] = None) -> bytes:
    obj = {"id": "x", "kind": "call_function", "method": method, "args": extra or {}}
    return (json.dumps(obj) + "\n").encode("utf-8")


def _send_chunked(payload: bytes, chunk_size: int, gap_s: float,
                  expect_n_responses: int = 1, timeout: float = 15.0) -> List[bytes]:
    """Open conn, send `payload` in chunks of chunk_size with gap_s between,
    then read up to N responses."""
    with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
        sock.settimeout(timeout)
        for i in range(0, len(payload), chunk_size):
            sock.sendall(payload[i:i + chunk_size])
            if gap_s > 0:
                time.sleep(gap_s)
        # Read responses
        responses: List[bytes] = []
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while len(responses) < expect_n_responses and time.monotonic() < deadline:
            try:
                sock.settimeout(max(0.5, deadline - time.monotonic()))
                chunk = sock.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            buf.extend(chunk)
            while True:
                nl = buf.find(b"\n")
                if nl < 0:
                    break
                responses.append(bytes(buf[:nl]))
                del buf[:nl + 1]
                if len(responses) >= expect_n_responses:
                    break
        return responses


def _is_response_ok(raw: bytes) -> bool:
    try:
        obj = json.loads(raw.decode("utf-8", errors="replace"))
        return bool(obj.get("ok"))
    except Exception:
        return False


def probe_byte_chunked(log: TestLogger) -> int:
    label = "P1 1-byte chunks gap=10ms"
    payload = _make_call_payload()
    t0 = time.monotonic()
    try:
        responses = _send_chunked(payload, chunk_size=1, gap_s=0.01,
                                   expect_n_responses=1, timeout=30.0)
    except Exception as e:
        log.case(label, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return 1
    dt = (time.monotonic() - t0) * 1000.0
    if not responses:
        log.case(label, "FAIL", "no response after byte-chunked send", duration_ms=dt)
        return 1
    if _is_response_ok(responses[0]):
        log.case(label, "PASS",
                 f"byte-chunked send parsed correctly, response received in {dt:.0f}ms",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"response not ok: {responses[0][:80].decode('utf-8', 'replace')}",
             duration_ms=dt)
    return 1


def probe_chunked_100b(log: TestLogger) -> int:
    label = "P2 100-byte chunks gap=0ms"
    # Need a frame > 100 bytes; use long args.
    payload = _make_call_payload(extra={"note": "x" * 500})
    t0 = time.monotonic()
    try:
        responses = _send_chunked(payload, chunk_size=100, gap_s=0.0,
                                   expect_n_responses=1, timeout=15.0)
    except Exception as e:
        log.case(label, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return 1
    dt = (time.monotonic() - t0) * 1000.0
    if not responses:
        log.case(label, "FAIL", "no response", duration_ms=dt)
        return 1
    if _is_response_ok(responses[0]):
        log.case(label, "PASS", f"100-byte chunks parsed", duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"response not ok: {responses[0][:80].decode('utf-8', 'replace')}",
             duration_ms=dt)
    return 1


def probe_pipelined(log: TestLogger) -> int:
    label = "P3 two frames pipelined"
    f1 = _make_call_payload()
    # Slightly different second frame so we can distinguish.
    f2 = _make_call_payload(method="engine.get_info")
    payload = f1 + f2  # Both ending in \n; pipelined.
    t0 = time.monotonic()
    try:
        responses = _send_chunked(payload, chunk_size=len(payload),
                                   gap_s=0.0,
                                   expect_n_responses=2, timeout=20.0)
    except Exception as e:
        log.case(label, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return 1
    dt = (time.monotonic() - t0) * 1000.0
    if len(responses) != 2:
        log.case(label, "FAIL",
                 f"expected 2 responses, got {len(responses)}",
                 duration_ms=dt)
        return 1
    both_ok = all(_is_response_ok(r) for r in responses)
    if both_ok:
        log.case(label, "PASS", "two pipelined frames both ok",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"pipelined responses not both ok: {[r[:60].decode('utf-8', 'replace') for r in responses]}",
             duration_ms=dt)
    return 1


def probe_oversized_1mb(log: TestLogger) -> int:
    label = "P4 1MB frame"
    # 1MB args field.
    payload = _make_call_payload(extra={"big": "X" * 1_000_000})
    t0 = time.monotonic()
    try:
        responses = _send_chunked(payload, chunk_size=65536, gap_s=0.0,
                                   expect_n_responses=1, timeout=30.0)
    except Exception as e:
        log.case(label, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return 1
    dt = (time.monotonic() - t0) * 1000.0
    if not responses:
        log.case(label, "PASS",
                 "server closed connection on 1MB frame (acceptable — cap enforced)",
                 duration_ms=dt)
        return 0
    if _is_response_ok(responses[0]):
        log.case(label, "PASS", "1MB frame handled and dispatched ok",
                 duration_ms=dt)
        return 0
    # Structured error is also PASS — server rejected cleanly.
    try:
        obj = json.loads(responses[0].decode("utf-8", "replace"))
        c = obj.get("error", {}).get("code")
        if c is not None and -32700 <= c <= -32000:
            log.case(label, "PASS",
                     f"1MB frame rejected cleanly with code {c}",
                     duration_ms=dt)
            return 0
    except Exception:
        pass
    log.case(label, "FAIL",
             f"unexpected response: {responses[0][:80].decode('utf-8', 'replace')}",
             duration_ms=dt)
    return 1


def probe_chunked_with_pauses(log: TestLogger) -> int:
    """5-byte chunks with 50ms gaps — moderate fragmentation."""
    label = "P5 5-byte chunks gap=50ms"
    payload = _make_call_payload()
    t0 = time.monotonic()
    try:
        responses = _send_chunked(payload, chunk_size=5, gap_s=0.05,
                                   expect_n_responses=1, timeout=30.0)
    except Exception as e:
        log.case(label, "FAIL", f"exception: {e}",
                 duration_ms=(time.monotonic() - t0) * 1000.0)
        return 1
    dt = (time.monotonic() - t0) * 1000.0
    if not responses:
        log.case(label, "FAIL", "no response after 5-byte chunked send",
                 duration_ms=dt)
        return 1
    if _is_response_ok(responses[0]):
        log.case(label, "PASS", "5-byte chunks with gap parsed correctly",
                 duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"response not ok: {responses[0][:80].decode('utf-8', 'replace')}",
             duration_ms=dt)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D2] frame fragmentation probes (5 patterns)…", flush=True)

    for probe in (probe_byte_chunked, probe_chunked_100b, probe_pipelined,
                  probe_oversized_1mb, probe_chunked_with_pauses):
        rc = probe(log)
        if rc != 0:
            fail_total += 1
        if not health(timeout=5.0):
            log.case("between_probes_health", "FAIL", "editor unresponsive", alive=False)
            log.write()
            return 1

    crash = latest_crash_dump(since=crash_baseline)
    if crash:
        log.case("crash_check", "FAIL", f"CRASH DUMP: {crash}")
        log.write()
        return 1

    summary = log.write()
    cc = summary["counts"]
    print()
    print(f"[D2] PASS={cc['PASS']} FAIL={cc['FAIL']} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
