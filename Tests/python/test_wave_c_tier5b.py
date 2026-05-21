#!/usr/bin/env python3
"""Wave C Tier 5b test: anim.list_sequences + create_montage + add_section + add_notify + set_blend_mode."""
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

# === T1: anim.list_sequences ===
print("=== T1: anim.list_sequences (find AnimSequences in project) ===")
r = call("anim.list_sequences", {"page_size": 50})
source_seq = None
if r and r.get("ok"):
    seqs = r["result"]["sequences"]
    ok("T1/list_sequences", f"total_known={r['result']['total_known']} returned={len(seqs)}")
    if seqs:
        source_seq = seqs[0]["asset_path"]
        print(f"    first: {source_seq} length={seqs[0].get('sequence_length','?')} skeleton={seqs[0].get('skeleton_path','?')}")
else:
    fail("T1/list_sequences", f"{r}")

if not source_seq:
    print("=== Cannot proceed without an AnimSequence in the project ===")
    sys.exit(1 if FAIL else 0)

# === T2: anim.create_montage from source ===
test_montage = f"/Game/MCPTest/AM_Tier5b_{random.randint(10000, 99999)}"
print(f"\n=== T2: anim.create_montage {test_montage} ===")
r = call("anim.create_montage", {
    "dest_path": test_montage,
    "source_sequence_path": source_seq,
    "save": False
}, t=30)
if r and r.get("ok"):
    res = r["result"]
    ok("T2/create_montage", f"path={res['asset_path']} skeleton={res['skeleton_path']} source_length={res['source_length']:.3f}")
else:
    fail("T2/create_montage", f"{r}")
    sys.exit(1)

# === T3: anim.add_section ===
print("\n=== T3: anim.add_section 'IntroSection' at 0.5s ===")
r = call("anim.add_section", {
    "montage_path": test_montage,
    "section_name": "IntroSection",
    "start_time": 0.5
})
if r and r.get("ok"):
    res = r["result"]
    ok("T3/add_section", f"index={res['section_index']} total={res['section_count']}")
else:
    fail("T3/add_section", f"{r}")

# === T4: anim.add_notify ===
print("\n=== T4: anim.add_notify 'FootStep_R' at 0.2s ===")
r = call("anim.add_notify", {
    "montage_path": test_montage,
    "notify_name": "FootStep_R",
    "time": 0.2
})
if r and r.get("ok"):
    res = r["result"]
    ok("T4/add_notify", f"notify_index={res['notify_index']} total={res['total_notifies']} track={res['track_index']}")
else:
    fail("T4/add_notify", f"{r}")

# === T5: anim.add_notify with duration (state notify) ===
print("\n=== T5: anim.add_notify 'ChargeWindow' at 0.4s duration=0.3 ===")
r = call("anim.add_notify", {
    "montage_path": test_montage,
    "notify_name": "ChargeWindow",
    "time": 0.4,
    "duration": 0.3
})
if r and r.get("ok"):
    ok("T5/add_notify_state", f"index={r['result']['notify_index']}")
else:
    fail("T5/add_notify_state", f"{r}")

# === T6: anim.set_blend_mode ===
print("\n=== T6: anim.set_blend_mode in=0.15 out=0.40 ===")
r = call("anim.set_blend_mode", {
    "montage_path": test_montage,
    "blend_in_time": 0.15,
    "blend_out_time": 0.40
})
if r and r.get("ok"):
    res = r["result"]
    ok("T6/set_blend_mode", f"prior_in={res['prior_blend_in']:.3f} prior_out={res['prior_blend_out']:.3f} new_in={res['new_blend_in']:.3f} new_out={res['new_blend_out']:.3f}")
else:
    fail("T6/set_blend_mode", f"{r}")

# === T7: anim.create_montage — bad source → -32011 ===
print("\n=== T7: create_montage wrong source class → -32011 ===")
r = call("anim.create_montage", {
    "dest_path": "/Game/MCPTest/AM_Tier5b_Bad",
    "source_sequence_path": "/Game/MCPTest/" + test_montage.split("/")[-1]  # the montage we just made, not a sequence
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
    ok("T7/wrong_source", "correctly rejected with -32011")
else:
    fail("T7/wrong_source", f"expected -32011, got {r}")

# === T8: anim.add_section bad time ===
print("\n=== T8: add_section start_time<0 → -32602 ===")
r = call("anim.add_section", {
    "montage_path": test_montage,
    "section_name": "Bad",
    "start_time": -1.0
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T8/bad_time", "correctly rejected with -32602")
else:
    fail("T8/bad_time", f"expected -32602, got {r}")

# === T9: anim.add_notify nonexistent track → -32055 ===
print("\n=== T9: add_notify with bad track_name → -32055 ===")
r = call("anim.add_notify", {
    "montage_path": test_montage,
    "notify_name": "Foo",
    "time": 0.1,
    "notify_track_name": "DoesNotExistTrack"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32055:
    ok("T9/track_not_found", "correctly rejected with -32055")
else:
    fail("T9/track_not_found", f"expected -32055, got {r}")

# === T10: anim.list_sequences with path_prefix filter ===
print("\n=== T10: anim.list_sequences path_prefix='/Game/package/Animation' ===")
r = call("anim.list_sequences", {"path_prefix": "/Game/package/Animation"})
if r and r.get("ok"):
    ok("T10/list_filtered", f"total_known={r['result']['total_known']}")
else:
    fail("T10/list_filtered", f"{r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": test_montage, "force": True})

print(f"\n{'='*60}")
print(f"WAVE C TIER 5b TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
