#!/usr/bin/env python3
"""Wave F Surface 4 test: bp.add_interface / bp.remove_interface / bp.list_interfaces.

Uses /Script/Engine.ActorSoundParameterInterface (UINTERFACE(BlueprintType, MinimalAPI) — confirmed
Blueprintable in UE 5.7 source). If that interface isn't available in this build (unlikely — it's
in the core Engine module), falls back to scanning /Game for any /Game/.../UIM_*_C asset and
documents a SKIP for the add-path tests.
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

# === Pick an interface to test with ===
# Primary candidate: BlueprintType engine interface (no CannotImplementInterfaceInBlueprint meta).
INTERFACE_PATH = "/Script/Engine.ActorSoundParameterInterface"
print(f"=== Probe: confirm {INTERFACE_PATH} is loadable as a UInterface ===")
# Use asset.search_by_class to confirm — but BP interfaces use the underlying UInterface UClass,
# which doesn't show up via asset.search. Instead, we just try add directly and let the bridge
# tell us if it's not a UInterface (-32011).
print(f"  (will probe via direct add_interface attempt)")

# === Setup: create a fresh Actor BP ===
TEST_BP = f"/Game/MCPTest/BP_WaveF_S4_{random.randint(10000, 99999)}"
print(f"\n=== Setup: create test blueprint {TEST_BP} (parent=Actor) ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})
r = call("bp.create_blueprint", {"dest_path": TEST_BP, "parent_class_path": "/Script/Engine.Actor", "save": False})
if not (r and r.get("ok")):
    fail("setup/create_bp", f"{r}")
    print("=== Cannot proceed without test BP ===")
    sys.exit(1)
ok("setup/create_bp", f"path={r['result']['asset_path']}")

# === T1: bp.list_interfaces → baseline (likely empty for blueprint source; parent may add some) ===
print(f"\n=== T1: bp.list_interfaces baseline ===")
r = call("bp.list_interfaces", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    baseline = r["result"]["implemented_interfaces"]
    bp_added = [e for e in baseline if e["source"] == "blueprint"]
    parent_added = [e for e in baseline if e["source"] == "parent"]
    if len(bp_added) == 0:
        ok("T1/list_baseline", f"blueprint=0 parent={len(parent_added)}; parent_interfaces={[e['interface_class'] for e in parent_added]}")
    else:
        fail("T1/list_baseline", f"expected blueprint=0, got {bp_added}")
else:
    fail("T1/list_baseline", f"{r}")

# === T1b: bp.list_interfaces with include_parent_interfaces=false ===
print(f"\n=== T1b: bp.list_interfaces include_parent_interfaces=false ===")
r = call("bp.list_interfaces", {"blueprint_path": TEST_BP, "include_parent_interfaces": False})
if r and r.get("ok"):
    items = r["result"]["implemented_interfaces"]
    # All entries must have source="blueprint" (parent walk should be skipped).
    if all(e["source"] == "blueprint" for e in items):
        ok("T1b/list_no_parent", f"items={len(items)} (all 'blueprint' source)")
    else:
        fail("T1b/list_no_parent", f"expected only 'blueprint' source, got {items}")
else:
    fail("T1b/list_no_parent", f"{r}")

# === T2: bp.add_interface ActorSoundParameterInterface ===
print(f"\n=== T2: bp.add_interface {INTERFACE_PATH} ===")
r = call("bp.add_interface", {
    "blueprint_path": TEST_BP,
    "interface_class_path": INTERFACE_PATH
})
add_ok = False
if r and r.get("ok"):
    if r["result"]["added"] is True:
        add_ok = True
        ok("T2/add_interface", f"added={r['result']['added']} class={r['result']['interface_class']} events={r['result']['generated_event_count']}")
    else:
        fail("T2/add_interface", f"unexpected added=false: {r['result']}")
elif r and not r.get("ok"):
    err = r.get("error", {})
    code = err.get("code")
    # If the chosen interface is CannotImplementInterfaceInBlueprint OR otherwise unimplementable,
    # ImplementNewInterface returns false → we surface -32603 Internal. Document but don't full-fail
    # the suite — fall through to error-path tests with a synthesised "won't add" path.
    if code == -32603:
        fail("T2/add_interface", f"primary interface refused (-32603): {err.get('message')[:120]}")
        print(f"  (likely meta CannotImplementInterfaceInBlueprint — switching to error-path-only mode)")
    else:
        fail("T2/add_interface", f"unexpected error: {r}")
else:
    fail("T2/add_interface", f"no response: {r}")

# === T3: bp.list_interfaces → contains new interface with source="blueprint" ===
if add_ok:
    print(f"\n=== T3: bp.list_interfaces → contains {INTERFACE_PATH} ===")
    r = call("bp.list_interfaces", {"blueprint_path": TEST_BP})
    if r and r.get("ok"):
        items = r["result"]["implemented_interfaces"]
        match = next((e for e in items if e["interface_class"] == INTERFACE_PATH), None)
        if match is not None:
            if match["source"] == "blueprint":
                ok("T3/list_after_add", f"present with source='blueprint'")
            else:
                fail("T3/list_after_add", f"present but source={match['source']!r}")
        else:
            fail("T3/list_after_add", f"interface not in list: {items}")
    else:
        fail("T3/list_after_add", f"{r}")

# === T4: bp.add_interface duplicate -> -32014 ===
if add_ok:
    print(f"\n=== T4: bp.add_interface duplicate {INTERFACE_PATH} -> -32014 ===")
    r = call("bp.add_interface", {
        "blueprint_path": TEST_BP,
        "interface_class_path": INTERFACE_PATH
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32014:
        ok("T4/duplicate", f"correctly -32014: {r['error']['message'][:100]}")
    else:
        fail("T4/duplicate", f"expected -32014, got {r}")

# === T5: bp.add_interface non-interface class (Actor) -> -32011 WrongClass ===
print(f"\n=== T5: bp.add_interface non-interface class /Script/Engine.Actor -> -32011 ===")
r = call("bp.add_interface", {
    "blueprint_path": TEST_BP,
    "interface_class_path": "/Script/Engine.Actor"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
    ok("T5/wrong_class", f"correctly -32011: {r['error']['message'][:100]}")
else:
    fail("T5/wrong_class", f"expected -32011, got {r}")

# === T5b: bp.add_interface malformed path -> -32023 InvalidClassPath ===
print(f"\n=== T5b: bp.add_interface malformed path 'BareName' -> -32023 ===")
r = call("bp.add_interface", {
    "blueprint_path": TEST_BP,
    "interface_class_path": "BareName"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32023:
    ok("T5b/invalid_path", f"correctly -32023: {r['error']['message'][:100]}")
else:
    fail("T5b/invalid_path", f"expected -32023, got {r}")

# === T5c: bp.add_interface bad path (doesn't load) -> -32020 ClassNotFound ===
print(f"\n=== T5c: bp.add_interface unknown class -> -32020 ===")
r = call("bp.add_interface", {
    "blueprint_path": TEST_BP,
    "interface_class_path": "/Script/Engine.TotallyMadeUpInterfaceClass"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32020:
    ok("T5c/class_not_found", f"correctly -32020: {r['error']['message'][:100]}")
else:
    fail("T5c/class_not_found", f"expected -32020, got {r}")

# === T5d: missing interface_class_path arg -> -32602 ===
print(f"\n=== T5d: bp.add_interface missing arg -> -32602 ===")
r = call("bp.add_interface", {"blueprint_path": TEST_BP})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
    ok("T5d/missing_arg", f"correctly -32602: {r['error']['message'][:100]}")
else:
    fail("T5d/missing_arg", f"expected -32602, got {r}")

# === T6: bp.remove_interface -> success ===
if add_ok:
    print(f"\n=== T6: bp.remove_interface {INTERFACE_PATH} ===")
    r = call("bp.remove_interface", {
        "blueprint_path": TEST_BP,
        "interface_class_path": INTERFACE_PATH
    })
    if r and r.get("ok") and r["result"]["removed"] is True:
        ok("T6/remove_interface", f"removed={r['result']['removed']} class={r['result']['interface_class']}")
    else:
        fail("T6/remove_interface", f"{r}")

# === T7: bp.list_interfaces → back to baseline (no blueprint-source entries) ===
print(f"\n=== T7: bp.list_interfaces → back to baseline ===")
r = call("bp.list_interfaces", {"blueprint_path": TEST_BP})
if r and r.get("ok"):
    items = r["result"]["implemented_interfaces"]
    bp_added = [e for e in items if e["source"] == "blueprint"]
    if len(bp_added) == 0:
        ok("T7/list_after_remove", f"blueprint=0 (back to baseline)")
    else:
        fail("T7/list_after_remove", f"expected blueprint=0, got {bp_added}")
else:
    fail("T7/list_after_remove", f"{r}")

# === T7b: bp.remove_interface idempotent on missing interface -> -32004 ObjectNotFound ===
print(f"\n=== T7b: bp.remove_interface non-implemented {INTERFACE_PATH} -> -32004 ===")
r = call("bp.remove_interface", {
    "blueprint_path": TEST_BP,
    "interface_class_path": INTERFACE_PATH
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T7b/remove_missing", f"correctly -32004: {r['error']['message'][:100]}")
else:
    fail("T7b/remove_missing", f"expected -32004, got {r}")

# === T7c: bp.remove_interface non-interface class -> -32011 ===
print(f"\n=== T7c: bp.remove_interface non-interface class /Script/Engine.Actor -> -32011 ===")
r = call("bp.remove_interface", {
    "blueprint_path": TEST_BP,
    "interface_class_path": "/Script/Engine.Actor"
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32011:
    ok("T7c/remove_wrong_class", f"correctly -32011: {r['error']['message'][:100]}")
else:
    fail("T7c/remove_wrong_class", f"expected -32011, got {r}")

# === T8: bp.compile clean ===
print(f"\n=== T8: bp.compile post-edits -> clean ===")
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

# === T9: list_interfaces on non-blueprint path -> -32031 ===
print(f"\n=== T9: bp.list_interfaces non-blueprint asset path -> -32031 ===")
r = call("bp.list_interfaces", {"blueprint_path": "/Script/Engine.Actor"})
if r and not r.get("ok") and r.get("error", {}).get("code") in (-32031, -32004, -32010):
    ok("T9/non_blueprint", f"correctly code={r['error']['code']}: {r['error']['message'][:100]}")
else:
    fail("T9/non_blueprint", f"expected -32031/-32004/-32010, got {r}")

# === Cleanup ===
print("\n=== Cleanup ===")
call("cb.delete", {"asset_path": TEST_BP, "force": True})

print(f"\n{'='*60}")
print(f"WAVE F SURFACE 4 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL")
print(f"{'='*60}")
for n in FAIL: print(f"  - {n}")
sys.exit(0 if not FAIL else 1)
