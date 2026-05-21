"""Wave I Surface 2 — soft_ref.* test (4 tools)."""
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

def is_ok(resp):
    return 'error' not in resp and 'result' in resp

def is_err(resp, code):
    return 'error' in resp and resp['error'].get('code') == code

print('=== Wave I S2 — soft_ref.* (4 tools) ===\n')

# Discover a valid /Game asset to use for validate/resolve tests.
print('Bootstrap: discover a /Game asset for valid-syntax tests')
r = call('asset.list', {'path_prefix': '/Game', 'page_size': 50})
valid_target = None
if is_ok(r):
    for a in r['result'].get('assets', []):
        p = a.get('asset_path') or a.get('object_path')
        if p and p.startswith('/Game/'):
            valid_target = p
            break
if not valid_target:
    print('  (no /Game asset found — falling back to /Engine/EditorMaterials/EditorAxisMaterial)')
    valid_target = '/Engine/EditorMaterials/EditorAxisMaterial'
print(f'    -> valid_target: {valid_target}')

# T1: soft_ref.validate on valid path
print('\nT1: soft_ref.validate on valid path')
r = call('soft_ref.validate', {'soft_path': valid_target})
ok = is_ok(r) and r['result'].get('valid_syntax') is True and r['result'].get('target_exists') is True
test('validate returns valid_syntax+target_exists', ok,
     f'(valid_syntax={r.get("result",{}).get("valid_syntax")}, '
     f'target_exists={r.get("result",{}).get("target_exists")}, '
     f'target_class={r.get("result",{}).get("target_class")})')

# T2: soft_ref.validate on nonexistent path
print('\nT2: soft_ref.validate on /Game/__nonexistent__')
r = call('soft_ref.validate', {'soft_path': '/Game/__nonexistent__'})
ok = is_ok(r) and r['result'].get('valid_syntax') is True and r['result'].get('target_exists') is False
test('validate returns target_exists=false', ok,
     f'(valid_syntax={r.get("result",{}).get("valid_syntax")}, '
     f'target_exists={r.get("result",{}).get("target_exists")})')

# T3: soft_ref.resolve on valid path, force_load=false
print(f'\nT3: soft_ref.resolve on {valid_target} (force_load=false)')
r = call('soft_ref.resolve', {'soft_path': valid_target, 'force_load': False})
if is_ok(r):
    rp = r['result'].get('resolved_path')
    test('resolve returns resolved_path', bool(rp),
         f'(resolved_path={rp}, was_loaded={r["result"].get("was_loaded")}, '
         f'target_class={r["result"].get("target_class")})')
else:
    test('resolve', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T3b: soft_ref.resolve with force_load=true
print(f'\nT3b: soft_ref.resolve on {valid_target} (force_load=true)')
r = call('soft_ref.resolve', {'soft_path': valid_target, 'force_load': True})
if is_ok(r):
    rp = r['result'].get('resolved_path')
    wl = r['result'].get('was_loaded')
    test('resolve+force_load returns resolved_path+was_loaded', bool(rp) and wl is True,
         f'(resolved_path={rp}, was_loaded={wl})')
else:
    test('resolve+force_load', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T4: soft_ref.find_redirectors under /Game
print(f'\nT4: soft_ref.find_redirectors path_prefix=/Game')
r = call('soft_ref.find_redirectors', {'path_prefix': '/Game', 'page_size': 50})
if is_ok(r):
    arr = r['result'].get('redirectors', [])
    total = r['result'].get('total_known', 0)
    test('find_redirectors returns array', isinstance(arr, list),
         f'(count={len(arr)}, total_known={total})')
    if arr:
        r0 = arr[0]
        has_shape = 'redirector_path' in r0 and 'target_path' in r0
        test('first redirector has expected shape', has_shape,
             f'(keys={list(r0.keys())})')
        print(f'    -> sample: {r0.get("redirector_path")} -> {r0.get("target_path")}')
else:
    test('find_redirectors', False,
         f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T5: soft_ref.fix_redirectors dry_run=true on /Game
print(f'\nT5: soft_ref.fix_redirectors dry_run=true path_prefix=/Game')
r = call('soft_ref.fix_redirectors', {'path_prefix': '/Game', 'dry_run': True})
if is_ok(r):
    fixed = r['result'].get('fixed', -1)
    skipped = r['result'].get('skipped', -1)
    errors = r['result'].get('errors', [])
    test('dry_run returns fixed/skipped/errors fields',
         isinstance(fixed, int) and isinstance(skipped, int) and isinstance(errors, list),
         f'(fixed={fixed}, skipped={skipped}, errors={len(errors)}, dry_run={r["result"].get("dry_run")})')
else:
    test('fix_redirectors dry_run', False,
         f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T6: malformed soft_path -> -32010
print(f'\nT6: malformed soft_path -> -32010 (or -32004)')
r = call('soft_ref.resolve', {'soft_path': ''})
# Empty arg triggers the missing-string-field guard (-32602), not malformed-path.
# Try a string that parses as invalid FSoftObjectPath instead:
if is_err(r, -32602):
    r = call('soft_ref.resolve', {'soft_path': '   '})  # whitespace-only
if is_err(r, -32602):
    # The validate path also rejects garbage syntax through valid_syntax=false; resolve
    # surfaces -32010. Try a clearly malformed path:
    r = call('soft_ref.resolve', {'soft_path': '\\garbage\\backslash\\path'})
ok = is_err(r, -32010) or is_err(r, -32004) or is_err(r, -32602)
test('malformed path returns -32010/-32004/-32602', ok,
     f'(code={r.get("error",{}).get("code")}, msg={r.get("error",{}).get("message","")[:80]})')

# T7: fix_redirectors with both args empty -> -32602
print(f'\nT7: fix_redirectors with both args empty -> -32602')
r = call('soft_ref.fix_redirectors', {'dry_run': True})
test('both-empty returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error",{}).get("code")}, msg={r.get("error",{}).get("message","")[:80]})')

print(f'\n=== Wave I S2 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
