#!/usr/bin/env python3
"""Wave G Surface 5 test: render.list_show_flags / set_show_flag / set_engine_stat /
set_post_process_volume_property.

Test plan:
  T1: render.list_show_flags -> returns >= 100 flags with name + enabled
  T2: render.set_show_flag flag_name="Wireframe" enabled=true -> prior=false, new=true
  T3: revert: set_show_flag Wireframe enabled=false -> prior=true, new=false
  T4: render.set_engine_stat stat_name="fps" enabled=true -> ok
  T5: revert: stat fps off -> ok
  T6: set_show_flag bad flag_name -> -32004
  T7: set_show_flag missing args -> -32602
  T8: set_show_flag bad viewport_index -> -32026
  Setup PPV: actor.spawn /Script/Engine.PostProcessVolume
  T9: set_post_process_volume_property AutoExposureBias=2.5 -> success + bOverride_set=true
  T10: revert AutoExposureBias=0.0 -> success
  T11: set_post_process_volume_property with "Settings.AutoExposureBias" prefix -> success
  T12: bad volume_actor_path -> -32004
  T13: non-PPV actor (StaticMeshActor) -> -32011
  T14: bad property_path -> -32005
  Cleanup: actor.destroy PPV
"""
import json, random, socket, sys, time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020


def send(req, t=20):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + t
    while time.time() < deadline:
        try:
            c = s.recv(512 * 1024)
            if not c:
                break
            buf += c
            if b"\n" in buf:
                return json.loads(buf[: buf.index(b"\n")].decode())
        except socket.timeout:
            pass
    return None


def call(method, args=None, t=20):
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


# === T1: render.list_show_flags ===
print("=== T1: render.list_show_flags ===")
r = call("render.list_show_flags", {})
if r and r.get("ok"):
    flags = r["result"].get("flags", [])
    count = r["result"].get("count", 0)
    # UE 5.7 has well over 100 engine show flags + a handful of custom flags.
    if count >= 100 and len(flags) == count and all("name" in f and "enabled" in f for f in flags[:5]):
        # Sanity: at least Wireframe must exist.
        names = {f["name"] for f in flags}
        if "Wireframe" in names:
            ok("T1/list", f"count={count}; first 3 names: {[f['name'] for f in flags[:3]]}")
        else:
            fail("T1/list", f"expected 'Wireframe' in flag list (got {len(names)} names)")
    else:
        fail("T1/list", f"unexpected shape: count={count}, len={len(flags)}, sample={flags[:2]}")
else:
    fail("T1/list", f"{r}")


# === T2: set Wireframe = true ===
print("\n=== T2: render.set_show_flag Wireframe enabled=true ===")
r = call("render.set_show_flag", {"flag_name": "Wireframe", "enabled": True})
if r and r.get("ok"):
    res = r["result"]
    if res.get("new_enabled") is True and "prior_enabled" in res:
        ok("T2/set_wireframe_true",
           f"prior={res['prior_enabled']}, new={res['new_enabled']}")
    else:
        fail("T2/set_wireframe_true", f"unexpected: {res}")
else:
    fail("T2/set_wireframe_true", f"{r}")


# === T3: revert Wireframe = false ===
print("\n=== T3: render.set_show_flag Wireframe enabled=false (revert) ===")
r = call("render.set_show_flag", {"flag_name": "Wireframe", "enabled": False})
if r and r.get("ok"):
    res = r["result"]
    if res.get("new_enabled") is False:
        ok("T3/revert_wireframe",
           f"prior={res['prior_enabled']}, new={res['new_enabled']}")
    else:
        fail("T3/revert_wireframe", f"unexpected: {res}")
else:
    fail("T3/revert_wireframe", f"{r}")


# === T4: render.set_engine_stat fps on ===
print("\n=== T4: render.set_engine_stat fps enabled=true ===")
r = call("render.set_engine_stat", {"stat_name": "fps", "enabled": True})
if r and r.get("ok"):
    ok("T4/stat_fps_on", f"world_kind={r['result'].get('world_kind')}")
else:
    fail("T4/stat_fps_on", f"{r}")


# === T5: revert stat fps off ===
print("\n=== T5: render.set_engine_stat fps enabled=false (revert) ===")
r = call("render.set_engine_stat", {"stat_name": "fps", "enabled": False})
if r and r.get("ok"):
    ok("T5/stat_fps_off", "")
else:
    fail("T5/stat_fps_off", f"{r}")


# === T6: bad flag_name -> -32004 ===
print("\n=== T6: set_show_flag bad flag_name -> -32004 ===")
r = call("render.set_show_flag", {"flag_name": "NotARealFlag_zzz", "enabled": True})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T6/bad_flag", "correctly -32004")
else:
    fail("T6/bad_flag", f"expected -32004, got {r}")


# === T7: missing args -> -32602 ===
print("\n=== T7: set_show_flag missing args -> -32602 ===")
r = call("render.set_show_flag", {})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T7/missing_args", "correctly -32602")
else:
    fail("T7/missing_args", f"expected -32602, got {r}")


# === T8: bad viewport_index -> -32026 ===
print("\n=== T8: set_show_flag bad viewport_index -> -32026 ===")
r = call("render.set_show_flag", {"flag_name": "Wireframe", "enabled": False, "viewport_index": 999})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32026:
    ok("T8/bad_viewport_idx", "correctly -32026")
else:
    fail("T8/bad_viewport_idx", f"expected -32026, got {r}")


