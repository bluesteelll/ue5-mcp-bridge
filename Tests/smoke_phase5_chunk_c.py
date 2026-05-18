#!/usr/bin/env python3
"""Phase 5 Chunk C smoke — UMG + Niagara + Physics traces (5 tools).

Verifies (against a live editor on port 30020 — PIE may be on or off; sub-tests adapt):

  Discovery (1):
    1. tools.list contains all 5 new tools:
       umg.list_widgets, umg.get_widget_property,
       niagara.list_parameters,
       physics.line_trace, physics.sweep_capsule.

  UMG (2-4):
    2. umg.list_widgets on a WBP: probes /Game/MCPTest/Phase5/WBP_Test first; falls back to the
       first /Game UWidgetBlueprint via asset.search_by_class. SKIPS entire UMG section if no WBP
       found in the project. Verifies widgets[] schema (name/class/parent/is_variable per entry,
       at least one widget with parent=null = root).
    3. umg.get_widget_property on a found widget — reads ``Visibility`` property (every UWidget has
       it). Validates {value, type} schema. SKIP if probe missed.
    4. umg.get_widget_property with bogus widget_name → -32039 WidgetNotFound; bogus property_path
       → -32005 PropertyNotFound. SKIPs negative cases gracefully if no WBP available.

  Niagara (5-6):
    5. niagara.list_parameters probes /Game/MCPTest/Phase5/NS_Test first; falls back to the first
       /Game UNiagaraSystem via asset.search_by_class. SKIPS if no Niagara System exists in /Game.
       Verifies user_params/system_params/emitter_params arrays with name+type per entry.
    6. Negative: niagara.list_parameters with bogus path → -32004 ObjectNotFound. With a wrong-class
       asset (a UWidgetBlueprint, when one was found in sub-test 2) → -32011 WrongClass. SKIP wrong-
       class if no WBP found in step 2.

  Physics (7-10):
    7. physics.line_trace from {0,0,1000} → {0,0,-1000} with channel='Visibility'. Validates
       {world, world_kind, hit:bool, hits:[]} schema. ``hit`` may be true or false (depends on
       whether anything is below the origin in the loaded level).
    8. physics.sweep_capsule with radius=50, half_height=50, similar geometry. Same schema check.
    9. physics.line_trace with channel='NotAChannel' → -32041 InvalidCollisionChannel; message
       contains 'Visibility' substring (the default-recovery hint).
   10. physics.sweep_capsule with radius=0 → -32602 InvalidParams (must be >=1).

Prints ``[SMOKE_PHASE5_CHUNK_C] PASS`` on success or ``[SMOKE_PHASE5_CHUNK_C] FAIL ...`` on first
mismatch. Sub-tests dependent on missing assets emit SKIP lines but still PASS overall.

Usage:
  python smoke_phase5_chunk_c.py [--host HOST] [--port PORT] [--wbp-path /Game/...] [--ns-path /Game/...]
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

# Candidate WBP paths probed in order. The plan-mandated test asset is WBP_Test under
# /Game/MCPTest/Phase5; we fall back to /Game search via asset.search_by_class if absent.
FALLBACK_WBP_PATHS = [
    "/Game/MCPTest/Phase5/WBP_Test",
]

# Candidate Niagara System paths probed first. /Game/MCPTest/Phase5/NS_Test is plan-mandated.
FALLBACK_NS_PATHS = [
    "/Game/MCPTest/Phase5/NS_Test",
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
    print(f"[SMOKE_PHASE5_CHUNK_C] FAIL reason={reason}")
    return 1


def skip(reason: str) -> None:
    print(f"[SMOKE_PHASE5_CHUNK_C]   SKIP {reason}")


def info(message: str) -> None:
    print(f"[SMOKE_PHASE5_CHUNK_C]   {message}")


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


def probe_widget_blueprint(host: str, port: int, candidates: List[str]) -> Optional[str]:
    """Return the first existing UWidgetBlueprint path among `candidates` + /Game search fallback."""
    # Direct path probe — fastest path; works when the operator built the plan-mandated test asset.
    for path in candidates:
        resp = call(host, port, "probe", f"probe-wbp-{path}", "umg.list_widgets",
                    {"widget_bp_path": path})
        if resp is not None and resp.get("ok") is True:
            return path
    # Fallback — scan /Game for ANY UWidgetBlueprint via asset.search_by_class.
    resp = call(host, port, "probe-search", "probe-wbp-search", "asset.search_by_class",
                {"class_path": "/Script/UMGEditor.WidgetBlueprint",
                 "package_paths": ["/Game"], "recursive_paths": True,
                 "page_size": 1})
    if resp is None or resp.get("ok") is not True:
        return None
    assets = resp.get("result", {}).get("assets", [])
    if not isinstance(assets, list) or not assets:
        return None
    first = assets[0]
    if isinstance(first, dict):
        path = first.get("object_path") or first.get("package_name") or first.get("path")
        if isinstance(path, str) and path.startswith("/"):
            # Strip the .Name suffix if present (umg.list_widgets accepts both)
            if "." in path.rsplit("/", 1)[-1]:
                path = path.rsplit(".", 1)[0]
            return path
    return None


def probe_niagara_system(host: str, port: int, candidates: List[str]) -> Optional[str]:
    """Return the first existing UNiagaraSystem path among `candidates` + /Game search fallback."""
    for path in candidates:
        resp = call(host, port, "probe", f"probe-ns-{path}", "niagara.list_parameters",
                    {"niagara_system_path": path})
        if resp is not None and resp.get("ok") is True:
            return path
    resp = call(host, port, "probe-search", "probe-ns-search", "asset.search_by_class",
                {"class_path": "/Script/Niagara.NiagaraSystem",
                 "package_paths": ["/Game"], "recursive_paths": True,
                 "page_size": 1})
    if resp is None or resp.get("ok") is not True:
        return None
    assets = resp.get("result", {}).get("assets", [])
    if not isinstance(assets, list) or not assets:
        return None
    first = assets[0]
    if isinstance(first, dict):
        path = first.get("object_path") or first.get("package_name") or first.get("path")
        if isinstance(path, str) and path.startswith("/"):
            if "." in path.rsplit("/", 1)[-1]:
                path = path.rsplit(".", 1)[0]
            return path
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--wbp-path", default=None,
                        help="Override WBP asset path; default probes FALLBACK_WBP_PATHS + /Game search")
    parser.add_argument("--ns-path", default=None,
                        help="Override Niagara System path; default probes FALLBACK_NS_PATHS + /Game search")
    args = parser.parse_args()

    print(f"[SMOKE_PHASE5_CHUNK_C] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 5 Chunk C tools ────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1", "p5-cc-1", "tools.list"),
                       "p5-cc-1", "1/tools.list")
    if result is None:
        return 1
    cpp_handlers = set(result.get("cpp_handlers") or [])
    expected_cpp = {
        "umg.list_widgets",
        "umg.get_widget_property",
        "niagara.list_parameters",
        "physics.line_trace",
        "physics.sweep_capsule",
    }
    missing = expected_cpp - cpp_handlers
    if missing:
        return fail(f"1/tools.list: missing handlers: {sorted(missing)}")
    info("1/tools.list contains all 5 Chunk C tools")

    # ─── Sub-test 2: umg.list_widgets ───────────────────────────────────────────────────────────
    wbp_path: Optional[str] = args.wbp_path or probe_widget_blueprint(
        args.host, args.port, FALLBACK_WBP_PATHS)
    if wbp_path is None:
        skip("2/umg.list_widgets: no UWidgetBlueprint found in /Game (operator can build "
             "/Game/MCPTest/Phase5/WBP_Test or pass --wbp-path); UMG sub-tests 2-4 SKIPPED")
        # Defer remaining UMG tests as SKIPs but continue.
        wbp_widget_name: Optional[str] = None
    else:
        result = expect_ok(
            call(args.host, args.port, "2", "p5-cc-2", "umg.list_widgets",
                 {"widget_bp_path": wbp_path}),
            "p5-cc-2", "2/umg.list_widgets")
        if result is None:
            return 1
        widgets = result.get("widgets")
        if not isinstance(widgets, list):
            return fail(f"2/list_widgets: widgets not list {result!r}")
        if not widgets:
            return fail(f"2/list_widgets: widgets empty (WBP {wbp_path} has no widgets in tree)")
        # Verify schema of first entry.
        first = widgets[0]
        for field in ("name", "class", "is_variable"):
            if field not in first:
                return fail(f"2/list_widgets: first widget missing field {field!r}: {first!r}")
        if not isinstance(first.get("name"), str) or not first["name"]:
            return fail(f"2/list_widgets: first.name not non-empty string {first!r}")
        if not isinstance(first.get("class"), str) or not first["class"].startswith("/Script/"):
            return fail(f"2/list_widgets: first.class doesn't look like a UClass path {first!r}")
        if not isinstance(first.get("is_variable"), bool):
            return fail(f"2/list_widgets: first.is_variable not bool {first!r}")
        # parent: explicit null OR string
        if first.get("parent") is not None and not isinstance(first.get("parent"), str):
            return fail(f"2/list_widgets: first.parent neither null nor string {first!r}")
        # At least one widget should have parent=null (the root).
        has_root = any(w.get("parent") is None for w in widgets if isinstance(w, dict))
        if not has_root:
            return fail(f"2/list_widgets: no root widget (none with parent=null) in {len(widgets)} entries")
        wbp_widget_name = str(first["name"])
        info(f"2/umg.list_widgets({wbp_path}) OK ({len(widgets)} widgets, root='{wbp_widget_name}')")

    # ─── Sub-test 3: umg.get_widget_property ────────────────────────────────────────────────────
    if wbp_path is None or wbp_widget_name is None:
        skip("3/umg.get_widget_property: SKIPPED (no WBP from sub-test 2)")
    else:
        # Read the Visibility enum that every UWidget has.
        result = expect_ok(
            call(args.host, args.port, "3", "p5-cc-3", "umg.get_widget_property",
                 {"widget_bp_path": wbp_path, "widget_name": wbp_widget_name,
                  "property_path": "Visibility"}),
            "p5-cc-3", "3/umg.get_widget_property")
        if result is None:
            return 1
        if "value" not in result:
            return fail(f"3/get_widget_property: missing 'value' field {result!r}")
        if not isinstance(result.get("type"), str) or not result["type"]:
            return fail(f"3/get_widget_property: type not non-empty string {result!r}")
        info(f"3/umg.get_widget_property OK (type='{result['type']}', value={result['value']!r})")

    # ─── Sub-test 4: umg.get_widget_property negative cases ─────────────────────────────────────
    if wbp_path is None:
        skip("4/umg.get_widget_property negative cases: SKIPPED (no WBP)")
    else:
        # Bogus widget_name → -32039 WidgetNotFound
        resp = call(args.host, args.port, "4a", "p5-cc-4a", "umg.get_widget_property",
                    {"widget_bp_path": wbp_path, "widget_name": "__no_such_widget_xyz__",
                     "property_path": "Visibility"})
        err = expect_error(resp, "p5-cc-4a", -32039, "4a/get_widget_property-bogus-widget")
        if err is None:
            return 1
        # Bogus property_path → -32005 PropertyNotFound
        if wbp_widget_name is not None:
            resp = call(args.host, args.port, "4b", "p5-cc-4b", "umg.get_widget_property",
                        {"widget_bp_path": wbp_path, "widget_name": wbp_widget_name,
                         "property_path": "__no_such_property_xyz__"})
            err = expect_error(resp, "p5-cc-4b", -32005, "4b/get_widget_property-bogus-prop")
            if err is None:
                return 1
        info("4/umg.get_widget_property negative cases OK (-32039 WidgetNotFound, -32005 PropertyNotFound)")

    # ─── Sub-test 5: niagara.list_parameters ────────────────────────────────────────────────────
    ns_path: Optional[str] = args.ns_path or probe_niagara_system(
        args.host, args.port, FALLBACK_NS_PATHS)
    if ns_path is None:
        skip("5/niagara.list_parameters: no UNiagaraSystem found in /Game (operator can build "
             "/Game/MCPTest/Phase5/NS_Test or pass --ns-path); Niagara sub-tests SKIPPED")
    else:
        result = expect_ok(
            call(args.host, args.port, "5", "p5-cc-5", "niagara.list_parameters",
                 {"niagara_system_path": ns_path}),
            "p5-cc-5", "5/niagara.list_parameters")
        if result is None:
            return 1
        user_params = result.get("user_params")
        system_params = result.get("system_params")
        emitter_params = result.get("emitter_params")
        if not isinstance(user_params, list):
            return fail(f"5/list_parameters: user_params not list {result!r}")
        if not isinstance(system_params, list):
            return fail(f"5/list_parameters: system_params not list {result!r}")
        if not isinstance(emitter_params, list):
            return fail(f"5/list_parameters: emitter_params not list {result!r}")
        # If user_params exists, validate schema of first entry.
        for entry in user_params:
            if not isinstance(entry, dict):
                return fail(f"5/list_parameters: user_param entry not dict {entry!r}")
            if not isinstance(entry.get("name"), str):
                return fail(f"5/list_parameters: user_param missing name {entry!r}")
            if not isinstance(entry.get("type"), str):
                return fail(f"5/list_parameters: user_param missing type {entry!r}")
            if "default" not in entry:
                return fail(f"5/list_parameters: user_param missing default {entry!r}")
        for entry in emitter_params:
            if not isinstance(entry, dict):
                return fail(f"5/list_parameters: emitter_param entry not dict {entry!r}")
            for field in ("emitter", "name", "type"):
                if field not in entry or not isinstance(entry[field], str):
                    return fail(f"5/list_parameters: emitter_param missing {field!r}: {entry!r}")
        info(f"5/niagara.list_parameters({ns_path}) OK "
             f"(user={len(user_params)}, system={len(system_params)}, emitter={len(emitter_params)})")

    # ─── Sub-test 6: niagara.list_parameters negative cases ─────────────────────────────────────
    # Bogus path → -32004 ObjectNotFound
    resp = call(args.host, args.port, "6a", "p5-cc-6a", "niagara.list_parameters",
                {"niagara_system_path": "/Game/bogus_path_xyz123"})
    err = expect_error(resp, "p5-cc-6a", -32004, "6a/niagara-bogus-path")
    if err is None:
        return 1
    # Wrong class — point at the WBP we found earlier
    if wbp_path is not None:
        resp = call(args.host, args.port, "6b", "p5-cc-6b", "niagara.list_parameters",
                    {"niagara_system_path": wbp_path})
        err = expect_error(resp, "p5-cc-6b", -32011, "6b/niagara-wrong-class")
        if err is None:
            return 1
        info("6/niagara.list_parameters negative cases OK (-32004 + -32011)")
    else:
        info("6/niagara.list_parameters negative cases OK (-32004 only; wrong-class skipped — no WBP)")

    # ─── Sub-test 7: physics.line_trace ─────────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "7", "p5-cc-7", "physics.line_trace",
             {"start": [0.0, 0.0, 1000.0], "end": [0.0, 0.0, -1000.0],
              "channel": "Visibility", "multi_hit": False}),
        "p5-cc-7", "7/physics.line_trace")
    if result is None:
        return 1
    for field in ("world", "world_kind", "hit", "hits", "ignored_count"):
        if field not in result:
            return fail(f"7/line_trace: missing field {field!r}: {result!r}")
    if not isinstance(result.get("world"), str):
        return fail(f"7/line_trace: world not string {result!r}")
    if result.get("world_kind") not in ("Editor", "PIE", "Game", "EditorPreview", "GamePreview", "Other", "None"):
        return fail(f"7/line_trace: world_kind unexpected {result!r}")
    if not isinstance(result.get("hit"), bool):
        return fail(f"7/line_trace: hit not bool {result!r}")
    if not isinstance(result.get("hits"), list):
        return fail(f"7/line_trace: hits not list {result!r}")
    info(f"7/physics.line_trace OK (world={result['world']!r}, world_kind={result['world_kind']!r}, "
         f"hit={result['hit']}, hits={len(result['hits'])})")

    # If we got a hit, validate per-hit schema.
    if result["hits"]:
        h0 = result["hits"][0]
        for field in ("location", "normal", "distance", "blocking"):
            if field not in h0:
                return fail(f"7/line_trace: first hit missing {field!r}: {h0!r}")
        if not isinstance(h0["location"], list) or len(h0["location"]) != 3:
            return fail(f"7/line_trace: first hit location not [x,y,z] {h0!r}")
        if not isinstance(h0["normal"], list) or len(h0["normal"]) != 3:
            return fail(f"7/line_trace: first hit normal not [x,y,z] {h0!r}")
        if not isinstance(h0["distance"], (int, float)):
            return fail(f"7/line_trace: first hit distance not number {h0!r}")
        if not isinstance(h0["blocking"], bool):
            return fail(f"7/line_trace: first hit blocking not bool {h0!r}")
        info(f"   first hit: actor_path={h0.get('actor_path')!r}, distance={h0['distance']:.2f}")

    # ─── Sub-test 8: physics.sweep_capsule ──────────────────────────────────────────────────────
    result = expect_ok(
        call(args.host, args.port, "8", "p5-cc-8", "physics.sweep_capsule",
             {"start": [0.0, 0.0, 1000.0], "end": [0.0, 0.0, -1000.0],
              "radius": 50.0, "half_height": 50.0, "rotation": [0.0, 0.0, 0.0],
              "channel": "Visibility"}),
        "p5-cc-8", "8/physics.sweep_capsule")
    if result is None:
        return 1
    for field in ("world", "world_kind", "hit", "hits", "ignored_count"):
        if field not in result:
            return fail(f"8/sweep_capsule: missing field {field!r}: {result!r}")
    if not isinstance(result.get("hit"), bool):
        return fail(f"8/sweep_capsule: hit not bool {result!r}")
    if not isinstance(result.get("hits"), list):
        return fail(f"8/sweep_capsule: hits not list {result!r}")
    info(f"8/physics.sweep_capsule OK (radius=50, half_height=50, hit={result['hit']}, "
         f"hits={len(result['hits'])})")

    # ─── Sub-test 9: physics.line_trace bad channel ─────────────────────────────────────────────
    resp = call(args.host, args.port, "9", "p5-cc-9", "physics.line_trace",
                {"start": [0.0, 0.0, 1000.0], "end": [0.0, 0.0, -1000.0],
                 "channel": "NotAChannelXyz"})
    err = expect_error(resp, "p5-cc-9", -32041, "9/line_trace-bad-channel")
    if err is None:
        return 1
    msg = str(err.get("message", ""))
    if "Visibility" not in msg:
        return fail(f"9/line_trace-bad-channel: expected 'Visibility' substring in message {msg!r}")
    info("9/physics.line_trace bad channel → -32041 InvalidCollisionChannel (accepted-list hint present)")

    # ─── Sub-test 10: physics.sweep_capsule bad radius ──────────────────────────────────────────
    resp = call(args.host, args.port, "10", "p5-cc-10", "physics.sweep_capsule",
                {"start": [0.0, 0.0, 1000.0], "end": [0.0, 0.0, -1000.0],
                 "radius": 0.0, "half_height": 50.0})
    err = expect_error(resp, "p5-cc-10", -32602, "10/sweep-bad-radius")
    if err is None:
        return 1
    info("10/physics.sweep_capsule radius=0 → -32602 InvalidParams")

    # ─── Sub-test 11: missing required args ─────────────────────────────────────────────────────
    # umg.list_widgets without widget_bp_path → -32602
    resp = call(args.host, args.port, "11a", "p5-cc-11a", "umg.list_widgets", {})
    if expect_error(resp, "p5-cc-11a", -32602, "11a/umg-no-path") is None:
        return 1
    # physics.line_trace without start → -32602
    resp = call(args.host, args.port, "11b", "p5-cc-11b", "physics.line_trace",
                {"end": [0, 0, 0]})
    if expect_error(resp, "p5-cc-11b", -32602, "11b/line_trace-no-start") is None:
        return 1
    info("11/missing-required-args negative cases OK (umg.list_widgets, physics.line_trace)")

    print("[SMOKE_PHASE5_CHUNK_C] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
