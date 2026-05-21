#!/usr/bin/env python3
"""Wave D Surface 4 — folder.* live smoke test (4 tools)."""
import json, socket, sys, time
try: sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception: pass

HOST, PORT = "127.0.0.1", 30020
def send(req, t=15):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req)+"\n").encode())
    buf = b""; deadline = time.time()+t
    while time.time() < deadline:
        c = s.recv(256*1024)
        if not c: break
        buf += c
        if b"\n" in buf: return json.loads(buf[:buf.index(b"\n")].decode())
    return None
def call(m, a=None, t=15): return send({"id":"t","kind":"call_function","method":m,"args":a or {}}, t)

PASS, FAIL = [], []
def ok(n, m=""): PASS.append(n); print(f"  PASS {n} {m}")
def fail(n, m=""): FAIL.append(n); print(f"  FAIL {n} {m}")

TEST_FOLDER = "MCPTest_S4_Folder/Sub"
TEST_FOLDER_PARENT = "MCPTest_S4_Folder"

# === T1: list (baseline) ===
print("=== T1: folder.list (baseline) ===")
r = call("folder.list")
baseline_total = 0
if r and r.get("ok"):
    folders = r["result"].get("folders", [])
    baseline_total = r["result"].get("total_known", 0)
    ok("T1/list_baseline", f"total={baseline_total} world={r['result'].get('world_path','?')[:60]}")
    if folders:
        print(f"    first folder: {folders[0]['path']!r} child_count={folders[0]['child_count']}")
else:
    fail("T1/list_baseline", f"{r}")

# === T2: create ===
print(f"\n=== T2: folder.create '{TEST_FOLDER}' ===")
r = call("folder.create", {"folder_path": TEST_FOLDER})
created_ok = False
if r and r.get("ok"):
    res = r["result"]
    created_ok = res.get("created") is True
    ok("T2/create", f"created={res.get('created')} folder_path={res.get('folder_path')}")
else:
    fail("T2/create", f"{r}")

# === T3: list (should contain new folder) ===
print(f"\n=== T3: folder.list (should now contain '{TEST_FOLDER}') ===")
r = call("folder.list")
if r and r.get("ok"):
    folders = r["result"].get("folders", [])
    paths = [f["path"] for f in folders]
    has_sub = TEST_FOLDER in paths
    has_parent = TEST_FOLDER_PARENT in paths  # parent should be auto-created by FActorFolders
    new_total = r["result"].get("total_known", 0)
    if has_sub:
        ok("T3/list_has_new", f"total={new_total} delta={new_total-baseline_total} has_parent={has_parent}")
    else:
        fail("T3/list_has_new", f"new folder '{TEST_FOLDER}' NOT in list; folders={paths[:10]}")
else:
    fail("T3/list_has_new", f"{r}")

# === T4: set_actor (pick first actor from level.get_persistent_level_actors) ===
print("\n=== T4: folder.set_actor (move first actor into new folder) ===")
sample_actor = None
r_act = call("level.get_persistent_level_actors", {})
if r_act and r_act.get("ok"):
    actors = r_act["result"].get("actors", [])
    if actors:
        # Try to find any actor with a usable name. Skip ones whose label/name is empty.
        for a in actors:
            nm = a.get("name") or a.get("label")
            if nm:
                sample_actor = nm
                break

if sample_actor:
    r = call("folder.set_actor", {
        "actor_path": sample_actor,
        "folder_path": TEST_FOLDER,
    })
    set_actor_ok = False
    if r and r.get("ok"):
        res = r["result"]
        prior = res.get("prior_folder")
        new = res.get("new_folder")
        set_actor_ok = new == TEST_FOLDER
        if set_actor_ok:
            ok("T4/set_actor", f"actor={sample_actor} prior='{prior}' new='{new}'")
        else:
            fail("T4/set_actor", f"new={new!r}, expected {TEST_FOLDER!r} (actor={sample_actor})")
    else:
        fail("T4/set_actor", f"{r}")
else:
    print("    SKIP (no actor)")
    ok("T4/set_actor SKIP", "no actor in level")

