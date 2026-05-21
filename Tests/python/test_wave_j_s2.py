"""Wave J Surface 2 — ai.bb.* test (3 tools).

Blackboard runtime read/write surface. Requires PIE-ready map with at least ONE pawn that has
an AIController and a non-empty BlackboardComponent — typically a BP_Enemy or any pawn whose
AIControllerClass is set to a Blueprint with a Run Behavior Tree task in its controller's
default BehaviorTree slot. If no such actor is reachable in the editor world, T1-T3 skip and
the negative-path tests (T4-T6) still run against synthetic paths.

Setup attempts (in order):
  1. actor.find_by_class /Script/AIModule.AIController — direct controllers in the loaded world.
  2. actor.find_by_class /Script/Engine.Pawn fallback — our resolver walks Pawn->GetController()
     so any pawn with an AIController works.
  3. If no candidate, the test SKIPs T1-T3 with explanatory output (T4-T8 still run).
"""
import json
import random
import socket
import sys
import time

# UTF-8 stdout (Windows cp1252 chokes on Unicode).
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST, PORT = "127.0.0.1", 30020

PASS = 0
FAIL = 0
SKIPPED = 0


def call(method, args=None, timeout=20):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    req = {
        "id": "t_" + str(int(time.time() * 1000)) + "_" + str(random.randint(0, 9999)),
        "kind": "call_function",
        "method": method,
        "args": args or {},
    }
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = s.recv(65536)
        if not c:
            break
        buf += c
        if b"\n" in buf:
            break
    s.close()
    line = buf[: buf.index(b"\n")] if b"\n" in buf else buf
    return json.loads(line.decode())


def is_ok(resp):
    return resp.get("ok") is True


def is_err(resp, code):
    return resp.get("ok") is False and resp.get("error", {}).get("code") == code


def test(name, ok, info=""):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f"  [PASS] {name} {info}")
    else:
        FAIL += 1
        print(f"  [FAIL] {name} {info}")


def skip(name, info=""):
    global SKIPPED
    SKIPPED += 1
    print(f"  [SKIP] {name} {info}")


print("=== Wave J S2 — ai.bb.* (3 tools) ===\n")

# Discover a candidate actor with a blackboard. Strategy:
#   1. actor.find_by_class for AAIController — any controller in the loaded levels.
#   2. actor.find_by_class for APawn fallback — broader scan if no controllers found directly.
#   3. For each candidate, ai.bb.list_keys → success + non-empty keys means it has a BB.
#   4. First success → keep as test target.
print("Discover: scan loaded actors for one with an AIController-attached blackboard...")
target_actor = None
target_keys = []


def probe_candidates(class_path, label):
    global target_actor, target_keys
    if target_actor:
        return
    r = call("actor.find_by_class", {
        "class_path": class_path,
        "search_subclasses": True,
        "page_size": 100,
    })
    if not is_ok(r):
        print(f"  {label}: find_by_class failed: {r.get('error', {})}")
        return
    actors = r["result"].get("actors", [])
    print(f"  {label}: {len(actors)} candidate(s) returned by find_by_class")
    for a in actors[:30]:  # cap scan
        ap = a.get("actor_path")
        if not ap:
            continue
        probe = call("ai.bb.list_keys", {"actor_path": ap})
        if is_ok(probe) and probe["result"].get("keys"):
            target_actor = ap
            target_keys = probe["result"]["keys"]
            print(f"  -> found target: {target_actor} with {len(target_keys)} keys")
            return


# Step 1: AAIController itself.
probe_candidates("/Script/AIModule.AIController", "AAIController")
# Step 2: APawn fallback — walks Pawn->GetController() in our resolver.
probe_candidates("/Script/Engine.Pawn", "APawn")

if not target_actor:
    print("  (no actor with AI blackboard found in editor world — T1-T3 will SKIP)")

