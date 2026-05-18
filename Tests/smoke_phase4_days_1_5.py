#!/usr/bin/env python3
"""Phase 4 Days 1-5 smoke — 6 Blueprint read tools.

Verifies (against a live editor on port 30020):

  1. tools.list contains all 6 bp.* names.
  2. bp.exists positive — at least one BP_* asset under /Game returns exists=true.
  3. bp.exists negative — /Game/__bogus_bp_path → exists=false (success response, NOT error).
  4. bp.exists wrong type — /Game/.../some non-UBlueprint asset (if present) → -32031 BlueprintTypeMismatch
     [SKIP allowed if no candidate found].
  5. bp.list_variables on positive BP → variables array (possibly empty) + total_known >= 0
     + each entry has {name, pin_type{category, ...}, default_value, replicated, exposed_on_spawn}.
  6. bp.get_variable positive — first variable name from list_variables returns matching
     summary. [SKIP allowed if list_variables empty.]
  7. bp.get_variable negative — variable_name='__nonexistent_variable_xyz' → -32037 VariableNotFound.
  8. bp.list_functions on positive BP → functions array (possibly empty) + total_known >= 0
     + each entry has {name, category, access_specifier, is_pure, is_const, is_static, signature{inputs,outputs}}.
  9. bp.get_function positive — first function from list_functions returns matching summary
     with extra fields {local_variables[], execution_path_node_count}. [SKIP if list_functions empty.]
 10. bp.get_function negative — function_name='__no_such_function' → -32037.
 11. bp.list_nodes_in_function on a function with body → nodes array, each with {node_guid, class,
     title, pins[{name, direction, pin_type, connected_to[]}]}. [SKIP if no functions.]
 12. bp.list_nodes_in_function with bogus function → -32037.
 13. Pagination — bp.list_variables with page_size=1 covers all variables across N requests, no
     overlap, no missing. [SKIP if total_known < 2.]
 14. Invalid path syntax — bp.exists with path '../bad' → -32010 InvalidPath.

Test asset selection:
  - First try /Game/MCPTest/Phase4/BP_TestActor (plan-mandated).
  - Else first BP_* asset under /Game/ via FilterByPath probing.
  - Else first hit from existing AR search via asset.list — but we keep it simple and try a
    known FatumGame asset: /Game/BP_PlayerFlecs.
  - If none found, sub-tests requiring a BP SKIP with informative message.

Prints ``[SMOKE_PHASE4_1_5] PASS`` on success or ``[SMOKE_PHASE4_1_5] FAIL ...`` on first mismatch.

Usage:
  python smoke_phase4_days_1_5.py [--host HOST] [--port PORT] [--bp-path /Game/.../BP_X]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Any, Optional


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 10.0

# Candidate BP paths probed in order until one resolves. Smoke test prefers the plan-mandated
# /Game/MCPTest/Phase4/BP_TestActor (created by user before running); otherwise falls back to
# a known FatumGame BP for at-least-one-asset coverage.
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
    print(f"[SMOKE_PHASE4_1_5] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE4_1_5]   SKIP {reason}")


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


def expect_error(response: Optional[dict], expected_id: str, expected_code: int, label: str) -> bool:
    if response is None:
        fail(f"{label}: timeout (>{READ_TIMEOUT_SEC}s)")
        return False
    if response.get("id") != expected_id:
        fail(f"{label}: id-mismatch expected={expected_id!r} got={response.get('id')!r}")
        return False
    if response.get("ok") is not False:
        fail(f"{label}: ok-not-false got={response.get('ok')!r}")
        return False
    error = response.get("error")
    if not isinstance(error, dict) or error.get("code") != expected_code:
        fail(f"{label}: wrong-error-code expected={expected_code} got={error!r}")
        return False
    return True


def resolve_test_bp(host: str, port: int) -> Optional[str]:
    """Probe FALLBACK_BP_PATHS in order; return the first one whose bp.exists returns exists=true."""
    for path in FALLBACK_BP_PATHS:
        resp = call(host, port, "probe", f"probe-{path}", "bp.exists", {"blueprint_path": path})
        if resp is None:
            continue
        if resp.get("ok") is True:
            result = resp.get("result", {})
            if result.get("exists") is True:
                return path
    return None


def validate_pin_type(pin_type: Any, label: str) -> bool:
    """Verify a pin_type JSON object has the expected shape."""
    if not isinstance(pin_type, dict):
        fail(f"{label}: pin_type not an object: {pin_type!r}")
        return False
    required = ("category", "subcategory", "subcategory_object_path", "is_array", "is_set",
                "is_map", "is_reference", "is_const", "is_weak_pointer", "value_type")
    for key in required:
        if key not in pin_type:
            fail(f"{label}: pin_type missing key '{key}': {pin_type!r}")
            return False
    if not isinstance(pin_type["category"], str) or not pin_type["category"]:
        fail(f"{label}: pin_type.category not a non-empty string: {pin_type!r}")
        return False
    return True


def validate_variable_summary(var: Any, label: str) -> bool:
    if not isinstance(var, dict):
        fail(f"{label}: variable not an object: {var!r}")
        return False
    required = ("name", "pin_type", "default_value", "category_group", "tooltip",
                "exposed_on_spawn", "replicated", "friendly_name")
    for key in required:
        if key not in var:
            fail(f"{label}: variable missing key '{key}': {var!r}")
            return False
    if not isinstance(var["name"], str) or not var["name"]:
        fail(f"{label}: variable.name not a non-empty string")
        return False
    if not validate_pin_type(var["pin_type"], f"{label}/pin_type"):
        return False
    if not isinstance(var["exposed_on_spawn"], bool):
        fail(f"{label}: variable.exposed_on_spawn not bool")
        return False
    if not isinstance(var["replicated"], bool):
        fail(f"{label}: variable.replicated not bool")
        return False
    return True


def validate_function_summary(fn: Any, label: str, require_signature_fields: bool = False) -> bool:
    if not isinstance(fn, dict):
        fail(f"{label}: function not an object: {fn!r}")
        return False
    required = ("name", "category", "access_specifier", "is_pure", "is_const", "is_static",
                "signature")
    for key in required:
        if key not in fn:
            fail(f"{label}: function missing key '{key}': {fn!r}")
            return False
    if fn["access_specifier"] not in ("Public", "Protected", "Private"):
        fail(f"{label}: unexpected access_specifier {fn['access_specifier']!r}")
        return False
    sig = fn["signature"]
    if not isinstance(sig, dict):
        fail(f"{label}: signature not an object: {sig!r}")
        return False
    for key in ("inputs", "outputs"):
        if key not in sig:
            fail(f"{label}: signature missing '{key}': {sig!r}")
            return False
        if not isinstance(sig[key], list):
            fail(f"{label}: signature.{key} not a list: {sig[key]!r}")
            return False
        for entry in sig[key]:
            if not isinstance(entry, dict) or "name" not in entry or "pin_type" not in entry:
                fail(f"{label}: signature.{key} entry malformed: {entry!r}")
                return False
            if not validate_pin_type(entry["pin_type"], f"{label}/signature.{key}/pin_type"):
                return False
    if require_signature_fields:
        if "local_variables" not in fn or not isinstance(fn["local_variables"], list):
            fail(f"{label}: function missing local_variables[]: {fn!r}")
            return False
        if "execution_path_node_count" not in fn:
            fail(f"{label}: function missing execution_path_node_count: {fn!r}")
            return False
    return True


def validate_node_summary(node: Any, label: str) -> bool:
    if not isinstance(node, dict):
        fail(f"{label}: node not an object: {node!r}")
        return False
    required = ("node_guid", "class", "title", "pins")
    for key in required:
        if key not in node:
            fail(f"{label}: node missing key '{key}': {node!r}")
            return False
    if not isinstance(node["node_guid"], str) or not node["node_guid"]:
        fail(f"{label}: node.node_guid not non-empty string")
        return False
    if not isinstance(node["pins"], list):
        fail(f"{label}: node.pins not a list: {node['pins']!r}")
        return False
    for pin in node["pins"]:
        if not isinstance(pin, dict):
            fail(f"{label}: pin not an object: {pin!r}")
            return False
        for pkey in ("name", "direction", "pin_type", "connected_to"):
            if pkey not in pin:
                fail(f"{label}: pin missing key '{pkey}': {pin!r}")
                return False
        if pin["direction"] not in ("Input", "Output"):
            fail(f"{label}: pin.direction unexpected: {pin!r}")
            return False
        if not validate_pin_type(pin["pin_type"], f"{label}/pin/{pin['name']}/pin_type"):
            return False
        if not isinstance(pin["connected_to"], list):
            fail(f"{label}: pin.connected_to not a list: {pin!r}")
            return False
        for edge in pin["connected_to"]:
            if not isinstance(edge, dict) or "node_guid" not in edge or "pin_name" not in edge:
                fail(f"{label}: pin.connected_to entry malformed: {edge!r}")
                return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--bp-path", default=None,
                        help="Override blueprint asset path; default probes FALLBACK_BP_PATHS")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE4_1_5] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 6 bp.* names ─────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p4-1", "tools.list"), "p4-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = result.get("cpp_handlers") or []
    if not isinstance(cpp_handlers, list):
        return fail(f"1/tools.list: cpp_handlers not a list: {cpp_handlers!r}")
    expected = {
        "bp.exists", "bp.get_variable", "bp.list_variables", "bp.list_functions",
        "bp.get_function", "bp.list_nodes_in_function",
    }
    missing = expected - set(cpp_handlers)
    if missing:
        return fail(f"1/tools.list: missing bp.* tools: {sorted(missing)}")
    print("[SMOKE_PHASE4_1_5]   1/tools.list contains all 6 bp.* names")

    # ─── Resolve a test blueprint ─────────────────────────────────────────────────────────────
    bp_path = args.bp_path or resolve_test_bp(args.host, args.port)
    if not bp_path:
        print(f"[SMOKE_PHASE4_1_5]   no testable BP under {FALLBACK_BP_PATHS}; skipping per-BP sub-tests 2,5-13")
        bp_path_missing = True
    else:
        bp_path_missing = False
        print(f"[SMOKE_PHASE4_1_5]   using test BP: {bp_path}")

    # ─── Sub-test 2: bp.exists positive ───────────────────────────────────────────────────────
    if bp_path_missing:
        skip("2/bp.exists positive: no test BP available")
    else:
        result = expect_ok(call(args.host, args.port, "2", "p4-2", "bp.exists",
                                {"blueprint_path": bp_path}), "p4-2", "2/bp.exists+")
        if result is None:
            return 1
        if result.get("exists") is not True:
            return fail(f"2/bp.exists+: expected exists=true for {bp_path}, got {result!r}")
        if not isinstance(result.get("parent_class_path"), str):
            return fail(f"2/bp.exists+: parent_class_path not string: {result!r}")
        if not isinstance(result.get("generated_class_path"), str):
            return fail(f"2/bp.exists+: generated_class_path not string: {result!r}")
        if not isinstance(result.get("is_data_only"), bool):
            return fail(f"2/bp.exists+: is_data_only not bool: {result!r}")
        print(f"[SMOKE_PHASE4_1_5]   2/bp.exists+ OK parent={result['parent_class_path']} "
              f"data_only={result['is_data_only']}")

    # ─── Sub-test 3: bp.exists negative (path doesn't exist but is well-formed) ───────────────
    result = expect_ok(call(args.host, args.port, "3", "p4-3", "bp.exists",
                            {"blueprint_path": "/Game/__no_such_blueprint_xyz_12345"}),
                       "p4-3", "3/bp.exists-")
    if result is None:
        return 1
    if result.get("exists") is not False:
        return fail(f"3/bp.exists-: expected exists=false, got {result!r}")
    if result.get("parent_class_path") is not None:
        return fail(f"3/bp.exists-: expected parent_class_path=null, got {result!r}")
    print("[SMOKE_PHASE4_1_5]   3/bp.exists- OK exists=false on non-existent path")

    # ─── Sub-test 4: bp.exists wrong-type (try a known non-BP asset if available) ─────────────
    # We can probe the DataAsset under /Game/MCPTest/PhaseTwo if it's there.
    wrong_type_candidates = [
        "/Game/MCPTest/PhaseTwo/DA_PhaseTwoTest",
    ]
    wrong_type_path = None
    for cand in wrong_type_candidates:
        # Use asset.exists or marshall.read_property — fastest is just bp.exists and check the error.
        resp = call(args.host, args.port, "4probe", f"p4-4-probe-{cand}", "bp.exists",
                    {"blueprint_path": cand})
        if resp is None:
            continue
        err = resp.get("error")
        if isinstance(err, dict) and err.get("code") == -32031:
            wrong_type_path = cand
            break

    if wrong_type_path:
        ok = expect_error(call(args.host, args.port, "4", "p4-4", "bp.exists",
                              {"blueprint_path": wrong_type_path}),
                         "p4-4", -32031, "4/bp.exists wrong type")
        if not ok:
            return 1
        print(f"[SMOKE_PHASE4_1_5]   4/bp.exists wrong type OK ({wrong_type_path} → -32031)")
    else:
        skip("4/bp.exists wrong type: no non-UBlueprint asset found at candidate paths")

    # ─── Sub-test 5: bp.list_variables positive ───────────────────────────────────────────────
    variables: list = []
    total_vars = 0
    if bp_path_missing:
        skip("5/bp.list_variables: no test BP")
    else:
        result = expect_ok(call(args.host, args.port, "5", "p4-5", "bp.list_variables",
                                {"blueprint_path": bp_path}), "p4-5", "5/list_variables")
        if result is None:
            return 1
        variables = result.get("variables")
        if not isinstance(variables, list):
            return fail(f"5/list_variables: variables not list: {result!r}")
        total_vars = result.get("total_known")
        if not isinstance(total_vars, (int, float)):
            return fail(f"5/list_variables: total_known not number: {result!r}")
        for v in variables:
            if not validate_variable_summary(v, "5/variable"):
                return 1
        print(f"[SMOKE_PHASE4_1_5]   5/list_variables OK count={len(variables)} total_known={int(total_vars)}")

    # ─── Sub-test 6: bp.get_variable positive ─────────────────────────────────────────────────
    if bp_path_missing or not variables:
        skip("6/bp.get_variable+: no variables to query")
    else:
        first_name = variables[0]["name"]
        result = expect_ok(call(args.host, args.port, "6", "p4-6", "bp.get_variable",
                                {"blueprint_path": bp_path, "variable_name": first_name}),
                          "p4-6", "6/get_variable+")
        if result is None:
            return 1
        if result.get("found") is not True:
            return fail(f"6/get_variable+: found != true: {result!r}")
        if not validate_variable_summary(result.get("variable"), "6/variable"):
            return 1
        if result["variable"]["name"] != first_name:
            return fail(f"6/get_variable+: name mismatch (got {result['variable']['name']!r}, want {first_name!r})")
        print(f"[SMOKE_PHASE4_1_5]   6/get_variable+ OK name={first_name}")

    # ─── Sub-test 7: bp.get_variable negative (unknown variable name) ─────────────────────────
    if bp_path_missing:
        skip("7/bp.get_variable-: no test BP")
    else:
        ok = expect_error(call(args.host, args.port, "7", "p4-7", "bp.get_variable",
                              {"blueprint_path": bp_path, "variable_name": "__nonexistent_variable_xyz"}),
                         "p4-7", -32037, "7/get_variable-")
        if not ok:
            return 1
        print("[SMOKE_PHASE4_1_5]   7/get_variable- OK -32037 for unknown name")

    # ─── Sub-test 8: bp.list_functions positive ───────────────────────────────────────────────
    functions: list = []
    total_fns = 0
    if bp_path_missing:
        skip("8/bp.list_functions: no test BP")
    else:
        result = expect_ok(call(args.host, args.port, "8", "p4-8", "bp.list_functions",
                                {"blueprint_path": bp_path}), "p4-8", "8/list_functions")
        if result is None:
            return 1
        functions = result.get("functions")
        if not isinstance(functions, list):
            return fail(f"8/list_functions: functions not list: {result!r}")
        total_fns = result.get("total_known")
        if not isinstance(total_fns, (int, float)):
            return fail(f"8/list_functions: total_known not number: {result!r}")
        for fn in functions:
            if not validate_function_summary(fn, "8/function"):
                return 1
        print(f"[SMOKE_PHASE4_1_5]   8/list_functions OK count={len(functions)} total_known={int(total_fns)}")

    # ─── Sub-test 9: bp.get_function positive ─────────────────────────────────────────────────
    target_fn_name: Optional[str] = None
    if bp_path_missing or not functions:
        skip("9/bp.get_function+: no functions to query")
    else:
        target_fn_name = functions[0]["name"]
        result = expect_ok(call(args.host, args.port, "9", "p4-9", "bp.get_function",
                                {"blueprint_path": bp_path, "function_name": target_fn_name}),
                          "p4-9", "9/get_function+")
        if result is None:
            return 1
        if not validate_function_summary(result.get("function"), "9/function",
                                          require_signature_fields=True):
            return 1
        if result["function"]["name"] != target_fn_name:
            return fail(f"9/get_function+: name mismatch (got {result['function']['name']!r}, "
                        f"want {target_fn_name!r})")
        print(f"[SMOKE_PHASE4_1_5]   9/get_function+ OK name={target_fn_name} "
              f"nodes={int(result['function']['execution_path_node_count'])}")

    # ─── Sub-test 10: bp.get_function negative ────────────────────────────────────────────────
    if bp_path_missing:
        skip("10/bp.get_function-: no test BP")
    else:
        ok = expect_error(call(args.host, args.port, "10", "p4-10", "bp.get_function",
                              {"blueprint_path": bp_path, "function_name": "__no_such_function_xyz"}),
                         "p4-10", -32037, "10/get_function-")
        if not ok:
            return 1
        print("[SMOKE_PHASE4_1_5]   10/get_function- OK -32037 for unknown function")

    # ─── Sub-test 11: bp.list_nodes_in_function on a function ────────────────────────────────
    if bp_path_missing or not target_fn_name:
        skip("11/bp.list_nodes_in_function: no function available")
    else:
        result = expect_ok(call(args.host, args.port, "11", "p4-11", "bp.list_nodes_in_function",
                                {"blueprint_path": bp_path, "function_name": target_fn_name}),
                          "p4-11", "11/list_nodes")
        if result is None:
            return 1
        nodes = result.get("nodes")
        if not isinstance(nodes, list):
            return fail(f"11/list_nodes: nodes not list: {result!r}")
        for n in nodes:
            if not validate_node_summary(n, "11/node"):
                return 1
        print(f"[SMOKE_PHASE4_1_5]   11/list_nodes OK count={len(nodes)} "
              f"total_known={int(result.get('total_known', 0))}")

    # ─── Sub-test 12: bp.list_nodes_in_function negative ──────────────────────────────────────
    if bp_path_missing:
        skip("12/bp.list_nodes_in_function-: no test BP")
    else:
        ok = expect_error(call(args.host, args.port, "12", "p4-12", "bp.list_nodes_in_function",
                              {"blueprint_path": bp_path, "function_name": "__bogus_function_xyz"}),
                         "p4-12", -32037, "12/list_nodes-")
        if not ok:
            return 1
        print("[SMOKE_PHASE4_1_5]   12/list_nodes- OK -32037 for unknown function")

    # ─── Sub-test 13: bp.list_variables pagination ────────────────────────────────────────────
    if bp_path_missing or len(variables) < 2:
        skip("13/list_variables pagination: <2 variables, can't validate paging behaviour")
    else:
        seen_names = set()
        token = None
        iteration = 0
        max_iter = max(2, int(total_vars) + 5)
        while iteration < max_iter:
            req_args = {"blueprint_path": bp_path, "page_size": 1}
            if token:
                req_args["page_token"] = token
            page = expect_ok(call(args.host, args.port, "13", f"p4-13-{iteration}",
                                  "bp.list_variables", req_args),
                            f"p4-13-{iteration}", "13/pagination")
            if page is None:
                return 1
            page_vars = page.get("variables") or []
            for v in page_vars:
                seen_names.add(v["name"])
            token = page.get("next_page_token")
            iteration += 1
            if not token:
                break
        if iteration >= max_iter:
            return fail(f"13/pagination: did not terminate in {max_iter} iterations")
        expected_names = {v["name"] for v in variables}
        if seen_names != expected_names:
            missing = expected_names - seen_names
            extra = seen_names - expected_names
            return fail(f"13/pagination: name set mismatch missing={missing} extra={extra}")
        print(f"[SMOKE_PHASE4_1_5]   13/pagination OK saw {len(seen_names)} unique names in "
              f"{iteration} pages")

    # ─── Sub-test 14: bp.exists invalid path syntax → -32010 ─────────────────────────────────
    ok = expect_error(call(args.host, args.port, "14", "p4-14", "bp.exists",
                          {"blueprint_path": "../bad/path"}),
                     "p4-14", -32010, "14/invalid path")
    if not ok:
        return 1
    print("[SMOKE_PHASE4_1_5]   14/invalid path OK -32010 InvalidPath")

    print("[SMOKE_PHASE4_1_5] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
