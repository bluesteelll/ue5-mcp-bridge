"""Wave I Surface 6 — landscape.* test (4 tools).

Tests landscape inspection tools:
  T1: landscape.list                  → enumerate ALandscape actors
  T2: landscape.get_info              → component_size_quads, layer_infos, etc.
  T3: landscape.get_height_at         → sample heightmap at world XY
  T4: landscape.get_layer_weights     → sample weights per layer at world XY
  T5: landscape.get_info bad path     → -32004
  T6: landscape.get_info non-landscape → -32011

If no landscape in current map → T1 passes with empty array; T2-T4 SKIP.
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

print('=== Wave I S6 — landscape.* (4 tools) ===\n')

# T1: landscape.list
print('T1: landscape.list — enumerate ALandscape actors')
r = call('landscape.list')
ok = is_ok(r) and isinstance(r['result'].get('landscapes'), list)
landscapes = r['result'].get('landscapes', []) if ok else []
test('list returns landscapes array', ok, f'(count={len(landscapes)})')

discovered_landscape = None
if ok and landscapes:
    first = landscapes[0]
    required = ('actor_path', 'component_count', 'world_bounds')
    missing = [k for k in required if k not in first]
    if missing:
        test('list entry shape', False, f'missing fields: {missing}')
    else:
        wb = first.get('world_bounds', {})
        origin = wb.get('origin')
        extent = wb.get('extent')
        shape_ok = (isinstance(origin, list) and len(origin) == 3
                    and isinstance(extent, list) and len(extent) == 3)
        test('list entry shape', shape_ok,
             f"actor={first['actor_path'][:60]}... "
             f"component_count={first['component_count']} "
             f"origin={origin} extent={extent}")
        if shape_ok:
            discovered_landscape = first['actor_path']
else:
    print('  (no ALandscape actors in current map — T2/T3/T4 will SKIP)')

# T2: landscape.get_info on discovered landscape
print('\nT2: landscape.get_info on discovered landscape')
if discovered_landscape:
    r = call('landscape.get_info', {'landscape_path': discovered_landscape})
    if is_ok(r):
        res = r['result']
        required = ('component_size_quads', 'num_subsections', 'layer_infos',
                    'min_z', 'max_z', 'total_components')
        missing = [k for k in required if k not in res]
        if missing:
            test('get_info shape', False, f'missing fields: {missing}')
        else:
            layer_infos = res['layer_infos']
            layer_ok = isinstance(layer_infos, list) and all(
                isinstance(li, dict) and 'name' in li and li.get('type') in ('Weight', 'Visibility')
                for li in layer_infos)
            test('get_info shape', layer_ok,
                 f"size_quads={res['component_size_quads']} subsections={res['num_subsections']} "
                 f"layers={len(layer_infos)} components={res['total_components']} "
                 f"z=[{res['min_z']:.1f}..{res['max_z']:.1f}]")
    else:
        test('get_info', False, f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:80]}')
else:
    skip('get_info', '(no landscape in map)')

# T3: landscape.get_height_at world (0, 0)
print('\nT3: landscape.get_height_at world_x=0 world_y=0')
if discovered_landscape:
    r = call('landscape.get_height_at', {
        'landscape_path': discovered_landscape,
        'world_x': 0.0,
        'world_y': 0.0,
    })
    if is_ok(r):
        res = r['result']
        required = ('height_z', 'has_data', 'world_xy')
        missing = [k for k in required if k not in res]
        if missing:
            test('get_height_at shape', False, f'missing fields: {missing}')
        else:
            shape_ok = (isinstance(res['height_z'], (int, float))
                        and isinstance(res['has_data'], bool)
                        and isinstance(res['world_xy'], list)
                        and len(res['world_xy']) == 2)
            test('get_height_at shape', shape_ok,
                 f"height_z={res['height_z']:.2f} has_data={res['has_data']} world_xy={res['world_xy']}")
    else:
        test('get_height_at', False, f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:80]}')
else:
    skip('get_height_at', '(no landscape in map)')

# T4: landscape.get_layer_weights world (0, 0)
print('\nT4: landscape.get_layer_weights world_x=0 world_y=0')
if discovered_landscape:
    r = call('landscape.get_layer_weights', {
        'landscape_path': discovered_landscape,
        'world_x': 0.0,
        'world_y': 0.0,
    })
    if is_ok(r):
        res = r['result']
        required = ('weights', 'has_data')
        missing = [k for k in required if k not in res]
        if missing:
            test('get_layer_weights shape', False, f'missing fields: {missing}')
        else:
            shape_ok = (isinstance(res['weights'], dict)
                        and isinstance(res['has_data'], bool))
            # If has_data=true, all weight values must be in [0,1]
            weights_valid = all(
                isinstance(v, (int, float)) and 0.0 <= v <= 1.0
                for v in res['weights'].values())
            test('get_layer_weights shape', shape_ok and weights_valid,
                 f"weights={dict(list(res['weights'].items())[:3])} has_data={res['has_data']}")
    else:
        test('get_layer_weights', False, f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:80]}')
else:
    skip('get_layer_weights', '(no landscape in map)')

# T5: landscape.get_info with bad path → -32004
print('\nT5: landscape.get_info with bad path → -32004')
r = call('landscape.get_info', {
    'landscape_path': '/Game/NotARealMap.NotARealMap:PersistentLevel.NotARealLandscape',
})
test('bad path → -32004', is_err(r, -32004),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

# T6: landscape.get_info on non-landscape actor → -32011
print('\nT6: landscape.get_info on non-landscape actor → -32011')
# Find an actor that's NOT a landscape — use actor.find_by_class(StaticMeshActor)
r = call('actor.find_by_class', {
    'class_path': '/Script/Engine.StaticMeshActor',
    'page_size': 5,
})
non_landscape_path = None
if is_ok(r):
    actors = r['result'].get('actors', [])
    for a in actors:
        non_landscape_path = a.get('actor_path')
        if non_landscape_path:
            break

if non_landscape_path:
    r = call('landscape.get_info', {'landscape_path': non_landscape_path})
    test('non-landscape actor → -32011', is_err(r, -32011),
         f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')
else:
    skip('non-landscape actor', '(no StaticMeshActor found via actor.find_by_class)')

# T7: missing required field → -32602
print('\nT7: landscape.get_height_at missing world_x → -32602')
r = call('landscape.get_height_at', {
    'landscape_path': discovered_landscape or '/Game/Foo',
    'world_y': 0.0,
})
test('missing world_x → -32602', is_err(r, -32602),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

print(f'\n{"="*60}')
print(f'WAVE I SURFACE 6 TEST: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP')
print(f'{"="*60}')
sys.exit(0 if FAIL == 0 else 1)