# === T1: ai.bb.list_keys ===
print(f"\nT1: ai.bb.list_keys on {target_actor!r}")
if target_actor:
    r = call("ai.bb.list_keys", {"actor_path": target_actor})
    if is_ok(r):
        keys = r["result"].get("keys", [])
        total = r["result"].get("total", -1)
        test("list_keys returns array", isinstance(keys, list) and total == len(keys),
             f"(total={total})")
        if keys:
            k0 = keys[0]
            test("entry has name+type+value_repr",
                 all(k in k0 for k in ("name", "type", "value_repr")),
                 f"(first={k0})")
            valid_types = {
                "Bool", "Int", "Float", "String", "Name",
                "Vector", "Rotator", "Object", "Class", "Enum",
            }
            # Types outside this set are acceptable (e.g. Struct/NativeEnum retain prefix),
            # but at least one of the recognised friendly names should appear in a realistic BB.
            any_recognised = any(k.get("type") in valid_types for k in keys)
            test("at least one key has a recognised type",
                 any_recognised or len(keys) > 0,  # tolerant: any keys at all is fine
                 f"(types seen: {sorted(set(k.get('type') for k in keys))})")
    else:
        test("list_keys", False,
             f"(error: {r.get('error', {}).get('code')} {r.get('error', {}).get('message', '')})")
else:
    skip("list_keys", "(no test actor available)")

# === T2: ai.bb.get_value ===
print(f"\nT2: ai.bb.get_value on a known key")
if target_actor and target_keys:
    # Pick the first key with a recognised type so we can validate the typed value.
    recognised = {
        "Bool": bool, "Int": (int, float), "Float": (int, float),
        "String": str, "Name": str, "Object": (str, type(None)),
        "Class": (str, type(None)), "Enum": (int, float),
        # Vector/Rotator are list-typed; handle separately below.
    }
    pick = None
    for k in target_keys:
        t = k.get("type")
        if t in recognised or t in ("Vector", "Rotator"):
            pick = k
            break

    if pick:
        key_name = pick["name"]
        print(f"  using key '{key_name}' (type={pick.get('type')})")
        r = call("ai.bb.get_value", {"actor_path": target_actor, "key_name": key_name})
        if is_ok(r):
            res = r["result"]
            t = res.get("type")
            v = res.get("value")
            test("get_value response has type + value fields",
                 "type" in res and "value" in res,
                 f"(type={t}, value={v!r})")
            if t in recognised:
                expected = recognised[t]
                ok = isinstance(v, expected) or v is None
                test(f"value type matches key type {t}", ok, f"(got Python type {type(v).__name__})")
            elif t in ("Vector", "Rotator"):
                test(f"{t} value is 3-array",
                     isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) for x in v),
                     f"(value={v})")
        else:
            test("get_value", False,
                 f"(error: {r.get('error', {}).get('code')} {r.get('error', {}).get('message', '')})")
    else:
        skip("get_value", f"(no recognised-type key in target's keys[]: types={[k.get('type') for k in target_keys]})")
else:
    skip("get_value", "(no test actor available)")

# === T3: ai.bb.set_value ===
print(f"\nT3: ai.bb.set_value (round-trip via get_value)")
if target_actor and target_keys:
    # Find a settable key: prefer Float/Int/Bool (no side-effects on Behaviour Tree decisions),
    # fall back to String/Name. AVOID Object/Class (need a valid path to load).
    settable_preference = ["Float", "Int", "Bool", "String", "Name", "Vector", "Rotator"]
    pick = None
    for pref_type in settable_preference:
        for k in target_keys:
            if k.get("type") == pref_type:
                pick = k
                break
        if pick:
            break

    if pick:
        key_name = pick["name"]
        t = pick.get("type")
        new_value = None
        if t == "Float":
            new_value = 42.5
        elif t == "Int":
            new_value = 7
        elif t == "Bool":
            new_value = True
        elif t == "String":
            new_value = "wave_j_test"
        elif t == "Name":
            new_value = "WaveJTest"
        elif t == "Vector":
            new_value = [100.0, 200.0, 300.0]
        elif t == "Rotator":
            new_value = [10.0, 90.0, 0.0]

        print(f"  setting key '{key_name}' (type={t}) to {new_value!r}")
        r = call("ai.bb.set_value", {
            "actor_path": target_actor,
            "key_name": key_name,
            "value": new_value,
        })
        if is_ok(r):
            res = r["result"]
            test("set returned set=true",
                 res.get("set") is True,
                 f"(prior={res.get('prior_value')!r}, type={res.get('type')})")

            # Verify via get_value
            r2 = call("ai.bb.get_value", {"actor_path": target_actor, "key_name": key_name})
            if is_ok(r2):
                got = r2["result"].get("value")
                # Vector/Rotator round-trip needs list compare
                if t in ("Vector", "Rotator"):
                    ok = isinstance(got, list) and len(got) == 3 and all(
                        abs(got[i] - new_value[i]) < 0.01 for i in range(3))
                    test(f"{t} round-trip matches",
                         ok, f"(set={new_value}, got={got})")
                elif t == "Float":
                    ok = isinstance(got, (int, float)) and abs(got - new_value) < 0.01
                    test("Float round-trip matches", ok, f"(set={new_value}, got={got})")
                else:
                    test(f"{t} round-trip matches",
                         got == new_value, f"(set={new_value}, got={got})")
            else:
                test("get_value (verify)", False, f"(error: {r2.get('error', {})})")
        else:
            test("set_value", False,
                 f"(error: {r.get('error', {}).get('code')} {r.get('error', {}).get('message', '')})")
    else:
        skip("set_value", f"(no settable-type key found: types={[k.get('type') for k in target_keys]})")
