"""Wave J Surface 3 — ai.controller.* test (3 tools).

Tests AI controller enumeration + introspection:
  T1: ai.controller.list                  → enumerate AAIController actors
  T2: ai.controller.get_state             → detailed runtime info on first controller
  T3: ai.controller.respawn_blackboard    → replace blackboard asset (SKIP if no BB available)
  T4: ai.controller.get_state bad path    → -32004
  T5: ai.controller.respawn_blackboard bad asset path → -32004
  T6: ai.controller.list bad class_filter → -32011

If no AI controllers in current map → T1 passes with empty array; T2/T3 SKIP.
"""
import socket, json, time, sys

HOST, PORT = '127.0.0.1', 30020
PASS = 0; FAIL = 0; SKIPPED = 0

def call(method, args=None, timeout=20):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    req = {'id': 't_'+str(int(time.time()*1000)), 'kind': 'call_function', 'method': method, 'args': args or {}}
    s.sendall((json.dumps(req)+'\n').encode())
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = s.recv(65536)
        if not c: break
        buf += c
        if b'\n' in buf: break
    s.close()
    line = buf[:buf.index(b'\n')] if b'\n' in buf else buf
    return json.loads(line.decode())

def test(name, ok, info=''):
    global PASS, FAIL
    if ok:
        PASS += 1
        print(f'  [PASS] {name} {info}')
    else:
        FAIL += 1
        print(f'  [FAIL] {name} {info}')

def skip(name, info=''):
    global SKIPPED
    SKIPPED += 1
    print(f'  [SKIP] {name} {info}')

def is_ok(resp):
    return 'error' not in resp and 'result' in resp

def is_err(resp, code):
    return 'error' in resp and resp['error'].get('code') == code

print('=== Wave J S3 — ai.controller.* (3 tools) ===\n')

# T1: ai.controller.list
print('T1: ai.controller.list — enumerate AAIController actors')
r = call('ai.controller.list')
ok = is_ok(r) and isinstance(r['result'].get('controllers'), list)
controllers = r['result'].get('controllers', []) if ok else []
test('list returns controllers array', ok, f'(count={len(controllers)})')

discovered_controller = None
if ok and controllers:
    first = controllers[0]
    required = ('actor_path', 'class_path', 'has_blackboard', 'has_active_bt')
    missing = [k for k in required if k not in first]
    if missing:
        test('list entry shape', False, f'missing fields: {missing}')
    else:
        shape_ok = (isinstance(first['actor_path'], str)
                    and isinstance(first['class_path'], str)
                    and isinstance(first['has_blackboard'], bool)
                    and isinstance(first['has_active_bt'], bool))
        test('list entry shape', shape_ok,
             f"actor={first['actor_path'][:60]}... "
             f"class={first['class_path'][:50]}... "
             f"BB={first['has_blackboard']} BT={first['has_active_bt']} "
             f"pawn={'possessed_pawn' in first}")
        if shape_ok:
            discovered_controller = first['actor_path']
else:
    print('  (no AAIController actors in current map — T2/T3 will SKIP)')

# T2: ai.controller.get_state on discovered controller
print('\nT2: ai.controller.get_state on discovered controller')
existing_bb_path = None
if discovered_controller:
    r = call('ai.controller.get_state', {'controller_path': discovered_controller})
    if is_ok(r):
        res = r['result']
        required = ('class_path', 'perception_components')
        missing = [k for k in required if k not in res]
        if missing:
            test('get_state shape', False, f'missing fields: {missing}')
        else:
            shape_ok = (isinstance(res['class_path'], str)
                        and isinstance(res['perception_components'], (int, float)))
            test('get_state shape', shape_ok,
                 f"class={res['class_path'][:50]}... "
                 f"pawn={'possessed_pawn' in res} "
                 f"BT={res.get('active_behavior_tree','-')[:30]} "
                 f"BB={res.get('blackboard_asset','-')[:30]} "
                 f"perc={res['perception_components']} "
                 f"navvol={'navmesh_volume' in res}")
            existing_bb_path = res.get('blackboard_asset')
    else:
        test('get_state', False, f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:80]}')
else:
    skip('get_state', '(no AI controllers in map)')

# T3: ai.controller.respawn_blackboard — round-trip existing BB onto itself
# We use the BB asset already installed on the discovered controller (if any) as a non-destructive
# replacement target. If the controller has no current BB, we SKIP (we don't try to invent a path).
print('\nT3: ai.controller.respawn_blackboard with discovered BB asset')
if discovered_controller and existing_bb_path:
    r = call('ai.controller.respawn_blackboard', {
        'controller_path': discovered_controller,
        'blackboard_asset_path': existing_bb_path,
    })
    if is_ok(r):
        res = r['result']
        shape_ok = (res.get('replaced') is True)
        test('respawn_blackboard shape', shape_ok,
             f"replaced={res.get('replaced')} prior={res.get('prior_blackboard','-')[:40]}")
    else:
        test('respawn_blackboard', False,
             f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:80]}')
else:
    skip('respawn_blackboard', '(no BB asset path discovered on any controller)')

# T4: ai.controller.get_state with bad path → -32004
print('\nT4: ai.controller.get_state with bad path → -32004')
r = call('ai.controller.get_state', {
    'controller_path': '/Game/NotARealMap.NotARealMap:PersistentLevel.NotARealAIController',
})
test('bad path → -32004', is_err(r, -32004),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

# T5: ai.controller.respawn_blackboard with bad asset path → -32004
print('\nT5: ai.controller.respawn_blackboard with bad asset path → -32004')
# Use discovered_controller if any, otherwise a deliberately-bad controller path (which would
# trigger -32004 BEFORE the asset lookup, still satisfying the expected error code).
ctrl_for_t5 = discovered_controller if discovered_controller else (
    '/Game/Foo.Foo:PersistentLevel.Bar')
r = call('ai.controller.respawn_blackboard', {
    'controller_path': ctrl_for_t5,
    'blackboard_asset_path': '/Game/NotARealPath/NotARealBlackboard.NotARealBlackboard',
})
test('bad asset path → -32004', is_err(r, -32004),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

# T6: ai.controller.list with bad class_filter → -32011 (non-AAIController class) or -32004 (no resolve)
print('\nT6: ai.controller.list with non-AAIController class_filter → -32011')
r = call('ai.controller.list', {'class_filter': '/Script/Engine.StaticMeshActor'})
test('non-AIController class_filter → -32011', is_err(r, -32011),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

# T7: ai.controller.get_state missing required field → -32602
print('\nT7: ai.controller.get_state missing controller_path → -32602')
r = call('ai.controller.get_state', {})
test('missing controller_path → -32602', is_err(r, -32602),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

print(f'\n{"="*60}')
print(f'WAVE J SURFACE 3 TEST: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP')
print(f'{"="*60}')
sys.exit(0 if FAIL == 0 else 1)
