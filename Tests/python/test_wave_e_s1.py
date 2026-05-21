"""Wave E Surface 1 — viewport.* smoke tests.

Tests cover the 4 new tools:
  - viewport.list           (enumerate; at least 1 perspective viewport)
  - viewport.get_camera     (default viewport_index=0)
  - viewport.set_camera     (location / rotation / fov; revert; error paths)
  - viewport.focus_on_actor (frame an existing actor; bad actor path)
"""

import json
import math
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


def approx_eq(a, b, eps=1e-3) -> bool:
    try:
        return all(math.isclose(float(x), float(y), abs_tol=eps) for x, y in zip(a, b))
    except Exception:
        return False


def main() -> int:
    print("=== Wave E Surface 1: viewport.* smoke tests ===\n")

    # T1: viewport.list — at least 1 perspective viewport.
    print("[T1] viewport.list")
    r = call("viewport.list")
    if "result" not in r:
        report("T1.list", False, f"error: {r.get('error')}")
        return 1
    vps = r["result"].get("viewports", [])
    count = r["result"].get("count", 0)
    has_persp = any(vp.get("viewport_type") == "perspective" for vp in vps)
    report("T1.list non-empty", count > 0, f"count={count}")
    report("T1.list has perspective", has_persp, f"types={[vp.get('viewport_type') for vp in vps]}")
    if not vps:
        return 1

    # T2: viewport.get_camera default index — returns location/rotation/fov.
    print("\n[T2] viewport.get_camera (default index=0)")
    r = call("viewport.get_camera")
    if "result" not in r:
        report("T2.get_camera", False, f"error: {r.get('error')}")
        return 1
    cam0 = r["result"]
    has_loc = isinstance(cam0.get("camera_location"), list) and len(cam0["camera_location"]) == 3
    has_rot = isinstance(cam0.get("camera_rotation"), list) and len(cam0["camera_rotation"]) == 3
    has_fov = isinstance(cam0.get("fov"), (int, float))
    report("T2.get_camera has location", has_loc, f"location={cam0.get('camera_location')}")
    report("T2.get_camera has rotation", has_rot, f"rotation={cam0.get('camera_rotation')}")
    report("T2.get_camera has fov", has_fov, f"fov={cam0.get('fov')}")
    orig_loc = cam0.get("camera_location")
    orig_rot = cam0.get("camera_rotation")
    orig_fov = cam0.get("fov")

    # T3: viewport.set_camera location → verify via subsequent get_camera.
    print("\n[T3] viewport.set_camera location=[1000,1000,500]")
    target_loc = [1000.0, 1000.0, 500.0]
    r = call("viewport.set_camera", {"location": target_loc})
    if "result" not in r:
        report("T3.set_camera location", False, f"error: {r.get('error')}")
    else:
        new_loc = r["result"].get("new", {}).get("location")
        prior_loc = r["result"].get("prior", {}).get("location")
        report("T3.set_camera prior matches orig", approx_eq(prior_loc, orig_loc),
               f"prior={prior_loc} orig={orig_loc}")
        report("T3.set_camera new matches target", approx_eq(new_loc, target_loc),
               f"new={new_loc} target={target_loc}")
        # Verify via get_camera (full round trip).
        r2 = call("viewport.get_camera")
        read_loc = r2.get("result", {}).get("camera_location")
        report("T3.set_camera persisted (read-back)", approx_eq(read_loc, target_loc),
               f"read={read_loc}")

    # T4: viewport.set_camera rotation=[-30, 45, 0] → verify.
    print("\n[T4] viewport.set_camera rotation=[-30,45,0]")
    target_rot = [-30.0, 45.0, 0.0]
    r = call("viewport.set_camera", {"rotation": target_rot})
    if "result" not in r:
        report("T4.set_camera rotation", False, f"error: {r.get('error')}")
    else:
        new_rot = r["result"].get("new", {}).get("rotation")
        report("T4.set_camera rotation new", approx_eq(new_rot, target_rot),
               f"new={new_rot} target={target_rot}")
        r2 = call("viewport.get_camera")
        read_rot = r2.get("result", {}).get("camera_rotation")
        report("T4.set_camera rotation persisted", approx_eq(read_rot, target_rot),
               f"read={read_rot}")

    # T5: viewport.set_camera fov=120 → verify.
    print("\n[T5] viewport.set_camera fov=120")
    target_fov = 120.0
    r = call("viewport.set_camera", {"fov": target_fov})
    if "result" not in r:
        report("T5.set_camera fov", False, f"error: {r.get('error')}")
    else:
        new_fov = r["result"].get("new", {}).get("fov")
        report("T5.set_camera fov new", approx_eq([new_fov], [target_fov]),
               f"new={new_fov} target={target_fov}")
        r2 = call("viewport.get_camera")
        read_fov = r2.get("result", {}).get("fov")
        report("T5.set_camera fov persisted", approx_eq([read_fov], [target_fov]),
               f"read={read_fov}")

    # T6: revert via single set_camera with all three fields back to T2 values.
    print("\n[T6] viewport.set_camera REVERT")
    r = call("viewport.set_camera", {
        "location": orig_loc, "rotation": orig_rot, "fov": orig_fov,
    })
    if "result" not in r:
        report("T6.revert", False, f"error: {r.get('error')}")
    else:
        new_state = r["result"].get("new", {})
        ok_loc = approx_eq(new_state.get("location"), orig_loc)
        ok_rot = approx_eq(new_state.get("rotation"), orig_rot)
        ok_fov = approx_eq([new_state.get("fov")], [orig_fov])
        report("T6.revert location", ok_loc, f"got={new_state.get('location')}")
        report("T6.revert rotation", ok_rot, f"got={new_state.get('rotation')}")
        report("T6.revert fov", ok_fov, f"got={new_state.get('fov')}")

    # T7: focus_on_actor on first actor from level.get_persistent_level_actors.
    print("\n[T7] viewport.focus_on_actor")
    r = call("level.get_persistent_level_actors", {"page_size": 5})
    actors = r.get("result", {}).get("actors", []) if "result" in r else []
    # Filter for an actor that likely has a bounding box (skip GameModeBase/PlayerStart-style).
    target_actor = None
    for a in actors:
        cls = a.get("class", "")
        # Pick something with mesh/visual presence first
        if any(k in cls for k in ("StaticMesh", "Light", "BrushActor", "PlayerStart")):
            target_actor = a
            break
    if not target_actor and actors:
        target_actor = actors[0]
    if not target_actor:
        report("T7.focus_on_actor", False, "no actors found in level.get_persistent_level_actors")
    else:
        actor_path = target_actor.get("path") or target_actor.get("actor_path") or target_actor.get("name")
        r = call("viewport.focus_on_actor", {"actor_path": actor_path})
        if "result" not in r:
            # Some actors may have invalid bounds — try a different one.
            print(f"    initial focus failed ({r.get('error', {}).get('message')}), trying others")
            for a in actors[1:]:
                actor_path = a.get("path") or a.get("actor_path") or a.get("name")
                if not actor_path:
                    continue
                r = call("viewport.focus_on_actor", {"actor_path": actor_path})
                if "result" in r:
                    break
        if "result" not in r:
            report("T7.focus_on_actor", False,
                   f"no actor with valid bounds found; last error: {r.get('error')}")
        else:
            new_loc = r["result"].get("new_camera_location")
            new_rot = r["result"].get("new_camera_rotation")
            report("T7.focus_on_actor returns location",
                   isinstance(new_loc, list) and len(new_loc) == 3,
                   f"actor={actor_path} new_camera_location={new_loc}")
            report("T7.focus_on_actor returns rotation",
                   isinstance(new_rot, list) and len(new_rot) == 3,
                   f"new_camera_rotation={new_rot}")

    # T8: viewport.set_camera with no fields → -32602.
    print("\n[T8] viewport.set_camera with NO fields → -32602 expected")
    r = call("viewport.set_camera", {})
    err = r.get("error", {})
    report("T8.set_camera empty → -32602",
           err.get("code") == -32602,
           f"code={err.get('code')} msg={err.get('message')}")

    # T9: viewport.set_camera with viewport_index=999 → -32026.
    print("\n[T9] viewport.set_camera viewport_index=999 → -32026 expected")
    r = call("viewport.set_camera", {"viewport_index": 999, "fov": 90})
    err = r.get("error", {})
    report("T9.set_camera OOB → -32026",
           err.get("code") == -32026,
           f"code={err.get('code')} msg={err.get('message')}")

    # T10: viewport.focus_on_actor with bad actor → -32004.
    print("\n[T10] viewport.focus_on_actor bad actor → -32004 expected")
    r = call("viewport.focus_on_actor", {"actor_path": "ThisActorDoesNotExist_XYZ_12345"})
    err = r.get("error", {})
    report("T10.focus_on_actor missing → -32004",
           err.get("code") == -32004,
           f"code={err.get('code')} msg={err.get('message')}")

    # Final restore — in case any sub-test left the camera in an odd state, push originals once more.
    print("\n[FINAL] restoring original camera state")
    call("viewport.set_camera", {
        "location": orig_loc, "rotation": orig_rot, "fov": orig_fov,
    })

    print(f"\n=== RESULTS: {PASSED} PASS / {FAILED} FAIL ===")
    return 0 if FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
