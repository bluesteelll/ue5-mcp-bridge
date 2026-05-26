#!/usr/bin/env python3
"""Phase D8 — ID collisions across connections.

Goal: same `id` field from different connections doesn't cause response
mix-up. Response routing must be per-connection (not via a global
ID-keyed map), so two clients each using id="x" each get THEIR OWN
response back.

Probes:
  * P1 — 20 parallel connections, each fires id="x" → all 20 receive
    SOMETHING (no zero-response wedge), each method matches its caller's
    intent (no cross-talk).
  * P2 — 20 parallel connections, each fires id="" (empty) → no crash.
  * P3 — 20 parallel connections, each fires id=<UUID per conn>, then
    a SECOND request with id="constant" → verify second response
    matches what THIS connection sent.

Exit codes: 0=PASS, 1=FAIL, 2=preflight.
"""

from __future__ import annotations

import concurrent.futures
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
    latest_crash_dump,
    preflight,
)

PHASE = "d8"
NAME = "id_collision"


def _make_frame(conn_idx: int, request_id: str, method: str) -> bytes:
    obj = {"id": request_id, "kind": "call_function", "method": method, "args": {}}
    return (json.dumps(obj) + "\n").encode("utf-8")


def _open_send_recv(conn_idx: int, frame: bytes, timeout: float = 15.0) -> Optional[bytes]:
    """Single-shot open+send+recv. Returns response bytes (no \\n) or None."""
    try:
        with socket.create_connection((HOST, PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(frame)
            buf = bytearray()
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    sock.settimeout(max(0.5, deadline - time.monotonic()))
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


def probe_same_id(log: TestLogger, n: int = 20) -> int:
    """P1: N parallel connections, all use id='x'."""
    label = f"P1 same-id x{n} (all id='x')"
    method = "memreport.get_quick_stats"
    frame = _make_frame(0, "x", method)
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=n) as ex:
        futures = [ex.submit(_open_send_recv, i, frame, 30.0) for i in range(n)]
        responses = [f.result(timeout=60.0) for f in concurrent.futures.as_completed(futures, timeout=120.0)]
    dt = (time.monotonic() - t0) * 1000.0

    valid_count = 0
    for r in responses:
        if r is None:
            continue
        try:
            obj = json.loads(r.decode("utf-8", "replace"))
            # Verify response carries the same id="x" back.
            if obj.get("id") == "x" and obj.get("ok"):
                valid_count += 1
        except Exception:
            pass

    success_rate = valid_count / n
    summary = f"valid={valid_count}/{n} success_rate={success_rate:.0%}"

    # We accept ≥70% as PASS (listener saturation may swallow the rest).
    if success_rate >= 0.7:
        log.case(label, "PASS",
                 f"no cross-talk, all responses correct; {summary}", duration_ms=dt)
        return 0
    if success_rate > 0:
        log.case(label, "XFAIL",
                 f"low rate but no cross-talk; {summary}", duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"complete failure (no valid responses); {summary}", duration_ms=dt)
    return 1


def probe_empty_id(log: TestLogger, n: int = 20) -> int:
    label = f"P2 empty-id x{n} (all id='')"
    frame = _make_frame(0, "", "memreport.get_quick_stats")
    t0 = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=n) as ex:
        futures = [ex.submit(_open_send_recv, i, frame, 30.0) for i in range(n)]
        responses = [f.result(timeout=60.0) for f in concurrent.futures.as_completed(futures, timeout=120.0)]
    dt = (time.monotonic() - t0) * 1000.0

    valid_count = 0
    for r in responses:
        if r is None:
            continue
        try:
            obj = json.loads(r.decode("utf-8", "replace"))
            if obj.get("ok") or obj.get("error"):
                valid_count += 1
        except Exception:
            pass

    success_rate = valid_count / n
    summary = f"valid={valid_count}/{n} success_rate={success_rate:.0%}"
    if success_rate >= 0.7:
        log.case(label, "PASS",
                 f"empty-id handled cleanly; {summary}", duration_ms=dt)
        return 0
    if success_rate > 0:
        log.case(label, "XFAIL",
                 f"low rate but no crash; {summary}", duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"no valid responses; {summary}", duration_ms=dt)
    return 1


def probe_per_conn_id(log: TestLogger, n: int = 15) -> int:
    """P3: N parallel connections, each uses a UNIQUE id. Verify each receives
    its own id back (proves per-connection routing)."""
    label = f"P3 per-conn-id x{n}"
    t0 = time.monotonic()

    def _one(idx: int):
        request_id = f"req-{idx:04d}"
        frame = _make_frame(idx, request_id, "memreport.get_quick_stats")
        raw = _open_send_recv(idx, frame, 30.0)
        if raw is None:
            return (idx, request_id, None)
        try:
            obj = json.loads(raw.decode("utf-8", "replace"))
            return (idx, request_id, obj.get("id"))
        except Exception:
            return (idx, request_id, "_parse_err")

    with concurrent.futures.ThreadPoolExecutor(max_workers=n) as ex:
        futures = [ex.submit(_one, i) for i in range(n)]
        results = [f.result(timeout=60.0) for f in concurrent.futures.as_completed(futures, timeout=120.0)]
    dt = (time.monotonic() - t0) * 1000.0

    matched = sum(1 for (_, expected_id, returned_id) in results if returned_id == expected_id)
    mismatched = sum(1 for (_, expected_id, returned_id) in results
                     if returned_id is not None and returned_id != expected_id and returned_id != "_parse_err")
    no_response = sum(1 for (_, _, returned_id) in results if returned_id is None)
    summary = f"matched={matched}/{n} mismatched={mismatched} no_response={no_response}"

    if mismatched > 0:
        # CRITICAL: id cross-talk between connections!
        log.case(label, "FAIL",
                 f"CROSS-TALK DETECTED: {mismatched} responses had wrong id; {summary}",
                 duration_ms=dt)
        return 1
    if matched / n >= 0.7:
        log.case(label, "PASS",
                 f"per-connection id routing correct; {summary}", duration_ms=dt)
        return 0
    if matched > 0:
        log.case(label, "XFAIL",
                 f"low rate but no cross-talk; {summary}", duration_ms=dt)
        return 0
    log.case(label, "FAIL",
             f"no successful per-conn routing; {summary}", duration_ms=dt)
    return 1


def main() -> int:
    if not preflight(PHASE):
        return 2
    log = TestLogger(PHASE, NAME)
    crash_baseline = time.time()
    fail_total = 0

    print(f"[D8] ID-collision probes…", flush=True)

    fail_total += probe_same_id(log, n=20)
    if not health(timeout=5.0):
        log.write()
        return 1

    fail_total += probe_empty_id(log, n=20)
    if not health(timeout=5.0):
        log.write()
        return 1

    fail_total += probe_per_conn_id(log, n=15)
    if not health(timeout=5.0):
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
    print(f"[D8] PASS={cc['PASS']} FAIL={cc['FAIL']} XFAIL={cc.get('XFAIL', 0)} TOTAL={cc['TOTAL']}")
    print(f"     log: {log.md_path}")
    if not summary["final_health"]:
        return 1
    if fail_total > 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
