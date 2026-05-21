#!/usr/bin/env python3
"""Wave J Surface 6 test: ai.crowd.* UCrowdManager / detour-crowd RVO tools.

Mirrors the test plan in the Wave J S6 brief:
  T1: ai.crowd.get_settings        -> returns settings object (or -32603 when no crowd in map -> SKIP rest)
  T2: ai.crowd.list_agents         -> array (may be empty)
  T3: ai.crowd.set_avoidance_quality on first agent -> set=true (SKIP if no agents)
  T4: bad actor_path               -> -32004
  T5: bad quality string           -> -32602

NOTE: Default editor maps typically have no AI pawns with UCrowdFollowingComponent, AND many maps
have no NavMeshBoundsVolume so the crowd manager never instantiates. We test RESPONSE SHAPE not
gameplay outcomes — "no crowd in scope" is a legitimate SKIP state, not a failure.
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


# === T1: ai.crowd.get_settings ===
print("=== T1: ai.crowd.get_settings ===")
r = call("ai.crowd.get_settings")
crowd_manager_present = False
if r and r.get("ok"):
    res = r["result"]
    world = res.get("world")
    has_nav = res.get("has_nav_data")
    if world in ("editor", "pie") and isinstance(has_nav, bool):
        ok("T1/get_settings", f"world={world} has_nav_data={has_nav}")
        crowd_manager_present = True
        # Sanity-check the reflected scalar fields. They MAY be missing if the engine renamed a
        # UPROPERTY in a future revision — we treat missing as informational, not failure.
        sample_fields = [
            "max_agents", "max_agent_radius", "max_avoided_agents", "max_avoided_walls",
            "navmesh_check_interval", "path_optimization_interval",
            "num_avoidance_configs", "num_sampling_patterns",
        ]
        present = [f for f in sample_fields if f in res]
        if present:
            sample_vals = {k: res[k] for k in present[:4]}
            ok("T1/scalar_fields", f"present={len(present)}/{len(sample_fields)} sample={sample_vals}")
        else:
            fail("T1/scalar_fields", f"no reflected fields present at all: {res}")
    else:
        fail("T1/get_settings", f"unexpected response shape: {res}")
elif r and not r.get("ok") and r.get("error", {}).get("code") == -32603:
    # No crowd manager in scope - legitimate state. SKIP rest of tests that depend on it.
    skip("T1/get_settings", f"no UCrowdManager in current map - SKIP rest "
         f"(msg={r['error']['message'][:80]})")
else:
    fail("T1/get_settings", f"{r}")


# === T2: ai.crowd.list_agents ===
print("\n=== T2: ai.crowd.list_agents ===")
r = call("ai.crowd.list_agents")
first_agent_path = None
if r and r.get("ok"):
    res = r["result"]
    agents = res.get("agents")
    world = res.get("world")
    count = res.get("count")
    if (isinstance(agents, list) and world in ("editor", "pie") and isinstance(count, int)
            and count == len(agents)):
        ok("T2/list_agents", f"world={world} count={count}")
        if agents:
            entry = agents[0]
            required = ("owner_actor_path", "current_velocity", "avoidance_quality", "radius")
            missing = [k for k in required if k not in entry]
            if missing:
                fail("T2/entry_shape", f"missing fields: {missing} in {entry}")
            else:
                vel = entry["current_velocity"]
                if not (isinstance(vel, list) and len(vel) == 3):
                    fail("T2/entry_shape", f"current_velocity must be [x,y,z], got: {vel}")
                else:
                    ok("T2/entry_shape",
                        f"owner={entry['owner_actor_path'][:60]}... "
                        f"quality={entry['avoidance_quality']} radius={entry['radius']} "
                        f"vel={vel}")
                    first_agent_path = entry["owner_actor_path"]
        else:
            skip("T2/entry_shape", "no UCrowdFollowingComponents in current map (legitimate empty)")
    else:
        fail("T2/list_agents", f"unexpected response shape: {res}")
else:
    fail("T2/list_agents", f"{r}")


# === T3: ai.crowd.set_avoidance_quality on first agent ===
print("\n=== T3: ai.crowd.set_avoidance_quality on first agent ===")
if first_agent_path:
    r = call("ai.crowd.set_avoidance_quality", {
        "actor_path": first_agent_path,
        "quality": "High",
    })
    if r and r.get("ok"):
        res = r["result"]
        if (res.get("set") is True
                and res.get("new_quality") == "High"
                and isinstance(res.get("prior_quality"), str)
                and isinstance(res.get("actor_path"), str)):
            ok("T3/set_quality",
               f"prior={res['prior_quality']} new={res['new_quality']} "
               f"actor={res['actor_path'][:60]}...")
        else:
            fail("T3/set_quality", f"unexpected: {res}")
    else:
        fail("T3/set_quality", f"{r}")
else:
    skip("T3/set_quality", "no agent available from T2")


# === T4: bad actor_path -> -32004 ===
print("\n=== T4: ai.crowd.set_avoidance_quality bad actor_path -> -32004 ===")
r = call("ai.crowd.set_avoidance_quality", {
    "actor_path": "/Game/NotARealMap.NotARealMap:PersistentLevel.NotARealActor",
    "quality": "Medium",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T4/bad_actor_path", f"correctly -32004: {r['error']['message'][:80]}")
else:
    fail("T4/bad_actor_path", f"expected -32004, got {r}")


# === T5: bad quality string -> -32602 ===
print("\n=== T5: ai.crowd.set_avoidance_quality bad quality string -> -32602 ===")
r = call("ai.crowd.set_avoidance_quality", {
    "actor_path": first_agent_path or "FakeActor",
    "quality": "Ultra",  # not one of the 3 accepted strings
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5/bad_quality", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T5/bad_quality", f"expected -32602, got {r}")


# === T5b: missing actor_path -> -32602 ===
print("\n=== T5b: ai.crowd.set_avoidance_quality missing actor_path -> -32602 ===")
r = call("ai.crowd.set_avoidance_quality", {
    "quality": "High",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5b/missing_actor_path", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T5b/missing_actor_path", f"expected -32602, got {r}")


# === T5c: missing quality -> -32602 ===
print("\n=== T5c: ai.crowd.set_avoidance_quality missing quality -> -32602 ===")
r = call("ai.crowd.set_avoidance_quality", {
    "actor_path": "FakeActor",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5c/missing_quality", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T5c/missing_quality", f"expected -32602, got {r}")


print(f"\n{'=' * 60}")
print(f"WAVE J SURFACE 6 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL, {len(SKIP)} SKIP")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  - {n}")
for n in SKIP:
    print(f"  SKIP - {n}")
sys.exit(0 if not FAIL else 1)
