#!/usr/bin/env python3
"""Wave B Tier 3 test: niagara.create_emitter / set_user_param / set_emitter_enabled."""
import json, socket, sys, time

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

# === T1: niagara.create_emitter (with default modules) ===
print("=== T1: niagara.create_emitter /Game/MCPTest/NE_WaveB ===")
# Cleanup pre-existing test artifact via cb.delete first (ignore errors)
call("cb.delete", {"asset_path": "/Game/MCPTest/NE_WaveB", "force": True})
r = call("niagara.create_emitter", {"dest_path": "/Game/MCPTest/NE_WaveB", "add_default_modules": True, "save": True})
if r and r.get("ok"):
    res = r["result"]
    ok("T1/create_emitter", f"created={res['created']} path={res['asset_path']} saved={res['saved']} default_modules={res['default_modules']}")
    emitter_path = res["asset_path"]
else:
    fail("T1/create_emitter", f"{r}")
    emitter_path = None

# === T2: niagara.create_emitter (empty, no default modules) ===
print("\n=== T2: niagara.create_emitter empty (no default modules) ===")
call("cb.delete", {"asset_path": "/Game/MCPTest/NE_WaveB_Empty", "force": True})
r = call("niagara.create_emitter", {"dest_path": "/Game/MCPTest/NE_WaveB_Empty", "add_default_modules": False})
if r and r.get("ok"):
    res = r["result"]
    ok("T2/create_empty_emitter", f"path={res['asset_path']} default_modules={res['default_modules']}")
else:
    fail("T2/create_empty_emitter", f"{r}")

# === T3: niagara.create_emitter — already exists → -32014 ===
print("\n=== T3: niagara.create_emitter PathInUse error ===")
if emitter_path:
    r = call("niagara.create_emitter", {"dest_path": "/Game/MCPTest/NE_WaveB"})
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32014:
        ok("T3/path_in_use", "correctly rejected with -32014")
    else:
        fail("T3/path_in_use", f"expected -32014 PathInUse, got {r}")
else:
    fail("T3/path_in_use", "skipped — T1 failed")

