#!/usr/bin/env python3
"""Wave G Surface 4 test: animbp.* anim blueprint state machine tools.

Test plan:
  T0: Create test Anim BP via exec_python (no skeleton — single-line lambda trick)
  T0b: Add a State Machine node via bp.add_node (graph_name=AnimGraph)
  T1: animbp.list_state_machines -> should find 1 SM
  T2: animbp.get_states -> 0 states initially
  T3: bad anim_blueprint_path -> -32004
  T4: non-Anim-BP asset -> -32011
  T5: missing args -> -32602 (all 4 tools)
  T6: animbp.add_state -> success, returns guid
  T6b: duplicate state name -> -32014
  T6c: animbp.get_states after add -> 1 state
  T7: animbp.add_state -> second state for transition target
  T8: animbp.add_transition -> success
  T8b: from==to -> -32602
  T9: bad state_machine_name on add_state -> -32004
"""
import json, socket, sys, time

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020
TEST_ABP_PATH = "/Game/MCPTest/WaveG4_TestAnimBP"


def send(req, t=60):
    s = socket.create_connection((HOST, PORT), timeout=t)
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + t
    while time.time() < deadline:
        try:
            c = s.recv(512 * 1024)
            if not c:
                break
            buf += c
            if b"\n" in buf:
                return json.loads(buf[: buf.index(b"\n")].decode())
        except socket.timeout:
            pass
    return None


def call(method, args=None, t=60):
    return send({"id": "t", "kind": "call_function", "method": method, "args": args or {}}, t)


def pyexec(expr, t=60):
    return send({"id": "t", "kind": "exec_python", "args": {"expression": expr}}, t)


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


# === T0: Create test Anim Blueprint via exec_python (no skeleton — uses default factory) ===
print(f"=== T0: create test Anim BP at {TEST_ABP_PATH} ===")
test_name = TEST_ABP_PATH.rsplit("/", 1)[-1]
pkg_dir = TEST_ABP_PATH.rsplit("/", 1)[0]
# Check if it already exists; if so, delete first
chk = call("asset.exists", {"path": TEST_ABP_PATH})
if chk and chk.get("ok") and chk["result"].get("exists"):
    print(f"  (existing {TEST_ABP_PATH} found — deleting first)")
    call("cb.delete", {"paths": [TEST_ABP_PATH], "force": True})

# Create — exec_python with a single-expression that returns the path.
create_expr = (
    f"type(__import__('unreal').AssetToolsHelpers.get_asset_tools()"
    f".create_asset('{test_name}', '{pkg_dir}', "
    f"__import__('unreal').AnimBlueprint.static_class(), "
    f"__import__('unreal').AnimBlueprintFactory())).__name__"
)
r = pyexec(create_expr, t=60)
if r and r.get("ok") and "AnimBlueprint" in r["result"].get("repr", ""):
    ok("T0/create", f"created via UAnimBlueprintFactory: {r['result']['repr']}")
else:
    fail("T0/create", f"could not create test Anim BP: {r}")
    print("\nABORTING — cannot run remaining tests without a test Anim BP")
    sys.exit(1)


# === T0b: Add a State Machine node into the AnimGraph ===
print("\n=== T0b: bp.add_node UAnimGraphNode_StateMachine into AnimGraph ===")
r = call("bp.add_node", {
    "blueprint_path": TEST_ABP_PATH,
    "node_class": "/Script/AnimGraph.AnimGraphNode_StateMachine",
    "graph_name": "AnimGraph",
    "position": [200, 200],
})
sm_node_guid = None
if r and r.get("ok"):
    sm_node_guid = r["result"].get("node_guid")
    ok("T0b/add_sm_node", f"node_guid={sm_node_guid} title={r['result'].get('title')!r}")
else:
    fail("T0b/add_sm_node", f"{r}")


# === T1: animbp.list_state_machines ===
print("\n=== T1: animbp.list_state_machines ===")
first_sm_name = None
r = call("animbp.list_state_machines", {"anim_blueprint_path": TEST_ABP_PATH})
if r and r.get("ok"):
    sms = r["result"].get("state_machines", [])
    if len(sms) >= 1:
        first_sm_name = sms[0]["name"]
        missing = [k for k in ("name", "graph_name", "state_count", "sm_node_guid", "parent_graph_name")
                   if k not in sms[0]]
        if missing:
            fail("T1/entry_shape", f"missing: {missing}")
        else:
            ok("T1/list",
               f"found {len(sms)} SM(s); first.name={first_sm_name!r} state_count={sms[0]['state_count']}")
    else:
        fail("T1/list", f"expected ≥1 state_machine, got: {sms}")
