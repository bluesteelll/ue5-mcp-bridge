#!/usr/bin/env python3
"""Wave F Surface 2 test: bp.add_function_parameter / bp.remove_function_parameter /
bp.list_function_parameters / bp.set_function_metadata.

Mirrors the test plan in the Wave F2 brief. Uses an ephemeral /Game/MCPTest BP, exercises
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

# === Setup: create a fresh BP with a function ===
import random
TEST_BP = f"/Game/MCPTest/BP_WaveF_S2_{random.randint(10000, 99999)}"
FN = "TestFunc"
print(f"=== Setup: create test blueprint {TEST_BP} ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})
r = call("bp.create_blueprint", {"dest_path": TEST_BP, "parent_class_path": "/Script/Engine.Actor", "save": False})
if not (r and r.get("ok")):
    fail("setup/create_bp", f"{r}")
    print("=== Cannot proceed without test BP ===")
    sys.exit(1)
ok("setup/create_bp", f"path={r['result']['asset_path']}")

# Add function with empty signature
r = call("bp.add_function", {"blueprint_path": TEST_BP, "function_name": FN})
if r and r.get("ok"):
    ok("setup/add_function", f"function={FN}")
else:
    fail("setup/add_function", f"{r}")
    sys.exit(1)

# === T1: bp.list_function_parameters -> empty inputs+outputs ===
print(f"\n=== T1: bp.list_function_parameters {FN} -> empty ===")
r = call("bp.list_function_parameters", {"blueprint_path": TEST_BP, "function_name": FN})
if r and r.get("ok"):
    inp = r["result"]["inputs"]
    out = r["result"]["outputs"]
    if isinstance(inp, list) and isinstance(out, list) and len(inp) == 0 and len(out) == 0:
        ok("T1/list_empty", f"inputs={len(inp)} outputs={len(out)}")
    else:
        fail("T1/list_empty", f"expected empty arrays, got {r['result']}")
else:
    fail("T1/list_empty", f"{r}")

# === T2: bp.add_function_parameter input "InValue" Real/double default="5.0" ===
print(f"\n=== T2: bp.add_function_parameter {FN}.InValue input Real ===")
r = call("bp.add_function_parameter", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "param_name": "InValue",
    "pin_type": {"category": "Real", "subcategory": "double"},
    "direction": "input",
    "default_value": "5.0"
})
if r and r.get("ok") and r["result"]["added"] is True:
    if r["result"]["direction"] == "input" and r["result"]["param_name"] == "InValue":
        ok("T2/add_input", f"added={r['result']['added']} dir={r['result']['direction']}")
    else:
        fail("T2/add_input", f"unexpected result fields: {r['result']}")
else:
    fail("T2/add_input", f"{r}")

# === T3: bp.add_function_parameter output "OutValue" Real/double ===
print(f"\n=== T3: bp.add_function_parameter {FN}.OutValue output Real ===")
r = call("bp.add_function_parameter", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "param_name": "OutValue",
    "pin_type": {"category": "Real", "subcategory": "double"},
    "direction": "output"
})
if r and r.get("ok") and r["result"]["added"] is True and r["result"]["direction"] == "output":
    ok("T3/add_output", f"added={r['result']['added']} dir={r['result']['direction']}")
else:
    fail("T3/add_output", f"{r}")

# === T4: bp.list_function_parameters -> 1 input + 1 output ===
print(f"\n=== T4: bp.list_function_parameters -> 1 input + 1 output ===")
r = call("bp.list_function_parameters", {"blueprint_path": TEST_BP, "function_name": FN})
if r and r.get("ok"):
    inp = r["result"]["inputs"]
    out = r["result"]["outputs"]
    if len(inp) == 1 and len(out) == 1:
        in_name = inp[0]["name"]
        in_default = inp[0].get("default_value")
        out_name = out[0]["name"]
        in_cat = inp[0]["pin_type"]["category"]
        out_cat = out[0]["pin_type"]["category"]
        if in_name == "InValue" and out_name == "OutValue" and in_cat == "Real" and out_cat == "Real":
            ok("T4/list_after_add", f"in='{in_name}'({in_cat}) default={in_default!r} out='{out_name}'({out_cat})")
        else:
            fail("T4/list_after_add", f"unexpected param shapes: in={inp[0]} out={out[0]}")
    else:
        fail("T4/list_after_add", f"expected 1 input + 1 output, got {len(inp)}+{len(out)}: {r['result']}")
else:
    fail("T4/list_after_add", f"{r}")

# === T5: bp.add_function_parameter same name -> -32057 ===
print(f"\n=== T5: bp.add_function_parameter duplicate -> -32057 ===")
r = call("bp.add_function_parameter", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "param_name": "InValue",
    "pin_type": {"category": "Real", "subcategory": "double"},
    "direction": "input"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32057:
    ok("T5/duplicate_param", f"correctly -32057: {r['error']['message'][:80]}")
else:
    fail("T5/duplicate_param", f"expected -32057, got {r}")

# === T6: bp.set_function_metadata is_pure + category + tooltip + verify via subsequent reads ===
print(f"\n=== T6: bp.set_function_metadata is_pure=true category=Math tooltip=test ===")
# Note: making pure may strip exec pins; we set BEFORE checking is_pure via bp.get_function which
# returns 'is_pure' from the function entry flags.
r = call("bp.set_function_metadata", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "metadata": {"is_pure": True, "category": "Math", "tooltip": "test"}
})
if r and r.get("ok"):
    prior = r["result"]["prior"]
    new = r["result"]["new"]
    if (prior.get("is_pure") is False and new.get("is_pure") is True
        and new.get("category") == "Math" and new.get("tooltip") == "test"):
        ok("T6/set_metadata", f"prior={prior} new={new}")
    else:
        fail("T6/set_metadata", f"unexpected prior/new: prior={prior} new={new}")
else:
    fail("T6/set_metadata", f"{r}")

# === T6b: bp.get_function -> verify is_pure persisted ===
print(f"\n=== T6b: bp.get_function -> verify is_pure=true persisted ===")
r = call("bp.get_function", {"blueprint_path": TEST_BP, "function_name": FN})
if r and r.get("ok"):
    fn = r["result"]["function"]
    if fn.get("is_pure") is True and fn.get("category") == "Math":
        ok("T6b/get_after_metadata", f"is_pure={fn['is_pure']} category={fn['category']!r}")
    else:
        fail("T6b/get_after_metadata", f"is_pure={fn.get('is_pure')} category={fn.get('category')!r}")
else:
    fail("T6b/get_after_metadata", f"{r}")

# === T7: bp.remove_function_parameter "InValue" -> verify list shrinks ===
print(f"\n=== T7: bp.remove_function_parameter {FN}.InValue ===")
r = call("bp.remove_function_parameter", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "param_name": "InValue"
})
if r and r.get("ok") and r["result"]["removed"] is True and r["result"]["direction"] == "input":
    ok("T7/remove_input", f"removed={r['result']['removed']} dir={r['result']['direction']}")
else:
    fail("T7/remove_input", f"{r}")

# Verify list now has 0 inputs + 1 output (OutValue still there)
r = call("bp.list_function_parameters", {"blueprint_path": TEST_BP, "function_name": FN})
if r and r.get("ok") and len(r["result"]["inputs"]) == 0 and len(r["result"]["outputs"]) == 1:
    ok("T7b/list_after_remove", f"inputs=0 outputs=1 (OutValue remains)")
else:
    fail("T7b/list_after_remove", f"unexpected counts: {r['result'] if r and r.get('ok') else r}")

# Idempotency: removing again returns removed=false
r = call("bp.remove_function_parameter", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "param_name": "InValue"
})
if r and r.get("ok") and r["result"]["removed"] is False and r["result"]["direction"] is None:
    ok("T7c/remove_idempotent", "second remove returns removed=false direction=null")
else:
    fail("T7c/remove_idempotent", f"{r}")

# === T8: bp.compile -> clean ===
print(f"\n=== T8: bp.compile post-edits ===")
r = call("bp.compile", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    res = r["result"]
    errs = len(res.get("errors", []))
    warns = len(res.get("warnings", []))
    if errs == 0:
        ok("T8/compile_clean", f"compiled={res['compiled']} status={res['status']} errors={errs} warnings={warns}")
    else:
        fail("T8/compile_clean", f"errors={errs}: {res.get('errors')}")
else:
    fail("T8/compile_clean", f"{r}")

# === Additional: error path tests ===
print(f"\n=== T9: error-path coverage ===")

# Bad direction
r = call("bp.add_function_parameter", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "param_name": "X",
    "pin_type": {"category": "Boolean"},
    "direction": "sideways"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T9a/bad_direction", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T9a/bad_direction", f"expected -32602, got {r}")

# Unknown function
r = call("bp.list_function_parameters", {"blueprint_path": TEST_BP, "function_name": "NonexistentFn"})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32037:
    ok("T9b/unknown_function", f"correctly -32037: {r['error']['message'][:80]}")
else:
    fail("T9b/unknown_function", f"expected -32037, got {r}")

# Bad access_specifier value
r = call("bp.set_function_metadata", {
    "blueprint_path": TEST_BP,
    "function_name": FN,
    "metadata": {"access_specifier": "wizard"}
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T9c/bad_access_specifier", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T9c/bad_access_specifier", f"expected -32602, got {r}")

# Missing metadata object
r = call("bp.set_function_metadata", {"blueprint_path": TEST_BP, "function_name": FN})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T9d/missing_metadata", f"correctly -32602: {r['error']['message'][:80]}")
else:
    fail("T9d/missing_metadata", f"expected -32602, got {r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})

print(f"\n{'='*60}")
print(f"WAVE F SURFACE 2 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
