#!/usr/bin/env python3
"""Wave G Surface 3 test: navmesh.* navigation system query tools.

Mirrors the test plan in the Wave G S3 brief:
  T1: navmesh.list                 → returns navmeshes array + world kind
  T2: navmesh.project_to_navmesh   → response shape verification (projected bool + location)
  T3: navmesh.find_path            → response shape verification (found + waypoints + path_length)
  T4: navmesh.rebuild              → returns duration_seconds + rebuilt flag
  T5: navmesh.find_path missing args → -32602

NOTE: Many default editor maps have no NavMeshBoundsVolume placed, so most queries return
empty/false results. We test RESPONSE SHAPE not gameplay outcomes — these tools are designed
to surface "no navmesh in scope" as legitimate data, not -32603 infrastructure errors.
"""
import json, socket, sys, time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020


def send(req, t=30):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + t
    while time.time() < deadline:
        c = s.recv(256 * 1024)
        if not c:
            break
        buf += c
        if b"\n" in buf:
            return json.loads(buf[: buf.index(b"\n")].decode())
    return None


def call(method, args=None, t=30):
    return send({"id": "t", "kind": "call_function", "method": method, "args": args or {}}, t)


PASS, FAIL, SKIP = [], [], []


def ok(n, m=""):
    PASS.append(n)
    print(f"  PASS {n} {m}")


def fail(n, m=""):
    FAIL.append(n)
    print(f"  FAIL {n} {m}")


def skip(n, m=""):
    SKIP.append(n)
    print(f"  SKIP {n} {m}")


# === T1: navmesh.list ===
print("=== T1: navmesh.list ===")
r = call("navmesh.list")
navmesh_actor_path = None
if r and r.get("ok"):
    res = r["result"]
    nms = res.get("navmeshes")
    world = res.get("world")
    if isinstance(nms, list) and world in ("editor", "pie"):
        ok("T1/list", f"world={world} count={len(nms)}")
        if nms:
            first = nms[0]
            required = ("actor_path", "agent_radius", "agent_height",
                        "cell_size", "tile_size_uu", "is_initialized")
            missing = [k for k in required if k not in first]
            if missing:
                fail("T1/entry_shape", f"missing fields: {missing} in {first}")
            else:
                ok("T1/entry_shape",
                    f"actor={first['actor_path'][:60]}... "
                    f"radius={first['agent_radius']} height={first['agent_height']} "
                    f"cell={first['cell_size']} tile={first['tile_size_uu']} init={first['is_initialized']}")
                navmesh_actor_path = first["actor_path"]
        else:
            skip("T1/entry_shape", "no navmesh actors in current map (legitimate empty state)")
    else:
        fail("T1/list", f"unexpected response shape: {res}")
else:
    fail("T1/list", f"{r}")


# === T2: navmesh.project_to_navmesh ===
print("\n=== T2: navmesh.project_to_navmesh location=[0,0,200] ===")
r = call("navmesh.project_to_navmesh", {
    "location": [0.0, 0.0, 200.0],
})
if r and r.get("ok"):
    res = r["result"]
    projected = res.get("projected")
    loc = res.get("location")
    world = res.get("world")
    if isinstance(projected, bool) and isinstance(loc, list) and len(loc) == 3 and world in ("editor", "pie"):
        ok("T2/project_default", f"projected={projected} location={loc} world={world}")
    else:
        fail("T2/project_default", f"unexpected shape: {res}")
else:
    fail("T2/project_default", f"{r}")


# === T2b: navmesh.project_to_navmesh with custom search_extent ===
print("\n=== T2b: navmesh.project_to_navmesh with search_extent=[500,500,500] ===")
r = call("navmesh.project_to_navmesh", {
    "location": [0.0, 0.0, 0.0],
    "search_extent": [500.0, 500.0, 500.0],
})
if r and r.get("ok"):
    res = r["result"]
    if isinstance(res.get("projected"), bool):
        ok("T2b/project_with_extent", f"projected={res['projected']} loc={res.get('location')}")
    else:
        fail("T2b/project_with_extent", f"unexpected: {res}")