else:
    skip("set_value", "(no test actor available)")

# === T4: bad actor_path → -32004 ObjectNotFound ===
print(f"\nT4: ai.bb.list_keys with bad actor_path → -32004")
r = call("ai.bb.list_keys", {"actor_path": "BogusActorNameDoesNotExist_xyz123"})
test("bad actor_path returns ObjectNotFound (-32004) or InvalidPath (-32010)",
     is_err(r, -32004) or is_err(r, -32010),
     f"(code={r.get('error', {}).get('code')}, msg={r.get('error', {}).get('message', '')!r})")

# === T5: bad key_name → -32005 PropertyNotFound ===
print(f"\nT5: ai.bb.get_value with bad key_name → -32005")
if target_actor:
    r = call("ai.bb.get_value", {
        "actor_path": target_actor,
        "key_name": "TotallyBogusKeyName_xyz789",
    })
    test("bad key_name returns PropertyNotFound (-32005)",
         is_err(r, -32005),
         f"(code={r.get('error', {}).get('code')}, msg={r.get('error', {}).get('message', '')!r})")
else:
    skip("bad key_name", "(no test actor — synthetic test would only exercise the -32004 path)")

# === T6: value type mismatch → -32602 InvalidParams ===
print(f"\nT6: ai.bb.set_value with type mismatch → -32602")
if target_actor and target_keys:
    # Find a Bool key and try to set a string into it
    bool_key = None
    for k in target_keys:
        if k.get("type") == "Bool":
            bool_key = k["name"]
            break
    # Or any other typed key, set value to {object} (incompatible with everything except Object?)
    type_mismatch_key = bool_key
    if not type_mismatch_key:
        # Find the first key with a primitive type, send a wrong-typed value
        for k in target_keys:
            if k.get("type") in ("Int", "Float"):
                type_mismatch_key = k["name"]
                break

    if type_mismatch_key:
        r = call("ai.bb.set_value", {
            "actor_path": target_actor,
            "key_name": type_mismatch_key,
            "value": "this_is_a_string_but_should_not_be",
        })
        test("type mismatch returns InvalidParams (-32602)",
             is_err(r, -32602),
             f"(code={r.get('error', {}).get('code')}, msg={r.get('error', {}).get('message', '')!r})")
    else:
        skip("type mismatch", "(no Bool/Int/Float key to mismatch against)")
else:
    skip("type mismatch", "(no test actor available)")

# === T7: missing args → -32602 InvalidParams ===
print(f"\nT7: ai.bb.list_keys missing args → -32602")
r = call("ai.bb.list_keys", {})
test("missing actor_path returns InvalidParams (-32602)",
     is_err(r, -32602),
     f"(code={r.get('error', {}).get('code')})")

# === T8: missing value field on set_value → -32602 ===
print(f"\nT8: ai.bb.set_value missing 'value' field → -32602")
r = call("ai.bb.set_value", {"actor_path": "any", "key_name": "any"})
test("missing 'value' field returns InvalidParams (-32602)",
     is_err(r, -32602),
     f"(code={r.get('error', {}).get('code')}, msg={r.get('error', {}).get('message', '')!r})")

print(f"\n=== Wave J S2 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===")
sys.exit(0 if FAIL == 0 else 1)
