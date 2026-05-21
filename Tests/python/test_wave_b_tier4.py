#!/usr/bin/env python3
"""Wave B Tier 4 test: bp.add_node / bp.connect_pins."""
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
TEST_BP = f"/Game/MCPTest/BP_Tier4Test_{random.randint(10000, 99999)}"
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
    ok("setup/add_var", f"variable=TestFloat")
else:
    fail("setup/add_var", f"{r}")

# Compile so the variable is fully bound + EventGraph exists
r = call("bp.compile", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    ok("setup/compile_pre", f"status={r['result'].get('status')}")
else:
    fail("setup/compile_pre", f"{r}")

# === T1: bp.add_node K2Node_VariableGet for TestFloat ===
print("\n=== T1: bp.add_node VariableGet ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_VariableGet",
    "variable_name": "TestFloat",
    "position": [-200, 0]
})
get_node_guid = None
if r and r.get("ok"):
    res = r["result"]
    get_node_guid = res["node_guid"]
    pin_names = [p["name"] for p in res["pins"]]
    ok("T1/var_get", f"guid={get_node_guid[:8]}... title='{res.get('title')}' pos={res['position']} pins={pin_names}")
else:
    fail("T1/var_get", f"{r}")

# === T2: bp.add_node K2Node_VariableSet for TestFloat ===
print("\n=== T2: bp.add_node VariableSet ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_VariableSet",
    "variable_name": "TestFloat",
    "position": [200, 0]
})
set_node_guid = None
if r and r.get("ok"):
    res = r["result"]
    set_node_guid = res["node_guid"]
    pin_names = [p["name"] for p in res["pins"]]
    ok("T2/var_set", f"guid={set_node_guid[:8]}... title='{res.get('title')}' pins={pin_names}")
else:
    fail("T2/var_set", f"{r}")

# === T3: bp.add_node K2Node_CallFunction for KismetMathLibrary.Multiply_FloatFloat ===
print("\n=== T3: bp.add_node K2Node_CallFunction (KismetMathLibrary.Multiply_FloatFloat) ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_CallFunction",
    "function_name": "Multiply_DoubleDouble",
    "function_class": "/Script/Engine.KismetMathLibrary",
    "position": [0, 0]
})
mul_node_guid = None
if r and r.get("ok"):
    res = r["result"]
    mul_node_guid = res["node_guid"]
    pin_names = [p["name"] for p in res["pins"]]
    ok("T3/call_function", f"guid={mul_node_guid[:8]}... title='{res.get('title')}' pins={pin_names}")
else:
    fail("T3/call_function", f"{r}")

# === T4: bp.connect_pins var_get.TestFloat -> Multiply.A ===
print("\n=== T4: bp.connect_pins var_get -> multiply.A ===")
if get_node_guid and mul_node_guid:
    r = call("bp.connect_pins", {
        "blueprint_path": TEST_BP,
        "from_node": get_node_guid,
        "from_pin": "TestFloat",
        "to_node": mul_node_guid,
        "to_pin": "A"
    })
    if r and r.get("ok"):
        res = r["result"]
        ok("T4/connect_a", f"connected={res['connected']} broke={res['broke_existing_count']} response='{res['response']}'")
    else:
        fail("T4/connect_a", f"{r}")
else:
    fail("T4/connect_a", "skipped — prereq guids missing")

# === T5: bp.connect_pins Multiply.ReturnValue -> var_set.TestFloat ===
print("\n=== T5: bp.connect_pins multiply.ReturnValue -> var_set ===")
if mul_node_guid and set_node_guid:
    r = call("bp.connect_pins", {
        "blueprint_path": TEST_BP,
        "from_node": mul_node_guid,
        "from_pin": "ReturnValue",
        "to_node": set_node_guid,
        "to_pin": "TestFloat"
    })
    if r and r.get("ok"):
        ok("T5/connect_return", f"connected={r['result']['connected']}")
    else:
        fail("T5/connect_return", f"{r}")