# === Setup: spawn a PostProcessVolume actor ===
PPV_LABEL = f"MCP_WaveG_S5_PPV_{random.randint(10000, 99999)}"
print(f"\n=== Setup: actor.spawn APostProcessVolume label={PPV_LABEL} ===")
r = call("actor.spawn", {
    "class_path": "/Script/Engine.PostProcessVolume",
    "location": {"x": 0, "y": 0, "z": 100},
    "label": PPV_LABEL,
})
ppv_path = None
if r and r.get("ok"):
    ppv_path = r["result"].get("actor_path")
    ok("setup/spawn_ppv", f"actor_path={ppv_path}")
else:
    fail("setup/spawn_ppv", f"could not spawn PPV: {r}")


# === T9: set AutoExposureBias = 2.5 ===
print(f"\n=== T9: set_post_process_volume_property AutoExposureBias=2.5 ===")
if ppv_path:
    r = call("render.set_post_process_volume_property", {
        "volume_actor_path": ppv_path,
        "property_path": "AutoExposureBias",
        "value": 2.5,
    })
    if r and r.get("ok"):
        res = r["result"]
        if res.get("bOverride_set") is True and res.get("property_path") == "AutoExposureBias":
            ok("T9/set_aeb",
               f"prior={res.get('prior_value')}, new={res.get('new_value')}, override_set=True")
        else:
            fail("T9/set_aeb", f"unexpected: {res}")
    else:
        fail("T9/set_aeb", f"{r}")
else:
    skip("T9/set_aeb", "no PPV spawned")


# === T10: revert AutoExposureBias=0 ===
print(f"\n=== T10: revert AutoExposureBias=0.0 ===")
if ppv_path:
    r = call("render.set_post_process_volume_property", {
        "volume_actor_path": ppv_path,
        "property_path": "AutoExposureBias",
        "value": 0.0,
    })
    if r and r.get("ok"):
        ok("T10/revert_aeb", f"new={r['result'].get('new_value')}")
    else:
        fail("T10/revert_aeb", f"{r}")
else:
    skip("T10/revert_aeb", "no PPV spawned")


# === T11: use "Settings." prefix in property_path ===
print(f"\n=== T11: property_path='Settings.AutoExposureBias' ===")
if ppv_path:
    r = call("render.set_post_process_volume_property", {
        "volume_actor_path": ppv_path,
        "property_path": "Settings.AutoExposureBias",
        "value": 1.0,
    })
    if r and r.get("ok"):
        res = r["result"]
        # property_path in response should be the normalised (stripped) form
        if res.get("property_path") == "AutoExposureBias":
            ok("T11/settings_prefix", f"normalised to {res['property_path']!r}")
        else:
            fail("T11/settings_prefix", f"property_path not normalised: {res}")
    else:
        fail("T11/settings_prefix", f"{r}")
else:
    skip("T11/settings_prefix", "no PPV spawned")


# === T12: bad volume_actor_path -> -32004 ===
print("\n=== T12: bad volume_actor_path -> -32004 ===")
r = call("render.set_post_process_volume_property", {
    "volume_actor_path": "NotARealActor_zzz",
    "property_path": "AutoExposureBias",
    "value": 1.0,
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T12/bad_volume_path", "correctly -32004")
else:
    fail("T12/bad_volume_path", f"expected -32004, got {r}")


# === T13: non-PPV actor (spawn a StaticMeshActor to test) -> -32011 ===
print("\n=== T13: non-PPV actor -> -32011 ===")
SMA_LABEL = f"MCP_WaveG_S5_SMA_{random.randint(10000, 99999)}"
r = call("actor.spawn", {
    "class_path": "/Script/Engine.StaticMeshActor",
    "location": {"x": 100, "y": 0, "z": 100},
    "label": SMA_LABEL,
})
sma_path = None
if r and r.get("ok"):
    sma_path = r["result"].get("actor_path")
    r2 = call("render.set_post_process_volume_property", {
        "volume_actor_path": sma_path,
        "property_path": "AutoExposureBias",
        "value": 1.0,
    })
    if r2 and not r2.get("ok") and r2.get("error", {}).get("code") == -32011:
        ok("T13/wrong_class", "correctly -32011")
    else:
        fail("T13/wrong_class", f"expected -32011, got {r2}")
    # cleanup SMA
    call("actor.destroy", {"actor_path": sma_path})
else:
    skip("T13/wrong_class", "could not spawn StaticMeshActor for test")


# === T14: bad property_path -> -32005 ===
print("\n=== T14: bad property_path -> -32005 ===")
if ppv_path:
    r = call("render.set_post_process_volume_property", {
        "volume_actor_path": ppv_path,
        "property_path": "NotARealField_zzz",
        "value": 1.0,
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32005:
        ok("T14/bad_property", "correctly -32005")
    else:
        fail("T14/bad_property", f"expected -32005, got {r}")
else:
    skip("T14/bad_property", "no PPV spawned")


# === Cleanup: destroy PPV ===
if ppv_path:
    print(f"\n=== Cleanup: actor.destroy {ppv_path} ===")
    r = call("actor.destroy", {"actor_path": ppv_path})
    print(f"  cleanup: {'ok' if r and r.get('ok') else 'failed'}")


print(f"\n{'=' * 60}")
print(f"WAVE G SURFACE 5 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL, {len(SKIP)} SKIP")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  FAIL - {n}")
for n in SKIP:
    print(f"  SKIP - {n}")
sys.exit(0 if not FAIL else 1)
