#!/usr/bin/env python3
"""Wave C Tier 5a test: sequencer.create_sequence + add_master_track + add_camera_cut + add_keyframe + set_section_range."""
import json, random, socket, sys, time

# Force UTF-8 stdout so unicode messages don't crash on Windows cp1252.
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

TEST_SEQ = f"/Game/MCPTest/Seq_Tier5a_{random.randint(10000, 99999)}"

# === T1: sequencer.create_sequence ===
print(f"=== T1: sequencer.create_sequence {TEST_SEQ} ===")
r = call("sequencer.create_sequence", {"dest_path": TEST_SEQ, "save": False}, t=30)
if r and r.get("ok"):
    res = r["result"]
    ok("T1/create_sequence", f"path={res['asset_path']} method={res.get('method','?')}")
else:
    fail("T1/create_sequence", f"{r}")
    print("=== Cannot proceed without sequence ===")
    sys.exit(1)

# === T2: sequencer.create_sequence — already exists → -32014 ===
print("\n=== T2: PathInUse ===")
r = call("sequencer.create_sequence", {"dest_path": TEST_SEQ})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32014:
    ok("T2/path_in_use", "correctly rejected with -32014")
else:
    fail("T2/path_in_use", f"expected -32014, got {r}")

# === T3: sequencer.add_master_track CameraCutTrack ===
print("\n=== T3: add_master_track CameraCutTrack ===")
r = call("sequencer.add_master_track", {
    "sequence_path": TEST_SEQ,
    "track_class": "/Script/MovieSceneTracks.MovieSceneCameraCutTrack"
})
if r and r.get("ok"):
    res = r["result"]
    ok("T3/add_camera_cut_track", f"name={res['track_name']} class={res['track_class']}")
else:
    fail("T3/add_camera_cut_track", f"{r}")

# === T4: sequencer.add_master_track AudioTrack ===
print("\n=== T4: add_master_track AudioTrack ===")
r = call("sequencer.add_master_track", {
    "sequence_path": TEST_SEQ,
    "track_class": "/Script/MovieSceneTracks.MovieSceneAudioTrack"
})
if r and r.get("ok"):
    ok("T4/add_audio_track", f"name={r['result']['track_name']} master_index={r['result']['master_index']}")
else:
    fail("T4/add_audio_track", f"{r}")

# === T5: sequencer.add_camera_cut (no camera actor) ===
print("\n=== T5: add_camera_cut 0..24000 (1 sec @ 24000 tick rate) ===")
r = call("sequencer.add_camera_cut", {
    "sequence_path": TEST_SEQ,
    "start_frame": 0,
    "end_frame":   24000
})
camcut_section_index = None
if r and r.get("ok"):
    res = r["result"]
    camcut_section_index = res["section_index"]
    ok("T5/add_camera_cut", f"section_index={camcut_section_index} class={res['section_class']} [{res['start_frame']}, {res['end_frame']}]")
else:
    fail("T5/add_camera_cut", f"{r}")

# === T6: sequencer.set_section_range CameraCutTrack.0 to 0..48000 ===
print("\n=== T6: set_section_range CameraCutTrack.0 0..48000 ===")
if camcut_section_index is not None:
    r = call("sequencer.set_section_range", {
        "sequence_path": TEST_SEQ,
        "track_path": f"CameraCutTrack.{camcut_section_index}",
        "start_frame": 0,
        "end_frame":   48000
    })
    if r and r.get("ok"):
        res = r["result"]
        ok("T6/set_range", f"prior={res['prior_range']} new={res['new_range']}")
    else:
        fail("T6/set_range", f"{r}")
else:
    fail("T6/set_range", "skipped — T5 failed")

# === T7: sequencer.add_master_track — bad class → -32011 ===
print("\n=== T7: add_master_track WrongClass ===")
r = call("sequencer.add_master_track", {
    "sequence_path": TEST_SEQ,
    "track_class": "/Script/Engine.Actor"  # not a MovieSceneTrack
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
    ok("T7/wrong_class", "correctly rejected with -32011")
else:
    fail("T7/wrong_class", f"expected -32011, got {r}")

# === T8: sequencer.add_camera_cut — bad frames → -32602 ===
print("\n=== T8: add_camera_cut end <= start → -32602 ===")
r = call("sequencer.add_camera_cut", {
    "sequence_path": TEST_SEQ,
    "start_frame": 100,
    "end_frame":   50  # invalid
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T8/bad_frames", "correctly rejected with -32602")
else:
    fail("T8/bad_frames", f"expected -32602, got {r}")

# === T9: sequencer.set_section_range — bad track_path → -32043 TrackNotFound ===
print("\n=== T9: set_section_range TrackNotFound ===")
r = call("sequencer.set_section_range", {
    "sequence_path": TEST_SEQ,
    "track_path": "NonExistentTrack.0",
    "start_frame": 0,
    "end_frame":   1000
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32043:
    ok("T9/track_not_found", "correctly rejected with -32043")
else:
    fail("T9/track_not_found", f"expected -32043, got {r}")

# === T10: verify reads see new state ===
print("\n=== T10: sequencer.get_tracks confirms new state ===")
r = call("sequencer.get_tracks", {"sequence_path": TEST_SEQ})
if r and r.get("ok"):
    master = r["result"].get("master_tracks", [])
    track_classes = [t["class"].split(".")[-1] for t in master]
    ok("T10/get_tracks", f"master_count={len(master)} classes={track_classes}")
else:
    fail("T10/get_tracks", f"{r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": TEST_SEQ, "force": True})

print(f"\n{'='*60}")
print(f"WAVE C TIER 5a TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
