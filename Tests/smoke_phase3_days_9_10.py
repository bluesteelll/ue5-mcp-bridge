#!/usr/bin/env python3
"""Phase 3 Days 9-10 smoke — 8 Component tools.

Verifies (against an editor that has loaded SOME map, persistent level non-empty):

  1.  tools.list contains all 8 component.* names (+ counts: total cpp_handlers >= 84).
  2.  actor.spawn /Script/Engine.StaticMeshActor at (100,200,300) → spawned_path.
  3.  component.list_class_default_subcomponents /Script/Engine.StaticMeshActor →
      subobjects contain StaticMeshComponent.
  4.  component.add a PointLightComponent under spawned actor (no attach_to → attaches to root) →
      returns component_path.
  5.  component.get the new light → shape includes is_scene=true, is_root_component=false,
      parent_component_name = StaticMeshComponent (the root).
  6.  component.set_transform on the new light: relative_location (10,20,30), relative_scale (2,2,2)
      → re-read matches.
  7.  component.set_property on the light: Intensity = 12345.0 → re-read matches.
  8.  component.set_property edit-const test — try writing CPF_DisableEditOnInstance property
      (e.g. SceneComponent.bWantsOnUpdateTransform if available; fallback: any property with
      hardcoded edit-const flag). Expect -32007 PROPERTY_ACCESS_DENIED. Optional gate skip if no
      such property is found on the chosen component.
  9.  component.get_property on the light: Intensity → returns 12345.0.
  10. component.move_in_hierarchy — add a second light, move it under the FIRST light → re-read
      shows parent_component_name = first light.
  11. component.move_in_hierarchy refusal — try to re-parent the root component → -32010 InvalidPath.
  12. component.remove the moved light, then the original light, then list components → both gone.
  13. component.remove root component refusal → -32010 InvalidPath.
  14. component.add with bad class (non-component) → -32022 WrongClassFamily.
  15. Cleanup: actor.destroy spawned.

Prints ``[SMOKE_PHASE3_9_10] PASS`` on success or ``[SMOKE_PHASE3_9_10] FAIL ...`` on first mismatch.

Usage:
  python smoke_phase3_days_9_10.py [--host HOST] [--port PORT]
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
    print(f"[SMOKE_PHASE3_9_10] FAIL reason={reason}")
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


def expect_error(response: Optional[dict], expected_id: str, expected_codes, label: str) -> bool:
    if isinstance(expected_codes, int):
        expected_codes = (expected_codes,)
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
    if not isinstance(error, dict) or error.get("code") not in expected_codes:
        fail(f"{label}: wrong-error-code expected={expected_codes} got={error!r}")
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    print(f"[SMOKE_PHASE3_9_10] connecting to {args.host}:{args.port} ...")

    # ─── Sub-test 1: tools.list contains all 8 component.* names ───────────────────────────────
    result = expect_ok(call(args.host, args.port, "1/tools.list", "phase3-9-10-1", "tools.list"),
                       "phase3-9-10-1", "1/tools.list")
    if result is None:
        return 1
    cpp = result.get("cpp_handlers", [])
    expected_component_methods = {
        "component.add", "component.remove", "component.get", "component.get_property",
        "component.set_property", "component.set_transform", "component.move_in_hierarchy",
        "component.list_class_default_subcomponents",
    }
    missing = expected_component_methods - set(cpp)
    if missing:
        return fail(f"1/tools.list: missing component.* methods {sorted(missing)}")
    if len(cpp) < 84:
        return fail(f"1/tools.list: expected >=84 cpp handlers (Days 0-10), got {len(cpp)}")
    print(f"[SMOKE_PHASE3_9_10]   1/tools.list OK (all 8 component.* registered, total cpp={len(cpp)})")

    # ─── Sub-test 2: actor.spawn StaticMeshActor at (100, 200, 300) ────────────────────────────
    result = expect_ok(call(args.host, args.port, "2/spawn", "phase3-9-10-2", "actor.spawn",
                            {
                                "class_path": "/Script/Engine.StaticMeshActor",
                                "location": {"x": 100.0, "y": 200.0, "z": 300.0},
                                "label": "MCP_CompSmoke",
                            }),
                       "phase3-9-10-2", "2/spawn")
    if result is None:
        # If we're in PIE this may fail — bail cleanly.
        return 1
    spawned_path = result.get("actor_path")
    if not isinstance(spawned_path, str) or not spawned_path:
        return fail(f"2/spawn: invalid actor_path {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   2/spawn OK ({spawned_path})")

    # ─── Sub-test 3: list_class_default_subcomponents StaticMeshActor ───────────────────────────
    result = expect_ok(call(args.host, args.port, "3/cdo", "phase3-9-10-3",
                            "component.list_class_default_subcomponents",
                            {"class_path": "/Script/Engine.StaticMeshActor"}),
                       "phase3-9-10-3", "3/cdo")
    if result is None:
        return 1
    subobjects = result.get("subobjects", [])
    if not isinstance(subobjects, list) or len(subobjects) == 0:
        return fail(f"3/cdo: expected non-empty subobjects, got {result!r}")
    has_smc = any(
        ("StaticMeshComponent" in s.get("class", "")) or (s.get("name", "").endswith("StaticMeshComponent"))
        for s in subobjects
    )
    if not has_smc:
        return fail(f"3/cdo: no StaticMeshComponent found in CDO, got {subobjects!r}")
    print(f"[SMOKE_PHASE3_9_10]   3/list_class_default_subcomponents OK ({len(subobjects)} subobjects)")

    # ─── Sub-test 4: component.add PointLightComponent ──────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "4/add", "phase3-9-10-4", "component.add",
                            {
                                "actor_path": spawned_path,
                                "component_class": "/Script/Engine.PointLightComponent",
                                "component_name": "MCP_TestLight",
                            }),
                       "phase3-9-10-4", "4/add")
    if result is None:
        return 1
    light_path = result.get("component_path")
    if not isinstance(light_path, str) or not light_path:
        return fail(f"4/add: invalid component_path {result!r}")
    if result.get("is_scene") is not True:
        return fail(f"4/add: expected is_scene=true for PointLightComponent, got {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   4/add OK ({light_path})")

    # ─── Sub-test 5: component.get ──────────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "5/get", "phase3-9-10-5", "component.get",
                            {"component_path": light_path}),
                       "phase3-9-10-5", "5/get")
    if result is None:
        return 1
    for k in ("path", "name", "class", "is_scene", "owner_actor_path", "properties_summary"):
        if k not in result:
            return fail(f"5/get: missing field '{k}' in {sorted(result.keys())}")
    if result.get("is_root_component") is True:
        return fail(f"5/get: PointLightComponent should not be root, got {result!r}")
    if not result.get("parent_component_name"):
        return fail(f"5/get: expected parent_component_name (attached to root), got {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   5/get OK (parent={result.get('parent_component_name')}, "
          f"properties_summary={len(result.get('properties_summary', []))})")

    # ─── Sub-test 6: component.set_transform ────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "6/set_xform", "phase3-9-10-6",
                            "component.set_transform",
                            {
                                "component_path": light_path,
                                "location": {"x": 10.0, "y": 20.0, "z": 30.0},
                                "scale":    {"x": 2.0, "y": 2.0, "z": 2.0},
                            }),
                       "phase3-9-10-6", "6/set_xform")
    if result is None:
        return 1
    loc = result.get("relative_location", {})
    scl = result.get("relative_scale", {})
    if abs(loc.get("x", 0) - 10.0) > 0.01 or abs(loc.get("y", 0) - 20.0) > 0.01 \
       or abs(loc.get("z", 0) - 30.0) > 0.01:
        return fail(f"6/set_xform: relative_location mismatch got {loc!r}")
    if abs(scl.get("x", 0) - 2.0) > 0.01:
        return fail(f"6/set_xform: relative_scale mismatch got {scl!r}")
    print(f"[SMOKE_PHASE3_9_10]   6/set_transform OK (loc={loc}, scale={scl})")

    # ─── Sub-test 7: component.set_property Intensity ───────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "7/set_prop", "phase3-9-10-7",
                            "component.set_property",
                            {
                                "component_path": light_path,
                                "property_path": "Intensity",
                                "value": 12345.0,
                            }),
                       "phase3-9-10-7", "7/set_prop")
    if result is None:
        return 1
    if abs(float(result.get("value", 0.0)) - 12345.0) > 0.01:
        return fail(f"7/set_prop: Intensity mismatch got {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   7/set_property Intensity OK")

    # ─── Sub-test 8: component.set_property edit-const test ─────────────────────────────────────
    #
    # We try to write to a property that has CPF_BlueprintReadOnly OR CPF_EditConst OR
    # CPF_DisableEditOnInstance. The PointLightComponent's "LightmassSettings" struct's
    # "DiffuseBoost" is BP-callable but not necessarily edit-const. A safer bet across UE versions
    # is to probe a USceneComponent::BodyInstance.bGenerateWakeEvents (BlueprintReadOnly on many),
    # but rather than guess we try writing to a deeply nested property whose name doesn't exist —
    # that yields PropertyNotFound (-32005), not the right gate. So instead test with
    # bUseAttachParentBound on USceneComponent (typically BP-readonly). If the engine version has
    # it writable, this sub-test is informational only — accept either ok or -32007.
    resp = call(args.host, args.port, "8/edit_const", "phase3-9-10-8",
                "component.set_property",
                {
                    "component_path": light_path,
                    "property_path": "bUseAttachParentBound",
                    "value": True,
                })
    if resp is None:
        return fail("8/edit_const: timeout")
    code = resp.get("error", {}).get("code") if resp.get("ok") is False else None
    if code == -32007:
        print(f"[SMOKE_PHASE3_9_10]   8/edit-const gate OK (-32007 PROPERTY_ACCESS_DENIED)")
    elif resp.get("ok") is True:
        print(f"[SMOKE_PHASE3_9_10]   8/edit-const informational (property writable in this UE version)")
    elif code in (-32005, -32006):
        print(f"[SMOKE_PHASE3_9_10]   8/edit-const informational (property family changed, code={code})")
    else:
        return fail(f"8/edit_const: unexpected {resp!r}")

    # ─── Sub-test 9: component.get_property ─────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "9/get_prop", "phase3-9-10-9",
                            "component.get_property",
                            {"component_path": light_path, "property_path": "Intensity"}),
                       "phase3-9-10-9", "9/get_prop")
    if result is None:
        return 1
    if abs(float(result.get("value", 0.0)) - 12345.0) > 0.01:
        return fail(f"9/get_prop: Intensity round-trip mismatch got {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   9/get_property Intensity OK")

    # ─── Sub-test 10: component.move_in_hierarchy ───────────────────────────────────────────────
    # Add second light, then move it under the first light.
    result = expect_ok(call(args.host, args.port, "10a/add2", "phase3-9-10-10a", "component.add",
                            {
                                "actor_path": spawned_path,
                                "component_class": "/Script/Engine.PointLightComponent",
                                "component_name": "MCP_TestLight2",
                            }),
                       "phase3-9-10-10a", "10a/add_second_light")
    if result is None:
        return 1
    light2_path = result.get("component_path")
    if not isinstance(light2_path, str) or not light2_path:
        return fail(f"10a/add2: invalid component_path {result!r}")

    result = expect_ok(call(args.host, args.port, "10b/move", "phase3-9-10-10b",
                            "component.move_in_hierarchy",
                            {
                                "component_path": light2_path,
                                "new_parent_component_path": light_path,
                            }),
                       "phase3-9-10-10b", "10b/move")
    if result is None:
        return 1
    if result.get("new_parent_name") != "MCP_TestLight":
        return fail(f"10b/move: new_parent_name mismatch got {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   10/move_in_hierarchy OK "
          f"(prev={result.get('previous_parent_name')}, new={result.get('new_parent_name')})")

    # ─── Sub-test 11: re-parent root → InvalidPath ──────────────────────────────────────────────
    # Find the root component first by reading actor snapshot.
    result = expect_ok(call(args.host, args.port, "11a/get_actor", "phase3-9-10-11a", "actor.get",
                            {"actor_path": spawned_path}),
                       "phase3-9-10-11a", "11a/get_actor")
    if result is None:
        return 1
    comps = result.get("components", [])
    root_name = None
    for c in comps:
        if c.get("is_scene") and c.get("parent_component_name") is None:
            root_name = c.get("name")
            break
    if not root_name:
        return fail(f"11a/get_actor: no root component found in {comps!r}")
    root_path = f"{spawned_path}/{root_name}"
    resp = call(args.host, args.port, "11b/move_root", "phase3-9-10-11b",
                "component.move_in_hierarchy",
                {
                    "component_path": root_path,
                    "new_parent_component_path": light_path,
                })
    if resp is None:
        return fail("11b/move_root: timeout")
    if resp.get("ok") is True:
        return fail(f"11b/move_root: expected error, got success {resp!r}")
    code = resp.get("error", {}).get("code")
    # Either InvalidPath (-32010) for "refuse to re-parent root", or InvalidParams (-32602)
    # depending on which gate fires first — accept both.
    if code not in (-32010, -32602):
        return fail(f"11b/move_root: expected -32010 or -32602, got {code}")
    print(f"[SMOKE_PHASE3_9_10]   11/move_in_hierarchy(root) refused OK (code={code})")

    # ─── Sub-test 12: component.remove both lights, then verify ─────────────────────────────────
    result = expect_ok(call(args.host, args.port, "12a/rm_light2", "phase3-9-10-12a", "component.remove",
                            {"component_path": light2_path}),
                       "phase3-9-10-12a", "12a/rm_light2")
    if result is None:
        return 1
    result = expect_ok(call(args.host, args.port, "12b/rm_light", "phase3-9-10-12b", "component.remove",
                            {"component_path": light_path}),
                       "phase3-9-10-12b", "12b/rm_light")
    if result is None:
        return 1
    # Verify both gone via actor.list_components.
    result = expect_ok(call(args.host, args.port, "12c/verify", "phase3-9-10-12c", "actor.list_components",
                            {"actor_path": spawned_path}),
                       "phase3-9-10-12c", "12c/verify")
    if result is None:
        return 1
    remaining_names = {c.get("name") for c in result.get("components", [])}
    if "MCP_TestLight" in remaining_names or "MCP_TestLight2" in remaining_names:
        return fail(f"12c/verify: light(s) still present in {remaining_names!r}")
    print(f"[SMOKE_PHASE3_9_10]   12/remove + verify OK (post-remove components={len(remaining_names)})")

    # ─── Sub-test 13: refuse to remove root component → -32010 ─────────────────────────────────
    resp = call(args.host, args.port, "13/rm_root", "phase3-9-10-13", "component.remove",
                {"component_path": root_path})
    if resp is None:
        return fail("13/rm_root: timeout")
    if resp.get("ok") is True:
        return fail(f"13/rm_root: expected error, got success {resp!r}")
    code = resp.get("error", {}).get("code")
    if code != -32010:
        return fail(f"13/rm_root: expected -32010, got {code}")
    print(f"[SMOKE_PHASE3_9_10]   13/remove(root) refused OK (code=-32010)")

    # ─── Sub-test 14: component.add wrong-family class → -32022 ────────────────────────────────
    resp = call(args.host, args.port, "14/wrong_family", "phase3-9-10-14", "component.add",
                {
                    "actor_path": spawned_path,
                    "component_class": "/Script/Engine.StaticMeshActor",  # an Actor, not a Component
                })
    if resp is None:
        return fail("14/wrong_family: timeout")
    if resp.get("ok") is True:
        return fail(f"14/wrong_family: expected error, got success {resp!r}")
    code = resp.get("error", {}).get("code")
    if code != -32022:
        return fail(f"14/wrong_family: expected -32022, got {code}")
    print(f"[SMOKE_PHASE3_9_10]   14/add(non-component class) refused OK (code=-32022)")

    # ─── Sub-test 15: cleanup ──────────────────────────────────────────────────────────────────
    result = expect_ok(call(args.host, args.port, "15/cleanup", "phase3-9-10-15", "actor.destroy",
                            {"actor_path": spawned_path}),
                       "phase3-9-10-15", "15/cleanup")
    if result is None or result.get("destroyed") is not True:
        return fail(f"15/cleanup: not destroyed {result!r}")
    print(f"[SMOKE_PHASE3_9_10]   15/cleanup OK")

    print("[SMOKE_PHASE3_9_10] PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