else:
    fail("T5/connect_return", "skipped")

# === T6: bp.connect_pins INCOMPATIBLE -> -32053 PinConnectionRefused ===
print("\n=== T6: bp.connect_pins incompatible types -> -32053 ===")
if get_node_guid and mul_node_guid:
    # Connect float (TestFloat) -> exec pin (won't work, exec ≠ float)
    # But it's hard to test without knowing exact exec pin name. Let's connect
    # var_get.TestFloat (float OUT) to mul_node_guid.A (float IN) — already done in T4.
    # New tactic: connect var_get.TestFloat (OUT) -> var_get.TestFloat (OUT) -> both outputs.
    r = call("bp.connect_pins", {
        "blueprint_path": TEST_BP,
        "from_node": get_node_guid,
        "from_pin": "TestFloat",
        "to_node": get_node_guid,  # same node
        "to_pin": "TestFloat"  # same pin — should be refused
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32053:
        ok("T6/refused", f"correctly -32053: {r['error']['message'][:80]}")
    else:
        fail("T6/refused", f"expected -32053, got {r}")
else:
    fail("T6/refused", "skipped")

# === T7: bp.add_node bad class -> -32020 ClassNotFound ===
print("\n=== T7: bp.add_node ClassNotFound ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_NotARealClass"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32020:
    ok("T7/class_not_found", "correctly rejected")
else:
    fail("T7/class_not_found", f"expected -32020, got {r}")

# === T8: bp.add_node wrong class family -> -32011 WrongClass ===
print("\n=== T8: bp.add_node WrongClass (not K2Node) ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/Engine.Actor"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
    ok("T8/wrong_class", "correctly rejected")
else:
    fail("T8/wrong_class", f"expected -32011, got {r}")

# === T9: bp.add_node unknown graph -> -32050 GraphNotFound ===
print("\n=== T9: bp.add_node GraphNotFound ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_BP,
    "node_class": "/Script/BlueprintGraph.K2Node_VariableGet",
    "graph_name": "DefinitelyDoesNotExist"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32050:
    ok("T9/graph_not_found", "correctly rejected")
else:
    fail("T9/graph_not_found", f"expected -32050, got {r}")

# === T10: bp.connect_pins unknown node -> -32051 NodeNotFound ===
print("\n=== T10: bp.connect_pins NodeNotFound ===")
r = call("bp.connect_pins", {
    "blueprint_path": TEST_BP,
    "from_node": "00000000000000000000000000000000",
    "from_pin": "Anything",
    "to_node": get_node_guid or "00000000000000000000000000000001",
    "to_pin": "TestFloat"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32051:
    ok("T10/node_not_found", "correctly rejected")
else:
    fail("T10/node_not_found", f"expected -32051, got {r}")

# === T11: bp.connect_pins unknown pin -> -32052 PinNotFound ===
print("\n=== T11: bp.connect_pins PinNotFound ===")
if get_node_guid and mul_node_guid:
    r = call("bp.connect_pins", {
        "blueprint_path": TEST_BP,
        "from_node": get_node_guid,
        "from_pin": "NotARealPinName_xyz",
        "to_node": mul_node_guid,
        "to_pin": "A"
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32052:
        ok("T11/pin_not_found", "correctly rejected")
    else:
        fail("T11/pin_not_found", f"expected -32052, got {r}")

# === T12: bp.compile after edits — should compile cleanly ===
print("\n=== T12: bp.compile post-edits ===")
r = call("bp.compile", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    res = r["result"]
    ok("T12/compile_post", f"compiled={res['compiled']} status={res['status']} errors={len(res.get('errors',[]))} warnings={len(res.get('warnings',[]))}")
else:
    fail("T12/compile_post", f"{r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})

print(f"\n{'='*60}")
print(f"WAVE B TIER 4 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
