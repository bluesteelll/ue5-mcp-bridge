"""Wave D Surface 6 — transform.* smoke tests.

Tests cover the 3 new tools:
  - transform.batch_set    (absolute + relative; rotation; missing-args)
  - transform.snap_to_floor (basic — graceful with no floor too)
  - transform.align        (set + average; bad axis)

The test loads/uses a scratch map and spawns 3 ephemeral StaticMeshActors as
targets so the suite works in any map (the scratch maps in the project don't
have user-placed transform-able actors). All spawned actors are destroyed at
the end.
"""

import json
import socket
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

# Force UTF-8 stdout (Windows console default cp1252 chokes on Unicode).
try:
    sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020
TEST_MAP = "/Game/MCPTest/Phase3/M_Scratch"


def call(method: str, args: Optional[Dict[str, Any]] = None, timeout: float = 30.0) -> Dict[str, Any]:
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    msg = {"id": f"t-{int(time.time()*1000)}", "kind": "call_function",
           "method": method, "args": args or {}}
    s.sendall((json.dumps(msg) + "\n").encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
        if b"\n" in buf:
            break
    s.close()
    if not buf:
        raise RuntimeError(f"empty response for {method}")
    return json.loads(buf[: buf.index(b"\n")].decode())


PASSED, FAILED, SKIPPED = 0, 0, 0


def report(name: str, ok: bool, detail: str = ""):
    global PASSED, FAILED
    if ok:
        PASSED += 1
        print(f"  PASS  {name}  {detail}")
    else:
        FAILED += 1
        print(f"  FAIL  {name}  {detail}")


def skip(name: str, reason: str):
    global SKIPPED
    SKIPPED += 1
    print(f"  SKIP  {name}  ({reason})")


def get_transform(actor_path: str) -> Dict[str, Any]:
    r = call("actor.get", {"actor_path": actor_path})
    if "result" not in r:
        return {}
    return r["result"].get("transform", {})


def transform_to_arrays(t: Dict[str, Any]) -> Tuple[List[float], List[float], List[float]]:
    # actor.get returns transform with "translation"/"rotation"/"scale" (not "location"),
    # each wrapped with a "_kind" marker.
    loc = t.get("translation", {})
    rot = t.get("rotation", {})
    sca = t.get("scale",    {})
    return (
        [loc.get("x", 0.0), loc.get("y", 0.0), loc.get("z", 0.0)],
        [rot.get("pitch", 0.0), rot.get("yaw", 0.0), rot.get("roll", 0.0)],
        [sca.get("x", 1.0), sca.get("y", 1.0), sca.get("z", 1.0)],
    )


def spawn_test_actor(name_suffix: str, location: List[float]) -> Optional[str]:
    """Spawn a StaticMeshActor. Returns canonical actor_path on success, else None."""
    r = call("actor.spawn", {
        "class_path": "/Script/Engine.StaticMeshActor",
        "location": {"x": location[0], "y": location[1], "z": location[2]},
        "label": f"MCP_TFM_Test_{name_suffix}"
    })
    if "result" not in r:
        print(f"    spawn failed: {r.get('error')}")
        return None
    return r["result"].get("actor_path")


def destroy_actor(actor_path: str) -> bool:
    r = call("actor.destroy", {"actor_path": actor_path})
    return "result" in r and r["result"].get("destroyed") is True


def main() -> int:
    print("=== Wave D Surface 6: transform.* smoke tests ===\n")

    # 0) Load scratch map (idempotent if already loaded).
    r = call("level.load", {"map_path": TEST_MAP})
    if "result" not in r or not r["result"].get("loaded"):
        print(f"FATAL: failed to load test map {TEST_MAP}: {r.get('error')}")
        return 1
    print(f"  map loaded: {TEST_MAP}")

    # 1) Spawn 3 test actors at distinct positions.
    test_locations = [
        [200.0, 0.0,  100.0],
        [400.0, 0.0,  100.0],
        [600.0, 0.0,  100.0],
    ]
    spawned: List[str] = []
    for i, loc in enumerate(test_locations):
        p = spawn_test_actor(f"A{i}", loc)
        if p:
            spawned.append(p)
            print(f"  spawned: {p}")

    if len(spawned) < 2:
        print(f"FATAL: spawn produced only {len(spawned)} actors — cannot test")
        for p in spawned:
            destroy_actor(p)
        return 1

    target_paths = spawned[:]
    print()

    # Capture original transforms (right after spawn).
    priors = {}
    for p in target_paths:
        priors[p] = transform_to_arrays(get_transform(p))

    try:
        # ─── T1: batch_set absolute location ──────────────────────────────────
        target_loc = [100.0, 100.0, 500.0]
        r = call("transform.batch_set", {"actor_paths": target_paths, "location": target_loc})
        if "result" in r and r["result"].get("updated", 0) >= 1:
            verified = 0
            for p in target_paths:
                loc, _, _ = transform_to_arrays(get_transform(p))
                if abs(loc[0] - 100.0) < 0.01 and abs(loc[1] - 100.0) < 0.01 and abs(loc[2] - 500.0) < 0.01:
                    verified += 1
            report("T1 batch_set absolute location",
                   verified == len(target_paths) and r["result"].get("failed", 999) == 0,
                   f"updated={r['result']['updated']} failed={r['result'].get('failed')} verified={verified}/{len(target_paths)}")
        else:
            report("T1 batch_set absolute location", False, f"resp={r}")

        # ─── T2: batch_set absolute rotation ──────────────────────────────────
        target_rot = [0.0, 90.0, 0.0]
        r = call("transform.batch_set", {"actor_paths": target_paths, "rotation": target_rot})
        if "result" in r and r["result"].get("updated", 0) >= 1:
            verified = 0
            for p in target_paths:
                _, rot, _ = transform_to_arrays(get_transform(p))
                if abs(rot[1] - 90.0) < 0.5:
                    verified += 1
            report("T2 batch_set absolute rotation (yaw=90)",
                   verified == len(target_paths),
                   f"updated={r['result']['updated']} verified={verified}/{len(target_paths)}")
        else:
            report("T2 batch_set absolute rotation", False, f"resp={r}")

        # ─── T3: batch_set relative location (+50 X) ──────────────────────────
        # After T1 all actors are at X=100. Relative +50 -> X=150.
        r = call("transform.batch_set", {
            "actor_paths": target_paths,
            "location": [50.0, 0.0, 0.0],
            "relative": True
        })
        if "result" in r and r["result"].get("updated", 0) >= 1:
            verified = 0
            for p in target_paths:
                loc, _, _ = transform_to_arrays(get_transform(p))
                if abs(loc[0] - 150.0) < 0.01:
                    verified += 1
            report("T3 batch_set relative location (+50 X)",
                   verified == len(target_paths),
                   f"updated={r['result']['updated']} verified={verified}/{len(target_paths)}")
        else:
            report("T3 batch_set relative location", False, f"resp={r}")

        # ─── T4: snap_to_floor — verify response shape, hits OR misses both OK ───
        # Place actors high above so a floor (if present) is below.
        call("transform.batch_set", {"actor_paths": target_paths, "location": [100.0, 100.0, 2000.0]})
        r = call("transform.snap_to_floor", {
            "actor_paths": target_paths,
            "max_trace_distance": 100000,
            "trace_channel": "WorldStatic"
        })
        if "result" in r and "snapped" in r["result"] and "results" in r["result"]:
            n = r["result"]["snapped"]
            m = r["result"]["missed"]
            entries = r["result"]["results"]
            shape_ok = (n + m) == len(target_paths) and len(entries) == len(target_paths)
            # Verify each entry has prior_z field
            entry_shape_ok = all("prior_z" in e and "new_z" in e and "hit_actor" in e for e in entries)
            report("T4 snap_to_floor response shape",
                   shape_ok and entry_shape_ok,
                   f"snapped={n} missed={m} total={len(target_paths)} entry_keys_ok={entry_shape_ok}")
        else:
            report("T4 snap_to_floor response shape", False, f"resp={r}")

        # ─── T5: align Z mode=set value=1000 ──────────────────────────────────
        r = call("transform.align", {"actor_paths": target_paths, "axis": "Z", "value": 1000.0, "mode": "set"})
        if "result" in r and r["result"].get("aligned", 0) >= 1:
            verified = 0
            for p in target_paths:
                loc, _, _ = transform_to_arrays(get_transform(p))
                if abs(loc[2] - 1000.0) < 0.01:
                    verified += 1
            report("T5 align Z mode=set value=1000",
                   verified == len(target_paths) and abs(r["result"].get("value", 0) - 1000.0) < 0.01,
                   f"aligned={r['result']['aligned']} value={r['result'].get('value')} verified={verified}/{len(target_paths)}")
        else:
            report("T5 align Z mode=set", False, f"resp={r}")

        # ─── T6: align Z mode=average ─────────────────────────────────────────
        # Make the Zs different so the average is non-trivial.
        per_z_settings = [
            (target_paths[0], 300.0),
            (target_paths[1], 500.0) if len(target_paths) >= 2 else None,
            (target_paths[2], 700.0) if len(target_paths) >= 3 else None,
        ]
        for entry in per_z_settings:
            if entry is None:
                continue
            p, z = entry
            call("transform.batch_set", {"actor_paths": [p], "location": [100.0, 100.0, z]})

        # Compute expected average.
        zs = []
        for p in target_paths:
            loc, _, _ = transform_to_arrays(get_transform(p))
            zs.append(loc[2])
        expected_avg = sum(zs) / len(zs) if zs else 0.0
        print(f"      (T6 setup: actual Zs={zs} expected_avg={expected_avg:.2f})")

        r = call("transform.align", {"actor_paths": target_paths, "axis": "Z", "mode": "average"})
        if "result" in r and r["result"].get("aligned", 0) >= 1:
            reported_value = r["result"].get("value", -999)
            ok_value = abs(reported_value - expected_avg) < 0.5
            verified = 0
            for p in target_paths:
                loc, _, _ = transform_to_arrays(get_transform(p))
                if abs(loc[2] - expected_avg) < 0.5:
                    verified += 1
            report("T6 align Z mode=average",
                   ok_value and verified == len(target_paths),
                   f"expected_avg={expected_avg:.2f} reported={reported_value:.2f} verified={verified}/{len(target_paths)}")
        else:
            report("T6 align Z mode=average", False, f"resp={r}")

        # ─── T6b: align mode=min ──────────────────────────────────────────────
        # Reset to different Zs first.
        for entry in per_z_settings:
            if entry is None:
                continue
            p, z = entry
            call("transform.batch_set", {"actor_paths": [p], "location": [100.0, 100.0, z]})
        r = call("transform.align", {"actor_paths": target_paths, "axis": "Z", "mode": "min"})
        if "result" in r:
            reported_value = r["result"].get("value", -999)
            expected_min = min(z for _, z in [e for e in per_z_settings if e is not None])
            ok_value = abs(reported_value - expected_min) < 0.5
            report("T6b align Z mode=min",
                   ok_value,
                   f"expected_min={expected_min} reported={reported_value}")
        else:
            report("T6b align Z mode=min", False, f"resp={r}")

        # ─── T6c: align mode=max ──────────────────────────────────────────────
        for entry in per_z_settings:
            if entry is None:
                continue
            p, z = entry
            call("transform.batch_set", {"actor_paths": [p], "location": [100.0, 100.0, z]})
        r = call("transform.align", {"actor_paths": target_paths, "axis": "Z", "mode": "max"})
        if "result" in r:
            reported_value = r["result"].get("value", -999)
            expected_max = max(z for _, z in [e for e in per_z_settings if e is not None])
            ok_value = abs(reported_value - expected_max) < 0.5
            report("T6c align Z mode=max",
                   ok_value,
                   f"expected_max={expected_max} reported={reported_value}")
        else:
            report("T6c align Z mode=max", False, f"resp={r}")

        # ─── T7: batch_set with no location/rotation/scale -> -32602 ──────────
        r = call("transform.batch_set", {"actor_paths": target_paths})
        err = r.get("error", {})
        report("T7 batch_set no fields -> -32602",
               err.get("code") == -32602,
               f"code={err.get('code')} msg={err.get('message', '')[:80]}")

        # ─── T8: align with bad axis -> -32602 ────────────────────────────────
        r = call("transform.align", {"actor_paths": target_paths, "axis": "Q", "value": 0, "mode": "set"})
        err = r.get("error", {})
        report("T8 align bad axis -> -32602",
               err.get("code") == -32602,
               f"code={err.get('code')} msg={err.get('message', '')[:80]}")

        # ─── T9: batch_set with all-bad actor_paths -> -32004 (top-level) ─────
        r = call("transform.batch_set", {
            "actor_paths": ["NoSuchActor_AAA_999", "NoSuchActor_BBB_777"],
            "location": [0.0, 0.0, 0.0]
        })
        err = r.get("error", {})
        report("T9 batch_set all unresolved -> -32004",
               err.get("code") == -32004,
               f"code={err.get('code')} msg={err.get('message', '')[:80]}")

        # ─── T10: batch_set mixed valid+invalid -> partial success ────────────
        r = call("transform.batch_set", {
            "actor_paths": [target_paths[0], "NoSuchActor_XXX_888"],
            "location": [50.0, 50.0, 50.0]
        })
        if "result" in r:
            res = r["result"]
            ok = res.get("updated") == 1 and res.get("failed") == 1 and len(res.get("failures", [])) == 1
            report("T10 batch_set mixed valid+invalid -> partial",
                   ok,
                   f"updated={res.get('updated')} failed={res.get('failed')} failures={len(res.get('failures', []))}")
        else:
            report("T10 batch_set mixed valid+invalid -> partial", False, f"resp={r}")

        # ─── T11: align mode=average with 0 resolved -> -32602 ────────────────
        r = call("transform.align", {
            "actor_paths": ["BadA", "BadB"],
            "axis": "Z",
            "mode": "average"
        })
        err = r.get("error", {})
        report("T11 align average empty -> -32602",
               err.get("code") == -32602,
               f"code={err.get('code')} msg={err.get('message', '')[:80]}")

    finally:
        # ─── Cleanup: destroy all spawned actors ─────────────────────────────
        print("\n  destroying spawned test actors...")
        destroyed = 0
        for p in spawned:
            if destroy_actor(p):
                destroyed += 1
        print(f"  destroyed {destroyed}/{len(spawned)} actors\n")

    print(f"=== RESULT: {PASSED} PASS, {FAILED} FAIL, {SKIPPED} SKIP ===")
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