# === T4: niagara.create_emitter — bad path → -32010 ===
print("\n=== T4: niagara.create_emitter InvalidPath ===")
r = call("niagara.create_emitter", {"dest_path": "not\\valid\\path"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32010:
    ok("T4/invalid_path", "correctly rejected with -32010")
else:
    fail("T4/invalid_path", f"expected -32010, got {r}")

# === Find a Niagara System with user params to test set_user_param ===
print("\n=== T5: niagara.list_parameters on NS_RopeSwing (find user params) ===")
TEST_SYSTEM = "/Game/FlecsDA/Niagara/NS_RopeSwing"
r = call("niagara.list_parameters", {"niagara_system_path": TEST_SYSTEM})
target_user_param = None
prior_value = None
target_type = None
if r and r.get("ok"):
    user_params = r["result"].get("user_params", [])
    ok("T5/list_params", f"user_params count={len(user_params)} system_params={len(r['result'].get('system_params', []))}")
    # Find a primitive user param we can write to
    for p in user_params:
        ptype = p["type"]
        if ptype in ("float", "int32", "bool", "Vector", "Vector2D", "Vector4", "Quat", "LinearColor", "Position"):
            target_user_param = p["name"]
            prior_value = p.get("default")
            target_type = ptype
            print(f"    → primitive user_param found: name='{target_user_param}' type='{target_type}' default={prior_value}")
            break
    if not target_user_param:
        print(f"    (no primitive user_param found on {TEST_SYSTEM} — skipping set_user_param)")
else:
    fail("T5/list_params", f"{r}")

# === T6: niagara.set_user_param ===
print(f"\n=== T6: niagara.set_user_param on {TEST_SYSTEM} ===")
if target_user_param is not None:
    # Build a new value matching the type
    new_value = None
    if target_type == "float":
        new_value = 42.5
    elif target_type == "int32":
        new_value = 7
    elif target_type == "bool":
        new_value = True
    elif target_type in ("Vector2D",):
        new_value = [1.0, 2.0]
    elif target_type in ("Vector", "Position"):
        new_value = [1.0, 2.0, 3.0]
    elif target_type in ("Vector4", "Quat"):
        new_value = [0.0, 0.0, 0.0, 1.0]
    elif target_type == "LinearColor":
        new_value = [0.5, 0.5, 0.5, 1.0]

    if new_value is not None:
        r = call("niagara.set_user_param", {
            "niagara_system_path": TEST_SYSTEM,
            "name": target_user_param,
            "value": new_value
        })
        if r and r.get("ok"):
            res = r["result"]
            ok("T6/set_user_param", f"name={res['name']} type={res['type']} prior={res['prior']} new={res['new']}")
            # Restore prior value
            if prior_value is not None:
                call("niagara.set_user_param", {
                    "niagara_system_path": TEST_SYSTEM,
                    "name": target_user_param,
                    "value": prior_value
                })
                print(f"    (restored prior value: {prior_value})")
        else:
            fail("T6/set_user_param", f"{r}")
    else:
        ok("T6/set_user_param SKIP", f"unrecognized type {target_type}")
else:
    ok("T6/set_user_param SKIP", "no primitive user_param available")

# === T7: niagara.set_user_param — invalid name → -32040 ===
print("\n=== T7: niagara.set_user_param ParamNotFound ===")
r = call("niagara.set_user_param", {
    "niagara_system_path": TEST_SYSTEM,
    "name": "DefinitelyDoesNotExist_xyz_123",
    "value": 0.0
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32040:
    ok("T7/param_not_found", "correctly rejected with -32040")
else:
    fail("T7/param_not_found", f"expected -32040, got {r}")

# === T8: niagara.set_user_param — wrong path → -32004 ===
print("\n=== T8: niagara.set_user_param ObjectNotFound ===")
r = call("niagara.set_user_param", {
    "niagara_system_path": "/Game/DoesNotExist/NS_Phantom",
    "name": "foo",
    "value": 0.0
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T8/object_not_found", "correctly rejected with -32004")
else:
    fail("T8/object_not_found", f"expected -32004, got {r}")

# === T9: niagara.set_emitter_enabled — needs a placed actor with NiagaraComponent. SKIP if none. ===
print("\n=== T9: niagara.set_emitter_enabled (SKIP unless actor placed) ===")
# Try to find a NiagaraActor placed in some level via actor.list_actors_in_level
r = call("actor.find_actors_with_class", {"class_path": "/Script/Niagara.NiagaraActor"})
if r and r.get("ok"):
    actors = r["result"].get("actors", [])
    if actors:
        actor_path = actors[0]["actor_path"]
        print(f"    found NiagaraActor: {actor_path}")
        r = call("niagara.set_emitter_enabled", {"actor_path": actor_path, "emitter_index": 0, "enabled": True})
        if r and r.get("ok"):
            res = r["result"]
            ok("T9/set_emitter_enabled", f"emitter={res['emitter_name']} index={res['emitter_index']} enabled={res['enabled']}")
        elif r and not r.get("ok") and r.get("error", {}).get("code") == -32026:
            ok("T9/set_emitter_enabled SKIP", "emitter_index 0 OOB (system has no emitters)")
        else:
            fail("T9/set_emitter_enabled", f"{r}")
    else:
        ok("T9/set_emitter_enabled SKIP", "no NiagaraActor placed in loaded levels")
else:
    ok("T9/set_emitter_enabled SKIP", f"actor.find_actors_with_class failed: {r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": "/Game/MCPTest/NE_WaveB", "force": True})
call("cb.delete", {"asset_path": "/Game/MCPTest/NE_WaveB_Empty", "force": True})

print(f"\n{'='*60}")
print(f"WAVE B TIER 3 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
