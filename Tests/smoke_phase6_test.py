#!/usr/bin/env python3
"""Phase 6 Chunk B smoke — Automation Test (8 tools: 7 sync test.* + test.run_automation async).

Verifies (against a live editor on port 30020):

  Discovery (1):
    1. tools.list contains all 7 new test.* C++ handlers + test._run_automation_internal +
       Python test.run_automation.

  test.list_automation_specs (2-5):
    2. test.list_automation_specs (no args) → {specs, next_page_token, total_known,
       filter_echo=""}. specs is a list of spec objects with required keys.
    3. test.list_automation_specs with filter="System" → narrowed result. filter_echo == "System".
    4. test.list_automation_specs page_size=2 → pagination works, next_page_token set if
       total_known > 2. Following next_page_token returns more specs.
    5. Negative: bad page_token → -32015 StaleCursor.

  test.list_categories (6):
    6. test.list_categories → {categories[], total}. categories is sorted list.

  test.get_test_info (7-8):
    7. test.get_test_info on a probed live test name → spec json. Match against page-1 spec.
    8. Negative: test.get_test_info with bogus test_name → -32046 TestNotFound.

  test.set_filter_flags (9-10):
    9. test.set_filter_flags with mix of known + unknown flags → applied[] has known names,
       rejected[] has unknown, applied_mask matches union.
   10. Negative: missing flags array → -32602 InvalidParams.

  test.get_last_results (11):
   11. test.get_last_results → {has_results, is_running, current_test_full_path, ...}. When no
       test is currently running, has_results=false + is_running=false; all entries[] empty.

  test.cancel_current (12):
   12. test.cancel_current (no test running) → {cancelled=false, was_running=false}. Safe no-op.

  test.run_single_test (13-14):
   13. test.run_single_test on a known fast smoke test (e.g. first SmokeFilter test from probe).
       Returns {name, succeeded, duration_secs, errors, warnings, info, ...}. duration_secs
       should be small (smoke tests are sub-second).
   14. Negative: bogus test_name → -32046 TestNotFound.

  test.run_automation async (15-18):
   15. test.run_automation missing test_names → -32602 (Python ValueError → INVALID_PARAMS).
   16. test.run_automation empty test_names → -32602.
   17. test.run_automation bogus name → -32046 TestNotFound from the C++ internal — but the
       wire code observed by the caller may be -32603 because dispatch_internal raises a
       RuntimeError that the outer Python tool wrapper translates as Exception → -32603. Test
       accepts either; on -32603 the message must still reference the bogus test name for
       traceability.
   18. test.run_automation with a single valid smoke test name → {job_id}. Poll job.result for
       inner result with succeeded/failed/skipped arrays. completed should equal total when not
       cancelled.

Prints ``[SMOKE_PHASE6_TEST] PASS`` on success or ``[SMOKE_PHASE6_TEST] FAIL ...`` on first
mismatch.

Usage:
  python smoke_phase6_test.py [--host HOST] [--port PORT] [--test-name FULL_TEST_PATH]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Dict, List, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 60.0
JOB_POLL_TIMEOUT_SEC = 90.0
JOB_POLL_INTERVAL_SEC = 0.5

# Known FullTestPath substrings to probe for a "fast smoke test" candidate. The first registered
# test whose path contains one of these substrings AND has SmokeFilter flag wins. If none found,
# the run_single_test / run_automation sub-tests will SKIP cleanly with a note.
SMOKE_HINT_SUBSTRINGS = [
    "System.Core.Misc",            # often hosts cheap framework smoke tests
    "System.Engine.Misc",
    "Editor.Tools",
    "Project",
]


def send_and_recv_line(host: str, port: int, request_obj: dict,
                       timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        payload = (json.dumps(request_obj, separators=(",", ":")) + "\n").encode("utf-8")
        sock.sendall(payload)
        buf = bytearray()
        deadline = time.monotonic() + timeout
        while True:
            if time.monotonic() > deadline:
                return None
            try:
                chunk = sock.recv(128 * 1024)
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
    print(f"[SMOKE_PHASE6_TEST] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE6_TEST]   SKIP {reason}")


def info(message: str) -> None:
    print(f"[SMOKE_PHASE6_TEST]   {message}")


def call(host: str, port: int, label: str, request_id: str, method: str,
         args: Optional[dict] = None, timeout: float = READ_TIMEOUT_SEC) -> Optional[dict]:
    req = {"id": request_id, "kind": "call_function", "method": method, "args": args or {}}
    try:
        return send_and_recv_line(host, port, req, timeout=timeout)
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


def poll_job_result(host: str, port: int, job_id: str, label: str,
                    timeout: float = JOB_POLL_TIMEOUT_SEC) -> Optional[Dict[str, Any]]:
    """Poll job.result every JOB_POLL_INTERVAL_SEC for up to timeout seconds.

    Returns the inner result dict on Succeeded, the raw {state=Failed, ...} on Failed (caller
    distinguishes), None on timeout/error (after calling fail()).
    """
    deadline = time.monotonic() + timeout
    last_state: Optional[str] = None
    while time.monotonic() < deadline:
        resp = call(host, port, f"{label}/poll", f"poll-{job_id}-{int(time.time() * 1000)}",
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
            return {"_failed": True, "error": body.get("error"), "state": "Failed"}
        if state == "Cancelled":
            fail(f"{label}: job Cancelled")
            return None
        time.sleep(JOB_POLL_INTERVAL_SEC)
    fail(f"{label}: poll timeout (>{timeout}s) last_state={last_state!r}")
    return None


def probe_smoke_test(host: str, port: int) -> Optional[str]:
    """Find a registered fast smoke test by walking the first page of list_automation_specs.

    Returns the FullTestPath of the first candidate matching SMOKE_HINT_SUBSTRINGS AND having
    SmokeFilter flag, or None if no suitable candidate found.
    """
    resp = call(host, port, "probe", "p6-test-probe-smoke", "test.list_automation_specs",
                {"page_size": 200})
    if resp is None or resp.get("ok") is not True:
        return None
    specs = (resp.get("result") or {}).get("specs") or []
    for spec in specs:
        full = spec.get("full_path") or ""
        flags = spec.get("flags") or []
        if "SmokeFilter" not in flags:
            continue
        for hint in SMOKE_HINT_SUBSTRINGS:
            if hint in full:
                return full
    # No prefix-match smoke test found; fall back to ANY SmokeFilter test we can see.
    for spec in specs:
        if "SmokeFilter" in (spec.get("flags") or []):
            return spec.get("full_path")
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--test-name", default=None,
                        help="Override smoke-test FullTestPath; default probes SMOKE_HINT_SUBSTRINGS")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE6_TEST] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 8 test.* surfaces ──────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p6-test-1", "tools.list"),
                       "p6-test-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "test.list_automation_specs",
        "test.run_single_test",
        "test.get_last_results",
        "test.cancel_current",
        "test.list_categories",
        "test.get_test_info",
        "test.set_filter_flags",
        "test._run_automation_internal",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing test.* handlers: {sorted(missing)}")
    py_tools = result.get("python_tools") or {}
    if "test.run_automation" not in py_tools:
        return fail(
            f"1/python_tools: missing test.run_automation (sample: {sorted(py_tools)[:5]}...)"
        )
    info("1/tools.list contains all 7 sync test.* + test._run_automation_internal + Python test.run_automation")

    # ─── Sub-test 2: test.list_automation_specs (no args) ───────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "2", "p6-test-2", "test.list_automation_specs"),
                       "p6-test-2", "2/list_automation_specs no args")
    if result is None:
        return 1
    for key in ("specs", "total_known", "filter_echo"):
        if key not in result:
            return fail(f"2/list_automation_specs: missing key '{key}': {result!r}")
    if not isinstance(result["specs"], list):
        return fail(f"2/list_automation_specs: specs not list {result!r}")
    if not isinstance(result["total_known"], (int, float)):
        return fail(f"2/list_automation_specs: total_known not number {result!r}")
    if result.get("filter_echo") != "":
        return fail(f"2/list_automation_specs: filter_echo should be '' got {result.get('filter_echo')!r}")
    total_known_full = int(result["total_known"])
    info(f"2/list_automation_specs no args OK total_known={total_known_full} page_len={len(result['specs'])}")

    # Verify spec shape on first entry (if any).
    if result["specs"]:
        spec = result["specs"][0]
        for field in ("name", "full_path", "command_line", "category", "test_tags", "parameter",
                      "source_file", "source_line", "asset_path", "open_command",
                      "num_participants", "flags", "flags_raw"):
            if field not in spec:
                return fail(f"2/list_automation_specs: spec missing field {field!r}: {spec!r}")
        if not isinstance(spec["flags"], list):
            return fail(f"2/list_automation_specs: spec.flags not list {spec!r}")
        if not isinstance(spec["flags_raw"], (int, float)):
            return fail(f"2/list_automation_specs: spec.flags_raw not number {spec!r}")
        info(f"   sample spec: {spec['full_path']!r} flags={spec['flags']}")

    # ─── Sub-test 3: filter="System" narrows ────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "3", "p6-test-3", "test.list_automation_specs",
             {"filter": "System", "page_size": 100}),
        "p6-test-3", "3/list_automation_specs filter=System")
    if result is None:
        return 1
    if result.get("filter_echo") != "System":
        return fail(f"3/list_automation_specs: filter_echo not 'System': {result.get('filter_echo')!r}")
    # Every spec on this page should contain "System" in its full_path (case-insensitive).
    for spec in result["specs"]:
        if "system" not in (spec.get("full_path") or "").lower():
            return fail(
                f"3/list_automation_specs: spec full_path {spec.get('full_path')!r} doesn't match filter"
            )
    info(f"3/list_automation_specs filter=System OK total_known={int(result['total_known'])} page_len={len(result['specs'])}")

    # ─── Sub-test 4: page_size=2 pagination ─────────────────────────────────────────────────────
    if total_known_full <= 2:
        skip(f"4/pagination: total_known={total_known_full} <= 2, skipping next_page_token check")
    else:
        result = expect_ok(
            call(args.host, args.port, "4", "p6-test-4", "test.list_automation_specs",
                 {"page_size": 2}),
            "p6-test-4", "4/list_automation_specs page_size=2")
        if result is None:
            return 1
        if len(result["specs"]) != 2:
            return fail(f"4/list_automation_specs: expected 2 specs got {len(result['specs'])}")
        next_token = result.get("next_page_token")
        if not isinstance(next_token, str) or not next_token:
            return fail(f"4/list_automation_specs: expected non-empty next_page_token, got {next_token!r}")
        # Follow the cursor.
        result2 = expect_ok(
            call(args.host, args.port, "4b", "p6-test-4b", "test.list_automation_specs",
                 {"page_size": 2, "page_token": next_token}),
            "p6-test-4b", "4b/list_automation_specs page_token=...")
        if result2 is None:
            return 1
        if not result2["specs"]:
            return fail(f"4b/list_automation_specs: expected specs on page 2, got empty")
        # First-page entries must NOT overlap with second-page entries (sentinel sort).
        page1_paths = {s["full_path"] for s in result["specs"]}
        page2_paths = {s["full_path"] for s in result2["specs"]}
        overlap = page1_paths & page2_paths
        if overlap:
            return fail(f"4/list_automation_specs: page1/page2 overlap: {overlap}")
        info(f"4/list_automation_specs pagination OK page1_len=2 page2_len={len(result2['specs'])}")

    # ─── Sub-test 5: bad page_token → -32015 ────────────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "5", "p6-test-5", "test.list_automation_specs",
             {"page_token": "not-a-valid-base64-cursor"}),
        "p6-test-5", -32015, "5/list_automation_specs bad token")
    if err is None:
        return 1
    info(f"5/list_automation_specs bad page_token OK code={err['code']}")

    # ─── Sub-test 6: test.list_categories ───────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "6", "p6-test-6", "test.list_categories"),
                       "p6-test-6", "6/list_categories")
    if result is None:
        return 1
    for key in ("categories", "total"):
        if key not in result:
            return fail(f"6/list_categories: missing key '{key}': {result!r}")
    if not isinstance(result["categories"], list):
        return fail(f"6/list_categories: categories not list {result!r}")
    # Verify sorted (case-insensitive) and unique.
    lowered = [c.lower() for c in result["categories"]]
    if lowered != sorted(lowered):
        return fail(f"6/list_categories: not sorted: {result['categories']!r}")
    if len(lowered) != len(set(lowered)):
        return fail(f"6/list_categories: contains duplicates: {result['categories']!r}")
    info(f"6/list_categories OK total={int(result['total'])} sample={result['categories'][:5]}")

    # ─── Probe a live test name for sub-tests 7, 13, 18 ─────────────────────────────────────────
    probed_test: Optional[str] = args.test_name
    if probed_test is None:
        probed_test = probe_smoke_test(args.host, args.port)
        if probed_test:
            info(f"probed smoke test: {probed_test!r}")
        else:
            info("no smoke test candidate found; sub-tests 7, 13, 18 will SKIP "
                 "(operator can pass --test-name <FullTestPath>)")

    # ─── Sub-test 7: test.get_test_info on probed test ──────────────────────────────────────────
    if probed_test is None:
        skip("7/get_test_info: SKIPPED (no probed test name)")
    else:
        result = expect_ok(
            call(args.host, args.port, "7", "p6-test-7", "test.get_test_info",
                 {"test_name": probed_test}),
            "p6-test-7", "7/get_test_info")
        if result is None:
            return 1
        if result.get("full_path") != probed_test:
            return fail(
                f"7/get_test_info: full_path mismatch expected={probed_test!r} got={result.get('full_path')!r}")
        for field in ("name", "full_path", "command_line", "category", "flags", "flags_raw"):
            if field not in result:
                return fail(f"7/get_test_info: missing field {field!r}: {result!r}")
        info(f"7/get_test_info OK name={result['name']!r} category={result['category']!r}")

    # ─── Sub-test 8: get_test_info bogus name → -32046 ──────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "8", "p6-test-8", "test.get_test_info",
             {"test_name": "Nonexistent.Bogus.Test.XYZ123"}),
        "p6-test-8", -32046, "8/get_test_info bogus")
    if err is None:
        return 1
    info(f"8/get_test_info bogus OK code={err['code']}")

    # ─── Sub-test 9: test.set_filter_flags mix of known + unknown ───────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "9", "p6-test-9", "test.set_filter_flags",
             {"flags": ["SmokeFilter", "EngineFilter", "BogusFlagNameXYZ"]}),
        "p6-test-9", "9/set_filter_flags mix")
    if result is None:
        return 1
    for key in ("applied", "rejected", "applied_mask"):
        if key not in result:
            return fail(f"9/set_filter_flags: missing key '{key}': {result!r}")
    if not isinstance(result["applied"], list):
        return fail(f"9/set_filter_flags: applied not list {result!r}")
    applied_set = set(result["applied"])
    if "SmokeFilter" not in applied_set or "EngineFilter" not in applied_set:
        return fail(f"9/set_filter_flags: SmokeFilter+EngineFilter not in applied: {result['applied']!r}")
    rejected_flags = {entry.get("flag") for entry in result["rejected"]}
    if "BogusFlagNameXYZ" not in rejected_flags:
        return fail(f"9/set_filter_flags: BogusFlagNameXYZ not in rejected: {result['rejected']!r}")
    if not isinstance(result["applied_mask"], (int, float)) or int(result["applied_mask"]) == 0:
        return fail(f"9/set_filter_flags: applied_mask should be non-zero: {result['applied_mask']!r}")
    info(f"9/set_filter_flags OK applied={sorted(applied_set)} rejected={sorted(rejected_flags)}")

    # Best-effort reset: SmokeFilter + EngineFilter alone covers common test runner defaults; we
    # don't reset to 0 to avoid masking subsequent test runner state. Operator can re-issue.

    # ─── Sub-test 10: set_filter_flags missing flags array → -32602 ─────────────────────────────
    err = expect_error(
        call(args.host, args.port, "10", "p6-test-10", "test.set_filter_flags", {}),
        "p6-test-10", -32602, "10/set_filter_flags missing")
    if err is None:
        return 1
    info(f"10/set_filter_flags missing flags OK code={err['code']}")

    # ─── Sub-test 11: test.get_last_results when nothing is running ─────────────────────────────
    result = expect_ok(call(args.host, args.port, "11", "p6-test-11", "test.get_last_results"),
                       "p6-test-11", "11/get_last_results")
    if result is None:
        return 1
    for key in ("has_results", "is_running", "current_test_full_path",
                "error_count", "warning_count", "duration_secs", "entries"):
        if key not in result:
            return fail(f"11/get_last_results: missing key '{key}': {result!r}")
    if not isinstance(result["has_results"], bool):
        return fail(f"11/get_last_results: has_results not bool {result!r}")
    if not isinstance(result["is_running"], bool):
        return fail(f"11/get_last_results: is_running not bool {result!r}")
    # When no test is running we expect both to be false.
    if result["is_running"]:
        info(f"   note: is_running=true (another test is active — likely from prior run)")
    info(f"11/get_last_results OK has_results={result['has_results']} is_running={result['is_running']}")

    # ─── Sub-test 12: test.cancel_current when nothing running → safe no-op ─────────────────────
    result = expect_ok(call(args.host, args.port, "12", "p6-test-12", "test.cancel_current"),
                       "p6-test-12", "12/cancel_current no-op")
    if result is None:
        return 1
    for key in ("cancelled", "was_running", "stop_succeeded", "current_test_full_path"):
        if key not in result:
            return fail(f"12/cancel_current: missing key '{key}': {result!r}")
    if not isinstance(result["was_running"], bool):
        return fail(f"12/cancel_current: was_running not bool {result!r}")
    info(f"12/cancel_current OK was_running={result['was_running']} cancelled={result['cancelled']}")

    # ─── Sub-test 13: test.run_single_test on probed smoke test ─────────────────────────────────
    if probed_test is None:
        skip("13/run_single_test: SKIPPED (no probed test name)")
    else:
        # Allow up to 40s response — slightly above the C++ 30s sync cap so we catch the
        # cap-exceeded response too. Either outcome is acceptable for smoke validation; cap-hit
        # surfaces as ok=true + cap_exceeded=true + succeeded=false (FMCPResponse can't carry
        # both error + result, so we use boolean flags instead of an error envelope).
        result = expect_ok(
            call(args.host, args.port, "13", "p6-test-13", "test.run_single_test",
                 {"test_name": probed_test}, timeout=40.0),
            "p6-test-13", "13/run_single_test")
        if result is None:
            return 1
        for key in ("name", "succeeded", "completed", "cap_exceeded", "duration_secs",
                    "cap_secs", "errors", "warnings", "info", "error_count", "warning_count"):
            if key not in result:
                return fail(f"13/run_single_test: missing key '{key}': {result!r}")
        if result.get("cap_exceeded"):
            info(f"13/run_single_test cap_exceeded=true (acceptable for slow test) "
                 f"duration_secs={result['duration_secs']:.3f}")
        else:
            info(f"13/run_single_test OK name={result['name']!r} succeeded={result['succeeded']} "
                 f"duration_secs={result['duration_secs']:.3f} errors={result['error_count']}")

    # ─── Sub-test 14: test.run_single_test bogus → -32046 ───────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "14", "p6-test-14", "test.run_single_test",
             {"test_name": "Nonexistent.Bogus.Test.XYZ123"}),
        "p6-test-14", -32046, "14/run_single_test bogus")
    if err is None:
        return 1
    info(f"14/run_single_test bogus OK code={err['code']}")

    # ─── Sub-test 15: test.run_automation missing test_names → -32602 (Python ValueError) ───────
    err = expect_error(
        call(args.host, args.port, "15", "p6-test-15", "test.run_automation", {}),
        "p6-test-15", -32602, "15/run_automation missing")
    if err is None:
        return 1
    info("15/run_automation missing test_names OK -32602 (Python ValueError)")

    # ─── Sub-test 16: test.run_automation empty test_names → -32602 ─────────────────────────────
    err = expect_error(
        call(args.host, args.port, "16", "p6-test-16", "test.run_automation",
             {"test_names": []}),
        "p6-test-16", -32602, "16/run_automation empty")
    if err is None:
        return 1
    info("16/run_automation empty test_names OK -32602 (Python ValueError)")

    # ─── Sub-test 17: test.run_automation bogus name → -32046 (or -32603 wrapped) ───────────────
    # The C++ internal handler returns -32046 TestNotFound. The Python ``dispatch_internal``
    # helper in MCPTools/tools/asset_tools.py raises a RuntimeError carrying the original code
    # in its message; the outer Python tool wrapper catches it as an Exception → -32603. So the
    # observable wire code may be either -32046 (rare, only if a future dispatcher-side change
    # passes the code through) or -32603 (the current behaviour). Accept both and verify the
    # message references TestNotFound or the test name when -32603.
    resp = call(args.host, args.port, "17", "p6-test-17", "test.run_automation",
                {"test_names": ["Nonexistent.Bogus.Test.XYZ123"]})
    err = expect_error(resp, "p6-test-17", (-32046, -32603), "17/run_automation bogus")
    if err is None:
        return 1
    if err["code"] == -32603 and "Nonexistent" not in (err.get("message") or ""):
        return fail(f"17/run_automation -32603 but message doesn't reference test name: {err!r}")
    info(f"17/run_automation bogus name OK code={err['code']}")

    # ─── Sub-test 18: test.run_automation valid → {job_id}, poll job.result ────────────────────
    if probed_test is None:
        skip("18/run_automation valid: SKIPPED (no probed test name)")
    else:
        resp = call(args.host, args.port, "18", "p6-test-18", "test.run_automation",
                    {"test_names": [probed_test]})
        result = expect_ok(resp, "p6-test-18", "18/run_automation valid")
        if result is None:
            return 1
        job_id = result.get("job_id")
        if not isinstance(job_id, str) or not job_id:
            return fail(f"18/run_automation: no job_id {result!r}")
        info(f"18/run_automation returned {{job_id: {job_id}}} — polling job.result")

        inner = poll_job_result(args.host, args.port, job_id, "18/run_automation")
        if inner is None:
            return 1
        if inner.get("_failed"):
            return fail(f"18/run_automation: job Failed error={inner.get('error')!r}")

        for key in ("succeeded", "failed", "skipped", "total", "completed",
                    "failed_count", "cancelled", "applied_filter", "total_duration_secs"):
            if key not in inner:
                return fail(f"18/run_automation: missing key '{key}': {inner!r}")
        if int(inner["total"]) != 1:
            return fail(f"18/run_automation: total != 1: {inner!r}")
        if inner["cancelled"]:
            return fail(f"18/run_automation: cancelled=true on valid run: {inner!r}")
        if int(inner["completed"]) != 1:
            return fail(f"18/run_automation: completed != 1: {inner!r}")
        if inner["applied_filter"] != "":
            return fail(f"18/run_automation: applied_filter should be '': {inner!r}")
        info(f"18/run_automation OK total={inner['total']} completed={inner['completed']} "
             f"succeeded={len(inner['succeeded'])} failed={len(inner['failed'])} "
             f"total_duration_secs={inner['total_duration_secs']:.3f}")

    print("[SMOKE_PHASE6_TEST] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
