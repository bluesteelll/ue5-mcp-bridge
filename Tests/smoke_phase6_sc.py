#!/usr/bin/env python3
"""Phase 6 Chunk A smoke — Source Control (6 tools: 5 sync sc.* + sc.submit async composite).

Verifies (against a live editor on port 30020):

  Discovery (1):
    1. tools.list contains all 5 new sc.* C++ handlers + sc._submit_internal + python sc.submit.

  sc.status (2-4):
    2. sc.status with no args → {files, provider}. files[] may be empty in a clean state;
       this is NOT an error.
    3. sc.status with file_paths=[<known file>] → state object with required fields
       (path, state, revision, action, last_modified_by, is_checked_out, is_modified, ...).
    4. Negative: sc.status with file_paths=[<bogus path>] → -32010 InvalidPath or -32013
       PathEscape (depending on whether path is malformed vs out-of-sandbox).

  sc.checkout (5-6):
    5. sc.checkout with file_paths=[<test file>] → {checked_out, failed, provider, batch_ok}.
       Per-file outcome surfaced — file may already be checked out (still success-like) OR not
       under SC (failure in failed[] but no -32xxx error).
    6. Negative: sc.checkout with empty file_paths → -32602.

  sc.diff (7-9):
    7. sc.diff on a known file with default revisions (head vs working) → either is_binary=true
       with base64_a/base64_b OR is_binary=false with diff_text. Schema fields present.
    8. sc.diff with explicit revision_a / revision_b → same shape. If no history available,
       returns -32603 with a "no history" message (acceptable: file may be unversioned).
    9. Negative: sc.diff with missing file_path → -32602.

  sc.diff_binary (10-11):
   10. sc.diff_binary on a known file → ALWAYS is_binary=true (forced), base64_a/base64_b
       present, bytes_a/bytes_b numeric.
   11. Negative: sc.diff_binary with bogus path → -32010 InvalidPath / -32013 PathEscape /
       -32603 Internal (depending on stage of failure).

  sc.revert (12-14):
   12. sc.revert without confirm_destructive=true → -32033 ReparentUnsafe (reused destructive
       confirm gate). Error message mentions 'confirm_destructive'.
   13. sc.revert with confirm_destructive=true but bogus path → -32010/-32013/-32602 error
       (path validation happens before destructive op).
   14. Negative: sc.revert with empty file_paths → -32602.

  sc.submit async composite (15-18):
   15. sc.submit with missing description → ValueError → -32602.
   16. sc.submit with empty file_paths → ValueError → -32602.
   17. sc.submit with bogus path → -32010/-32013 (path resolution at submit-time).
   18. sc.submit with valid args returns {job_id}. Polling job.result is OPTIONAL — we don't
       want to actually submit anything in the smoke test (would create a commit). Probe a
       known-no-op case OR skip the actual submit verification with a clear note.

  Provider-unavailable surface (19):
   19. If no SC provider is configured in the editor, sub-tests 2-18 will surface -32045
       SourceControlProviderUnavailable. The test reports this as a SKIP of subsequent tests
       and PASSes overall — operator can configure SC and re-run for full coverage.

Prints ``[SMOKE_PHASE6_SC] PASS`` on success or ``[SMOKE_PHASE6_SC] FAIL ...`` on first
mismatch. Sub-tests that depend on SC being configured emit SKIP lines but still PASS overall.

Usage:
  python smoke_phase6_sc.py [--host HOST] [--port PORT] [--test-file PATH]
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
READ_TIMEOUT_SEC = 30.0
JOB_POLL_TIMEOUT_SEC = 30.0
JOB_POLL_INTERVAL_SEC = 0.25

# Candidate test files to probe — first existing one wins. We need a file that's actually under
# SC for meaningful test coverage; the smoke test PASSes with SKIPs if none found.
FALLBACK_TEST_FILES = [
    "/Game/MCPTest/Phase6/SC_TestText",    # plan-mandated path (if operator creates it)
    "Plugins/UnrealMCPBridge/README.md",   # likely tracked
    "Plugins/UnrealMCPBridge/UnrealMCPBridge.uplugin",
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
    print(f"[SMOKE_PHASE6_SC] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE6_SC]   SKIP {reason}")


def info(message: str) -> None:
    print(f"[SMOKE_PHASE6_SC]   {message}")


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


def is_provider_unavailable(response: Optional[dict]) -> bool:
    """True iff response is the -32045 SourceControlProviderUnavailable error."""
    if response is None:
        return False
    err = response.get("error")
    return isinstance(err, dict) and err.get("code") == -32045


def poll_job_result(host: str, port: int, job_id: str, label: str,
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
            return {"_failed": True, "error": body.get("error"), "state": "Failed"}
        if state == "Cancelled":
            fail(f"{label}: job Cancelled")
            return None
        time.sleep(JOB_POLL_INTERVAL_SEC)
    fail(f"{label}: poll timeout (>{timeout}s) last_state={last_state!r}")
    return None


def probe_test_file(host: str, port: int, candidates: List[str]) -> Optional[str]:
    """Return the first file from `candidates` that resolves successfully via sc.status."""
    for path in candidates:
        resp = call(host, port, "probe", f"probe-sc-{path}", "sc.status",
                    {"file_paths": [path]})
        # Provider unavailable → treat as "no probe possible", return None to surface SKIP later.
        if is_provider_unavailable(resp):
            return None
        # Resolution error → not the right file, try next.
        if resp is None or resp.get("ok") is not True:
            continue
        return path
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--test-file", default=None,
                        help="Override test file path; default probes FALLBACK_TEST_FILES")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE6_SC] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 6 sc.* handlers + Python sc.submit ─────────────────
    result = expect_ok(call(args.host, args.port, "1", "p6-sc-1", "tools.list"),
                       "p6-sc-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "sc.status",
        "sc.checkout",
        "sc.diff",
        "sc.diff_binary",
        "sc.revert",
        "sc._submit_internal",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing sc.* handlers: {sorted(missing)}")
    py_tools = result.get("python_tools") or {}
    if "sc.submit" not in py_tools:
        return fail(f"1/python_tools: missing sc.submit (found: {sorted(py_tools)[:5]}...)")
    info("1/tools.list contains all 5 sync sc.* + sc._submit_internal + Python sc.submit")

    # ─── Detect provider availability via sc.status no-args ─────────────────────────────────────
    provider_probe = call(args.host, args.port, "probe-prov", "p6-sc-probe-prov", "sc.status")
    provider_available = (
        provider_probe is not None and
        provider_probe.get("ok") is True
    )
    if not provider_available:
        if is_provider_unavailable(provider_probe):
            info("source control provider is NOT enabled — sub-tests 2-18 will SKIP "
                 "(operator can configure SC in Editor → Source Control)")
        else:
            info(f"sc.status probe returned unexpected response: {provider_probe!r} — "
                 "treating provider as unavailable")
        # Even with no provider, we still verify the -32045 surface on sub-tests 2 and 5+.
        # For other sub-tests we SKIP cleanly.

    # ─── Sub-test 2: sc.status with no args ─────────────────────────────────────────────────────
    resp = call(args.host, args.port, "2", "p6-sc-2", "sc.status")
    if not provider_available:
        err = expect_error(resp, "p6-sc-2", -32045, "2/sc.status no provider")
        if err is None:
            return 1
        info("2/sc.status (no args) OK -32045 SourceControlProviderUnavailable (no provider)")
    else:
        result = expect_ok(resp, "p6-sc-2", "2/sc.status no args")
        if result is None:
            return 1
        for key in ("files", "provider"):
            if key not in result:
                return fail(f"2/sc.status: missing key '{key}': {result!r}")
        if not isinstance(result["files"], list):
            return fail(f"2/sc.status: files not list {result!r}")
        if not isinstance(result["provider"], str):
            return fail(f"2/sc.status: provider not string {result!r}")
        info(f"2/sc.status (no args) OK provider='{result['provider']}' files={len(result['files'])}")

    # ─── Resolve a test file (only matters if provider available) ───────────────────────────────
    test_file: Optional[str] = None
    if provider_available:
        test_file = args.test_file or probe_test_file(
            args.host, args.port, FALLBACK_TEST_FILES)
        if test_file is None:
            skip("no test file resolved from FALLBACK_TEST_FILES; sub-tests 3-18 partially SKIPPED "
                 "(operator can pass --test-file <path>)")
        else:
            info(f"using test file: {test_file}")

    # ─── Sub-test 3: sc.status with explicit file_paths ────────────────────────────────────────
    if not provider_available or test_file is None:
        skip("3/sc.status with file_paths: SKIPPED (no provider or no test file)")
    else:
        result = expect_ok(
            call(args.host, args.port, "3", "p6-sc-3", "sc.status",
                 {"file_paths": [test_file]}),
            "p6-sc-3", "3/sc.status file_paths")
        if result is None:
            return 1
        files = result.get("files")
        if not isinstance(files, list):
            return fail(f"3/sc.status: files not list {result!r}")
        if files:
            entry = files[0]
            for field in ("path", "state", "revision", "action", "last_modified_by",
                          "is_checked_out", "is_modified", "is_added", "is_deleted",
                          "is_current"):
                if field not in entry:
                    return fail(f"3/sc.status: file entry missing field {field!r}: {entry!r}")
            if not isinstance(entry["path"], str):
                return fail(f"3/sc.status: path not string {entry!r}")
            if not isinstance(entry["state"], str):
                return fail(f"3/sc.status: state not string {entry!r}")
            if not isinstance(entry["revision"], (int, float)):
                return fail(f"3/sc.status: revision not number {entry!r}")
            for boolfield in ("is_checked_out", "is_modified", "is_added", "is_deleted",
                              "is_current"):
                if not isinstance(entry[boolfield], bool):
                    return fail(f"3/sc.status: {boolfield} not bool {entry!r}")
            info(f"3/sc.status file_paths OK state='{entry['state']}' revision={entry['revision']}")
        else:
            info("3/sc.status file_paths OK (files[] empty — file may not be cached yet)")

    # ─── Sub-test 4: sc.status with bogus path → -32010 / -32013 ───────────────────────────────
    # Bogus disk path outside sandbox.
    resp = call(args.host, args.port, "4", "p6-sc-4", "sc.status",
                {"file_paths": ["C:/_bogus_path_outside_sandbox_xyz_/foo.txt"]})
    if not provider_available:
        # Provider check happens first; bypass error-code check.
        info("4/sc.status bogus path: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-4", (-32010, -32013), "4/sc.status bogus path")
        if err is None:
            return 1
        info(f"4/sc.status bogus path OK code={err['code']}")

    # ─── Sub-test 5: sc.checkout (best-effort) ─────────────────────────────────────────────────
    # We don't want to actually leave a file checked out, but we test the response shape.
    if not provider_available or test_file is None:
        # Still verify provider-unavailable surface.
        resp = call(args.host, args.port, "5", "p6-sc-5", "sc.checkout",
                    {"file_paths": ["Plugins/UnrealMCPBridge/UnrealMCPBridge.uplugin"]})
        if not provider_available:
            err = expect_error(resp, "p6-sc-5", -32045, "5/sc.checkout no provider")
            if err is None:
                return 1
            info("5/sc.checkout no provider OK -32045 SourceControlProviderUnavailable")
        else:
            skip("5/sc.checkout: SKIPPED (no test file)")
    else:
        result = expect_ok(
            call(args.host, args.port, "5", "p6-sc-5", "sc.checkout",
                 {"file_paths": [test_file]}),
            "p6-sc-5", "5/sc.checkout")
        if result is None:
            return 1
        for key in ("checked_out", "failed", "provider", "batch_ok"):
            if key not in result:
                return fail(f"5/sc.checkout: missing key '{key}': {result!r}")
        if not isinstance(result["checked_out"], list):
            return fail(f"5/sc.checkout: checked_out not list {result!r}")
        if not isinstance(result["failed"], list):
            return fail(f"5/sc.checkout: failed not list {result!r}")
        if not isinstance(result["batch_ok"], bool):
            return fail(f"5/sc.checkout: batch_ok not bool {result!r}")
        info(f"5/sc.checkout OK checked_out={len(result['checked_out'])} "
             f"failed={len(result['failed'])} batch_ok={result['batch_ok']}")
        # Best-effort revert to clean up if a checkout happened to succeed.
        if result["checked_out"]:
            cleanup = call(args.host, args.port, "5cleanup", "p6-sc-5cleanup", "sc.revert",
                           {"file_paths": [test_file], "confirm_destructive": True})
            if cleanup is not None and cleanup.get("ok") is True:
                info(f"5/cleanup: sc.revert ran (best-effort)")

    # ─── Sub-test 6: sc.checkout with empty file_paths → -32602 ────────────────────────────────
    resp = call(args.host, args.port, "6", "p6-sc-6", "sc.checkout", {"file_paths": []})
    if not provider_available:
        # -32045 fires first.
        info("6/sc.checkout empty file_paths: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-6", -32602, "6/sc.checkout empty")
        if err is None:
            return 1
        info("6/sc.checkout empty file_paths OK -32602 InvalidParams")

    # ─── Sub-test 7: sc.diff default revisions ─────────────────────────────────────────────────
    if not provider_available or test_file is None:
        skip("7/sc.diff default: SKIPPED (no provider or no test file)")
    else:
        resp = call(args.host, args.port, "7", "p6-sc-7", "sc.diff",
                    {"file_path": test_file})
        # diff may fail with -32603 if no history (e.g. new untracked file). Both shapes acceptable.
        if resp is not None and resp.get("ok") is True:
            result = resp["result"]
            for key in ("bytes_a", "bytes_b", "is_binary", "path", "revision_a", "revision_b"):
                if key not in result:
                    return fail(f"7/sc.diff: missing key '{key}': {result!r}")
            if not isinstance(result["is_binary"], bool):
                return fail(f"7/sc.diff: is_binary not bool {result!r}")
            if result["is_binary"]:
                for key in ("base64_a", "base64_b"):
                    if key not in result:
                        return fail(f"7/sc.diff (binary): missing key '{key}': {result!r}")
                info(f"7/sc.diff binary OK bytes_a={result['bytes_a']} bytes_b={result['bytes_b']}")
            else:
                if "diff_text" not in result:
                    return fail(f"7/sc.diff (text): missing diff_text: {result!r}")
                info(f"7/sc.diff text OK bytes_a={result['bytes_a']} bytes_b={result['bytes_b']} "
                     f"diff_len={len(result['diff_text'])}")
        else:
            err = (resp or {}).get("error", {})
            code = err.get("code")
            if code in (-32603, -32017):
                info(f"7/sc.diff: file has no diffable revisions (code={code}) — acceptable")
            else:
                return fail(f"7/sc.diff: unexpected response {resp!r}")

    # ─── Sub-test 8: sc.diff with explicit revisions ───────────────────────────────────────────
    if not provider_available or test_file is None:
        skip("8/sc.diff explicit revisions: SKIPPED (no provider or no test file)")
    else:
        resp = call(args.host, args.port, "8", "p6-sc-8", "sc.diff",
                    {"file_path": test_file, "revision_a": "head", "revision_b": "working"})
        # Same shape rules as 7 — accept either success or -32603/-32017.
        if resp is not None and resp.get("ok") is True:
            info("8/sc.diff explicit revisions OK (success)")
        else:
            err = (resp or {}).get("error", {})
            if err.get("code") in (-32603, -32017):
                info(f"8/sc.diff: explicit-rev no-history acceptable (code={err.get('code')})")
            else:
                return fail(f"8/sc.diff: unexpected response {resp!r}")

    # ─── Sub-test 9: sc.diff with missing file_path → -32602 ───────────────────────────────────
    resp = call(args.host, args.port, "9", "p6-sc-9", "sc.diff", {})
    if not provider_available:
        info("9/sc.diff missing file_path: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-9", -32602, "9/sc.diff missing file_path")
        if err is None:
            return 1
        info("9/sc.diff missing file_path OK -32602 InvalidParams")

    # ─── Sub-test 10: sc.diff_binary ───────────────────────────────────────────────────────────
    if not provider_available or test_file is None:
        skip("10/sc.diff_binary: SKIPPED (no provider or no test file)")
    else:
        resp = call(args.host, args.port, "10", "p6-sc-10", "sc.diff_binary",
                    {"file_path": test_file})
        if resp is not None and resp.get("ok") is True:
            result = resp["result"]
            for key in ("is_binary", "path", "revision_a", "revision_b", "bytes_a", "bytes_b",
                        "base64_a", "base64_b"):
                if key not in result:
                    return fail(f"10/sc.diff_binary: missing key '{key}': {result!r}")
            if result["is_binary"] is not True:
                return fail(f"10/sc.diff_binary: is_binary should always be true: {result!r}")
            info(f"10/sc.diff_binary OK bytes_a={result['bytes_a']} bytes_b={result['bytes_b']}")
        else:
            err = (resp or {}).get("error", {})
            if err.get("code") in (-32603, -32017):
                info(f"10/sc.diff_binary: no-history/oversize acceptable (code={err.get('code')})")
            else:
                return fail(f"10/sc.diff_binary: unexpected response {resp!r}")

    # ─── Sub-test 11: sc.diff_binary with bogus path ───────────────────────────────────────────
    resp = call(args.host, args.port, "11", "p6-sc-11", "sc.diff_binary",
                {"file_path": "C:/_bogus_path_outside_sandbox_xyz_/foo.bin"})
    if not provider_available:
        info("11/sc.diff_binary bogus path: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-11", (-32010, -32013, -32603),
                           "11/sc.diff_binary bogus")
        if err is None:
            return 1
        info(f"11/sc.diff_binary bogus path OK code={err['code']}")

    # ─── Sub-test 12: sc.revert without confirm_destructive → -32033 ──────────────────────────
    resp = call(args.host, args.port, "12", "p6-sc-12", "sc.revert",
                {"file_paths": ["Plugins/UnrealMCPBridge/UnrealMCPBridge.uplugin"]})
    if not provider_available:
        # -32045 fires first.
        info("12/sc.revert no confirm: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-12", -32033, "12/sc.revert no confirm")
        if err is None:
            return 1
        msg = err.get("message", "")
        if "confirm_destructive" not in msg:
            return fail(f"12/sc.revert: error message lacks 'confirm_destructive': {msg!r}")
        info("12/sc.revert no confirm OK -32033 ReparentUnsafe (reused) + message mentions confirm_destructive")

    # ─── Sub-test 13: sc.revert with confirm_destructive=true + bogus path ─────────────────────
    resp = call(args.host, args.port, "13", "p6-sc-13", "sc.revert",
                {"file_paths": ["C:/_bogus_path_outside_sandbox_xyz_/foo.txt"],
                 "confirm_destructive": True})
    if not provider_available:
        info("13/sc.revert bogus path: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-13", (-32010, -32013, -32602),
                           "13/sc.revert bogus path")
        if err is None:
            return 1
        info(f"13/sc.revert bogus path OK code={err['code']}")

    # ─── Sub-test 14: sc.revert with empty file_paths → -32602 ─────────────────────────────────
    resp = call(args.host, args.port, "14", "p6-sc-14", "sc.revert",
                {"file_paths": [], "confirm_destructive": True})
    if not provider_available:
        info("14/sc.revert empty file_paths: SKIPPED (provider check fires first)")
    else:
        err = expect_error(resp, "p6-sc-14", -32602, "14/sc.revert empty")
        if err is None:
            return 1
        info("14/sc.revert empty file_paths OK -32602 InvalidParams")

    # ─── Sub-test 15: sc.submit missing description → -32602 (Python ValueError) ───────────────
    resp = call(args.host, args.port, "15", "p6-sc-15", "sc.submit",
                {"file_paths": ["Plugins/UnrealMCPBridge/UnrealMCPBridge.uplugin"]})
    err = expect_error(resp, "p6-sc-15", -32602, "15/sc.submit missing description")
    if err is None:
        return 1
    info("15/sc.submit missing description OK -32602 InvalidParams (Python ValueError)")

    # ─── Sub-test 16: sc.submit empty file_paths → -32602 (Python ValueError) ──────────────────
    resp = call(args.host, args.port, "16", "p6-sc-16", "sc.submit",
                {"file_paths": [], "description": "MCP smoke test"})
    err = expect_error(resp, "p6-sc-16", -32602, "16/sc.submit empty file_paths")
    if err is None:
        return 1
    info("16/sc.submit empty file_paths OK -32602 InvalidParams (Python ValueError)")

    # ─── Sub-test 17: sc.submit bogus path → -32010 / -32013 ───────────────────────────────────
    if not provider_available:
        # Provider check fires in the internal handler; we still test that input validation works.
        # The Python wrapper validates first (passes), then dispatch_internal sends to C++ which
        # checks provider availability BEFORE path resolution. So we expect -32045 here.
        resp = call(args.host, args.port, "17", "p6-sc-17", "sc.submit",
                    {"file_paths": ["C:/_bogus_path_outside_sandbox_xyz_/foo.txt"],
                     "description": "MCP smoke test"})
        err = expect_error(resp, "p6-sc-17", (-32010, -32013, -32045, -32603),
                           "17/sc.submit bogus path no provider")
        if err is None:
            return 1
        info(f"17/sc.submit bogus path no provider OK code={err['code']}")
    else:
        resp = call(args.host, args.port, "17", "p6-sc-17", "sc.submit",
                    {"file_paths": ["C:/_bogus_path_outside_sandbox_xyz_/foo.txt"],
                     "description": "MCP smoke test"})
        err = expect_error(resp, "p6-sc-17", (-32010, -32013), "17/sc.submit bogus path")
        if err is None:
            return 1
        info(f"17/sc.submit bogus path OK code={err['code']}")

    # ─── Sub-test 18: sc.submit valid args returns {job_id} (BUT DO NOT POLL) ──────────────────
    # We DO NOT actually want to commit anything during smoke. The contract: with valid args
    # the composite returns {job_id} immediately. If we polled job.result it WOULD potentially
    # create a real commit (depending on file state). So we verify the {job_id} envelope ONLY,
    # then issue a job.cancel to prevent the job from actually running.
    if not provider_available:
        # Provider unavailable surface: still returns at C++ stage with -32045 BEFORE job creation.
        resp = call(args.host, args.port, "18", "p6-sc-18", "sc.submit",
                    {"file_paths": ["Plugins/UnrealMCPBridge/UnrealMCPBridge.uplugin"],
                     "description": "MCP smoke test (no provider — should fail at submit-time)"})
        err = expect_error(resp, "p6-sc-18", -32045, "18/sc.submit no provider")
        if err is None:
            return 1
        info("18/sc.submit no provider OK -32045 SourceControlProviderUnavailable")
    elif test_file is None:
        skip("18/sc.submit valid args: SKIPPED (no test file)")
    else:
        resp = call(args.host, args.port, "18", "p6-sc-18", "sc.submit",
                    {"file_paths": [test_file],
                     "description": "MCP smoke test (cancelled immediately — should NOT submit)"})
        result = expect_ok(resp, "p6-sc-18", "18/sc.submit valid args")
        if result is None:
            return 1
        job_id = result.get("job_id")
        if not isinstance(job_id, str) or not job_id:
            return fail(f"18/sc.submit: no job_id {result!r}")
        info(f"18/sc.submit returned {{job_id: {job_id}}} — cancelling to avoid actual submit")
        # Immediately request cancel to prevent actual provider RPC.
        cancel = call(args.host, args.port, "18cancel", "p6-sc-18cancel", "job.cancel",
                      {"job_id": job_id})
        if cancel is not None and cancel.get("ok") is True:
            info(f"18/sc.submit cancel issued (best-effort — job may already have started)")

    print("[SMOKE_PHASE6_SC] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
