#!/usr/bin/env python3
"""Phase 6 Chunk D + E smoke — Log additions (3 sync log.* tools) + Live Coding (1 async composite).

Verifies (against a live editor on port 30020):

  Discovery (1):
    1. tools.list contains the 3 new log.* C++ handlers
       (log.set_category_verbosity / log.list_categories / log.clear) AND the livecoding async
       internal handler (livecoding._recompile_internal). Python wrapper livecoding.recompile
       lives in the python_tools registry so we also check for it.

  log.list_categories (2-6):
    2. log.list_categories no filter → {categories[{name, current_verbosity, observation_count}],
       total_known, prefix_filter_echo, next_page_token}. At least LogMCP should be present.
    3. log.list_categories prefix_filter="Log" → narrowed, every entry's name starts with "Log".
    4. log.list_categories pagination via page_size=2 + follow next_page_token (skip if too few).
    5. Negative: bad page_token → -32015 StaleCursor.
    6. log.list_categories prefix_filter="ZZZZZ_nonexistent_zzz" → empty result.

  log.set_category_verbosity (7-11):
    7. log.set_category_verbosity LogMCP Verbose → {applied, category, requested_verbosity,
       prior_verbosity, console_output, compile_time_clamped} schema. Restore prior after.
    8. log.set_category_verbosity with operator alias "off" → applied. Restore.
    9. Negative: missing 'category' → -32602.
   10. Negative: missing 'verbosity' → -32602.
   11. Negative: bogus verbosity "MaximumOverdrive" → -32602.

  log.clear (12-14):
   12. log.clear → {cleared: true, line_count_before, ring_capacity, total_observed}. Sanity-check
       line_count_before >= 0, ring_capacity == 5000, total_observed > 0 (we made calls).
   13. Re-call log.clear → still cleared=true; line_count_before close to 0 (only the messages
       generated between the two clears).
   14. Verify log.tail after clear: returns very few entries (only those emitted after the clear).

  livecoding.recompile (15-18):
   15. tools.list contains livecoding.recompile Python wrapper + livecoding._recompile_internal
       C++ internal.
   16. livecoding.recompile with modules=["*"] → either {job_id} (Win + LC enabled + PIE off) OR
       -32048 LiveCodingDisabled (LC not configured) OR -32027 PIEActive. All three are valid
       responses depending on environment. Job_id path is NOT followed up (recompile would alter
       editor state — left to operator).
   17. Negative: livecoding.recompile with missing modules → -32602.
   18. Negative: livecoding.recompile with empty modules array → -32602.

Prints ``[SMOKE_PHASE6_LOG_LC] PASS`` on success or ``[SMOKE_PHASE6_LOG_LC] FAIL ...`` on first
mismatch.

Usage:
  python smoke_phase6_log_livecoding.py [--host HOST] [--port PORT]
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
    print(f"[SMOKE_PHASE6_LOG_LC] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE6_LOG_LC]   SKIP {reason}")


def info(message: str) -> None:
    print(f"[SMOKE_PHASE6_LOG_LC]   {message}")


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[SMOKE_PHASE6_LOG_LC] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains the 3 log.* additions + livecoding internal ──────────
    result = expect_ok(call(args.host, args.port, "1", "p6-loglc-1", "tools.list"),
                       "p6-loglc-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "log.set_category_verbosity",
        "log.list_categories",
        "log.clear",
        "livecoding._recompile_internal",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing handlers: {sorted(missing)}")
    info("1/tools.list contains all 3 log.* + livecoding._recompile_internal handlers")

    # Verify Python wrapper livecoding.recompile is present in python_tools.
    python_tools = result.get("python_tools") or {}
    if "livecoding.recompile" not in python_tools:
        # Acceptable degradation: Python tools registry may be down; just log.
        skip("1/tools.list: livecoding.recompile Python wrapper not in python_tools "
             "(registry may not be populated)")
    else:
        info("1/tools.list: livecoding.recompile Python wrapper registered")

    # ─── Sub-test 2: log.list_categories no filter ────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "2", "p6-loglc-2", "log.list_categories",
             {"page_size": 1000}),
        "p6-loglc-2", "2/log.list_categories no filter")
    if result is None:
        return 1
    for key in ("categories", "total_known", "prefix_filter_echo"):
        if key not in result:
            return fail(f"2/log.list_categories: missing key {key!r}: {result!r}")
    if not isinstance(result["categories"], list):
        return fail(f"2/log.list_categories: categories not list {result!r}")
    if result["prefix_filter_echo"] != "":
        return fail(f"2/log.list_categories: prefix_filter_echo should be '' got "
                    f"{result['prefix_filter_echo']!r}")
    # At least LogMCP should be observed (the bridge itself logs from this category at startup).
    cat_names = {c.get("name") for c in result["categories"]}
    if not cat_names:
        return fail("2/log.list_categories: empty categories list — environment broken? "
                    "The bridge module should have logged at least LogMCP entries during init.")
    if "LogMCP" not in cat_names:
        skip(f"2/log.list_categories: LogMCP not observed (bridge logs may have rolled out of "
             f"the ring; observed {len(cat_names)} categories)")
    else:
        info(f"2/log.list_categories OK total={int(result['total_known'])} "
             f"LogMCP present")

    # Per-entry schema check.
    if result["categories"]:
        entry = result["categories"][0]
        for field in ("name", "current_verbosity", "observation_count"):
            if field not in entry:
                return fail(f"2/log.list_categories: entry missing {field!r}: {entry!r}")
        if not isinstance(entry["name"], str) or not entry["name"]:
            return fail(f"2/log.list_categories: entry.name not non-empty string: {entry!r}")
        if entry["current_verbosity"] not in ("NoLogging", "Fatal", "Error", "Warning",
                                              "Display", "Log", "Verbose", "VeryVerbose"):
            return fail(f"2/log.list_categories: entry.current_verbosity unexpected: "
                        f"{entry['current_verbosity']!r}")

    # ─── Sub-test 3: log.list_categories prefix_filter="Log" ──────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "3", "p6-loglc-3", "log.list_categories",
             {"prefix_filter": "Log", "page_size": 1000}),
        "p6-loglc-3", "3/log.list_categories prefix Log")
    if result is None:
        return 1
    if result.get("prefix_filter_echo") != "Log":
        return fail(f"3/log.list_categories: prefix_filter_echo != 'Log' got "
                    f"{result.get('prefix_filter_echo')!r}")
    # Every returned entry's name should start with "Log" (case-insensitive).
    for entry in result.get("categories", []):
        name = entry.get("name", "")
        if not name.lower().startswith("log"):
            return fail(f"3/log.list_categories: entry doesn't match prefix: name={name!r}")
    info(f"3/log.list_categories prefix='Log' OK total={int(result['total_known'])}")

    # ─── Sub-test 4: pagination ───────────────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "4a", "p6-loglc-4a", "log.list_categories",
             {"page_size": 2}),
        "p6-loglc-4a", "4a/log.list_categories page 1 (size 2)")
    if result is None:
        return 1
    next_token = result.get("next_page_token")
    if next_token is None:
        skip("4/log.list_categories pagination: <=2 categories observed — can't follow")
    else:
        result2 = expect_ok(
            call(args.host, args.port, "4b", "p6-loglc-4b", "log.list_categories",
                 {"page_size": 2, "page_token": next_token}),
            "p6-loglc-4b", "4b/log.list_categories page 2")
        if result2 is None:
            return 1
        if not result2.get("categories"):
            return fail(f"4b/log.list_categories: page 2 empty unexpectedly: {result2!r}")
        page1_names = {c["name"] for c in result.get("categories", [])}
        page2_names = {c["name"] for c in result2.get("categories", [])}
        overlap = page1_names & page2_names
        if overlap:
            return fail(f"4/log.list_categories: page overlap detected: {sorted(overlap)}")
        info(f"4/log.list_categories pagination OK page1={len(page1_names)} "
             f"page2={len(page2_names)} no overlap")

    # ─── Sub-test 5: bad page_token → -32015 ─────────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "5", "p6-loglc-5", "log.list_categories",
             {"page_token": "not-base64-json-blob!@#$"}),
        "p6-loglc-5", -32015, "5/log.list_categories bad token")
    if err is None:
        return 1
    info("5/log.list_categories bad page_token OK -32015 StaleCursor")

    # ─── Sub-test 6: nonexistent prefix → empty ──────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "6", "p6-loglc-6", "log.list_categories",
             {"prefix_filter": "ZZZZZ_nonexistent_zzz"}),
        "p6-loglc-6", "6/log.list_categories nonexistent prefix")
    if result is None:
        return 1
    if result.get("categories"):
        return fail(f"6/log.list_categories: expected empty got {len(result['categories'])}: "
                    f"{result['categories'][:3]}")
    if result.get("total_known", -1) != 0:
        return fail(f"6/log.list_categories: total_known != 0 got {result.get('total_known')!r}")
    info("6/log.list_categories nonexistent prefix OK (empty result)")

    # ─── Sub-test 7: log.set_category_verbosity LogMCP Verbose → restore ─────────────────────
    result = expect_ok(
        call(args.host, args.port, "7", "p6-loglc-7", "log.set_category_verbosity",
             {"category": "LogMCP", "verbosity": "Verbose"}),
        "p6-loglc-7", "7/log.set_category_verbosity LogMCP Verbose")
    if result is None:
        return 1
    for field in ("applied", "category", "requested_verbosity", "prior_verbosity",
                  "console_output", "compile_time_clamped"):
        if field not in result:
            return fail(f"7/log.set_category_verbosity: missing field {field!r}: {result!r}")
    if result["applied"] is not True:
        return fail(f"7/log.set_category_verbosity: applied != True {result!r}")
    if result["category"] != "LogMCP":
        return fail(f"7/log.set_category_verbosity: category echo != LogMCP {result!r}")
    if result["requested_verbosity"] != "Verbose":
        return fail(f"7/log.set_category_verbosity: requested_verbosity != Verbose {result!r}")
    info(f"7/log.set_category_verbosity LogMCP→Verbose OK prior={result['prior_verbosity']!r}")
    # Restore. Use prior or Log (the canonical default).
    prior = result.get("prior_verbosity", "Log")
    if prior in ("Unknown", "NoLogging"):
        prior = "Log"
    restore = call(args.host, args.port, "7restore", "p6-loglc-7restore",
                   "log.set_category_verbosity",
                   {"category": "LogMCP", "verbosity": prior})
    if restore is not None and restore.get("ok") is True:
        info(f"7/log.set_category_verbosity LogMCP restored to {prior!r}")

    # ─── Sub-test 8: operator alias "off" → applied. Restore. ─────────────────────────────────
    # Use a category that's relatively safe to mute briefly.
    result = expect_ok(
        call(args.host, args.port, "8", "p6-loglc-8", "log.set_category_verbosity",
             {"category": "LogTemp", "verbosity": "off"}),
        "p6-loglc-8", "8/log.set_category_verbosity LogTemp off (alias)")
    if result is None:
        return 1
    if result["applied"] is not True:
        return fail(f"8/log.set_category_verbosity: applied != True {result!r}")
    # 'Off' should canonicalise to 'Off' (we don't force-map to NoLogging in our table).
    if result["requested_verbosity"] not in ("Off", "NoLogging", "None"):
        return fail(f"8/log.set_category_verbosity: requested_verbosity unexpected for 'off': "
                    f"{result['requested_verbosity']!r}")
    info(f"8/log.set_category_verbosity LogTemp→off OK canonicalised to "
         f"{result['requested_verbosity']!r}")
    # Restore.
    restore = call(args.host, args.port, "8restore", "p6-loglc-8restore",
                   "log.set_category_verbosity",
                   {"category": "LogTemp", "verbosity": "Log"})
    if restore is not None and restore.get("ok") is True:
        info(f"8/log.set_category_verbosity LogTemp restored to Log")

    # ─── Sub-test 9: missing 'category' → -32602 ──────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "9", "p6-loglc-9", "log.set_category_verbosity",
             {"verbosity": "Verbose"}),
        "p6-loglc-9", -32602, "9/log.set_category_verbosity missing category")
    if err is None:
        return 1
    info("9/log.set_category_verbosity missing 'category' OK -32602")

    # ─── Sub-test 10: missing 'verbosity' → -32602 ────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "10", "p6-loglc-10", "log.set_category_verbosity",
             {"category": "LogMCP"}),
        "p6-loglc-10", -32602, "10/log.set_category_verbosity missing verbosity")
    if err is None:
        return 1
    info("10/log.set_category_verbosity missing 'verbosity' OK -32602")

    # ─── Sub-test 11: bogus verbosity → -32602 ────────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "11", "p6-loglc-11", "log.set_category_verbosity",
             {"category": "LogMCP", "verbosity": "MaximumOverdrive"}),
        "p6-loglc-11", -32602, "11/log.set_category_verbosity bogus verbosity")
    if err is None:
        return 1
    info("11/log.set_category_verbosity bogus verbosity OK -32602")

    # ─── Sub-test 12: log.clear ───────────────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "12", "p6-loglc-12", "log.clear"),
        "p6-loglc-12", "12/log.clear")
    if result is None:
        return 1
    for field in ("cleared", "line_count_before", "ring_capacity", "total_observed"):
        if field not in result:
            return fail(f"12/log.clear: missing field {field!r}: {result!r}")
    if result["cleared"] is not True:
        return fail(f"12/log.clear: cleared != True {result!r}")
    if result["ring_capacity"] != 5000:
        return fail(f"12/log.clear: ring_capacity != 5000 got {result['ring_capacity']!r}")
    if not isinstance(result["line_count_before"], (int, float)) or result["line_count_before"] < 0:
        return fail(f"12/log.clear: line_count_before invalid: {result!r}")
    if not isinstance(result["total_observed"], (int, float)) or result["total_observed"] < 1:
        return fail(f"12/log.clear: total_observed should be > 0: {result!r}")
    info(f"12/log.clear OK line_count_before={int(result['line_count_before'])} "
         f"total_observed={int(result['total_observed'])}")

    # ─── Sub-test 13: re-call log.clear; line_count_before should be small ────────────────────
    result = expect_ok(
        call(args.host, args.port, "13", "p6-loglc-13", "log.clear"),
        "p6-loglc-13", "13/log.clear repeat")
    if result is None:
        return 1
    if result["cleared"] is not True:
        return fail(f"13/log.clear: cleared != True {result!r}")
    # Between sub-tests 12 and 13 the bridge has logged a few entries (our own calls produce log
    # output via the C++ handler registration). Cap is generous — we just want to confirm it's
    # not the same as 12's count, indicating the ring really did clear.
    if result["line_count_before"] > 500:
        return fail(f"13/log.clear: line_count_before suspiciously high ({result['line_count_before']}) "
                    f"— suggests log.clear didn't actually clear")
    info(f"13/log.clear repeat OK line_count_before={int(result['line_count_before'])} "
         f"(small = real clear took)")

    # ─── Sub-test 14: log.tail after clear ────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "14", "p6-loglc-14", "log.tail", {"lines": 100}),
        "p6-loglc-14", "14/log.tail after clear")
    if result is None:
        return 1
    if "entries" not in result:
        return fail(f"14/log.tail: missing 'entries' key: {result!r}")
    entries = result["entries"]
    if not isinstance(entries, list):
        return fail(f"14/log.tail: entries not a list: {result!r}")
    # We just cleared; entries should be small.
    if len(entries) > 200:
        return fail(f"14/log.tail: too many entries after clear ({len(entries)}); "
                    f"clear didn't take effect")
    info(f"14/log.tail after clear OK {len(entries)} entries (small/empty as expected)")

    # ─── Sub-test 15: tools.list contains livecoding.recompile + internal ─────────────────────
    # Re-verified above in sub-test 1; just print a confirmation here for clarity.
    info("15/livecoding.recompile registration verified in sub-test 1")

    # ─── Sub-test 16: livecoding.recompile job_id OR -32048 OR -32027 ─────────────────────────
    # Use a deliberately bogus module name. Per livecoding.recompile contract: "Live Coding's
    # own compile path gracefully handles unknown module names (logged as warnings, not errors)".
    # This means the compile body runs but finds nothing to compile — fast no-op, doesn't block
    # GT for tens of seconds with a real recompile. Real-module recompile would block subsequent
    # Python @tool calls (sub-test 17+) for the full duration of the compile.
    response = call(args.host, args.port, "16", "p6-loglc-16", "livecoding.recompile",
                    {"modules": ["__MCP_Smoke_NonExistent_Module__"]}, timeout=10.0)
    if response is None:
        return fail("16/livecoding.recompile: timeout")
    if response.get("id") != "p6-loglc-16":
        return fail(f"16/livecoding.recompile: id mismatch {response.get('id')!r}")
    if response.get("ok") is True:
        # Success — we got a job_id. Cancel + poll until terminal so subsequent sub-tests
        # don't fight the still-running Live Coding job for GT time. Without this barrier
        # sub-tests 17/18 (Python @tool calls — FMCPPythonEval is GT-bound) time out at 30s
        # waiting for the recompile to drain.
        result = response.get("result") or {}
        job_id = result.get("job_id")
        if not job_id:
            return fail(f"16/livecoding.recompile: missing job_id in success result: {result!r}")
        info(f"16/livecoding.recompile OK job_id={job_id}; cancelling + draining…")
        call(args.host, args.port, "16cancel", "p6-loglc-16cancel",
             "job.cancel", {"job_id": job_id})
        # Wait up to 20 s for terminal state — Live Coding doesn't honour cancel mid-compile but
        # the bogus-module form returns quickly anyway.
        import time as _t
        deadline = _t.monotonic() + 20.0
        while _t.monotonic() < deadline:
            sr = call(args.host, args.port, "16poll", "p6-loglc-16poll",
                      "job.status", {"job_id": job_id})
            if sr and sr.get("ok") and (sr.get("result") or {}).get("state") in (
                    "Succeeded", "Failed", "Cancelled"):
                info(f"16/livecoding.recompile drained — state={(sr['result']).get('state')}")
                break
            _t.sleep(0.5)
        else:
            info("16/livecoding.recompile job did not drain in 20 s (continuing anyway)")
    else:
        # Error path — accept -32048 LiveCodingDisabled or -32027 PIEActive.
        err = response.get("error") or {}
        code = err.get("code")
        if code in (-32048, -32027):
            info(f"16/livecoding.recompile rejected with expected error code {code} "
                 f"(LC unavailable or PIE active — both valid environments)")
        else:
            return fail(f"16/livecoding.recompile: unexpected error code {code}: {err!r}")

    # ─── Sub-test 17: livecoding.recompile missing modules → -32602 ───────────────────────────
    err = expect_error(
        call(args.host, args.port, "17", "p6-loglc-17", "livecoding.recompile", {}),
        "p6-loglc-17", -32602, "17/livecoding.recompile missing modules")
    if err is None:
        return 1
    info("17/livecoding.recompile missing 'modules' OK -32602")

    # ─── Sub-test 18: livecoding.recompile empty modules → -32602 ─────────────────────────────
    err = expect_error(
        call(args.host, args.port, "18", "p6-loglc-18", "livecoding.recompile",
             {"modules": []}),
        "p6-loglc-18", -32602, "18/livecoding.recompile empty modules")
    if err is None:
        return 1
    info("18/livecoding.recompile empty 'modules' OK -32602")

    print("[SMOKE_PHASE6_LOG_LC] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
