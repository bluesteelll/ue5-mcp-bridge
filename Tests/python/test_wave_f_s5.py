#!/usr/bin/env python3
"""Wave F Surface 5 test: bp.set_variable_metadata / bp.list_categories /
bp.add_comment / bp.delete_comment.

Mirrors the test plan in the Wave F5 brief. Uses an ephemeral /Game/MCPTest BP, exercises
each new tool with both happy-path and error-path inputs, then cleans up.
"""
import json, random, socket, sys, time

# Force UTF-8 stdout so messages with arrows / accented chars don't crash on Windows cp1252.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020


def send(req, t=15):
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


def call(method, args=None, t=15):
    return send({"id": "t", "kind": "call_function", "method": method, "args": args or {}}, t)


PASS, FAIL = [], []


def ok(n, m=""):
    PASS.append(n)
    print(f"  PASS {n} {m}")


def fail(n, m=""):
    FAIL.append(n)
    print(f"  FAIL {n} {m}")


# === Setup: create a fresh Actor BP + two variables ===
TEST_BP = f"/Game/MCPTest/BP_WaveF_S5_{random.randint(10000, 99999)}"
print(f"=== Setup: create test blueprint {TEST_BP} (parent=Actor) ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})
r = call("bp.create_blueprint",
         {"dest_path": TEST_BP, "parent_class_path": "/Script/Engine.Actor", "save": False})
if not (r and r.get("ok")):
    fail("setup/create_bp", f"{r}")
    print("=== Cannot proceed without test BP ===")
    sys.exit(1)
ok("setup/create_bp", f"path={r['result']['asset_path']}")

# Add Var1 (Real)
r = call("bp.add_variable", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var1",
    "pin_type": {"category": "Real", "subcategory": "double"},
})
if r and r.get("ok"):
    ok("setup/add_var1", "Real")
else:
    fail("setup/add_var1", f"{r}")
    sys.exit(1)

# Add Var2 (Bool)
r = call("bp.add_variable", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var2",
    "pin_type": {"category": "Boolean"},
})
if r and r.get("ok"):
    ok("setup/add_var2", "Boolean")
else:
    fail("setup/add_var2", f"{r}")
    sys.exit(1)


# === T1: bp.set_variable_metadata Var1 — full metadata block ===
print(f"\n=== T1: bp.set_variable_metadata Var1 category=Stats tooltip='health value' edit_anywhere=true expose_on_spawn=true ===")
r = call("bp.set_variable_metadata", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var1",
    "metadata": {
        "category": "Stats",
        "tooltip": "health value",
        "edit_anywhere": True,
        "expose_on_spawn": True,
        "save_game": True,
        "transient": False,
    },
})
if r and r.get("ok"):
    prior = r["result"]["prior"]
    new = r["result"]["new"]
    # Validate "new" reflects what we asked for.
    if (new.get("category") == "Stats"
            and new.get("tooltip") == "health value"
            and new.get("edit_anywhere") is True
            and new.get("expose_on_spawn") is True
            and new.get("save_game") is True):
        ok("T1/set_metadata", f"new={new}")
    else:
        fail("T1/set_metadata", f"new mismatch: {new}")
else:
    fail("T1/set_metadata", f"{r}")

# === T1b: bp.get_variable -> confirm metadata persisted ===
print(f"\n=== T1b: bp.get_variable Var1 -> reflects new metadata ===")
r = call("bp.get_variable", {"blueprint_path": TEST_BP, "variable_name": "Var1"})
if r and r.get("ok"):
    v = r["result"]["variable"]
    if (v.get("category_group") == "Stats"
            and v.get("tooltip") == "health value"
            and v.get("exposed_on_spawn") is True):
        ok("T1b/get_variable", f"category={v['category_group']!r} expose_on_spawn={v['exposed_on_spawn']}")
    else:
        fail("T1b/get_variable", f"unexpected: {v}")
else:
    fail("T1b/get_variable", f"{r}")

