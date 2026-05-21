"""Wave J Surface 5 — ai.perception.* test (3 tools).

Tests AI perception component introspection:
  T1: ai.perception.list_components    → enumerate UAIPerceptionComponent in world
  T2: ai.perception.get_config         → per-sense config for one perception component
  T3: ai.perception.get_perceived_actors → currently-sensed actors with stimulus details
  T4: ai.perception.get_config bad path → -32004 ObjectNotFound
  T5: ai.perception.get_config on actor without perception → -32011 WrongClass

If no AI pawn with perception in current map → T1 returns empty array; T2/T3 SKIP.
"""
import socket, json, time, sys

HOST, PORT = '127.0.0.1', 30020
PASS = 0; FAIL = 0; SKIPPED = 0


def call(method, args=None, timeout=20):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    req = {'id': 't_' + str(int(time.time() * 1000)), 'kind': 'call_function', 'method': method, 'args': args or {}}
    s.sendall((json.dumps(req) + '\n').encode())
    buf = b''
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = s.recv(65536)
        if not c:
            break
        buf += c
        if b'\n' in buf:
            break
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


print('=== Wave J S5 — ai.perception.* (3 tools) ===\n')

# ── T1: list_components ─────────────────────────────────────────────────────
print('T1: ai.perception.list_components — enumerate perception components')
r = call('ai.perception.list_components')
ok = is_ok(r) and isinstance(r['result'].get('perception_components'), list)
components = r['result'].get('perception_components', []) if ok else []
world_kind = r['result'].get('world_kind', '?') if ok else '?'
test('list_components returns array',
     ok,
     f'(count={len(components)}, world_kind={world_kind})')

discovered_actor = None
if ok and components:
    first = components[0]
    required = ('owner_actor_path', 'sense_configs')
    missing = [k for k in required if k not in first]
    if missing:
        test('list entry shape', False, f'missing fields: {missing}')
    else:
        sense_configs = first.get('sense_configs', [])
        cfg_shape_ok = isinstance(sense_configs, list) and all(
            isinstance(s, dict) and 'sense_class' in s and 'dominant' in s
            for s in sense_configs)
        test('list entry shape',
             cfg_shape_ok,
             f"owner={first['owner_actor_path'][:60]}... senses={len(sense_configs)}")
        if cfg_shape_ok:
            discovered_actor = first['owner_actor_path']
            print(f'  discovered actor: {discovered_actor}')
            if sense_configs:
                print(f'  first sense: {sense_configs[0].get("sense_class", "?")}'
                      f' dominant={sense_configs[0].get("dominant")}')
else:
    print('  (no UAIPerceptionComponent in current world — T2/T3 will SKIP)')

# ── T2: get_config on discovered actor ──────────────────────────────────────
print('\nT2: ai.perception.get_config on discovered perception component')
if discovered_actor:
    r = call('ai.perception.get_config', {'actor_path': discovered_actor})
    if is_ok(r):
        res = r['result']
        required = ('actor_path', 'sense_configs')
        missing = [k for k in required if k not in res]
        if missing:
            test('get_config shape', False, f'missing fields: {missing}')
        else:
            sense_configs = res['sense_configs']
            cfg_shape_ok = isinstance(sense_configs, list) and all(
                isinstance(s, dict) and 'sense_class' in s
                and 'max_age' in s and 'starts_enabled' in s
                and 'dominant' in s and 'sense_specific_props' in s
                and isinstance(s['sense_specific_props'], dict)
                for s in sense_configs)
            dominant_str = res.get('dominant_sense_class', '(none)')
            test('get_config shape',
                 cfg_shape_ok,
                 f'senses={len(sense_configs)} dominant={dominant_str}')
            if cfg_shape_ok and sense_configs:
                first_cfg = sense_configs[0]
                print(f'  first sense_class: {first_cfg["sense_class"]}')
                print(f'  max_age={first_cfg["max_age"]} starts_enabled={first_cfg["starts_enabled"]}')
                if first_cfg['sense_specific_props']:
                    props_keys = list(first_cfg['sense_specific_props'].keys())[:5]
                    print(f'  sense_specific_props sample keys: {props_keys}')
    else:
        test('get_config call', False, f'error={r.get("error")}')
else:
    skip('get_config', '(no actor with perception)')