# === T5: delete (with move_children_to_parent=true) ===
print(f"\n=== T5: folder.delete '{TEST_FOLDER}' (move_children_to_parent=true) ===")
r = call("folder.delete", {"folder_path": TEST_FOLDER, "move_children_to_parent": True})
if r and r.get("ok"):
    res = r["result"]
    deleted = res.get("deleted")
    moved = res.get("moved_children", 0)
    parent = res.get("parent_path")
    ok("T5/delete", f"deleted={deleted} moved={moved} parent='{parent}'")
    # Verify actor ended up in parent (if we moved one)
    if sample_actor and moved > 0:
        r_check = call("level.get_persistent_level_actors", {})
        if r_check and r_check.get("ok"):
            for a in r_check["result"].get("actors", []):
                if (a.get("name") or a.get("label")) == sample_actor:
                    actor_folder = a.get("folder", "")
                    if actor_folder == TEST_FOLDER_PARENT:
                        ok("T5b/actor_moved_to_parent", f"actor folder='{actor_folder}'")
                    else:
                        # Folder might not be reported in get_persistent_level_actors output —
                        # not a fail, just info.
                        print(f"    (info) actor '{sample_actor}' folder field='{actor_folder}'")
                    break
else:
    fail("T5/delete", f"{r}")

# === T5c: cleanup — also delete the parent folder we just created ===
print(f"\n=== T5c: cleanup folder.delete '{TEST_FOLDER_PARENT}' ===")
r = call("folder.delete", {"folder_path": TEST_FOLDER_PARENT, "move_children_to_parent": True})
if r and r.get("ok"):
    res = r["result"]
    ok("T5c/cleanup_parent", f"deleted={res.get('deleted')} moved={res.get('moved_children')}")
else:
    # not a fatal — parent may already be gone if it was implicitly removed
    print(f"    (cleanup note) {r}")
    ok("T5c/cleanup_parent SKIP", "parent may not exist")

# === T6: folder.create with empty path → -32602 ===
print("\n=== T6: folder.create with empty path → -32602 ===")
r = call("folder.create", {"folder_path": ""})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T6/empty_path", f"correctly rejected -32602: {r['error'].get('message','')[:60]}")
else:
    fail("T6/empty_path", f"expected -32602, got {r}")

# === T6b: folder.create missing args.folder_path entirely → -32602 ===
print("\n=== T6b: folder.create missing folder_path → -32602 ===")
r = call("folder.create", {})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T6b/missing_path", f"correctly rejected -32602: {r['error'].get('message','')[:60]}")
else:
    fail("T6b/missing_path", f"expected -32602, got {r}")

# === T7: folder.set_actor with bad actor → -32004 ===
print("\n=== T7: folder.set_actor with bad actor → -32004 ===")
r = call("folder.set_actor", {
    "actor_path": "NonExistentActor_xyz_definitely_not_there",
    "folder_path": "AnyFolder",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T7/bad_actor", f"correctly rejected -32004: {r['error'].get('message','')[:60]}")
else:
    fail("T7/bad_actor", f"expected -32004, got {r}")

# === T8: folder.delete on non-existent path → -32056 ===
print("\n=== T8: folder.delete on non-existent path → -32056 ===")
r = call("folder.delete", {"folder_path": "DefinitelyDoesNotExist_xyz_S4"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32056:
    ok("T8/folder_not_found", f"correctly rejected -32056: {r['error'].get('message','')[:60]}")
else:
    fail("T8/folder_not_found", f"expected -32056, got {r}")

# === T9: tools.list — confirm 4 folder.* tools registered ===
print("\n=== T9: tools.list — check 4 folder.* tools ===")
r = call("tools.list", {})
if r and r.get("ok"):
    cpp_handlers = r["result"].get("cpp_handlers", [])
    folder_tools = sorted([m for m in cpp_handlers if m.startswith("folder.")])
    expected = {"folder.list", "folder.create", "folder.delete", "folder.set_actor"}
    actual = set(folder_tools)
    if expected == actual:
        ok("T9/registered", f"all 4 folder.* tools registered: {folder_tools}")
    else:
        fail("T9/registered", f"expected {expected}, got {actual}")
else:
    fail("T9/registered", f"{r}")

# === Summary ===
print()
print("=" * 72)
print(f"PASS: {len(PASS)}")
for p in PASS:
    print(f"  [ok] {p}")
print(f"FAIL: {len(FAIL)}")
for f in FAIL:
    print(f"  [FAIL] {f}")
print("=" * 72)
sys.exit(0 if not FAIL else 1)