# === T2: bp.list_categories -> contains "Stats" (and other engine defaults) ===
print(f"\n=== T2: bp.list_categories -> contains Stats ===")
r = call("bp.list_categories", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    cats = r["result"]["categories"]
    if isinstance(cats, list) and "Stats" in cats:
        ok("T2/list_categories", f"categories={cats}")
    else:
        fail("T2/list_categories", f"'Stats' missing from {cats}")
else:
    fail("T2/list_categories", f"{r}")

# === T3: bp.set_variable_metadata Var2 -> replicate=rep_notify with rep_notify_function ===
print(f"\n=== T3: bp.set_variable_metadata Var2 replicate=rep_notify rep_notify_function=OnRep_Var2 ===")
r = call("bp.set_variable_metadata", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var2",
    "metadata": {
        "replicate": "rep_notify",
        "rep_notify_function": "OnRep_Var2",
    },
})
if r and r.get("ok"):
    new = r["result"]["new"]
    if new.get("replicate") == "rep_notify" and new.get("rep_notify_function") == "OnRep_Var2":
        ok("T3/replicate_rep_notify", f"new={new}")
    else:
        fail("T3/replicate_rep_notify", f"unexpected: {new}")
else:
    fail("T3/replicate_rep_notify", f"{r}")

# === T3b: replicate=replicated clears rep_notify ===
print(f"\n=== T3b: bp.set_variable_metadata Var2 replicate=replicated -> clears RepNotify ===")
r = call("bp.set_variable_metadata", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var2",
    "metadata": {"replicate": "replicated"},
})
if r and r.get("ok"):
    new = r["result"]["new"]
    if new.get("replicate") == "replicated" and new.get("rep_notify_function") == "":
        ok("T3b/replicate_replicated", f"new={new}")
    else:
        fail("T3b/replicate_replicated", f"unexpected: {new}")
else:
    fail("T3b/replicate_replicated", f"{r}")

# === T3c: replicate=none clears CPF_Net ===
print(f"\n=== T3c: bp.set_variable_metadata Var2 replicate=none ===")
r = call("bp.set_variable_metadata", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var2",
    "metadata": {"replicate": "none"},
})
if r and r.get("ok"):
    new = r["result"]["new"]
    if new.get("replicate") == "none":
        ok("T3c/replicate_none", f"new={new}")
    else:
        fail("T3c/replicate_none", f"unexpected: {new}")
else:
    fail("T3c/replicate_none", f"{r}")

