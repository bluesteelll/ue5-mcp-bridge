#!/usr/bin/env python3
"""Wave F Surface 1 test: bp.set_node_property / bp.set_pin_default / bp.delete_node /
bp.disconnect_pin / bp.move_node.

Mirrors the test plan in the Wave F1 brief. Uses an ephemeral /Game/MCPTest BP, exercises
each new tool with both happy-path and error-path inputs, then cleans up.
"""
import json, socket, sys, time

# Force UTF-8 stdout so messages with arrows / accented chars don't crash on Windows cp1252.
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

# === Setup: create a fresh BP to add nodes to ===
import random
TEST_BP = f"/Game/MCPTest/BP_WaveF_S1_{random.randint(10000, 99999)}"
print(f"=== Setup: create test blueprint {TEST_BP} ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})
r = call("bp.create_blueprint", {"dest_path": TEST_BP, "parent_class_path": "/Script/Engine.Actor", "save": False})
if not (r and r.get("ok")):
    fail("setup/create_bp", f"{r}")
    print("=== Cannot proceed without test BP ===")
    sys.exit(1)
ok("setup/create_bp", f"path={r['result']['asset_path']}")

# Add a variable so we can test K2Node_VariableGet
r = call("bp.add_variable", {
    "blueprint_path": TEST_BP,
    "variable_name": "TestFloat",
    "pin_type": {"category": "Real", "subcategory": "double"},
    "default_value": "0.0"
})
if r and r.get("ok"):
    ok("setup/add_var", "variable=TestFloat")
else:
    fail("setup/add_var", f"{r}")

r = call("bp.compile", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    ok("setup/compile_pre", f"status={r['result'].get('status')}")
else:
    fail("setup/compile_pre", f"{r}")

# Add K2Node_VariableGet (guid_get)
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_VariableGet",
    "variable_name": "TestFloat",
    "position": [-200, 0]
})
guid_get = None
if r and r.get("ok"):
    guid_get = r["result"]["node_guid"]
    ok("setup/add_get", f"guid={guid_get[:8]}...")
else:
    fail("setup/add_get", f"{r}")

# Add K2Node_CallFunction (Multiply_DoubleDouble) → guid_mul
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_CallFunction",
    "function_name": "Multiply_DoubleDouble",
    "function_class": "/Script/Engine.KismetMathLibrary",
    "position": [0, 0]
})
guid_mul = None
if r and r.get("ok"):
    guid_mul = r["result"]["node_guid"]
    pin_names = [p["name"] for p in r["result"]["pins"]]
    ok("setup/add_mul", f"guid={guid_mul[:8]}... pins={pin_names}")
else:
    fail("setup/add_mul", f"{r}")

# Wire get.TestFloat → mul.A
if guid_get and guid_mul:
    r = call("bp.connect_pins", {
        "blueprint_path": TEST_BP,
        "from_node": guid_get,
        "from_pin": "TestFloat",
        "to_node": guid_mul,
        "to_pin": "A"
    })
    if r and r.get("ok") and r["result"]["connected"]:
        ok("setup/connect", f"connected={r['result']['connected']}")
    else:
        fail("setup/connect", f"{r}")

# === T1: bp.set_pin_default mul.B = 2.5 (unconnected pin) ===
print("\n=== T1: bp.set_pin_default mul.B = 2.5 ===")
if guid_mul:
    r = call("bp.set_pin_default", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_mul,
        "pin_name": "B",
        "value": 2.5
    })
    if r and r.get("ok"):
        res = r["result"]
        prior = res["prior_default"]["default_value"]
        new = res["new_default"]["default_value"]
        # New default should be something like "2.5" or "2.500000" depending on schema canonicalisation
        if new and "2.5" in new:
            ok("T1/set_pin_default", f"prior='{prior}' new='{new}'")
        else:
            fail("T1/set_pin_default", f"unexpected new_default={new!r} (full={res})")
    else:
        fail("T1/set_pin_default", f"{r}")
else:
    fail("T1/set_pin_default", "skipped - no guid_mul")

# === T2: bp.set_pin_default on CONNECTED pin (mul.A) -> -32602 ===
print("\n=== T2: bp.set_pin_default mul.A (connected) -> -32602 ===")
if guid_mul:
    r = call("bp.set_pin_default", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_mul,
        "pin_name": "A",
        "value": 5.0
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
        ok("T2/connected_pin_refused", f"correctly -32602: {r['error']['message'][:80]}")
    else:
        fail("T2/connected_pin_refused", f"expected -32602, got {r}")
else:
    fail("T2/connected_pin_refused", "skipped")

# === T3: bp.move_node mul -> [300, 100] ===
print("\n=== T3: bp.move_node mul -> [300, 100] ===")
if guid_mul:
    r = call("bp.move_node", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_mul,
        "position": [300, 100]
    })
    if r and r.get("ok"):
        res = r["result"]
        prior = res["prior_position"]
        new = res["new_position"]
        if new == [300, 100]:
            ok("T3/move_node", f"prior={prior} new={new}")
        else:
            fail("T3/move_node", f"unexpected new={new}, full={res}")
    else:
        fail("T3/move_node", f"{r}")
else:
    fail("T3/move_node", "skipped")