else:
    fail("T1/list", f"{r}")


# === T2: animbp.get_states on the (empty) SM ===
print(f"\n=== T2: animbp.get_states (sm={first_sm_name!r}) ===")
if first_sm_name:
    r = call("animbp.get_states", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
    })
    if r and r.get("ok"):
        states = r["result"].get("states")
        if isinstance(states, list):
            ok("T2/get_states_empty", f"empty SM has {len(states)} state(s) (expected 0)")
        else:
            fail("T2/get_states_shape", f"states not a list: {r['result']}")
    else:
        fail("T2/get_states", f"{r}")
else:
    skip("T2/get_states", "no SM discovered in T1")


# === T3: bad anim_blueprint_path -> -32004 ===
print("\n=== T3: bad anim_blueprint_path -> -32004 ===")
r = call("animbp.list_state_machines", {
    "anim_blueprint_path": "/Game/NeverGonnaExist/ABP_FakeBP",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T3/bad_path", f"correctly -32004")
else:
    fail("T3/bad_path", f"expected -32004, got {r}")


# === T4: non-Anim-BP asset -> -32011 ===
print("\n=== T4: regular UBlueprint asset -> -32011 ===")
candidate_bps = ["/Game/BP_PlayerFlecs"]
non_abp_path = None
for cand in candidate_bps:
    test = call("animbp.list_state_machines", {"anim_blueprint_path": cand})
    if test and not test.get("ok") and test.get("error", {}).get("code") == -32011:
        non_abp_path = cand
        ok("T4/non_abp", f"{cand} -> -32011")
        break
if not non_abp_path:
    skip("T4/non_abp", "no non-Anim UBlueprint to test with")


# === T5: missing args -> -32602 ===
print("\n=== T5: missing args -> -32602 ===")
r = call("animbp.list_state_machines", {})
ok_or_fail = (r and not r.get("ok") and r.get("error", {}).get("code") == -32602)
(ok if ok_or_fail else fail)("T5/missing_args/list", "" if ok_or_fail else f"got {r}")

r = call("animbp.get_states", {"anim_bp_path": TEST_ABP_PATH})
ok_or_fail = (r and not r.get("ok") and r.get("error", {}).get("code") == -32602)
(ok if ok_or_fail else fail)("T5/missing_args/get_states", "" if ok_or_fail else f"got {r}")

r = call("animbp.add_state", {"anim_bp_path": TEST_ABP_PATH, "state_machine_name": "x"})
ok_or_fail = (r and not r.get("ok") and r.get("error", {}).get("code") == -32602)
(ok if ok_or_fail else fail)("T5/missing_args/add_state", "" if ok_or_fail else f"got {r}")

r = call("animbp.add_transition", {"anim_bp_path": TEST_ABP_PATH, "state_machine_name": "x", "from_state": "x"})
ok_or_fail = (r and not r.get("ok") and r.get("error", {}).get("code") == -32602)
(ok if ok_or_fail else fail)("T5/missing_args/add_transition", "" if ok_or_fail else f"got {r}")


# === T6: animbp.add_state -> success ===
state_a_name = f"StateA_{int(time.time())}"
state_b_name = f"StateB_{int(time.time())}"
print(f"\n=== T6: animbp.add_state name={state_a_name!r} ===")
state_a_guid = None
if first_sm_name:
    r = call("animbp.add_state", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
        "state_name": state_a_name,
        "position": [400, 200],
    })
    if r and r.get("ok"):
        state_a_guid = r["result"].get("state_node_guid")
        ok("T6/add_state",
           f"created state_name={r['result'].get('state_name')!r} guid={state_a_guid} "
           f"position={r['result'].get('position')}")
    else:
        fail("T6/add_state", f"{r}")
else:
    skip("T6/add_state", "no SM")


# === T6b: duplicate state name -> -32014 ===
print("\n=== T6b: duplicate state name -> -32014 ===")
if first_sm_name and state_a_guid:
    r = call("animbp.add_state", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
        "state_name": state_a_name,  # same name
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32014:
        ok("T6b/dup_state_name", f"correctly -32014")
    else:
        fail("T6b/dup_state_name", f"expected -32014, got {r}")
else:
    skip("T6b/dup_state_name", "no state added in T6")


# === T6c: animbp.get_states after add ===
print("\n=== T6c: animbp.get_states after add ===")
if first_sm_name:
    r = call("animbp.get_states", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
    })
    if r and r.get("ok"):
        states = r["result"].get("states", [])
        names = [s.get("name") for s in states]
        if state_a_name in names:
            ok("T6c/get_states_after_add", f"states: {names}")
        else:
            fail("T6c/get_states_after_add", f"{state_a_name!r} not in {names}")
    else:
        fail("T6c/get_states_after_add", f"{r}")
else:
    skip("T6c/get_states_after_add", "no SM")


# === T7: add second state for transition ===
print(f"\n=== T7: add second state {state_b_name!r} ===")
if first_sm_name:
    r = call("animbp.add_state", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
        "state_name": state_b_name,
        "position": [700, 200],
    })
    if r and r.get("ok"):
        ok("T7/add_state_b", f"state_name={r['result'].get('state_name')!r}")
    else:
        fail("T7/add_state_b", f"{r}")
