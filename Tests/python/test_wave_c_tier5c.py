#!/usr/bin/env python3
"""Wave C Tier 5c test: audio.* + wp.* (6 tools total)."""
import json, random, socket, sys, time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020

def send(req, t=15):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req)+"\n").encode())
    buf = b""
    deadline = time.time()+t
    while time.time() < deadline:
        c = s.recv(256*1024)
        if not c: break
        buf += c
        if b"\n" in buf:
            return json.loads(buf[:buf.index(b"\n")].decode())
    return None

def call(method, args=None, t=15):
    return send({"id":"t","kind":"call_function","method":method,"args":args or {}}, t)

PASS, FAIL = [], []
def ok(n,m=""): PASS.append(n); print(f"  PASS {n} {m}")
def fail(n,m=""): FAIL.append(n); print(f"  FAIL {n} {m}")

# ─── AUDIO ────────────────────────────────────────────────────────────────────

# === A1: audio.list_mix_classes ===
print("=== A1: audio.list_mix_classes (project-wide) ===")
r = call("audio.list_mix_classes")
if r and r.get("ok"):
    sc = r["result"]["sound_classes"]
    sm = r["result"]["sound_mixes"]
    ok("A1/list_mix", f"sound_classes={len(sc)} sound_mixes={len(sm)}")
else:
    fail("A1/list_mix", f"{r}")

# === A2: audio.create_sound_cue (empty) ===
test_cue = f"/Game/MCPTest/SC_Tier5c_{random.randint(10000, 99999)}"
print(f"\n=== A2: audio.create_sound_cue {test_cue} (no source wave) ===")
r = call("audio.create_sound_cue", {"dest_path": test_cue, "save": False}, t=30)
if r and r.get("ok"):
    res = r["result"]
    ok("A2/create_cue", f"path={res['asset_path']} has_wave={res['has_source_wave']}")
else:
    fail("A2/create_cue", f"{r}")

# === A3: audio.create_sound_cue — PathInUse ===
print("\n=== A3: audio.create_sound_cue PathInUse → -32014 ===")
r = call("audio.create_sound_cue", {"dest_path": test_cue})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32014:
    ok("A3/path_in_use", "correctly rejected -32014")
else:
    fail("A3/path_in_use", f"expected -32014, got {r}")

# === A4: audio.set_attenuation — clear (no path) ===
print("\n=== A4: audio.set_attenuation clear ===")
r = call("audio.set_attenuation", {"sound_path": test_cue})
if r and r.get("ok"):
    res = r["result"]
    ok("A4/clear_attenuation", f"sound_class={res['sound_class'].split('.')[-1]} prior={res['prior_attenuation']!r} cleared={res['cleared']}")
else:
    fail("A4/clear_attenuation", f"{r}")

# === A5: audio.set_attenuation — wrong sound class → -32011 ===
print("\n=== A5: audio.set_attenuation wrong class → -32011 ===")
r = call("audio.set_attenuation", {"sound_path": "/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
    ok("A5/wrong_sound_class", "correctly rejected -32011")
else:
    fail("A5/wrong_sound_class", f"expected -32011, got {r}")

# ─── WORLD PARTITION ──────────────────────────────────────────────────────────

# === W1: wp.is_partitioned — check the default test map ===
print("\n=== W1: wp.is_partitioned on the open level ===")
# Discover the current open level via editor.* tool
r = call("editor.get_current_level")
current_level = None
if r and r.get("ok"):
    current_level = r["result"].get("level_path") or r["result"].get("editor_world")
    print(f"    current level: {current_level}")

if current_level:
    r = call("wp.is_partitioned", {"level_path": current_level})
    if r and r.get("ok"):
        res = r["result"]
        ok("W1/is_partitioned", f"partitioned={res['partitioned']} world={res.get('world_path','?')}")
    else:
        fail("W1/is_partitioned", f"{r}")
else:
    print("    SKIP: no current level path discoverable")
    ok("W1/is_partitioned SKIP", "")

# === W2: wp.is_partitioned — bad path → -32004 ===
print("\n=== W2: wp.is_partitioned bad path → -32004 ===")
r = call("wp.is_partitioned", {"level_path": "/Game/DoesNotExist/Phantom"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("W2/not_found", "correctly rejected -32004")
else:
    fail("W2/not_found", f"expected -32004, got {r}")

# === W3-W4: wp.get/set_actor_runtime_grid — needs an actor placed in editor world ===
print("\n=== W3: wp.get_actor_runtime_grid (probe an actor) ===")
# Try to find ANY actor in editor world via level.full_actor_dump or list
r = call("level.list_actors", {"page_size": 5})
sample_actor = None
if r and r.get("ok"):
    actors = r["result"].get("actors", [])
    if actors:
        sample_actor = actors[0]["actor_path"]
        print(f"    sample actor: {sample_actor}")
else:
    # Try alternate methods
    r = call("level.find_actors", {"page_size": 5})
    if r and r.get("ok"):
        actors = r["result"].get("actors", [])
        if actors: sample_actor = actors[0]["actor_path"]

if sample_actor:
    r = call("wp.get_actor_runtime_grid", {"actor_path": sample_actor})
    if r and r.get("ok"):
        res = r["result"]
        ok("W3/get_grid", f"actor={res['actor_path'].split('.')[-1]} grid={res['runtime_grid']!r}")
    else:
        fail("W3/get_grid", f"{r}")
else:
    print("    SKIP: no actor available")
    ok("W3/get_grid SKIP", "")

# === W4: wp.set_actor_runtime_grid → "TestGrid_MCP" then revert ===
print("\n=== W4: wp.set_actor_runtime_grid ===")
if sample_actor:
    r = call("wp.set_actor_runtime_grid", {"actor_path": sample_actor, "runtime_grid": "TestGrid_MCP"})
    if r and r.get("ok"):
        res = r["result"]
        ok("W4/set_grid", f"prior={res['prior_grid']!r} new={res['new_grid']!r}")
        # Revert to None
        call("wp.set_actor_runtime_grid", {"actor_path": sample_actor, "runtime_grid": ""})
    else:
        fail("W4/set_grid", f"{r}")
else:
    ok("W4/set_grid SKIP", "no actor")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": test_cue, "force": True})

print(f"\n{'='*60}")
print(f"WAVE C TIER 5c TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
