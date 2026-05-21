#!/usr/bin/env python3
"""Wave J Surface 1 test: ai.bt.* AI Behavior Tree tools (4 tools).

Tests:
  T1: ai.bt.list_assets                    → returns behavior_trees array + total_known
  T2: ai.bt.get_nodes on first BT          → returns root tree structure (SKIP if no BTs)
  T3: ai.bt.start_on_actor on pawn         → started bool (SKIP if no AAIController-bearing pawn)
  T4: ai.bt.stop_on_actor                  → stopped=true (SKIP if T3 skipped)
  T5: bad bt_path                          → -32004
  T6: bad actor_path                       → -32004
  T7: actor without AIController           → -32011 (synthesised via known non-pawn actor)
  T8: missing required arg                 → -32602
  T9: bt_path is not a UBehaviorTree class → -32011
  T10: pagination round-trip (page_size=1) → next_page_token + decode

FatumGame may have no Behavior Trees in its content (the project uses Flecs for AI rather than UE
BehaviorTrees), so T2 and beyond are SKIP-tolerant. The point of the test is to verify the tool
surface shape + error pathways, not gameplay outcomes.
"""
import json
import socket
import sys
import time

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


# === T1: ai.bt.list_assets ===
print("=== T1: ai.bt.list_assets ===")
first_bt_path = None
total_bts = 0
r = call("ai.bt.list_assets")
if r and r.get("ok"):
    res = r["result"]
    bts = res.get("behavior_trees")
    total = res.get("total_known")
    if isinstance(bts, list) and isinstance(total, int):
        ok("T1/list_assets", f"total_known={total} returned={len(bts)}")
        total_bts = total
        if bts:
            first = bts[0]
            if "asset_path" in first:
                ok("T1/entry_shape", f"asset_path={first['asset_path'][:80]}")
                first_bt_path = first["asset_path"]
            else:
                fail("T1/entry_shape", f"missing asset_path: {first}")
        else:
            skip("T1/entry_shape", "no UBehaviorTree assets in project (FatumGame uses Flecs AI)")
    else:
        fail("T1/list_assets", f"unexpected response shape: {res}")
else:
    fail("T1/list_assets", f"{r}")


# === T2: ai.bt.get_nodes on first BT ===
print("\n=== T2: ai.bt.get_nodes ===")
if first_bt_path:
    r = call("ai.bt.get_nodes", {"bt_path": first_bt_path})
    if r and r.get("ok"):
        res = r["result"]
        root = res.get("root")
        if root is None:
            skip("T2/get_nodes", f"BT {first_bt_path} has null root (newly-created, unedited)")
        elif isinstance(root, dict) and "node_class" in root and "node_name" in root:
            ok("T2/get_nodes",
               f"root_class={root['node_class']} root_name={root['node_name']} "
               f"children={len(root.get('children', []))}")
        else:
            fail("T2/get_nodes", f"unexpected root shape: {root}")
    else:
        fail("T2/get_nodes", f"{r}")
else:
    skip("T2/get_nodes", "no BT to inspect")


# === T3: ai.bt.start_on_actor — try to find an APawn in the level ===
# Discovery via level.get_persistent_level_actors (avoids exec_python brittleness).
print("\n=== T3: ai.bt.start_on_actor (discovery phase) ===")
target_actor_path = None
non_pawn_path = None

r = call("level.get_persistent_level_actors", {})
if r and r.get("ok"):
    actors = r["result"].get("actors", [])
    print(f"  scanning {len(actors)} actors in persistent level...")
    for a in actors:
        cls = a.get("class", "")
        name = a.get("name", "")
        # Pawn / Character subclasses are typical AAIController targets.
        # The path classification is heuristic — we look for known Pawn-family suffixes.
        # If our pick is wrong, ai.bt.start_on_actor surfaces -32011 and we recover via skip.
        is_pawn_like = ("Pawn" in cls or "Character" in cls or "AICharacter" in cls)
        if is_pawn_like and name and target_actor_path is None:
            target_actor_path = name
        if not is_pawn_like and name and non_pawn_path is None:
            non_pawn_path = name

if target_actor_path and first_bt_path:
    r = call("ai.bt.start_on_actor", {
        "actor_path": target_actor_path,
        "bt_path": first_bt_path,
    })
    if r and r.get("ok"):
        res = r["result"]
        if isinstance(res.get("started"), bool) and res.get("controller_path"):
            ok("T3/start_on_actor",
               f"started={res['started']} ctrl={res['controller_path'][:60]}")
        else:
            fail("T3/start_on_actor", f"unexpected shape: {res}")
    elif r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
        # Pawn-like actor exists but has no AAIController (e.g. editor-world pawn pre-PIE) —
        # legitimate result, treat as skip rather than fail since the surface is doing its job.
        skip("T3/start_on_actor",
             f"actor {target_actor_path} has no AAIController (likely editor-world pawn)")
    else:
        fail("T3/start_on_actor", f"{r}")
