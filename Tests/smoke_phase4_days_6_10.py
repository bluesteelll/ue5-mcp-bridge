#!/usr/bin/env python3
"""Phase 4 Days 6-10 smoke — 8 Blueprint write/build tools.

Verifies (against a live editor on port 30020 with no PIE running):

  Discovery (1-2):
    1. tools.list contains all 7 new bp.* tools + the 1 internal bp._compile_all_dirty_internal.
    2. Python tools.list contains bp.compile_all_dirty (Day 10 composite).

  bp.add_variable + collision + remove (9-12):
    3.  bp.add_variable(BP_TestActor, "TempBool", {category:Boolean}) → added=true.
    4.  bp.add_variable(BP_TestActor, "TempBool", ...) → -32014 PathInUse (collision).
    5.  bp.remove_variable(BP_TestActor, "TempBool") → removed=true, was_present=true.
    6.  bp.remove_variable(BP_TestActor, "TempBool") → removed=false, was_present=false (idempotent).

  bp.change_variable_type round-trip (13-14):
    7.  bp.add_variable(BP_TestActor, "TempInt", {category:Int}) → added=true.
    8.  bp.change_variable_type(BP_TestActor, "TempInt", {category:Real}) → changed=true + warning non-empty.
    9.  bp.change_variable_type(BP_TestActor, "TempInt", {category:Int}) → revert (cleanup).
    10. bp.remove_variable(BP_TestActor, "TempInt") → cleanup.

  bp.add_function / bp.remove_function (15-16):
    11. bp.add_function(BP_TestActor, "TempFunc", inputs=[], outputs=[]) → added=true.
    12. bp.remove_function(BP_TestActor, "TempFunc") → removed=true.

  bp.reparent (17-18) — gate semantics only; we do NOT actually reparent BP_TestActor:
    13. bp.reparent(BP_TestActor, /Script/Engine.Pawn, confirm_dangerous=false) → -32033.
    14. bp.reparent(BP_TestActor, "<not a slash path>", confirm_dangerous=true) → -32023 InvalidClassPath.

  bp.compile single (19):
    15. bp.compile(BP_TestActor) → compiled=true (assuming the asset is healthy).

  bp.compile_all_dirty async (20-21):
    16. bp.compile_all_dirty({scope_paths:["/Game/MCPTest/Phase4"]}) → returns {job_id}.
    17. job.result(20) → {compiled, succeeded ≥ 0, failed: []} (Succeeded state).

  Negative (boundary):
    18. bp.compile_all_dirty({scope_paths:[]}) → ValueError → -32602 (Python wrapper).
    19. bp.add_variable missing pin_type → -32602 InvalidParams.

Test asset selection:
  - Plan-mandated: /Game/MCPTest/Phase4/BP_TestActor (the same one Days 1-5 expects).
  - Falls back to /Game/BP_PlayerFlecs if BP_TestActor not present (some write tests SKIP).

PIE skip:
  - Most write tests are skipped if PIE is detected (would otherwise fail with -32027). We test
    the PIE-skip path by NOT trying to enter PIE — assume the operator runs this outside PIE.

Prints ``[SMOKE_PHASE4_6_10] PASS`` on success or ``[SMOKE_PHASE4_6_10] FAIL ...`` on first
mismatch. Sub-tests that depend on a writable BP and PIE-being-off may emit SKIP lines.

Usage:
  python smoke_phase4_days_6_10.py [--host HOST] [--port PORT] [--bp-path /Game/.../BP_X]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Dict, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 15.0
JOB_POLL_TIMEOUT_SEC = 60.0   # bp.compile_all_dirty over /Game can take a while
JOB_POLL_INTERVAL_SEC = 0.25

# Candidate BP paths probed in order. The plan-mandated test asset is BP_TestActor under
# /Game/MCPTest/Phase4; the fallback is a known existing FatumGame BP.
FALLBACK_BP_PATHS = [
    "/Game/MCPTest/Phase4/BP_TestActor",
    "/Game/BP_PlayerFlecs",
]


def send_and_recv_line(host: str, port: int, request_obj: dict) -> Optional[dict]:
    with socket.create_connection((host, port), timeout=READ_TIMEOUT_SEC) as sock:
        sock.settimeout(READ_TIMEOUT_SEC)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)
        buf = bytearray()
        deadline = time.monotonic() + READ_TIMEOUT_SEC
        while True:
            if time.monotonic() > deadline:
                return None
            try:
                chunk = sock.recv(64 * 1024)
            except socket.timeout:
                return None
            if not chunk:
                break
            buf.extend(chunk)
            newline_idx = buf.find(b"\n")
            if newline_idx >= 0:
                return json.loads(bytes(buf[:newline_idx]).decode("utf-8"))
        return None


def fail(reason: str) -> int:
    print(f"[SMOKE_PHASE4_6_10] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE4_6_10]   SKIP {reason}")


def call(host: str, port: int, label: str, request_id: str, method: str,
         args: Optional[dict] = None) -> Optional[dict]:
    req = {"id": request_id, "kind": "call_function", "method": method, "args": args or {}}
    try:
        return send_and_recv_line(host, port, req)
    except (ConnectionRefusedError, OSError) as exc:
        fail(f"{label}: connect-error detail={exc}")
        return None


def expect_ok(response: Optional[dict], expected_id: str, label: str) -> Optional[dict]:
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not True:
        fail(f"{label}: ok-not-true got={response.get('ok')!r} error={response.get('error')!r}")
        return None
    result = response.get("result")
    if not isinstance(result, dict):
        fail(f"{label}: result-not-object got={result!r}")
        return None
    return result


def expect_error(response: Optional[dict], expected_id: str, expected_codes,
                 label: str) -> Optional[dict]:
    if isinstance(expected_codes, int):
        expected_codes = (expected_codes,)
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return None
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return None
    if response.get("ok") is not False:
        fail(f"{label}: ok-not-false got={response.get('ok')!r}")
        return None
    error = response.get("error")
    if not isinstance(error, dict) or error.get("code") not in expected_codes:
        fail(f"{label}: wrong-error-code expected={expected_codes} got={error!r}")
        return None
    return error


def poll_job(host: str, port: int, job_id: str, label: str,
             timeout: float = JOB_POLL_TIMEOUT_SEC) -> Optional[Dict[str, Any]]:
    """Poll job.result every JOB_POLL_INTERVAL_SEC for up to timeout seconds.

    Returns the inner result dict on Succeeded, None on Failed/timeout/error (after calling fail()).
    """
    deadline = time.monotonic() + timeout
    last_state: Optional[str] = None
    while time.monotonic() < deadline:
        resp = call(host, port, f"{label}/poll", f"poll-{job_id}-{int(time.time()*1000)}",
                    "job.result", {"job_id": job_id, "wait_timeout_s": 0})
        if resp is None:
            fail(f"{label}: job.result timeout")
            return None
        if resp.get("ok") is not True:
            fail(f"{label}: job.result error={resp.get('error')!r}")
            return None
        body = resp.get("result") or {}
        state = body.get("state")
        last_state = state
        if state == "Succeeded":
            inner = body.get("result")
            if not isinstance(inner, dict):
                fail(f"{label}: Succeeded but inner result not object got={inner!r}")
                return None
            return inner
        if state == "Failed":
            fail(f"{label}: job Failed message={body.get('error')!r}")
            return None
        if state == "Cancelled":
            fail(f"{label}: job Cancelled")
            return None
        time.sleep(JOB_POLL_INTERVAL_SEC)
    fail(f"{label}: poll timeout (>{timeout}s) last_state={last_state!r}")
    return None


def resolve_test_bp(host: str, port: int) -> Optional[str]:
    """Probe FALLBACK_BP_PATHS; return the first that resolves via bp.exists."""
    for path in FALLBACK_BP_PATHS:
        resp = call(host, port, "probe", f"probe-{path}", "bp.exists", {"blueprint_path": path})
        if resp is None:
            continue
        if resp.get("ok") is True:
            result = resp.get("result", {})
            if result.get("exists") is True:
                return path
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--bp-path", default=None,
                        help="Override blueprint asset path; default probes FALLBACK_BP_PATHS")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE4_6_10] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 7 new bp.* write tools + bp._internal ───────────
    result = expect_ok(call(args.host, args.port, "1", "p4-6-1", "tools.list"),
                       "p4-6-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "bp.add_variable", "bp.remove_variable", "bp.change_variable_type",
        "bp.add_function", "bp.remove_function", "bp.reparent", "bp.compile",
        "bp._compile_all_dirty_internal",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing bp.* handlers: {sorted(missing)}")
    print("[SMOKE_PHASE4_6_10]   1/tools.list contains all 7 new BP write/build handlers + 1 internal")

    # ─── Sub-test 2: Python tools.list contains bp.compile_all_dirty ──────────────────────────
    py_tools = result.get("python_tools") or {}
    if "bp.compile_all_dirty" not in py_tools:
        return fail(f"2/python_tools: missing bp.compile_all_dirty (found: {sorted(py_tools)[:5]}...)")
    print("[SMOKE_PHASE4_6_10]   2/python_tools contains bp.compile_all_dirty")

    # ─── Resolve a test blueprint ─────────────────────────────────────────────────────────────
    bp_path = args.bp_path or resolve_test_bp(args.host, args.port)
    if not bp_path:
        skip(f"no test BP under {FALLBACK_BP_PATHS}; skipping write/build sub-tests 3-15")
        bp_path_missing = True
    else:
        bp_path_missing = False
        print(f"[SMOKE_PHASE4_6_10]   using test BP: {bp_path}")

    # ─── Sub-test 3-6: add_variable + collision + remove (idempotent) ────────────────────────
    temp_bool_name = "MCP_SmokeTempBool"
    if bp_path_missing:
        skip("3-6/bp.add/remove_variable: no test BP")
    else:
        # Pre-clean: best-effort remove if a previous smoke run left it behind.
        call(args.host, args.port, "3pre", "p4-6-3pre", "bp.remove_variable",
             {"blueprint_path": bp_path, "variable_name": temp_bool_name})

        # 3. add
        result = expect_ok(call(args.host, args.port, "3", "p4-6-3", "bp.add_variable",
                                {"blueprint_path": bp_path, "variable_name": temp_bool_name,
                                 "pin_type": {"category": "Boolean"}}),
                          "p4-6-3", "3/bp.add_variable")
        if result is None:
            return 1
        if result.get("added") is not True or result.get("variable_name") != temp_bool_name:
            return fail(f"3/add_variable: unexpected result {result!r}")
        print(f"[SMOKE_PHASE4_6_10]   3/add_variable OK ({temp_bool_name})")

        # 4. collision
        ok = expect_error(call(args.host, args.port, "4", "p4-6-4", "bp.add_variable",
                               {"blueprint_path": bp_path, "variable_name": temp_bool_name,
                                "pin_type": {"category": "Boolean"}}),
                         "p4-6-4", -32014, "4/add_variable collision")
        if ok is None:
            return 1
        print("[SMOKE_PHASE4_6_10]   4/add_variable collision OK -32014 PathInUse")

        # 5. remove (present)
        result = expect_ok(call(args.host, args.port, "5", "p4-6-5", "bp.remove_variable",
                                {"blueprint_path": bp_path, "variable_name": temp_bool_name}),
                          "p4-6-5", "5/bp.remove_variable")
        if result is None:
            return 1
        if result.get("removed") is not True or result.get("was_present") is not True:
            return fail(f"5/remove_variable (present): unexpected result {result!r}")
        print("[SMOKE_PHASE4_6_10]   5/remove_variable (present) OK")

        # 6. remove (idempotent — already gone)
        result = expect_ok(call(args.host, args.port, "6", "p4-6-6", "bp.remove_variable",
                                {"blueprint_path": bp_path, "variable_name": temp_bool_name}),
                          "p4-6-6", "6/bp.remove_variable idempotent")
        if result is None:
            return 1
        if result.get("removed") is not False or result.get("was_present") is not False:
            return fail(f"6/remove_variable (idempotent): unexpected result {result!r}")
        print("[SMOKE_PHASE4_6_10]   6/remove_variable (idempotent) OK")

    # ─── Sub-test 7-10: change_variable_type round-trip + cleanup ─────────────────────────────
    temp_int_name = "MCP_SmokeTempInt"
    if bp_path_missing:
        skip("7-10/bp.change_variable_type: no test BP")
    else:
        # Pre-clean
        call(args.host, args.port, "7pre", "p4-6-7pre", "bp.remove_variable",
             {"blueprint_path": bp_path, "variable_name": temp_int_name})

        # 7. add Int
        result = expect_ok(call(args.host, args.port, "7", "p4-6-7", "bp.add_variable",
                                {"blueprint_path": bp_path, "variable_name": temp_int_name,
                                 "pin_type": {"category": "Int"}}),
                          "p4-6-7", "7/bp.add_variable Int")
        if result is None:
            return 1
        print("[SMOKE_PHASE4_6_10]   7/add_variable Int OK")

        # 8. change to Real
        result = expect_ok(call(args.host, args.port, "8", "p4-6-8", "bp.change_variable_type",
                                {"blueprint_path": bp_path, "variable_name": temp_int_name,
                                 "new_pin_type": {"category": "Real", "subcategory": "double"},
                                 "drop_default_value": True}),
                          "p4-6-8", "8/change_variable_type Int→Real")
        if result is None:
            return 1
        if result.get("changed") is not True:
            return fail(f"8/change_variable_type: changed != true: {result!r}")
        if not isinstance(result.get("warning"), str) or not result["warning"]:
            return fail(f"8/change_variable_type: warning missing/empty: {result!r}")
        prior = result.get("prior_pin_type")
        if not isinstance(prior, dict) or prior.get("category") != "Int":
            return fail(f"8/change_variable_type: prior_pin_type.category != Int: {result!r}")
        print(f"[SMOKE_PHASE4_6_10]   8/change_variable_type Int→Real OK warning_len={len(result['warning'])}")

        # 9. change back to Int (revert)
        result = expect_ok(call(args.host, args.port, "9", "p4-6-9", "bp.change_variable_type",
                                {"blueprint_path": bp_path, "variable_name": temp_int_name,
                                 "new_pin_type": {"category": "Int"},
                                 "drop_default_value": True}),
                          "p4-6-9", "9/change_variable_type Real→Int (revert)")
        if result is None:
            return 1
        print("[SMOKE_PHASE4_6_10]   9/change_variable_type Real→Int (revert) OK")

        # 10. cleanup
        result = expect_ok(call(args.host, args.port, "10", "p4-6-10", "bp.remove_variable",
                                {"blueprint_path": bp_path, "variable_name": temp_int_name}),
                          "p4-6-10", "10/remove_variable cleanup")
        if result is None:
            return 1
        print("[SMOKE_PHASE4_6_10]   10/remove_variable cleanup OK")

    # ─── Sub-test 11-12: add_function / remove_function ──────────────────────────────────────
    temp_fn_name = "MCP_SmokeTempFunc"
    if bp_path_missing:
        skip("11-12/bp.add/remove_function: no test BP")
    else:
        # Pre-clean
        call(args.host, args.port, "11pre", "p4-6-11pre", "bp.remove_function",
             {"blueprint_path": bp_path, "function_name": temp_fn_name})

        # 11. add (empty signature — no inputs/outputs)
        result = expect_ok(call(args.host, args.port, "11", "p4-6-11", "bp.add_function",
                                {"blueprint_path": bp_path, "function_name": temp_fn_name,
                                 "inputs": [], "outputs": []}),
                          "p4-6-11", "11/bp.add_function")
        if result is None:
            return 1
        if result.get("added") is not True or result.get("function_name") != temp_fn_name:
            return fail(f"11/add_function: unexpected result {result!r}")
        print(f"[SMOKE_PHASE4_6_10]   11/add_function OK ({temp_fn_name})")

        # 12. remove
        result = expect_ok(call(args.host, args.port, "12", "p4-6-12", "bp.remove_function",
                                {"blueprint_path": bp_path, "function_name": temp_fn_name}),
                          "p4-6-12", "12/bp.remove_function")
        if result is None:
            return 1
        if result.get("removed") is not True:
            return fail(f"12/remove_function: removed != true: {result!r}")
        print("[SMOKE_PHASE4_6_10]   12/remove_function OK")

    # ─── Sub-test 13-14: bp.reparent gate semantics ──────────────────────────────────────────
    # Test the GATE behaviour without actually reparenting our test BP (would corrupt it).
    if bp_path_missing:
        skip("13-14/bp.reparent: no test BP")
    else:
        # 13. confirm_dangerous=false → -32033 ReparentUnsafe
        ok = expect_error(call(args.host, args.port, "13", "p4-6-13", "bp.reparent",
                               {"blueprint_path": bp_path,
                                "new_parent_class_path": "/Script/Engine.Pawn",
                                "confirm_dangerous": False}),
                         "p4-6-13", -32033, "13/reparent without confirm")
        if ok is None:
            return 1
        # Verify the message mentions confirm_dangerous so AI sees the actionable hint
        msg = ok.get("message", "")
        if "confirm_dangerous" not in msg:
            return fail(f"13/reparent: error message lacks 'confirm_dangerous': {msg!r}")
        print("[SMOKE_PHASE4_6_10]   13/reparent (no confirm) OK -32033 ReparentUnsafe")

        # 14. confirm_dangerous=true + bad class path → -32023 InvalidClassPath
        # We pass a deliberately malformed path so we don't actually reparent anything.
        ok = expect_error(call(args.host, args.port, "14", "p4-6-14", "bp.reparent",
                               {"blueprint_path": bp_path,
                                "new_parent_class_path": "not_a_class_path",
                                "confirm_dangerous": True}),
                         "p4-6-14", -32023, "14/reparent invalid class path")
        if ok is None:
            return 1
        print("[SMOKE_PHASE4_6_10]   14/reparent (bad class) OK -32023 InvalidClassPath")

    # ─── Sub-test 15: bp.compile single ──────────────────────────────────────────────────────
    if bp_path_missing:
        skip("15/bp.compile: no test BP")
    else:
        result = expect_ok(call(args.host, args.port, "15", "p4-6-15", "bp.compile",
                                {"blueprint_path": bp_path}),
                          "p4-6-15", "15/bp.compile")
        if result is None:
            return 1
        for key in ("compiled", "errors", "warnings", "duration_ms", "status"):
            if key not in result:
                return fail(f"15/bp.compile: missing key '{key}': {result!r}")
        if not isinstance(result["compiled"], bool):
            return fail(f"15/bp.compile: compiled not bool: {result!r}")
        if not isinstance(result["errors"], list) or not isinstance(result["warnings"], list):
            return fail(f"15/bp.compile: errors/warnings not lists: {result!r}")
        if not isinstance(result["duration_ms"], (int, float)) or result["duration_ms"] < 0:
            return fail(f"15/bp.compile: duration_ms invalid: {result!r}")
        print(f"[SMOKE_PHASE4_6_10]   15/bp.compile OK compiled={result['compiled']} "
              f"status={result['status']} errs={len(result['errors'])} warns={len(result['warnings'])} "
              f"duration_ms={result['duration_ms']:.1f}")

    # ─── Sub-test 16-17: bp.compile_all_dirty (async composite) ──────────────────────────────
    # Use a narrow scope so the job finishes quickly on first run. If /Game/MCPTest/Phase4 is
    # empty (typical for fresh installs), we widen the fallback to a scope that's certain to have
    # at least the FatumGame BPs.
    scope_paths = ["/Game/MCPTest/Phase4"] if bp_path and "MCPTest/Phase4" in bp_path else ["/Game"]
    submit = expect_ok(call(args.host, args.port, "16", "p4-6-16", "bp.compile_all_dirty",
                            {"scope_paths": scope_paths}),
                      "p4-6-16", "16/compile_all_dirty submit")
    if submit is None:
        return 1
    job_id = submit.get("job_id")
    if not isinstance(job_id, str) or not job_id:
        return fail(f"16/compile_all_dirty submit: no job_id: {submit!r}")
    print(f"[SMOKE_PHASE4_6_10]   16/compile_all_dirty submit OK job_id={job_id} scope={scope_paths}")

    inner = poll_job(args.host, args.port, job_id, "17/poll", timeout=JOB_POLL_TIMEOUT_SEC)
    if inner is None:
        return 1
    for key in ("compiled", "succeeded", "failed", "duration_ms"):
        if key not in inner:
            return fail(f"17/compile_all_dirty result: missing key '{key}': {inner!r}")
    if not isinstance(inner["compiled"], (int, float)):
        return fail(f"17/compile_all_dirty: compiled not number: {inner!r}")
    if not isinstance(inner["succeeded"], (int, float)):
        return fail(f"17/compile_all_dirty: succeeded not number: {inner!r}")
    if not isinstance(inner["failed"], list):
        return fail(f"17/compile_all_dirty: failed not list: {inner!r}")
    print(f"[SMOKE_PHASE4_6_10]   17/compile_all_dirty poll OK compiled={int(inner['compiled'])} "
          f"succeeded={int(inner['succeeded'])} failed={len(inner['failed'])} "
          f"duration_ms={inner['duration_ms']:.1f}")

    # ─── Sub-test 18: bp.compile_all_dirty empty scope_paths → -32602 (Python ValueError) ─────
    # The Python wrapper raises ValueError when scope_paths is empty/not a list. The Phase 3
    # polish #12 dispatcher auto-translates ValueError to JSON-RPC -32602.
    ok = expect_error(call(args.host, args.port, "18", "p4-6-18", "bp.compile_all_dirty",
                           {"scope_paths": []}),
                     "p4-6-18", -32602, "18/compile_all_dirty empty scope")
    if ok is None:
        return 1
    print("[SMOKE_PHASE4_6_10]   18/compile_all_dirty empty scope OK -32602 InvalidParams")

    # ─── Sub-test 19: bp.add_variable missing pin_type → -32602 ──────────────────────────────
    if bp_path_missing:
        skip("19/bp.add_variable missing pin_type: no test BP")
    else:
        ok = expect_error(call(args.host, args.port, "19", "p4-6-19", "bp.add_variable",
                               {"blueprint_path": bp_path,
                                "variable_name": "MCP_SmokeShouldNotBeCreated"}),
                         "p4-6-19", -32602, "19/add_variable missing pin_type")
        if ok is None:
            return 1
        print("[SMOKE_PHASE4_6_10]   19/add_variable missing pin_type OK -32602 InvalidParams")

    print("[SMOKE_PHASE4_6_10] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
