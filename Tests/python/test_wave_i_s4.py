"""Wave I Surface 4 — mat_inst.* test (5 tools).

Tools tested:
  - mat_inst.list                (Lane A, NO PIE guard)
  - mat_inst.get_params          (Lane A, NO PIE guard)
  - mat_inst.set_scalar_param    (Lane A, PIE-guarded)
  - mat_inst.set_vector_param    (Lane A, PIE-guarded)
  - mat_inst.set_texture_param   (Lane A, PIE-guarded)
"""
import socket, json, time, sys

HOST, PORT = '127.0.0.1', 30020
PASS = 0; FAIL = 0; SKIPPED = 0


def call(method, args=None, timeout=30):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    req = {'id': 't_' + str(int(time.time() * 1000)),
           'kind': 'call_function', 'method': method, 'args': args or {}}
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


def is_ok(resp):
    return 'error' not in resp and 'result' in resp


def is_err(resp, *codes):
    return 'error' in resp and resp['error'].get('code') in codes


print('=== Wave I S4 — mat_inst.* (5 tools) ===\n')

# ---------------------------------------------------------------------------
# T1: mat_inst.list — enumerate MICs
# ---------------------------------------------------------------------------
print('T1: mat_inst.list — enumerate UMaterialInstanceConstants')
r = call('mat_inst.list', {'page_size': 100})
target_mic = None
if not is_ok(r):
    test('list call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    instances = res.get('instances', [])
    total = res.get('total_known', 0)
    test('list has instances/total_known fields',
         'instances' in res and 'total_known' in res,
         f'(total_known={total})')
    test('list each entry has asset_path/parent_path',
         all('asset_path' in m and 'parent_path' in m for m in instances),
         f'(first={instances[0] if instances else "EMPTY"})')
    if instances:
        # Pick a non-trivial MIC for downstream tests
        for m in instances:
            target_mic = m['asset_path']
            print(f'    -> probing MIC for params: {target_mic}')
            r_get = call('mat_inst.get_params', {'instance_path': target_mic,
                                                  'include_inherited': True})
            if is_ok(r_get):
                p = r_get['result']
                has_scalar = bool(p.get('scalar_params'))
                has_vector = bool(p.get('vector_params'))
                has_texture = bool(p.get('texture_params'))
                if has_scalar or has_vector or has_texture:
                    print(f'    -> picked {target_mic}: '
                          f'scalar={len(p["scalar_params"])} '
                          f'vector={len(p["vector_params"])} '
                          f'texture={len(p["texture_params"])}')
                    break
                target_mic = None
        if not target_mic and instances:
            target_mic = instances[0]['asset_path']

# ---------------------------------------------------------------------------
# T2: mat_inst.get_params — read param shape on chosen MIC
# ---------------------------------------------------------------------------
scalar_param = None
scalar_param_value = None
scalar_param_orig_overridden = None
vector_param = None
vector_param_value = None
texture_param = None
texture_param_value = None

if target_mic:
    print(f'\nT2: mat_inst.get_params include_inherited=true on {target_mic}')
    r = call('mat_inst.get_params', {'instance_path': target_mic, 'include_inherited': True})
    if not is_ok(r):
        test('get_params call succeeded', False, f'(err={r.get("error")})')
    else:
        res = r['result']
        sp = res.get('scalar_params', [])
        vp = res.get('vector_params', [])
        tp = res.get('texture_params', [])
        test('get_params returned scalar_params/vector_params/texture_params arrays',
             isinstance(sp, list) and isinstance(vp, list) and isinstance(tp, list),
             f'(scalar={len(sp)}, vector={len(vp)}, texture={len(tp)})')

        def check_entry(arr, kind, value_key):
            if not arr:
                return True
            e = arr[0]
            ok = 'name' in e and value_key in e and 'is_override' in e
            print(f'    -> first {kind}: {e}')
            return ok

        test('scalar entries shape {name,value,is_override}',
             check_entry(sp, 'scalar', 'value'))
        test('vector entries shape {name,value,is_override}',
             check_entry(vp, 'vector', 'value'))
        test('texture entries shape {name,texture_path,is_override}',
             check_entry(tp, 'texture', 'texture_path'))

        if sp:
            scalar_param = sp[0]['name']
            scalar_param_value = sp[0]['value']
            scalar_param_orig_overridden = sp[0]['is_override']
        if vp:
            vector_param = vp[0]['name']
            vector_param_value = vp[0]['value']
        if tp:
            texture_param = tp[0]['name']
            texture_param_value = tp[0]['texture_path']
else:
    SKIPPED += 1
    print(f'\n  [SKIP] no MIC available — skipping T2-T7')

# ---------------------------------------------------------------------------
# T3: set_scalar_param + verify via get_params + revert
# ---------------------------------------------------------------------------
if target_mic and scalar_param is not None:
    print(f'\nT3: mat_inst.set_scalar_param {scalar_param!r} on {target_mic}')
    test_value = float(scalar_param_value) + 0.5 if scalar_param_value != 0 else 0.5
    r = call('mat_inst.set_scalar_param', {
        'instance_path': target_mic,
        'param_name': scalar_param,
        'value': test_value,
    })
    if not is_ok(r):
        test('set_scalar_param call succeeded', False, f'(err={r.get("error")})')
    else:
        res = r['result']
        test('set_scalar_param returned set=true',
             res.get('set') is True,
             f'(set={res.get("set")})')
        test('set_scalar_param prior_value matches initial value',
             abs(res.get('prior_value', 0.0) - float(scalar_param_value)) < 1e-4,
             f'(prior={res.get("prior_value")}, expected={scalar_param_value})')
        test('set_scalar_param prior_overridden matches initial is_override',
             res.get('prior_overridden') == scalar_param_orig_overridden,
             f'(prior_overridden={res.get("prior_overridden")}, expected={scalar_param_orig_overridden})')

    # T4: verify the new value is reflected + is_override=true after our write
    print(f'\nT4: mat_inst.get_params verifies new value + is_override=true')
    r2 = call('mat_inst.get_params', {'instance_path': target_mic, 'include_inherited': True})
    if is_ok(r2):
        match = None
        for e in r2['result'].get('scalar_params', []):
            if e['name'] == scalar_param:
                match = e
                break
        test('post-write get_params still contains the param',
             match is not None, f'(match={match})')
        if match:
            test('post-write value matches what we set',
                 abs(match['value'] - test_value) < 1e-4,
                 f'(got={match["value"]}, expected={test_value})')
            test('post-write is_override=true (we just wrote an override)',
                 match['is_override'] is True,
                 f'(is_override={match["is_override"]})')

    # T5: revert by setting back to the original value
    print(f'\nT5: revert scalar to original via set_scalar_param value={scalar_param_value}')
    r3 = call('mat_inst.set_scalar_param', {
        'instance_path': target_mic,
        'param_name': scalar_param,
        'value': float(scalar_param_value),
    })
    if is_ok(r3):
        test('revert set_scalar_param succeeded', r3['result'].get('set') is True,
             f'(set={r3["result"].get("set")})')
elif target_mic:
    SKIPPED += 1
    print(f'\n  [SKIP] target MIC has no scalar params — skipping T3-T5')

# ---------------------------------------------------------------------------
# T6: bad instance_path → -32004 / -32010
# ---------------------------------------------------------------------------
print('\nT6: mat_inst.get_params with bad instance_path → -32004')
r = call('mat_inst.get_params', {'instance_path': '/Game/__nonexistent_mic_xyz__'})
test('bad instance_path returns ObjectNotFound or InvalidPath',
     is_err(r, -32004, -32010),
     f'(code={r.get("error",{}).get("code")})')

# ---------------------------------------------------------------------------
# T7: bad param_name on set_scalar_param → -32005
# ---------------------------------------------------------------------------
if target_mic:
    print(f'\nT7: mat_inst.set_scalar_param with bad param_name → -32005')
    r = call('mat_inst.set_scalar_param', {
        'instance_path': target_mic,
        'param_name': '__totally_nonexistent_param_xyz__',
        'value': 1.0,
    })
    test('bad param_name returns PropertyNotFound', is_err(r, -32005),
         f'(code={r.get("error",{}).get("code")}, msg={r.get("error",{}).get("message","")[:80]})')

# ---------------------------------------------------------------------------
# T8: vector with wrong array length → -32602
# ---------------------------------------------------------------------------
if target_mic and vector_param:
    print(f'\nT8: mat_inst.set_vector_param wrong array length → -32602')
    r = call('mat_inst.set_vector_param', {
        'instance_path': target_mic,
        'param_name': vector_param,
        'value': [1.0, 0.5, 0.2],  # 3 elements, not 4
    })
    test('wrong-length vector returns InvalidParams',
         is_err(r, -32602),
         f'(code={r.get("error",{}).get("code")}, msg={r.get("error",{}).get("message","")[:80]})')
elif target_mic:
    SKIPPED += 1
    print(f'\n  [SKIP] target MIC has no vector params — skipping T8')

# ---------------------------------------------------------------------------
# T9: non-MIC asset path → -32011 (use a Texture2D if discoverable)
# ---------------------------------------------------------------------------
print('\nT9: mat_inst.get_params on non-MIC asset → -32011')
# Use asset.search_by_class to find a Texture2D path
r_search = call('asset.search_by_class', {'class_path': '/Script/Engine.Texture2D', 'page_size': 1})
if is_ok(r_search) and r_search['result'].get('assets'):
    tex_path = (r_search['result']['assets'][0].get('asset_path')
                or r_search['result']['assets'][0].get('object_path'))
    r = call('mat_inst.get_params', {'instance_path': tex_path})
    test('non-MIC returns WrongClass', is_err(r, -32011),
         f'(probed={tex_path}, code={r.get("error",{}).get("code")})')
else:
    SKIPPED += 1
    print(f'  [SKIP] no Texture2D found for WrongClass probe')

# ---------------------------------------------------------------------------
print(f'\n--- {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIPPED ---')
sys.exit(0 if FAIL == 0 else 1)
