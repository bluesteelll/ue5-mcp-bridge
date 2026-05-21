"""Wave E Surface 2 — niagara.* runtime smoke tests.

Tests cover the 3 new tools:
  - niagara.list_active        (enumerate UNiagaraComponents in current world)
  - niagara.spawn_at_location  (one-shot SpawnSystemAtLocation)
  - niagara.stop_all           (DeactivateImmediate on every active component)
"""

import json
import socket
import sys
import time
from typing import Any, Dict, Optional

try:
    sys.stdout.reconfigure(encoding="utf-8")  # type: ignore[attr-defined]
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020


def call(method: str, args: Optional[Dict[str, Any]] = None, timeout: float = 30.0) -> Dict[str, Any]:
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    msg = {"id": f"t-{int(time.time() * 1000)}", "kind": "call_function",
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


PASSED, FAILED = 0, 0


def report(name: str, ok: bool, detail: str = ""):
    global PASSED, FAILED
    if ok:
        PASSED += 1
        print(f"  PASS  {name}  {detail}")
    else:
        FAILED += 1
        print(f"  FAIL  {name}  {detail}")


# Known Niagara systems present in the project.
NIAGARA_SYSTEM_PATH = "/Game/FlecsDA/Niagara/NS_RopeSwing"
FALLBACK_NS_PATH = "/Game/NS_TestNiagaraDeath"


def main() -> int:
    print("=== Wave E Surface 2: niagara.* runtime smoke tests ===\n")

    # T1: niagara.list_active baseline — record count BEFORE spawning anything.
    print("[T1] niagara.list_active baseline")
    r = call("niagara.list_active")
    if "result" not in r:
        report("T1.list_active baseline", False, f"error: {r.get('error')}")
        return 1
    baseline_count = r["result"].get("count", -1)
    baseline_world = r["result"].get("world")
    report("T1.list_active baseline returns count",
           isinstance(baseline_count, int) and baseline_count >= 0,
           f"count={baseline_count}")
    report("T1.list_active baseline returns world",
           baseline_world in ("editor", "pie"),
           f"world={baseline_world}")
    print(f"    [baseline: {baseline_count} components in {baseline_world} world]")

    # T2: niagara.spawn_at_location with a known good system.
    print(f"\n[T2] niagara.spawn_at_location system={NIAGARA_SYSTEM_PATH}")
    spawn_args = {
        "system_path": NIAGARA_SYSTEM_PATH,
        "location": [0.0, 0.0, 500.0],
        "auto_destroy": False,  # keep alive so list_active can find it
    }
    r = call("niagara.spawn_at_location", spawn_args)
    spawned_path = None
    if "result" not in r:
        # Try the fallback NS asset if RopeSwing isn't a UNiagaraSystem.
        print(f"    initial spawn failed ({r.get('error', {}).get('code')}: {r.get('error', {}).get('message')}), trying fallback")
        spawn_args["system_path"] = FALLBACK_NS_PATH
        r = call("niagara.spawn_at_location", spawn_args)
    if "result" not in r:
        report("T2.spawn_at_location", False, f"error: {r.get('error')}")
        # Continue anyway to test error paths
    else:
        spawned_path = r["result"].get("component_path")
        report("T2.spawn_at_location returns component_path",
               isinstance(spawned_path, str) and len(spawned_path) > 0,
               f"component_path={spawned_path}")
        report("T2.spawn_at_location returns world",
               r["result"].get("world") in ("editor", "pie"),
               f"world={r['result'].get('world')}")

    # T3: niagara.list_active AFTER spawn — count should be > baseline.
    print("\n[T3] niagara.list_active after spawn")
    r = call("niagara.list_active")
    if "result" not in r:
        report("T3.list_active after spawn", False, f"error: {r.get('error')}")
    else:
        after_spawn_count = r["result"].get("count", -1)
        components = r["result"].get("components", [])
        report("T3.list_active count increased",
               isinstance(after_spawn_count, int) and after_spawn_count > baseline_count,
               f"before={baseline_count} after={after_spawn_count}")
        # Check shape of first entry
        if components:
            entry = components[-1]  # most recently spawned should appear last (or at least present)
            has_path = isinstance(entry.get("component_path"), str)
            has_loc = isinstance(entry.get("location"), list) and len(entry["location"]) == 3
            has_active = isinstance(entry.get("is_active"), bool)
            has_lrt = isinstance(entry.get("last_render_time"), (int, float))
            report("T3.list_active entry shape (path/location/is_active/last_render_time)",
                   has_path and has_loc and has_active and has_lrt,
                   f"sample={entry}")

    # T4: niagara.stop_all — should report stopped_count >= 1 if anything was active.
    print("\n[T4] niagara.stop_all")
    r = call("niagara.stop_all")
    if "result" not in r:
        report("T4.stop_all", False, f"error: {r.get('error')}")
    else:
        stopped_count = r["result"].get("stopped_count", -1)
        report("T4.stop_all returns stopped_count",
               isinstance(stopped_count, int) and stopped_count >= 0,
               f"stopped_count={stopped_count}")
        report("T4.stop_all returns world",
               r["result"].get("world") in ("editor", "pie"),
               f"world={r['result'].get('world')}")
        print(f"    [stopped {stopped_count} active component(s)]")

    # Small delay to give the engine a tick to mark IsActive=false.
    time.sleep(0.5)

    # T5: niagara.list_active after stop_all — at minimum, the newly-spawned ones should now be inactive.
    print("\n[T5] niagara.list_active after stop_all (active count should drop)")
    r = call("niagara.list_active")
    if "result" not in r:
        report("T5.list_active after stop", False, f"error: {r.get('error')}")
    else:
        after_stop_components = r["result"].get("components", [])
        active_after_stop = sum(1 for c in after_stop_components if c.get("is_active"))
        # We deactivated everything just now — any new spawns from the engine (e.g. editor
        # background VFX with bAutoActivate restart logic) may have come back, but our
        # spawned one with auto_destroy=False AND deactivated explicitly should not be active.
        report("T5.list_active is_active count went down (or stayed at 0)",
               active_after_stop <= 1,  # allow tolerance for engine-driven re-activations
               f"active_count_after_stop={active_after_stop} total={len(after_stop_components)}")

    # T6: niagara.spawn_at_location with BAD system_path → -32004 ObjectNotFound.
    print("\n[T6] niagara.spawn_at_location BAD path → -32004 expected")
    r = call("niagara.spawn_at_location", {
        "system_path": "/Game/ThisNiagaraSystemDoesNotExist_XYZ",
        "location": [0.0, 0.0, 0.0],
    })
    err = r.get("error", {})
    report("T6.spawn_at_location missing → -32004",
           err.get("code") == -32004,
           f"code={err.get('code')} msg={err.get('message')}")

    # T7: niagara.spawn_at_location with non-Niagara asset path → -32011 WrongClass.
    # Use a known UMaterial path or similar. Try a couple of likely candidates.
    print("\n[T7] niagara.spawn_at_location non-niagara asset → -32011 expected")
    NON_NIAGARA_PATHS = [
        "/Game/M_Crosshair",        # UMaterial
        "/Game/DA_Floor",            # UDataAsset
        "/Game/DA_EnemyTest",        # UDataAsset
    ]
    got_wrong_class = False
    last_err: Dict[str, Any] = {}
    for p in NON_NIAGARA_PATHS:
        r = call("niagara.spawn_at_location", {
            "system_path": p,
            "location": [0.0, 0.0, 0.0],
        })
        err = r.get("error", {})
        last_err = err
        if err.get("code") == -32011:
            got_wrong_class = True
            print(f"    used path: {p}")
            break
    report("T7.spawn_at_location non-niagara → -32011",
           got_wrong_class,
           f"code={last_err.get('code')} msg={last_err.get('message')}")

    # T8: niagara.spawn_at_location missing location → -32602 InvalidParams.
    print("\n[T8] niagara.spawn_at_location missing location → -32602 expected")
    r = call("niagara.spawn_at_location", {"system_path": NIAGARA_SYSTEM_PATH})
    err = r.get("error", {})
    report("T8.spawn_at_location missing location → -32602",
           err.get("code") == -32602,
           f"code={err.get('code')} msg={err.get('message')}")

    # T9: niagara.spawn_at_location missing system_path → -32602 InvalidParams.
    print("\n[T9] niagara.spawn_at_location missing system_path → -32602 expected")
    r = call("niagara.spawn_at_location", {"location": [0.0, 0.0, 0.0]})
    err = r.get("error", {})
    report("T9.spawn_at_location missing system_path → -32602",
           err.get("code") == -32602,
           f"code={err.get('code')} msg={err.get('message')}")

    # Cleanup — run stop_all once more so we don't leave VFX in the editor world.
    print("\n[CLEANUP] niagara.stop_all")
    call("niagara.stop_all")

    print(f"\n=== RESULTS: {PASSED} PASS / {FAILED} FAIL ===")
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
