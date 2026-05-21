"""Wave H Surface 3 - data_validation.* test (3 tools)."""
import socket, json, time, sys

HOST, PORT = '127.0.0.1', 30020
PASS = 0; FAIL = 0; SKIPPED = 0

def call(method, args=None, timeout=60):
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

print('=== Wave H S3 - data_validation.* (3 tools) ===\n')

# ---------------------------------------------------------------------------
# T1: data_validation.list_validators - enumerate registered UEditorValidatorBase classes
# ---------------------------------------------------------------------------
print('T1: data_validation.list_validators - enumerate validators')
r = call('data_validation.list_validators')
ok = is_ok(r) and isinstance(r['result'].get('validators'), list)
total = r['result'].get('total', 0) if ok else 0
test('list_validators returns array', ok, f'(total={total})')
if ok and total > 0:
    first = r['result']['validators'][0]
    has_keys = all(k in first for k in ('class_path', 'is_enabled', 'description'))
    test('validator entries have class_path/is_enabled/description', has_keys,
         f'(example: class_path={first.get("class_path")!r}, is_enabled={first.get("is_enabled")})')
    # Verify sort is stable
    paths = [v['class_path'] for v in r['result']['validators']]
    is_sorted = paths == sorted(paths)
    test('validators sorted by class_path', is_sorted, f'(sorted={is_sorted})')
elif ok and total == 0:
    SKIPPED += 2
    print(f'  [SKIP] no validators in project - 2 structure subtests skipped')

# ---------------------------------------------------------------------------
# T2: data_validation.validate_asset on a known-good asset (use a stock engine texture)
# ---------------------------------------------------------------------------
print('\nT2: data_validation.validate_asset on a known asset')
# Find any asset to validate - use cb.list to find one we know exists
# Try a stock engine asset path that should always be present
KNOWN_ASSETS = [
    '/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial',
    '/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial',
    '/Engine/EngineResources/Black.Black',
]
asset_validated = False
for asset_path in KNOWN_ASSETS:
    r = call('data_validation.validate_asset', {'asset_path': asset_path})
    if is_ok(r):
        result_val = r['result'].get('result')
        errors = r['result'].get('errors', [])
        warnings = r['result'].get('warnings', [])
        vrun = r['result'].get('validators_run', -1)
        # Any of valid|invalid|not_validated is an OK shape - we want the wire shape to work
        shape_ok = (result_val in ('valid', 'invalid', 'not_validated') and
                    isinstance(errors, list) and isinstance(warnings, list))
        test(f'validate_asset on {asset_path}', shape_ok,
             f'(result={result_val}, errors={len(errors)}, warnings={len(warnings)}, validators_run={vrun})')
        asset_validated = True
        break
    else:
        print(f'  (skipping {asset_path} - {r.get("error", {}).get("message", "<no err msg>")})')
if not asset_validated:
    FAIL += 1
    print('  [FAIL] could not find any known asset to validate')

# ---------------------------------------------------------------------------
# T3: data_validation.validate_asset on bad path -> -32004 or -32010
# ---------------------------------------------------------------------------
print('\nT3: data_validation.validate_asset on bad path -> -32004 or -32010')
r = call('data_validation.validate_asset', {'asset_path': '/Game/__nonexistent_test_asset__'})
test('bad asset_path returns ObjectNotFound or InvalidPath',
     is_err(r, -32004) or is_err(r, -32010),
     f'(code={r.get("error", {}).get("code")})')

# T3b: missing required field
print('\nT3b: data_validation.validate_asset without asset_path -> -32602')
r = call('data_validation.validate_asset', {})
test('missing asset_path returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error", {}).get("code")})')

# ---------------------------------------------------------------------------
# T4: data_validation.validate_path on /Game/MCPTest with small max_assets
# ---------------------------------------------------------------------------
print('\nT4: data_validation.validate_path on /Game (small max_assets=20)')
r = call('data_validation.validate_path', {'path_prefix': '/Game', 'max_assets': 20})
if is_ok(r):
    total_v = r['result'].get('total_validated', -1)
    total_known = r['result'].get('total_known', -1)
    vc = r['result'].get('valid_count', -1)
    ic = r['result'].get('invalid_count', -1)
    nv = r['result'].get('not_validated_count', -1)
    vrun = r['result'].get('validators_run', -1)
    failures = r['result'].get('failures', None)
    # total_validated + total_known >= total_validated; counts sum to total_validated
    counts_ok = (total_v >= 0 and total_known >= total_v and
                 (vc + ic + nv) == total_v and isinstance(failures, list))
    test('validate_path aggregate counts coherent', counts_ok,
         f'(total_validated={total_v}, total_known={total_known}, valid={vc}, invalid={ic}, not_validated={nv}, failures={len(failures) if failures is not None else "<err>"})')
else:
    # Empty /Game project? Re-try with /Engine (which always has assets)
    if is_err(r, -32004):
        print(f'  (no assets in /Game, retrying with /Engine)')
        r = call('data_validation.validate_path', {'path_prefix': '/Engine', 'max_assets': 20})
        if is_ok(r):
            total_v = r['result'].get('total_validated', -1)
            counts_ok = total_v > 0
            test('validate_path on /Engine returns aggregate', counts_ok,
                 f'(total_validated={total_v})')
        else:
            test('validate_path on /Engine returned', False,
                 f'(err={r.get("error")})')
    else:
        test('validate_path on /Game succeeded', False, f'(err={r.get("error")})')

# T4b: validate_path on bad path -> -32010
print('\nT4b: validate_path on bad path -> -32010 or -32004')
r = call('data_validation.validate_path', {'path_prefix': '/__InvalidMount__/Foo', 'max_assets': 10})
test('bad path_prefix returns InvalidPath or ObjectNotFound',
     is_err(r, -32010) or is_err(r, -32004),
     f'(code={r.get("error", {}).get("code")})')

# ---------------------------------------------------------------------------
# T5: max_assets exceeded hard cap -> -32602
# ---------------------------------------------------------------------------
print('\nT5: max_assets > 10000 -> -32602')
r = call('data_validation.validate_path', {'path_prefix': '/Game', 'max_assets': 99999})
test('max_assets over 10000 returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error", {}).get("code")})')

# T5b: max_assets < 1 -> -32602
print('\nT5b: max_assets = 0 -> -32602')
r = call('data_validation.validate_path', {'path_prefix': '/Game', 'max_assets': 0})
test('max_assets=0 returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error", {}).get("code")})')

print(f'\n=== Wave H S3 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