else:
    skip("T3/start_on_actor",
         "no Pawn-like actor in current level OR no BT to start"
         f" (actor={target_actor_path} bt={first_bt_path})")


# === T4: ai.bt.stop_on_actor ===
print("\n=== T4: ai.bt.stop_on_actor ===")
if target_actor_path:
    r = call("ai.bt.stop_on_actor", {"actor_path": target_actor_path})
    if r and r.get("ok"):
        res = r["result"]
        if isinstance(res.get("stopped"), bool) and res.get("controller_path"):
            ok("T4/stop_on_actor",
               f"stopped={res['stopped']} prior_bt={res.get('prior_active_bt', '<none>')[:60]}")
        else:
            fail("T4/stop_on_actor", f"unexpected shape: {res}")
    else:
        fail("T4/stop_on_actor", f"{r}")
else:
    skip("T4/stop_on_actor", "no target actor available")


# === T5: bad bt_path → -32004 ===
print("\n=== T5: bad bt_path → -32004 ===")
r = call("ai.bt.get_nodes", {"bt_path": "/Game/Nonexistent/BT_DoesNotExist"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T5/bad_bt_path", f"correctly -32004: {r['error']['message'][:80]}")
else:
    fail("T5/bad_bt_path", f"expected -32004, got {r}")


# === T6: bad actor_path → -32004 ===
print("\n=== T6: bad actor_path → -32004 ===")
r = call("ai.bt.stop_on_actor", {
    "actor_path": "/Game/Maps/NotReal.NotReal:PersistentLevel.NotARealActor",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T6/bad_actor_path", f"correctly -32004: {r['error']['message'][:80]}")
else:
    fail("T6/bad_actor_path", f"expected -32004, got {r}")


# === T7: actor without AIController → -32011 ===
# Use non_pawn_path discovered in T3 (any non-pawn actor in level).
print("\n=== T7: actor without AIController → -32011 ===")
if non_pawn_path:
    r = call("ai.bt.stop_on_actor", {"actor_path": non_pawn_path})
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
        ok("T7/non_pawn_no_controller", f"correctly -32011: {r['error']['message'][:80]}")
    else:
        fail("T7/non_pawn_no_controller", f"expected -32011, got {r}")
else:
    skip("T7/non_pawn_no_controller", "no non-pawn actor found in level")


# === T8: missing required arg → -32602 ===
print("\n=== T8: missing bt_path → -32602 ===")
r = call("ai.bt.get_nodes", {})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T8/missing_bt_path", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T8/missing_bt_path", f"expected -32602, got {r}")


# === T9: bt_path resolves to non-BT asset → -32011 ===
# Use any known non-BT asset. Try a few common paths in the FatumGame project.
print("\n=== T9: bt_path is not a UBehaviorTree → -32011 ===")
candidate_paths = [
    "/Engine/EngineMaterials/DefaultMaterial",  # UMaterial — always present
    "/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial",
]
found = False
for p in candidate_paths:
    r = call("ai.bt.get_nodes", {"bt_path": p})
    if r and not r.get("ok"):
        code = r.get("error", {}).get("code")
        if code == -32011:
            ok("T9/wrong_class", f"correctly -32011 for {p}: {r['error']['message'][:60]}")
            found = True
            break
        elif code == -32004:
            # Path didn't resolve — try the next candidate.
            continue
        else:
            fail("T9/wrong_class", f"expected -32011, got code={code} msg={r['error']['message']}")
            found = True
            break
if not found:
    skip("T9/wrong_class", "no known non-BT asset path resolved")


# === T10: pagination round-trip ===
print("\n=== T10: ai.bt.list_assets pagination ===")
if total_bts > 1:
    r = call("ai.bt.list_assets", {"page_size": 1})
    if r and r.get("ok"):
        res = r["result"]
        if len(res.get("behavior_trees", [])) == 1 and res.get("next_page_token"):
            ok("T10/page_1", f"got 1 entry + next_page_token={res['next_page_token'][:30]}")
            # Fetch page 2.
            r2 = call("ai.bt.list_assets", {
                "page_size": 1,
                "page_token": res["next_page_token"],
            })
            if r2 and r2.get("ok"):
                res2 = r2["result"]
                if len(res2.get("behavior_trees", [])) >= 1:
                    ok("T10/page_2", f"got {len(res2['behavior_trees'])} entry on page 2")
                else:
                    fail("T10/page_2", f"empty page 2: {res2}")
            else:
                fail("T10/page_2", f"{r2}")
        else:
            fail("T10/page_1",
                 f"expected 1 entry + next_page_token, got {len(res.get('behavior_trees', []))} entries, "
                 f"token={res.get('next_page_token')}")
    else:
        fail("T10/pagination", f"{r}")
else:
    skip("T10/pagination", f"need >1 BT for pagination round-trip (have {total_bts})")


print(f"\n{'=' * 60}")
print(f"WAVE J SURFACE 1 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL, {len(SKIP)} SKIP")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  - {n}")
for n in SKIP:
    print(f"  SKIP - {n}")
sys.exit(0 if not FAIL else 1)
