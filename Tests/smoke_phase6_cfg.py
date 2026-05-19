#!/usr/bin/env python3
"""Phase 6 Chunk C smoke — Config / CVars (6 sync cfg.* tools).

Verifies (against a live editor on port 30020):

  Discovery (1):
    1. tools.list contains all 6 new cfg.* C++ handlers.

  cfg.get_cvar (2-4):
    2. cfg.get_cvar on a known-existent cvar (probed live from cfg.list_cvars) →
       {name, type, value, value_string, help, default_value, set_by, flags_raw,
        is_read_only, is_cheat, is_unregistered}. Schema fields present + typed.
    3. Negative: cfg.get_cvar with bogus name → -32004 ObjectNotFound.
    4. Negative: cfg.get_cvar with missing 'name' → -32602 InvalidParams.

  cfg.set_cvar (5-9):
    5. Round-trip int: cfg.set_cvar on a probed int-typed cvar with a new value, capture prior,
       then restore. Verify {set, prior_value, type, new_value, set_by} shape.
    6. Round-trip float: same on a probed float cvar.
    7. Round-trip bool: same on a probed bool cvar (if any visible).
    8. Negative: cfg.set_cvar with bogus name → -32004.
    9. Negative: cfg.set_cvar with missing 'value' → -32602.
   10. Negative: cfg.set_cvar with object value → -32006 PropertyTypeMismatch.

  cfg.list_cvars (11-12):
   11. cfg.list_cvars page 1 (no prefix) → {cvars, total_known, next_page_token, prefix_filter_echo}.
       Schema fields on per-entry: {name, type, value_summary, help_first_line, set_by,
       is_read_only, is_cheat, is_command, flags_raw}.
   12. cfg.list_cvars with prefix_filter="r." → narrowed results, filter_echo="r.".
   13. Pagination follow-up: if next_page_token returned, follow it and verify more entries.
   14. Negative: bad page_token → -32015 StaleCursor.

  cfg.read (15-17):
   15. cfg.read on a known section/key (resolved live from cfg.list_sections) → {found, value,
       raw_string, type_hint, ini_path, ini_file_echo, ...}.
   16. Negative: cfg.read with bogus ini_file → -32013 PathEscape.
   17. Negative: cfg.read with missing key → -32004 ObjectNotFound.

  cfg.write (18-20):
   18. Round-trip write: cfg.write to DefaultGame.ini under a sandbox-safe section/key,
       cfg.read back to verify, cfg.write the prior value (or restore via removed-key write).
   19. Negative: cfg.write with bogus ini_file → -32013.
   20. Negative: cfg.write with object value → -32006.

  cfg.list_sections (21-23):
   21. cfg.list_sections on DefaultEngine.ini → {sections, total_known, ini_path, ...}.
   22. cfg.list_sections pagination via page_size=2 + follow next_page_token.
   23. Negative: cfg.list_sections with bogus ini_file → -32013.

Prints ``[SMOKE_PHASE6_CFG] PASS`` on success or ``[SMOKE_PHASE6_CFG] FAIL ...`` on first
mismatch.

Usage:
  python smoke_phase6_cfg.py [--host HOST] [--port PORT]
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

# Whitelisted ini files per D8.
WHITELISTED_INIS = ["DefaultEngine", "DefaultGame", "DefaultInput", "DefaultEditor"]

# Probe cvars to find sample names for typed round-trip tests. We resolve live via cfg.list_cvars
# because the exact set varies by build config + loaded plugins. These are fallback hints used as
# prefix filters when probing.
TYPED_CVAR_HINTS = {
    "int":   ["r.Streaming.PoolSize", "r.MaxAnisotropy", "r.Shadow.MaxResolution"],
    "float": ["r.ScreenPercentage", "t.MaxFPS", "r.SkyAtmosphere.SampleCountMax"],
    "bool":  ["r.VSync", "r.Streaming.UseFixedPoolSize", "t.UseFastFixedFrameTime"],
}

# Section/key the round-trip cfg.write test uses. Sits inside DefaultGame.ini under a clearly-
# scoped fixture section so it doesn't collide with real project settings. Cleanup-on-exit.
ROUND_TRIP_INI = "DefaultGame"
ROUND_TRIP_SECTION = "/Script/MCPBridgeSmoke.CfgRoundTrip"
ROUND_TRIP_KEY = "SmokeTestValue"


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
    print(f"[SMOKE_PHASE6_CFG] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE6_CFG]   SKIP {reason}")


def info(message: str) -> None:
    print(f"[SMOKE_PHASE6_CFG]   {message}")


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


def probe_typed_cvar(host: str, port: int, want_type: str) -> Optional[Dict[str, Any]]:
    """Find a non-read-only cvar of the desired type by walking list_cvars + verifying via
    cfg.get_cvar. Returns the {name, type, value_string, value, ...} of the matched cvar or
    None if no suitable candidate found.

    Strategy: walk a large page of list_cvars (page_size=1000), then for each entry that's
    not a command AND is the right type AND is not read_only AND is not a cheat → call
    cfg.get_cvar to verify + return.
    """
    resp = call(host, port, f"probe-{want_type}",
                f"p6-cfg-probe-{want_type}-{int(time.time() * 1000)}",
                "cfg.list_cvars", {"page_size": 1000})
    if resp is None or resp.get("ok") is not True:
        return None
    cvars = (resp.get("result") or {}).get("cvars") or []
    # Prefer hinted names first.
    hint_set = set(TYPED_CVAR_HINTS.get(want_type, []))
    hint_matches = [c for c in cvars
                    if c.get("name") in hint_set
                    and c.get("type") == want_type
                    and not c.get("is_command")
                    and not c.get("is_read_only")
                    and not c.get("is_cheat")]
    fallback = [c for c in cvars
                if c.get("type") == want_type
                and not c.get("is_command")
                and not c.get("is_read_only")
                and not c.get("is_cheat")]
    candidates = hint_matches + fallback
    for c in candidates:
        name = c.get("name")
        if not name:
            continue
        verify = call(host, port, f"probe-{want_type}-verify",
                      f"p6-cfg-probe-verify-{name}-{int(time.time() * 1000)}",
                      "cfg.get_cvar", {"name": name})
        if verify is None or verify.get("ok") is not True:
            continue
        full = verify.get("result") or {}
        if full.get("type") == want_type and not full.get("is_read_only"):
            return full
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[SMOKE_PHASE6_CFG] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 6 cfg.* handlers ──────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p6-cfg-1", "tools.list"),
                       "p6-cfg-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "cfg.get_cvar",
        "cfg.set_cvar",
        "cfg.list_cvars",
        "cfg.read",
        "cfg.write",
        "cfg.list_sections",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing cfg.* handlers: {sorted(missing)}")
    info("1/tools.list contains all 6 cfg.* sync handlers")

    # ─── Sub-test 2: cfg.get_cvar on probed cvar ────────────────────────────────────────────────
    # Probe a float (most likely to exist + be mutable).
    probed_float = probe_typed_cvar(args.host, args.port, "float")
    if probed_float is None:
        skip("2/cfg.get_cvar: no mutable float cvar found via probe — try int")
        probed_float = probe_typed_cvar(args.host, args.port, "int")
    probed_any = probed_float
    if probed_any is None:
        return fail("2/cfg.get_cvar: probe failed to find ANY mutable cvar — environment broken?")

    probe_name = probed_any.get("name")
    result = expect_ok(
        call(args.host, args.port, "2", "p6-cfg-2", "cfg.get_cvar", {"name": probe_name}),
        "p6-cfg-2", "2/cfg.get_cvar")
    if result is None:
        return 1
    for field in ("name", "type", "value", "value_string", "help", "default_value", "set_by",
                  "flags_raw", "is_read_only", "is_cheat", "is_unregistered"):
        if field not in result:
            return fail(f"2/cfg.get_cvar: missing field {field!r}: {result!r}")
    if result["name"] != probe_name:
        return fail(f"2/cfg.get_cvar: name echo mismatch expected={probe_name!r} got={result['name']!r}")
    if result["type"] not in ("bool", "int", "float", "string", "unknown"):
        return fail(f"2/cfg.get_cvar: unexpected type {result['type']!r}")
    if not isinstance(result["is_read_only"], bool):
        return fail(f"2/cfg.get_cvar: is_read_only not bool {result!r}")
    info(f"2/cfg.get_cvar OK name='{probe_name}' type={result['type']} set_by={result['set_by']}")

    # ─── Sub-test 3: cfg.get_cvar bogus name → -32004 ──────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "3", "p6-cfg-3", "cfg.get_cvar",
             {"name": "bogus.nonexistent.cvar.zzz"}),
        "p6-cfg-3", -32004, "3/cfg.get_cvar bogus")
    if err is None:
        return 1
    info("3/cfg.get_cvar bogus name OK -32004 ObjectNotFound")

    # ─── Sub-test 4: cfg.get_cvar missing 'name' → -32602 ──────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "4", "p6-cfg-4", "cfg.get_cvar", {}),
        "p6-cfg-4", -32602, "4/cfg.get_cvar missing name")
    if err is None:
        return 1
    info("4/cfg.get_cvar missing name OK -32602 InvalidParams")

    # ─── Sub-test 5: round-trip int cvar ────────────────────────────────────────────────────────
    probed_int = probe_typed_cvar(args.host, args.port, "int")
    if probed_int is None:
        skip("5/cfg.set_cvar int round-trip: no mutable int cvar found")
    else:
        int_name = probed_int["name"]
        try:
            prior_int = int(probed_int.get("value", 0))
        except (TypeError, ValueError):
            prior_int = 0
        new_int = prior_int + 1
        result = expect_ok(
            call(args.host, args.port, "5", "p6-cfg-5", "cfg.set_cvar",
                 {"name": int_name, "value": new_int}),
            "p6-cfg-5", "5/cfg.set_cvar int round-trip")
        if result is None:
            return 1
        for field in ("set", "name", "type", "prior_value", "prior_value_string",
                      "new_value", "new_value_string", "set_by"):
            if field not in result:
                return fail(f"5/cfg.set_cvar: missing field {field!r}: {result!r}")
        if result["set"] is not True:
            return fail(f"5/cfg.set_cvar: set != True {result!r}")
        if result["type"] != "int":
            return fail(f"5/cfg.set_cvar: type != 'int' got {result['type']!r}")
        info(f"5/cfg.set_cvar int round-trip OK name='{int_name}' "
             f"{result['prior_value']!r} → {result['new_value']!r}")
        # Restore prior value.
        restore = call(args.host, args.port, "5restore", "p6-cfg-5restore",
                       "cfg.set_cvar", {"name": int_name, "value": prior_int})
        if restore is not None and restore.get("ok") is True:
            info(f"5/cfg.set_cvar int restore OK")

    # ─── Sub-test 6: round-trip float cvar ──────────────────────────────────────────────────────
    if probed_float is None:
        skip("6/cfg.set_cvar float round-trip: no mutable float cvar found")
    else:
        float_name = probed_float["name"]
        try:
            prior_float = float(probed_float.get("value", 0.0))
        except (TypeError, ValueError):
            prior_float = 0.0
        new_float = prior_float + 1.5
        result = expect_ok(
            call(args.host, args.port, "6", "p6-cfg-6", "cfg.set_cvar",
                 {"name": float_name, "value": new_float}),
            "p6-cfg-6", "6/cfg.set_cvar float round-trip")
        if result is None:
            return 1
        if result.get("type") != "float":
            return fail(f"6/cfg.set_cvar: type != 'float' got {result.get('type')!r}")
        info(f"6/cfg.set_cvar float round-trip OK name='{float_name}' "
             f"{result['prior_value']!r} → {result['new_value']!r}")
        restore = call(args.host, args.port, "6restore", "p6-cfg-6restore",
                       "cfg.set_cvar", {"name": float_name, "value": prior_float})
        if restore is not None and restore.get("ok") is True:
            info(f"6/cfg.set_cvar float restore OK")

    # ─── Sub-test 7: round-trip bool cvar ───────────────────────────────────────────────────────
    probed_bool = probe_typed_cvar(args.host, args.port, "bool")
    if probed_bool is None:
        skip("7/cfg.set_cvar bool round-trip: no mutable bool cvar found (UE often exposes "
             "bool cvars as int — count it informational, not a failure)")
    else:
        bool_name = probed_bool["name"]
        prior_bool = bool(probed_bool.get("value", False))
        new_bool = not prior_bool
        result = expect_ok(
            call(args.host, args.port, "7", "p6-cfg-7", "cfg.set_cvar",
                 {"name": bool_name, "value": new_bool}),
            "p6-cfg-7", "7/cfg.set_cvar bool round-trip")
        if result is None:
            return 1
        if result.get("type") != "bool":
            return fail(f"7/cfg.set_cvar: type != 'bool' got {result.get('type')!r}")
        info(f"7/cfg.set_cvar bool round-trip OK name='{bool_name}' "
             f"{result['prior_value']!r} → {result['new_value']!r}")
        restore = call(args.host, args.port, "7restore", "p6-cfg-7restore",
                       "cfg.set_cvar", {"name": bool_name, "value": prior_bool})
        if restore is not None and restore.get("ok") is True:
            info(f"7/cfg.set_cvar bool restore OK")

    # ─── Sub-test 8: cfg.set_cvar bogus name → -32004 ───────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "8", "p6-cfg-8", "cfg.set_cvar",
             {"name": "bogus.nonexistent.cvar.zzz", "value": 1}),
        "p6-cfg-8", -32004, "8/cfg.set_cvar bogus")
    if err is None:
        return 1
    info("8/cfg.set_cvar bogus name OK -32004 ObjectNotFound")

    # ─── Sub-test 9: cfg.set_cvar missing 'value' → -32602 ──────────────────────────────────────
    if probed_any is not None:
        err = expect_error(
            call(args.host, args.port, "9", "p6-cfg-9", "cfg.set_cvar",
                 {"name": probed_any["name"]}),
            "p6-cfg-9", -32602, "9/cfg.set_cvar missing value")
        if err is None:
            return 1
        info("9/cfg.set_cvar missing value OK -32602 InvalidParams")
    else:
        skip("9/cfg.set_cvar missing value: no probed cvar")

    # ─── Sub-test 10: cfg.set_cvar object value → -32006 ────────────────────────────────────────
    if probed_any is not None:
        # Send an object value (illegal — must be primitive).
        err = expect_error(
            call(args.host, args.port, "10", "p6-cfg-10", "cfg.set_cvar",
                 {"name": probed_any["name"], "value": {"nested": "object"}}),
            "p6-cfg-10", -32006, "10/cfg.set_cvar object value")
        if err is None:
            return 1
        info("10/cfg.set_cvar object value OK -32006 PropertyTypeMismatch")
    else:
        skip("10/cfg.set_cvar object value: no probed cvar")

    # ─── Sub-test 11: cfg.list_cvars page 1 ─────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "11", "p6-cfg-11", "cfg.list_cvars",
             {"page_size": 50}),
        "p6-cfg-11", "11/cfg.list_cvars page 1")
    if result is None:
        return 1
    for key in ("cvars", "total_known", "prefix_filter_echo"):
        if key not in result:
            return fail(f"11/cfg.list_cvars: missing key {key!r}: {result!r}")
    if not isinstance(result["cvars"], list):
        return fail(f"11/cfg.list_cvars: cvars not list {result!r}")
    if result.get("prefix_filter_echo") != "":
        return fail(f"11/cfg.list_cvars: prefix_filter_echo should be '' got {result.get('prefix_filter_echo')!r}")
    if result["cvars"]:
        entry = result["cvars"][0]
        for field in ("name", "type", "value_summary", "help_first_line", "set_by",
                      "is_read_only", "is_cheat", "is_command", "flags_raw"):
            if field not in entry:
                return fail(f"11/cfg.list_cvars: entry missing field {field!r}: {entry!r}")
        for bool_field in ("is_read_only", "is_cheat", "is_command"):
            if not isinstance(entry[bool_field], bool):
                return fail(f"11/cfg.list_cvars: {bool_field} not bool {entry!r}")
    info(f"11/cfg.list_cvars page 1 OK total_known={int(result['total_known'])} "
         f"page_len={len(result['cvars'])}")

    # ─── Sub-test 12: cfg.list_cvars prefix_filter="r." ─────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "12", "p6-cfg-12", "cfg.list_cvars",
             {"prefix_filter": "r.", "page_size": 50}),
        "p6-cfg-12", "12/cfg.list_cvars prefix r.")
    if result is None:
        return 1
    if result.get("prefix_filter_echo") != "r.":
        return fail(f"12/cfg.list_cvars: prefix_filter_echo != 'r.' got {result.get('prefix_filter_echo')!r}")
    # Every returned entry's name should start with "r." (case-insensitive).
    for entry in result.get("cvars", []):
        name = entry.get("name", "")
        if not name.lower().startswith("r."):
            return fail(f"12/cfg.list_cvars: entry doesn't match prefix: name={name!r}")
    info(f"12/cfg.list_cvars prefix='r.' OK total_known={int(result['total_known'])}")

    # ─── Sub-test 13: cfg.list_cvars pagination follow ──────────────────────────────────────────
    # Use small page_size to force pagination.
    result = expect_ok(
        call(args.host, args.port, "13a", "p6-cfg-13a", "cfg.list_cvars",
             {"page_size": 5}),
        "p6-cfg-13a", "13a/cfg.list_cvars page 1 (size 5)")
    if result is None:
        return 1
    next_token = result.get("next_page_token")
    if next_token is None:
        # Engine has <5 cvars total? Implausible — but skip the follow-up.
        skip("13/cfg.list_cvars pagination: total_known<=5, can't follow")
    else:
        result2 = expect_ok(
            call(args.host, args.port, "13b", "p6-cfg-13b", "cfg.list_cvars",
                 {"page_size": 5, "page_token": next_token}),
            "p6-cfg-13b", "13b/cfg.list_cvars page 2")
        if result2 is None:
            return 1
        if not result2.get("cvars"):
            return fail(f"13b/cfg.list_cvars: page 2 empty unexpectedly: {result2!r}")
        # Verify no overlap with page 1 (names must be strictly greater).
        page1_names = {c["name"] for c in result.get("cvars", [])}
        page2_names = {c["name"] for c in result2.get("cvars", [])}
        overlap = page1_names & page2_names
        if overlap:
            return fail(f"13/cfg.list_cvars: page overlap detected: {sorted(overlap)[:5]}")
        info(f"13/cfg.list_cvars pagination OK page1={len(page1_names)} page2={len(page2_names)} no overlap")

    # ─── Sub-test 14: bad page_token → -32015 ───────────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "14", "p6-cfg-14", "cfg.list_cvars",
             {"page_token": "not-base64-json-blob!@#$"}),
        "p6-cfg-14", -32015, "14/cfg.list_cvars bad token")
    if err is None:
        return 1
    info("14/cfg.list_cvars bad page_token OK -32015 StaleCursor")

    # ─── Sub-test 15: cfg.read on probed section/key ────────────────────────────────────────────
    # Probe DefaultEngine.ini for a section to read from.
    sect_resp = expect_ok(
        call(args.host, args.port, "15probe", "p6-cfg-15probe", "cfg.list_sections",
             {"ini_file": "DefaultEngine", "page_size": 200}),
        "p6-cfg-15probe", "15probe/list_sections")
    if sect_resp is None:
        return 1
    sections = sect_resp.get("sections") or []
    if not sections:
        skip("15/cfg.read: DefaultEngine.ini has no sections to probe — environment unusual")
    else:
        # Just verify cfg.read succeeds (or returns -32004 for a section with no keys we know).
        # We probe well-known section /Script/Engine.Engine first; fall back to first section.
        probe_section = "/Script/Engine.Engine"
        if probe_section not in sections:
            probe_section = sections[0]
        # Try several known keys; first one that exists wins.
        candidate_keys = ["bSmoothFrameRate", "MaxFPS", "FrameRateLimit",
                          "DefaultGameMode", "EngineFontSize"]
        found_key = None
        found_value = None
        for k in candidate_keys:
            r = call(args.host, args.port, f"15try-{k}", f"p6-cfg-15try-{k}",
                     "cfg.read",
                     {"ini_file": "DefaultEngine", "section": probe_section, "key": k})
            if r is not None and r.get("ok") is True:
                found_key = k
                found_value = (r.get("result") or {}).get("value")
                break
        if found_key is None:
            skip(f"15/cfg.read: no known key found in [{probe_section}]; spot-check still requires "
                 "manual key supply")
        else:
            result = expect_ok(
                call(args.host, args.port, "15", "p6-cfg-15", "cfg.read",
                     {"ini_file": "DefaultEngine", "section": probe_section, "key": found_key}),
                "p6-cfg-15", "15/cfg.read")
            if result is None:
                return 1
            for field in ("found", "value", "raw_string", "type_hint", "ini_path",
                          "ini_file_echo", "section_echo", "key_echo"):
                if field not in result:
                    return fail(f"15/cfg.read: missing field {field!r}: {result!r}")
            if result["found"] is not True:
                return fail(f"15/cfg.read: found != True {result!r}")
            if result["type_hint"] not in ("bool", "int", "float", "string"):
                return fail(f"15/cfg.read: type_hint unexpected: {result['type_hint']!r}")
            info(f"15/cfg.read OK [{probe_section}] {found_key}={found_value!r} "
                 f"type_hint={result['type_hint']}")

    # ─── Sub-test 16: cfg.read bogus ini_file → -32013 ──────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "16", "p6-cfg-16", "cfg.read",
             {"ini_file": "EvilCustomIni", "section": "Foo", "key": "Bar"}),
        "p6-cfg-16", -32013, "16/cfg.read bogus ini")
    if err is None:
        return 1
    info("16/cfg.read bogus ini_file OK -32013 PathEscape")

    # Also test path-escape attempts.
    err = expect_error(
        call(args.host, args.port, "16b", "p6-cfg-16b", "cfg.read",
             {"ini_file": "../DefaultEngine", "section": "Foo", "key": "Bar"}),
        "p6-cfg-16b", -32013, "16b/cfg.read path escape")
    if err is None:
        return 1
    info("16b/cfg.read path-escape attempt OK -32013 PathEscape")

    # ─── Sub-test 17: cfg.read missing key → -32004 ─────────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "17", "p6-cfg-17", "cfg.read",
             {"ini_file": "DefaultEngine", "section": "/Script/Engine.Engine",
              "key": "ThisKeyMustNotExistInTheConfigZZZ"}),
        "p6-cfg-17", -32004, "17/cfg.read missing key")
    if err is None:
        return 1
    info("17/cfg.read missing key OK -32004 ObjectNotFound")

    # ─── Sub-test 18: cfg.write round-trip ──────────────────────────────────────────────────────
    # Capture prior state (probably absent).
    prior_state = call(args.host, args.port, "18prior", "p6-cfg-18prior", "cfg.read",
                       {"ini_file": ROUND_TRIP_INI, "section": ROUND_TRIP_SECTION,
                        "key": ROUND_TRIP_KEY})
    had_prior = prior_state is not None and prior_state.get("ok") is True
    prior_value = (prior_state.get("result") or {}).get("value", "") if had_prior else ""

    # Write a new value.
    test_value = f"SmokeTest_{int(time.time())}"
    result = expect_ok(
        call(args.host, args.port, "18", "p6-cfg-18", "cfg.write",
             {"ini_file": ROUND_TRIP_INI, "section": ROUND_TRIP_SECTION,
              "key": ROUND_TRIP_KEY, "value": test_value}),
        "p6-cfg-18", "18/cfg.write round-trip")
    if result is None:
        return 1
    for field in ("written", "prior_existed", "prior_value", "new_value_string",
                  "ini_path", "ini_file_echo", "section_echo", "key_echo", "flushed"):
        if field not in result:
            return fail(f"18/cfg.write: missing field {field!r}: {result!r}")
    if result["written"] is not True:
        return fail(f"18/cfg.write: written != True {result!r}")
    if result["new_value_string"] != test_value:
        return fail(f"18/cfg.write: new_value_string mismatch expected={test_value!r} got={result['new_value_string']!r}")
    info(f"18/cfg.write OK ini_path={result['ini_path']} prior_existed={result['prior_existed']} flushed={result['flushed']}")

    # Read-back verify.
    verify = expect_ok(
        call(args.host, args.port, "18verify", "p6-cfg-18verify", "cfg.read",
             {"ini_file": ROUND_TRIP_INI, "section": ROUND_TRIP_SECTION,
              "key": ROUND_TRIP_KEY}),
        "p6-cfg-18verify", "18verify/cfg.read after write")
    if verify is None:
        return 1
    if verify.get("value") != test_value:
        return fail(f"18verify/cfg.read: value mismatch expected={test_value!r} got={verify.get('value')!r}")
    info(f"18/cfg.write read-back verify OK value={verify.get('value')!r}")

    # Cleanup: restore prior state if there was one, else write empty + leave in-memory cache.
    # We don't have cfg.remove_key yet, so just write back the prior (or empty string).
    cleanup_value = prior_value if had_prior else ""
    cleanup = call(args.host, args.port, "18cleanup", "p6-cfg-18cleanup", "cfg.write",
                   {"ini_file": ROUND_TRIP_INI, "section": ROUND_TRIP_SECTION,
                    "key": ROUND_TRIP_KEY, "value": cleanup_value})
    if cleanup is not None and cleanup.get("ok") is True:
        info(f"18/cleanup wrote prior value={cleanup_value!r}")

    # ─── Sub-test 19: cfg.write bogus ini_file → -32013 ─────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "19", "p6-cfg-19", "cfg.write",
             {"ini_file": "EvilCustomIni", "section": "Foo", "key": "Bar", "value": "x"}),
        "p6-cfg-19", -32013, "19/cfg.write bogus ini")
    if err is None:
        return 1
    info("19/cfg.write bogus ini_file OK -32013 PathEscape")

    # ─── Sub-test 20: cfg.write object value → -32006 ───────────────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "20", "p6-cfg-20", "cfg.write",
             {"ini_file": ROUND_TRIP_INI, "section": ROUND_TRIP_SECTION,
              "key": ROUND_TRIP_KEY, "value": {"nested": True}}),
        "p6-cfg-20", -32006, "20/cfg.write object value")
    if err is None:
        return 1
    info("20/cfg.write object value OK -32006 PropertyTypeMismatch")

    # ─── Sub-test 21: cfg.list_sections on DefaultEngine ────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "21", "p6-cfg-21", "cfg.list_sections",
             {"ini_file": "DefaultEngine", "page_size": 100}),
        "p6-cfg-21", "21/cfg.list_sections DefaultEngine")
    if result is None:
        return 1
    for field in ("sections", "total_known", "ini_path", "ini_file_echo"):
        if field not in result:
            return fail(f"21/cfg.list_sections: missing field {field!r}: {result!r}")
    if not isinstance(result["sections"], list):
        return fail(f"21/cfg.list_sections: sections not list {result!r}")
    if result["ini_file_echo"] != "DefaultEngine":
        return fail(f"21/cfg.list_sections: ini_file_echo mismatch {result!r}")
    info(f"21/cfg.list_sections DefaultEngine OK total_known={int(result['total_known'])} "
         f"page_len={len(result['sections'])}")

    # ─── Sub-test 22: cfg.list_sections pagination ──────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "22a", "p6-cfg-22a", "cfg.list_sections",
             {"ini_file": "DefaultEngine", "page_size": 2}),
        "p6-cfg-22a", "22a/cfg.list_sections page 1 (size 2)")
    if result is None:
        return 1
    next_token = result.get("next_page_token")
    if next_token is None:
        skip("22/cfg.list_sections pagination: DefaultEngine has <=2 sections — skip follow")
    else:
        result2 = expect_ok(
            call(args.host, args.port, "22b", "p6-cfg-22b", "cfg.list_sections",
                 {"ini_file": "DefaultEngine", "page_size": 2, "page_token": next_token}),
            "p6-cfg-22b", "22b/cfg.list_sections page 2")
        if result2 is None:
            return 1
        if not result2.get("sections"):
            return fail(f"22b/cfg.list_sections: page 2 empty unexpectedly: {result2!r}")
        page1_names = set(result.get("sections", []))
        page2_names = set(result2.get("sections", []))
        overlap = page1_names & page2_names
        if overlap:
            return fail(f"22/cfg.list_sections: page overlap detected: {sorted(overlap)}")
        info(f"22/cfg.list_sections pagination OK page1={len(page1_names)} page2={len(page2_names)} no overlap")

    # ─── Sub-test 23: cfg.list_sections bogus ini_file → -32013 ─────────────────────────────────
    err = expect_error(
        call(args.host, args.port, "23", "p6-cfg-23", "cfg.list_sections",
             {"ini_file": "EvilCustomIni"}),
        "p6-cfg-23", -32013, "23/cfg.list_sections bogus ini")
    if err is None:
        return 1
    info("23/cfg.list_sections bogus ini_file OK -32013 PathEscape")

    print("[SMOKE_PHASE6_CFG] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