# === T4: bp.disconnect_pin get.TestFloat -> links_broken=1 ===
print("\n=== T4: bp.disconnect_pin get.TestFloat -> links_broken=1 ===")
if guid_get:
    r = call("bp.disconnect_pin", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_get,
        "pin_name": "TestFloat"
    })
    if r and r.get("ok") and r["result"]["links_broken"] == 1:
        ok("T4/disconnect_pin", f"links_broken={r['result']['links_broken']}")
    else:
        fail("T4/disconnect_pin", f"expected links_broken=1, got {r}")
else:
    fail("T4/disconnect_pin", "skipped")

# === T5: bp.set_node_property on K2Node_CallFunction.bDefaultsToPureFunc (flip + revert) ===
# (`bIsPureFunc` is deprecated since UE 5.5 and no longer a real UPROPERTY; the modern
# replacement is `bDefaultsToPureFunc` — same semantic, but actually exposed via reflection.)
print("\n=== T5: bp.set_node_property bDefaultsToPureFunc (flip + revert) ===")
prior_pure = None
if guid_mul:
    r = call("bp.set_node_property", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_mul,
        "property_name": "bDefaultsToPureFunc",
        "value": True
    })
    if r and r.get("ok"):
        prior_pure = r["result"]["prior_value"]
        new_pure = r["result"]["new_value"]
        if new_pure is True:
            ok("T5/set_pure_to_true", f"prior={prior_pure} new={new_pure}")
        else:
            fail("T5/set_pure_to_true", f"expected new=true, got {r['result']}")

        # Revert to prior value so the graph keeps its expected exec-pin layout for subsequent tests
        r2 = call("bp.set_node_property", {
            "blueprint_path": TEST_BP,
            "node_guid": guid_mul,
            "property_name": "bDefaultsToPureFunc",
            "value": prior_pure if prior_pure is not None else False
        })
        if r2 and r2.get("ok"):
            ok("T5/revert_pure", f"reverted to {r2['result']['new_value']}")
        else:
            fail("T5/revert_pure", f"{r2}")
    else:
        fail("T5/set_pure_to_true", f"{r}")
else:
    fail("T5/set_pure_to_true", "skipped")

# === T6: bp.delete_node mul ===
print("\n=== T6: bp.delete_node mul ===")
if guid_mul:
    r = call("bp.delete_node", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_mul
    })
    if r and r.get("ok") and r["result"]["deleted"] is True:
        ok("T6/delete_node", f"node_class={r['result']['node_class']} links_broken={r['result']['links_broken']}")
        guid_mul = None  # no longer exists
    else:
        fail("T6/delete_node", f"{r}")
else:
    fail("T6/delete_node", "skipped")

# === T7: bp.delete_node ENTRY NODE -> -32602 (use function entry — guaranteed undeletable) ===
print("\n=== T7: bp.delete_node entry node -> -32602 ===")
# Most reliable path: add a new function graph, then locate its K2Node_FunctionEntry. The default
# Actor BP EventGraph also contains K2Node_Event stubs (BeginPlay etc.) but their presence depends
# on parent-class hooks; the function-entry path is unconditional.
r_addfn = call("bp.add_function", {
    "blueprint_path": TEST_BP,
    "function_name": "TestFn_WaveF1"
})
entry_guid = None
if r_addfn and r_addfn.get("ok"):
    r_list = call("bp.list_nodes_in_function", {"blueprint_path": TEST_BP, "function_name": "TestFn_WaveF1"})
    if r_list and r_list.get("ok"):
        for n in r_list["result"].get("nodes", []):
            # bp.list_nodes_in_function returns 'class' (NOT 'node_class') and node_guid in
            # hyphenated form (EGuidFormats::DigitsWithHyphensInBraces variant) — both fields work
            # transparently as our BGT_FindNodeByGuid calls FGuid::Parse which accepts both shapes.
            if "K2Node_FunctionEntry" in n.get("class", ""):
                entry_guid = n["node_guid"]
                break
    else:
        print(f"   list_nodes_in_function failed: {r_list}")
else:
    print(f"   bp.add_function failed: {r_addfn}")

if entry_guid:
    r = call("bp.delete_node", {
        "blueprint_path": TEST_BP,
        "graph_name": "TestFn_WaveF1",
        "node_guid": entry_guid
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
        ok("T7/delete_entry_refused", f"correctly -32602 via FunctionEntry: {r['error']['message'][:80]}")
    else:
        fail("T7/delete_entry_refused", f"expected -32602, got {r}")
else:
    fail("T7/delete_entry_refused", "could not locate FunctionEntry node in TestFn_WaveF1")

# === T8: bp.set_node_property bad property -> -32005 ===
print("\n=== T8: bp.set_node_property bad property -> -32005 ===")
if guid_get:
    r = call("bp.set_node_property", {
        "blueprint_path": TEST_BP,
        "node_guid": guid_get,
        "property_name": "ThisPropertyDoesNotExist_xyz",
        "value": 42
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32005:
        ok("T8/bad_property", f"correctly -32005: {r['error']['message'][:80]}")
    else:
        fail("T8/bad_property", f"expected -32005, got {r}")
else:
    fail("T8/bad_property", "skipped")

# === T9: bp.compile after edits -> clean (0 errors preferred) ===
print("\n=== T9: bp.compile post-edits ===")
r = call("bp.compile", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    res = r["result"]
    errs = len(res.get("errors", []))
    warns = len(res.get("warnings", []))
    ok("T9/compile_post", f"compiled={res['compiled']} status={res['status']} errors={errs} warnings={warns}")
else:
    fail("T9/compile_post", f"{r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})

print(f"\n{'='*60}")
print(f"WAVE F SURFACE 1 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