# ── T3: get_perceived_actors ────────────────────────────────────────────────
print('\nT3: ai.perception.get_perceived_actors on discovered component')
if discovered_actor:
    r = call('ai.perception.get_perceived_actors', {'actor_path': discovered_actor})
    if is_ok(r):
        res = r['result']
        perceived = res.get('perceived', [])
        is_list = isinstance(perceived, list)
        test('get_perceived_actors returns array',
             is_list,
             f'(count={len(perceived)})')
        if is_list and perceived:
            first = perceived[0]
            required = ('sense_class', 'stimulus_age', 'stimulus_location',
                        'is_active', 'is_successfully_sensed')
            missing = [k for k in required if k not in first]
            shape_ok = not missing
            test('perceived entry shape',
                 shape_ok,
                 f'sample: sense={first.get("sense_class", "?")} '
                 f'age={first.get("stimulus_age", "?")} '
                 f'active={first.get("is_active", "?")}')
        elif is_list:
            print('  (perceived list is empty — editor world has no live stimuli flowing,'
                  ' or no targets perceived yet)')

        # T3b: get_perceived_actors with sense_filter
        print('\nT3b: ai.perception.get_perceived_actors with sense_filter=AISense_Sight')
        r2 = call('ai.perception.get_perceived_actors', {
            'actor_path': discovered_actor,
            'sense_filter': 'AISense_Sight',
        })
        ok2 = is_ok(r2) and isinstance(r2['result'].get('perceived'), list)
        filtered = r2['result'].get('perceived', []) if ok2 else []
        test('filter sense_class', ok2, f'filtered_count={len(filtered)}')
    else:
        test('get_perceived_actors call', False, f'error={r.get("error")}')
else:
    skip('get_perceived_actors', '(no actor with perception)')

# ── T4: get_config with bad actor_path → -32004 ─────────────────────────────
print('\nT4: ai.perception.get_config bad actor_path (expect -32004)')
r = call('ai.perception.get_config', {'actor_path': '/Game/NoSuch.NoSuch:PersistentLevel.NotARealActor_zZz9'})
ok = is_err(r, -32004)
err_msg = r.get('error', {}).get('message', '')[:80] if not ok else ''
test('bad actor_path → -32004', ok, f'msg={err_msg}' if err_msg else '')

# ── T5: get_config on actor without perception component → -32011 ──────────
# Use actor.find_by_class('/Script/Engine.Actor') to enumerate every actor in the world, then
# pick the first that isn't in our perception_components list. SKIP if no non-perception
# actor exists OR if actor.find_by_class is unavailable.
print('\nT5: ai.perception.get_config on actor without perception (expect -32011)')
try:
    r_actors = call('actor.find_by_class', {'class_path': '/Script/Engine.Actor'})
    if is_ok(r_actors):
        actor_entries = r_actors['result'].get('actors', [])
        perception_owners = {c.get('owner_actor_path') for c in components}
        non_perception_actor = None
        for a in actor_entries:
            actor_path = a.get('actor_path') if isinstance(a, dict) else None
            if actor_path and actor_path not in perception_owners:
                non_perception_actor = actor_path
                break
        if non_perception_actor:
            r5 = call('ai.perception.get_config', {'actor_path': non_perception_actor})
            ok5 = is_err(r5, -32011)
            err_msg5 = r5.get('error', {}).get('message', '')[:80] if not ok5 else ''
            test('actor without perception → -32011',
                 ok5,
                 f'actor={non_perception_actor[:50]}... msg={err_msg5}' if err_msg5
                 else f'actor={non_perception_actor[:50]}...')
        else:
            skip('actor without perception → -32011', '(no non-perception actor found)')
    else:
        skip('actor without perception → -32011',
             f'(actor.find_by_class unavailable: {r_actors.get("error")})')
except Exception as exc:
    skip('actor without perception → -32011', f'(actor.find_by_class raised: {exc})')

# ── T6: missing required arg → -32602 ──────────────────────────────────────
print('\nT6: ai.perception.get_config missing actor_path (expect -32602)')
r = call('ai.perception.get_config', {})
ok = is_err(r, -32602)
test('missing actor_path → -32602', ok)

# ── T7: get_perceived_actors with malformed sense_filter → -32602 ──────────
print('\nT7: ai.perception.get_perceived_actors malformed sense_filter (expect -32602)')
if discovered_actor:
    r = call('ai.perception.get_perceived_actors', {
        'actor_path': discovered_actor,
        'sense_filter': 'NotARealSenseClass_zZz9',
    })
    ok = is_err(r, -32602)
    test('malformed sense_filter → -32602', ok)
else:
    skip('malformed sense_filter → -32602', '(no actor with perception)')

print(f'\n=== Wave J S5 result: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
