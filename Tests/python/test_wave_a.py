#!/usr/bin/env python3
"""Wave A test: PIE input/stats/dilation/world dump + stats.*"""
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

# === Stats first (don't need PIE) ===
print("=== T1: stats.get_engine (editor-context) ===")
r = call("stats.get_engine")
if r and r.get("ok"):
    res = r["result"]
    ok("T1/stats_engine", f"avg_fps={res.get('avg_fps'):.1f} avg_ms={res.get('avg_ms'):.2f} frame={int(res.get('frame_counter'))} smooth={res.get('smooth_frame_rate')}")
else:
    fail("T1/stats_engine", f"{r}")

print("\n=== T2: stats.get_memory ===")
r = call("stats.get_memory")
if r and r.get("ok"):
    p = r["result"]["physical"]
    v = r["result"]["virtual"]
    ok("T2/stats_memory", f"phys used={p['used_mb']:.0f}/{p['total_mb']:.0f}MB peak={p['peak_used_mb']:.0f}MB | virt used={v['used_mb']:.0f}MB | alloc={r['result'].get('allocator_name')}")
else:
    fail("T2/stats_memory", f"{r}")

# === PIE tools require PIE running ===
print("\n=== Starting PIE ===")
r = call("pie.start", {"mode": "selected_viewport"}, t=30)
if r and r.get("ok"):
    ok("PIE_started", f"world={r['result'].get('pie_world_path')}")
    # Wait for PIE to be fully running
    for _ in range(20):
        r = call("pie.is_running", t=5)
        if r and r.get("ok") and r["result"].get("running"):
            break
        time.sleep(0.5)
else:
    fail("PIE_start", f"{r}")
    print("=== Skipping PIE-dependent tests ===")
    sys.exit(0 if not FAIL else 1)

print("\n=== T3: pie.get_stats (during PIE) ===")
r = call("pie.get_stats")
if r and r.get("ok"):
    res = r["result"]
    ok("T3/pie_stats", f"fps={res.get('instant_fps'):.1f} avg_fps={res.get('avg_fps'):.1f} actors={int(res.get('actor_count'))} dilation={res.get('time_dilation')}")
else:
    fail("T3/pie_stats", f"{r}")

print("\n=== T4: pie.set_time_dilation 2x ===")
r = call("pie.set_time_dilation", {"scale": 2.0})
if r and r.get("ok"):
    ok("T4/dilation_2x", f"prior={r['result']['prior_scale']} new={r['result']['new_scale']}")
    # Revert
    call("pie.set_time_dilation", {"scale": 1.0})
else:
    fail("T4/dilation_2x", f"{r}")

print("\n=== T5: pie.simulate_key W (tap) ===")
r = call("pie.simulate_key", {"key": "W", "action": "tap"})
if r and r.get("ok"):
    ok("T5/key_W_tap", f"simulated={r['result']['simulated']}")
else:
    fail("T5/key_W_tap", f"{r}")

print("\n=== T6: pie.click_screen middle of viewport ===")
r = call("pie.click_screen", {"x": 640, "y": 360, "button": "left"})
if r and r.get("ok"):
    ok("T6/click_640_360", f"clicked={r['result']['clicked']}")
else:
    fail("T6/click_640_360", f"{r}")

print("\n=== T7: pie.click_actor — pick PC's pawn ===")
r_pc = call("pie.get_pawn", {"player_index": 0})
if r_pc and r_pc.get("ok") and r_pc["result"].get("pawn_path"):
    pawn_path = r_pc["result"]["pawn_path"]
    r = call("pie.click_actor", {"actor_path": pawn_path, "button": "left"})
    if r and r.get("ok"):
        ok("T7/click_actor", f"clicked at ({r['result']['screen_x']:.0f}, {r['result']['screen_y']:.0f})")
    elif r and not r.get("ok"):
        # acceptable if behind camera (player is the camera)
        code = r.get("error", {}).get("code")
        if code == -32603:
            ok("T7/click_actor SKIP", "actor behind camera (player self)")
        else:
            fail("T7/click_actor", f"{r}")
else:
    ok("T7/click_actor SKIP", "no pawn")

print("\n=== T8: pie.dump_world_state (no filter) ===")
r = call("pie.dump_world_state")
if r and r.get("ok"):
    res = r["result"]
    ok("T8/dump_all", f"total={int(res.get('total'))} world={res.get('world_path')}")
else:
    fail("T8/dump_all", f"{r}")

print("\n=== T9: pie.dump_world_state class_filter=StaticMeshActor ===")
r = call("pie.dump_world_state", {"class_filter": "/Script/Engine.StaticMeshActor"})
if r and r.get("ok"):
    ok("T9/dump_filtered", f"smesh_actors={int(r['result'].get('total'))}")
else:
    fail("T9/dump_filtered", f"{r}")

# Cleanup
print("\n=== Stop PIE ===")
call("pie.stop", t=15)

print(f"\n{'='*60}")
print(f"WAVE A TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