else:
    skip("T7/add_state_b", "no SM")


# === T8: animbp.add_transition -> success ===
print(f"\n=== T8: animbp.add_transition {state_a_name!r} -> {state_b_name!r} ===")
if first_sm_name:
    r = call("animbp.add_transition", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
        "from_state": state_a_name,
        "to_state": state_b_name,
    })
    if r and r.get("ok"):
        res = r["result"]
        if "transition_node_guid" in res and res.get("from_state") == state_a_name and res.get("to_state") == state_b_name:
            ok("T8/add_transition", f"transition_node_guid={res['transition_node_guid']}")
        else:
            fail("T8/add_transition", f"unexpected response: {res}")
    else:
        fail("T8/add_transition", f"{r}")
else:
    skip("T8/add_transition", "no SM")


# === T8b: from==to -> -32602 ===
print(f"\n=== T8b: self-transition (from==to) -> -32602 ===")
if first_sm_name:
    r = call("animbp.add_transition", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
        "from_state": state_a_name,
        "to_state": state_a_name,
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32602:
        ok("T8b/self_transition_rejected", "correctly -32602")
    else:
        fail("T8b/self_transition_rejected", f"expected -32602, got {r}")
else:
    skip("T8b/self_transition_rejected", "no SM")


# === T9: bad state_machine_name on add_state -> -32004 ===
print("\n=== T9: bad state_machine_name -> -32004 ===")
r = call("animbp.add_state", {
    "anim_bp_path": TEST_ABP_PATH,
    "state_machine_name": "DefinitelyNotRealSM_zzz",
    "state_name": "X",
})
if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
    ok("T9/bad_sm_name", "correctly -32004")
else:
    fail("T9/bad_sm_name", f"expected -32004, got {r}")


# === T10: bad from_state on add_transition -> -32004 ===
print("\n=== T10: bad from_state -> -32004 ===")
if first_sm_name:
    r = call("animbp.add_transition", {
        "anim_bp_path": TEST_ABP_PATH,
        "state_machine_name": first_sm_name,
        "from_state": "NotARealState_xyz",
        "to_state": state_b_name or "x",
    })
    if r and not r.get("ok") and r.get("error", {}).get("code") == -32004:
        ok("T10/bad_from_state", "correctly -32004")
    else:
        fail("T10/bad_from_state", f"expected -32004, got {r}")
else:
    skip("T10/bad_from_state", "no SM")


print(f"\n{'=' * 60}")
print(f"WAVE G SURFACE 4 TEST: {len(PASS)} PASS, {len(FAIL)} FAIL, {len(SKIP)} SKIP")
print(f"{'=' * 60}")
for n in FAIL:
    print(f"  FAIL - {n}")
for n in SKIP:
    print(f"  SKIP - {n}")
sys.exit(0 if not FAIL else 1)