else:
    fail("T2b/project_with_extent", f"{r}")


# === T3: navmesh.find_path ===
print("\n=== T3: navmesh.find_path start=[0,0,0] end=[1000,0,0] ===")
r = call("navmesh.find_path", {
    "start": [0.0, 0.0, 0.0],
    "end": [1000.0, 0.0, 0.0],
})
if r and r.get("ok"):
    res = r["result"]
    found = res.get("found")
    path_length = res.get("path_length")
    waypoints = res.get("waypoints")
    world = res.get("world")
    if (isinstance(found, bool)
        and isinstance(path_length, (int, float))
        and isinstance(waypoints, list)
        and world in ("editor", "pie")):
        ok("T3/find_path",
            f"found={found} path_length={path_length:.2f} waypoint_count={len(waypoints)} world={world}")
        # If found, validate each waypoint is [x,y,z]
        if found and waypoints:
            bad = [w for w in waypoints if not (isinstance(w, list) and len(w) == 3)]
            if bad:
                fail("T3/waypoint_shape", f"malformed waypoints: {bad[:3]}")
            else:
                ok("T3/waypoint_shape", f"all {len(waypoints)} waypoints are [x,y,z]")
        else:
            skip("T3/waypoint_shape", "found=false or empty (no navmesh in scope)")
    else:
        fail("T3/find_path", f"unexpected shape: {res}")
else:
    fail("T3/find_path", f"{r}")


# === T4: navmesh.rebuild ===
print("\n=== T4: navmesh.rebuild ===")
r = call("navmesh.rebuild", t=60)
if r and r.get("ok"):
    res = r["result"]
    rebuilt = res.get("rebuilt")
    duration = res.get("duration_seconds")
    nm_path = res.get("navmesh_actor_path")
    world = res.get("world")
    if (rebuilt is True
        and isinstance(duration, (int, float))
        and (nm_path is None or isinstance(nm_path, str))
        and world == "editor"):
        ok("T4/rebuild_all", f"duration_seconds={duration:.4f} nm_path={nm_path} world={world}")
    else:
        fail("T4/rebuild_all", f"unexpected: {res}")
else:
    fail("T4/rebuild_all", f"{r}")


# === T4b: navmesh.rebuild with bad actor path → -32004 ===
print("\n=== T4b: navmesh.rebuild bad navmesh_actor_path → -32004 ===")
r = call("navmesh.rebuild", {
    "navmesh_actor_path": "/Game/NotARealMap.NotARealMap:PersistentLevel.NotARealNavMesh",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T4b/rebuild_bad_path", f"correctly -32004: {r['error']['message'][:80]}")
else:
    fail("T4b/rebuild_bad_path", f"expected -32004, got {r}")


# === T5: navmesh.find_path missing required arg (no start) → -32602 ===
print("\n=== T5: navmesh.find_path missing start arg → -32602 ===")
r = call("navmesh.find_path", {
    "end": [1000.0, 0.0, 0.0],
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5/missing_start", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T5/missing_start", f"expected -32602, got {r}")


# === T5b: navmesh.find_path malformed start (not 3 elements) → -32602 ===
print("\n=== T5b: navmesh.find_path malformed start (2 elements) → -32602 ===")
r = call("navmesh.find_path", {
    "start": [0.0, 0.0],
    "end": [1000.0, 0.0, 0.0],
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5b/malformed_start", f"correctly -32602")
else:
    fail("T5b/malformed_start", f"expected -32602, got {r}")


# === T5c: navmesh.project_to_navmesh missing location → -32602 ===
print("\n=== T5c: navmesh.project_to_navmesh missing location → -32602 ===")
r = call("navmesh.project_to_navmesh", {})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5c/project_missing_location", f"correctly -32602")
else:
    fail("T5c/project_missing_location", f"expected -32602, got {r}")


print(f"\n{'=' * 60}")
print(f"WAVE G SURFACE 3 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL, {len(SKIP)} SKIP")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  - {n}")
for n in SKIP:
    print(f"  SKIP - {n}")
sys.exit(0 if not FAIL else 1)
