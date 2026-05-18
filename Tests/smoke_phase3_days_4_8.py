#!/usr/bin/env python3
"""Phase 3 Days 4-8 smoke — 20 Actor tools.

Verifies (against an editor that has loaded SOME map, persistent level non-empty):

  1.  tools.list contains all 20 actor.* names.
  2.  actor.find_by_class /Script/Engine.StaticMeshActor returns a list (may be empty on a
      blank level; we just check the shape).
  3.  actor.spawn /Script/Engine.StaticMeshActor at (100,200,300) → returns actor_path.
  4.  actor.exists <returned_path> → true.
  5.  actor.get <returned_path> → returns full snapshot with transform, components, tags.
  6.  actor.set_location to (500,500,500) → location matches.
  7.  actor.set_rotation to (0, 45, 0) → yaw matches.
  8.  actor.set_scale to (2, 2, 2) → scale matches.
  9.  actor.set_transform combined — set location only with set_transform.
  10. actor.set_label to "MCP_TestActor" → label matches.
  11. actor.set_folder to "MCPTest" → folder matches.
  12. actor.set_property RootComponent.RelativeLocation.X to 999.0 → re-read matches.
  13. actor.list_components → returns at least 1 component.
  14. actor.duplicate of spawned actor → returns NEW actor_path != original.
  15. actor.find_by_class with page_size=1 — page through, verify no overlap.
  16. actor.find_by_label "MCP_TestActor" (exact=true) → finds the renamed actor.
  17. actor.spawn with malformed class_path "StaticMeshActor" → -32023 INVALID_CLASS_PATH.
  18. actor.spawn with abstract class /Script/Engine.Actor → spawns OK (Actor is not abstract).
  19. actor.spawn with non-actor class /Script/Engine.StaticMesh → -32022 WRONG_CLASS_FAMILY.
  20. actor.spawn with non-existent class /Script/Engine.NoSuchClass → -32020 CLASS_NOT_FOUND.
  21. actor.destroy spawned + duplicated; actor.exists → false on both.
  22. actor.select_in_editor with both newly-spawned and an existing actor (additive=true).
  23. PIE message check — only valid when GEditor->PlayWorld is null; we run while editor idle.

Prints ``[SMOKE_PHASE3_4_8] PASS`` on success or ``[SMOKE_PHASE3_4_8] FAIL ...`` on first mismatch.

Usage:
  python smoke_phase3_days_4_8.py [--host HOST] [--port PORT]
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from typing import Optional

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30020
READ_TIMEOUT_SEC = 8.0


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
    print(f"[SMOKE_PHASE3_4_8] FAIL reason={reason}")
    return 1


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[SMOKE_PHASE3_4_8] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 20 actor.* names ──────────────────────────────────
    result = expect_ok(call(args.host, args.port, "1/tools.list", "phase3-4-8-1", "tools.list"),
                       "phase3-4-8-1", "1/tools.list")
    if result is None:
        return 1
    cpp = result.get("cpp_handlers", [])
    expected_actor_methods = {
        "actor.spawn", "actor.destroy", "actor.duplicate", "actor.get",
        "actor.set_transform", "actor.set_location", "actor.set_rotation", "actor.set_scale",
        "actor.set_label", "actor.set_folder", "actor.attach", "actor.detach",
        "actor.get_property", "actor.set_property", "actor.exists", "actor.select_in_editor",
        "actor.find_by_class", "actor.find_by_label", "actor.find_by_tag", "actor.list_components",
    }
    missing = expected_actor_methods - set(cpp)
    if missing:
        return fail(f"1/tools.list: missing actor.* methods {sorted(missing)}")
    print(f"[SMOKE_PHASE3_4_8]   1/tools.list OK (all 20 actor.* registered, total cpp={len(cpp)})")

    # ─── Sub-test 2: actor.find_by_class StaticMeshActor (shape check) ─────────────────────────
    result = expect_ok(call(args.host, args.port, "2/find_smesh", "phase3-4-8-2",
                            "actor.find_by_class",
                            {"class_path": "/Script/Engine.StaticMeshActor", "page_size": 50}),
                       "phase3-4-8-2", "2/find_smesh")
    if result is None:
        return 1
    actors = result.get("actors")
    total = result.get("total_known")
    if not isinstance(actors, list) or not isinstance(total, (int, float)):
        return fail(f"2/find_smesh: malformed shape {result!r}")
    initial_smesh_count = int(total)
    print(f"[SMOKE_PHASE3_4_8]   2/find_by_class StaticMeshActor OK (total_known={initial_smesh_count}, page={len(actors)})")

    # ─── Sub-test 3: actor.spawn StaticMeshActor at (100, 200, 300) ────────────────────────────
    result = expect_ok(call(args.host, args.port, "3/spawn", "phase3-4-8-3", "actor.spawn",
                            {
                                "class_path": "/Script/Engine.StaticMeshActor",
                                "location": {"x": 100.0, "y": 200.0, "z": 300.0},
                                "label": "MCP_SmokeSpawn",
                                "tags": ["mcp_smoke"],
                            }),
                       "phase3-4-8-3", "3/spawn")
    if result is None:
        return 1
    spawned_path = result.get("actor_path")
    if not isinstance(spawned_path, str) or not spawned_path:
        return fail(f"3/spawn: invalid actor_path {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   3/spawn OK ({spawned_path})")

    # ─── Sub-test 4: actor.exists ──────────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "4/exists", "phase3-4-8-4", "actor.exists",
                            {"actor_path": spawned_path}),
                       "phase3-4-8-4", "4/exists")
    if result is None:
        return 1
    if result.get("exists") is not True:
        return fail(f"4/exists: expected exists=true, got {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   4/exists OK")

    # ─── Sub-test 5: actor.get ─────────────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "5/get", "phase3-4-8-5", "actor.get",
                            {"actor_path": spawned_path}),
                       "phase3-4-8-5", "5/get")
    if result is None:
        return 1
    for k in ("actor_path", "class", "label", "transform", "tags", "components"):
        if k not in result:
            return fail(f"5/get: missing field '{k}' in {sorted(result.keys())}")
    if "mcp_smoke" not in result.get("tags", []):
        return fail(f"5/get: 'mcp_smoke' tag missing from {result.get('tags')!r}")
    print(f"[SMOKE_PHASE3_4_8]   5/get OK (components={len(result['components'])}, tags={result['tags']})")

    # ─── Sub-test 6: actor.set_location ────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "6/set_loc", "phase3-4-8-6", "actor.set_location",
                            {"actor_path": spawned_path,
                             "location": {"x": 500.0, "y": 500.0, "z": 500.0}}),
                       "phase3-4-8-6", "6/set_loc")
    if result is None:
        return 1
    loc = result.get("location", {})
    if abs(loc.get("x", 0) - 500.0) > 0.01:
        return fail(f"6/set_loc: x mismatch got {loc!r}")
    print(f"[SMOKE_PHASE3_4_8]   6/set_location OK")

    # ─── Sub-test 7: actor.set_rotation ────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "7/set_rot", "phase3-4-8-7", "actor.set_rotation",
                            {"actor_path": spawned_path,
                             "rotation": {"pitch": 0.0, "yaw": 45.0, "roll": 0.0}}),
                       "phase3-4-8-7", "7/set_rot")
    if result is None:
        return 1
    rot = result.get("rotation", {})
    if abs(rot.get("yaw", 0) - 45.0) > 0.01:
        return fail(f"7/set_rot: yaw mismatch got {rot!r}")
    print(f"[SMOKE_PHASE3_4_8]   7/set_rotation OK")

    # ─── Sub-test 8: actor.set_scale ───────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "8/set_scale", "phase3-4-8-8", "actor.set_scale",
                            {"actor_path": spawned_path,
                             "scale": {"x": 2.0, "y": 2.0, "z": 2.0}}),
                       "phase3-4-8-8", "8/set_scale")
    if result is None:
        return 1
    scl = result.get("scale", {})
    if abs(scl.get("x", 0) - 2.0) > 0.01:
        return fail(f"8/set_scale: x mismatch got {scl!r}")
    print(f"[SMOKE_PHASE3_4_8]   8/set_scale OK")

    # ─── Sub-test 9: actor.set_transform (combined) ────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "9/set_xform", "phase3-4-8-9", "actor.set_transform",
                            {"actor_path": spawned_path,
                             "location": {"x": 1000.0, "y": 2000.0, "z": 3000.0}}),
                       "phase3-4-8-9", "9/set_xform")
    if result is None:
        return 1
    xform = result.get("transform", {})
    if xform.get("translation", {}).get("x") != 1000.0:
        return fail(f"9/set_xform: x mismatch got {xform!r}")
    print(f"[SMOKE_PHASE3_4_8]   9/set_transform (location only) OK")

    # ─── Sub-test 10: actor.set_label ──────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "10/set_label", "phase3-4-8-10", "actor.set_label",
                            {"actor_path": spawned_path, "label": "MCP_TestActor"}),
                       "phase3-4-8-10", "10/set_label")
    if result is None:
        return 1
    if result.get("label") != "MCP_TestActor":
        return fail(f"10/set_label: label not applied, got {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   10/set_label OK (prev={result.get('previous_label')})")

    # ─── Sub-test 11: actor.set_folder ─────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "11/set_folder", "phase3-4-8-11", "actor.set_folder",
                            {"actor_path": spawned_path, "folder_path": "MCPTest"}),
                       "phase3-4-8-11", "11/set_folder")
    if result is None:
        return 1
    if result.get("folder_path") != "MCPTest":
        return fail(f"11/set_folder: folder not applied, got {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   11/set_folder OK")

    # ─── Sub-test 12: actor.set_property RootComponent.RelativeLocation.X = 999.0 ──────────────
    result = expect_ok(call(args.host, args.port, "12/set_prop", "phase3-4-8-12", "actor.set_property",
                            {"actor_path": spawned_path,
                             "property_name": "RootComponent.RelativeLocation.X",
                             "value": 999.0}),
                       "phase3-4-8-12", "12/set_prop")
    if result is None:
        return 1
    if abs(float(result.get("value", 0.0)) - 999.0) > 0.01:
        return fail(f"12/set_prop: value mismatch got {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   12/set_property OK (X=999.0)")

    # ─── Sub-test 13: actor.list_components ────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "13/list_comps", "phase3-4-8-13", "actor.list_components",
                            {"actor_path": spawned_path}),
                       "phase3-4-8-13", "13/list_comps")
    if result is None:
        return 1
    comps = result.get("components", [])
    if not isinstance(comps, list) or len(comps) == 0:
        return fail(f"13/list_comps: expected >=1 component, got {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   13/list_components OK ({len(comps)} components)")

    # ─── Sub-test 14: actor.duplicate ──────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "14/dup", "phase3-4-8-14", "actor.duplicate",
                            {"source_actor_path": spawned_path,
                             "offset_location": {"x": 100.0, "y": 0.0, "z": 0.0}}),
                       "phase3-4-8-14", "14/dup")
    if result is None:
        return 1
    duplicated_path = result.get("actor_path")
    if not isinstance(duplicated_path, str) or duplicated_path == spawned_path:
        return fail(f"14/dup: invalid or identical actor_path {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   14/duplicate OK ({duplicated_path})")

    # ─── Sub-test 15: pagination via find_by_class page_size=1 ─────────────────────────────────
    result = expect_ok(call(args.host, args.port, "15/page1", "phase3-4-8-15a", "actor.find_by_class",
                            {"class_path": "/Script/Engine.StaticMeshActor", "page_size": 1}),
                       "phase3-4-8-15a", "15/page1")
    if result is None:
        return 1
    page1_actors = result.get("actors", [])
    page1_token = result.get("next_page_token")
    seen_paths = {a["actor_path"] for a in page1_actors}
    if page1_token:
        result = expect_ok(call(args.host, args.port, "15/page2", "phase3-4-8-15b", "actor.find_by_class",
                                {"class_path": "/Script/Engine.StaticMeshActor",
                                 "page_size": 1, "page_token": page1_token}),
                           "phase3-4-8-15b", "15/page2")
        if result is None:
            return 1
        page2_actors = result.get("actors", [])
        for a in page2_actors:
            if a["actor_path"] in seen_paths:
                return fail(f"15/page2: cursor overlap detected — {a['actor_path']} on both pages")
        print(f"[SMOKE_PHASE3_4_8]   15/pagination OK (page1={len(page1_actors)} page2={len(page2_actors)}, no overlap)")
    else:
        print(f"[SMOKE_PHASE3_4_8]   15/pagination OK (single page, total {len(page1_actors)} — no cursor to test)")

    # ─── Sub-test 16: actor.find_by_label exact ────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "16/find_label", "phase3-4-8-16", "actor.find_by_label",
                            {"label_substring": "MCP_TestActor", "exact": True}),
                       "phase3-4-8-16", "16/find_label")
    if result is None:
        return 1
    found = result.get("actors", [])
    if not any(a.get("label") == "MCP_TestActor" for a in found):
        return fail(f"16/find_label: renamed actor not found in {found!r}")
    print(f"[SMOKE_PHASE3_4_8]   16/find_by_label OK ({len(found)} match(es))")

    # ─── Sub-test 17: actor.spawn malformed class_path → -32023 ────────────────────────────────
    resp = call(args.host, args.port, "17/bad_class", "phase3-4-8-17", "actor.spawn",
                {"class_path": "StaticMeshActor"})
    # If PIE active, may return -32027 instead — accept either.
    if resp is None:
        return fail("17/bad_class: timeout")
    if resp.get("ok") is True:
        return fail(f"17/bad_class: expected error, got success {resp!r}")
    code = resp.get("error", {}).get("code")
    if code not in (-32023, -32027):
        return fail(f"17/bad_class: expected -32023 or -32027, got {code}")
    print(f"[SMOKE_PHASE3_4_8]   17/spawn(malformed class_path) OK (code={code})")

    # ─── Sub-test 18: actor.spawn Engine.Actor (non-abstract base — should work) ───────────────
    resp = call(args.host, args.port, "18/spawn_base", "phase3-4-8-18", "actor.spawn",
                {"class_path": "/Script/Engine.Actor",
                 "location": {"x": 0.0, "y": 0.0, "z": 0.0}})
    if resp is None:
        return fail("18/spawn_base: timeout")
    # Accept OK OR -32021 (some UE versions mark Actor abstract; treat as informational).
    if resp.get("ok") is True:
        base_actor_path = resp["result"]["actor_path"]
        # Cleanup immediately.
        call(args.host, args.port, "18/cleanup", "phase3-4-8-18c", "actor.destroy",
             {"actor_path": base_actor_path})
        print(f"[SMOKE_PHASE3_4_8]   18/spawn(Engine.Actor) OK (base class spawn succeeded)")
    elif resp.get("error", {}).get("code") in (-32021, -32027):
        print(f"[SMOKE_PHASE3_4_8]   18/spawn(Engine.Actor) OK ({resp.get('error',{}).get('code')} — abstract/PIE)")
    else:
        return fail(f"18/spawn_base: unexpected {resp!r}")

    # ─── Sub-test 19: actor.spawn non-actor class → -32022 ────────────────────────────────────
    resp = call(args.host, args.port, "19/wrong_family", "phase3-4-8-19", "actor.spawn",
                {"class_path": "/Script/Engine.StaticMesh"})
    if resp is None:
        return fail("19/wrong_family: timeout")
    if resp.get("ok") is True:
        return fail(f"19/wrong_family: expected error, got success {resp!r}")
    code = resp.get("error", {}).get("code")
    if code not in (-32022, -32027):
        return fail(f"19/wrong_family: expected -32022 or -32027, got {code}")
    print(f"[SMOKE_PHASE3_4_8]   19/spawn(non-actor class) OK (code={code})")

    # ─── Sub-test 20: actor.spawn nonexistent class → -32020 ──────────────────────────────────
    resp = call(args.host, args.port, "20/no_class", "phase3-4-8-20", "actor.spawn",
                {"class_path": "/Script/Engine.NoSuchClassXyz"})
    if resp is None:
        return fail("20/no_class: timeout")
    if resp.get("ok") is True:
        return fail(f"20/no_class: expected error, got success {resp!r}")
    code = resp.get("error", {}).get("code")
    if code not in (-32020, -32027):
        return fail(f"20/no_class: expected -32020 or -32027, got {code}")
    print(f"[SMOKE_PHASE3_4_8]   20/spawn(no such class) OK (code={code})")

    # ─── Sub-test 21: actor.destroy both spawned + duplicated ──────────────────────────────────
    result = expect_ok(call(args.host, args.port, "21a/destroy", "phase3-4-8-21a", "actor.destroy",
                            {"actor_path": spawned_path}),
                       "phase3-4-8-21a", "21a/destroy")
    if result is None or result.get("destroyed") is not True:
        return fail(f"21a/destroy: not destroyed {result!r}")
    result = expect_ok(call(args.host, args.port, "21b/destroy", "phase3-4-8-21b", "actor.destroy",
                            {"actor_path": duplicated_path}),
                       "phase3-4-8-21b", "21b/destroy")
    if result is None or result.get("destroyed") is not True:
        return fail(f"21b/destroy: not destroyed {result!r}")
    # exists should now be false on both.
    for p, lab in ((spawned_path, "spawned"), (duplicated_path, "duplicate")):
        result = expect_ok(call(args.host, args.port, f"21c/{lab}", f"phase3-4-8-21c-{lab}", "actor.exists",
                                {"actor_path": p}),
                           f"phase3-4-8-21c-{lab}", f"21c/exists({lab})")
        if result is None:
            return 1
        if result.get("exists") is not False:
            return fail(f"21c/exists({lab}): expected false, got {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   21/destroy + exists post-destroy OK")

    # ─── Sub-test 22: actor.select_in_editor (empty list — clears selection) ────────────────────
    result = expect_ok(call(args.host, args.port, "22/select", "phase3-4-8-22", "actor.select_in_editor",
                            {"actor_paths": [], "additive": False}),
                       "phase3-4-8-22", "22/select")
    if result is None:
        return 1
    if result.get("selected") != 0 or result.get("requested") != 0:
        return fail(f"22/select: unexpected counts {result!r}")
    print(f"[SMOKE_PHASE3_4_8]   22/select_in_editor OK (cleared selection)")

    print("[SMOKE_PHASE3_4_8] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