# === T3d: bad 'replicate' value -> -32602 InvalidParams ===
print(f"\n=== T3d: bp.set_variable_metadata replicate='garbage' -> -32602 ===")
r = call("bp.set_variable_metadata", {
    "blueprint_path": TEST_BP,
    "variable_name": "Var2",
    "metadata": {"replicate": "garbage"},
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T3d/bad_replicate", f"correctly -32602: {r['error']['message'][:100]}")
else:
    fail("T3d/bad_replicate", f"expected -32602, got {r}")

# === T3e: missing metadata object -> -32602 ===
print(f"\n=== T3e: bp.set_variable_metadata missing 'metadata' -> -32602 ===")
r = call("bp.set_variable_metadata", {"blueprint_path": TEST_BP, "variable_name": "Var1"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T3e/missing_metadata", f"correctly -32602")
else:
    fail("T3e/missing_metadata", f"expected -32602, got {r}")

# === T3f: unknown variable -> -32037 VariableNotFound ===
print(f"\n=== T3f: bp.set_variable_metadata variable_name='Bogus' -> -32037 ===")
r = call("bp.set_variable_metadata", {
    "blueprint_path": TEST_BP,
    "variable_name": "Bogus",
    "metadata": {"category": "Whatever"},
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32037:
    ok("T3f/unknown_var", f"correctly -32037")
else:
    fail("T3f/unknown_var", f"expected -32037, got {r}")


# === T4: bp.add_comment ===
print(f"\n=== T4: bp.add_comment EventGraph [200,200] [400,300] 'Test comment block' red ===")
r = call("bp.add_comment", {
    "blueprint_path": TEST_BP,
    "graph_name": "EventGraph",
    "position": [200, 200],
    "size": [400, 300],
    "text": "Test comment block",
    "color": [1.0, 0.0, 0.0, 1.0],
})
comment_guid = None
if r and r.get("ok"):
    res = r["result"]
    comment_guid = res.get("node_guid")
    if (comment_guid
            and res.get("text") == "Test comment block"
            and res.get("position") == [200, 200]
            and res.get("size") == [400, 300]):
        ok("T4/add_comment", f"guid={comment_guid}")
    else:
        fail("T4/add_comment", f"unexpected: {res}")
else:
    fail("T4/add_comment", f"{r}")

# === T4b: add_comment minimal args (default size/color) ===
print(f"\n=== T4b: bp.add_comment minimal args (default size/color) ===")
r = call("bp.add_comment", {
    "blueprint_path": TEST_BP,
    "position": [600, 0],
    "text": "minimal",
})
comment_guid_minimal = None
if r and r.get("ok"):
    comment_guid_minimal = r["result"].get("node_guid")
    # Should have used default size [300, 200]
    if r["result"].get("size") == [300, 200]:
        ok("T4b/add_minimal", f"guid={comment_guid_minimal} defaults applied")
    else:
        fail("T4b/add_minimal", f"size default not applied: {r['result']}")
else:
    fail("T4b/add_minimal", f"{r}")


# === T5: bp.delete_comment ===
if comment_guid:
    print(f"\n=== T5: bp.delete_comment {comment_guid} ===")
    r = call("bp.delete_comment", {
        "blueprint_path": TEST_BP,
        "graph_name": "EventGraph",
        "node_guid": comment_guid,
    })
    if r and r.get("ok") and r["result"].get("deleted") is True:
        ok("T5/delete_comment", "deleted=true")
    else:
        fail("T5/delete_comment", f"{r}")

# Clean up second comment too
if comment_guid_minimal:
    call("bp.delete_comment", {
        "blueprint_path": TEST_BP,
        "node_guid": comment_guid_minimal,
    })

# === T6: bp.add_comment bad graph -> -32050 GraphNotFound ===
print(f"\n=== T6: bp.add_comment graph_name='NoSuchGraph' -> -32050 ===")
r = call("bp.add_comment", {
    "blueprint_path": TEST_BP,
    "graph_name": "NoSuchGraph",
    "position": [0, 0],
    "text": "won't land",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32050:
    ok("T6/bad_graph", f"correctly -32050: {r['error']['message'][:100]}")
else:
    fail("T6/bad_graph", f"expected -32050, got {r}")

# === T6b: bp.add_comment missing position -> -32602 ===
print(f"\n=== T6b: bp.add_comment missing position -> -32602 ===")
r = call("bp.add_comment", {
    "blueprint_path": TEST_BP,
    "text": "no position",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T6b/missing_position", f"correctly -32602")
else:
    fail("T6b/missing_position", f"expected -32602, got {r}")

# === T6c: bp.delete_comment unknown guid -> -32051 NodeNotFound ===
print(f"\n=== T6c: bp.delete_comment unknown guid -> -32051 ===")
r = call("bp.delete_comment", {
    "blueprint_path": TEST_BP,
    "node_guid": "00000000-0000-0000-0000-000000000000",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32051:
    ok("T6c/unknown_guid", f"correctly -32051")
else:
    fail("T6c/unknown_guid", f"expected -32051, got {r}")


# === T7: bp.compile clean ===
print(f"\n=== T7: bp.compile post-edits -> clean ===")
r = call("bp.compile", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    res = r["result"]
    errs = len(res.get("errors", []))
    warns = len(res.get("warnings", []))
    if errs == 0:
        ok("T7/compile_clean", f"compiled={res['compiled']} status={res['status']} errors={errs} warnings={warns}")
    else:
        fail("T7/compile_clean", f"errors={errs}: {res.get('errors')}")
else:
    fail("T7/compile_clean", f"{r}")


# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})

print(f"\n{'=' * 60}")
print(f"WAVE F SURFACE 5 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
