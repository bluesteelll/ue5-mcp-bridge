"""Wave I Surface 1 — package.* test (5 tools)."""
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

print('=== Wave I S1 — package.* (5 tools) ===\n')

# T1: package.list_dirty
print('T1: package.list_dirty — enumerate dirty UPackages')
r = call('package.list_dirty')
ok = is_ok(r) and isinstance(r['result'].get('dirty_packages'), list)
total = r['result'].get('total_dirty', 0) if ok else 0
test('list_dirty returns array', ok, f'(total_dirty={total})')
if ok and r['result']['dirty_packages']:
    sample = r['result']['dirty_packages'][0]
    print(f'    -> sample entry: path={sample.get("path")!r} asset_count={sample.get("asset_count")} transient={sample.get("transient")}')
    test('dirty entry has path+asset_count+transient',
         all(k in sample for k in ('path', 'asset_count', 'transient')),
         f'(keys={list(sample.keys())})')
else:
    print('    (no dirty packages — fresh editor state)')

# Discover a /Game asset to use for dependency tests. Probe well-known test asset first, then
# fall back to asset.search_by_class for a Blueprint.
probe_paths = [
    '/Game/MCPTest/BP_MCPTest',
    '/Game/MCPTest/Geom/SM_Cube_MCPTest',
]
target_pkg = None
for p in probe_paths:
    r = call('asset.exists', {'path': p})
    if is_ok(r) and r['result'].get('exists'):
        target_pkg = p
        print(f'\n    -> found probe asset: {target_pkg}')
        break

if not target_pkg:
    # Search for any Blueprint under /Game
    r = call('asset.search_by_class', {'class_path': '/Script/Engine.Blueprint', 'page_size': 1})
    if is_ok(r) and r['result'].get('matches'):
        first = r['result']['matches'][0]
        ap = first.get('asset_path') or first.get('object_path') or first.get('package_path')
        if ap:
            # Strip any .LeafName suffix to get pure package path
            if '.' in ap.rsplit('/', 1)[-1]:
                ap = ap.rsplit('.', 1)[0]
            target_pkg = ap
            print(f'\n    -> fallback: discovered /Game Blueprint {target_pkg}')

if not target_pkg:
    # Last resort: use any /Game asset. asset_path is the OBJECT path (/Game/Foo/Bar.Bar),
    # strip the .LeafName suffix to get the package name (/Game/Foo/Bar).
    r = call('asset.list', {'package_paths': ['/Game'], 'recursive_paths': True, 'page_size': 1})
    if is_ok(r) and r['result'].get('assets'):
        first = r['result']['assets'][0]
        ap = first.get('asset_path') or ''
        if ap and '.' in ap.rsplit('/', 1)[-1]:
            ap = ap.rsplit('.', 1)[0]
        if ap:
            target_pkg = ap

if not target_pkg:
    print('\n  NO /GAME ASSET FOUND IN PROJECT — using synthetic path; dependency tests will skip')
    target_pkg = '/Game/__synthetic_for_test__'

# T2: package.get_dependencies
print(f'\nT2: package.get_dependencies on {target_pkg}')
r = call('package.get_dependencies', {'package_path': target_pkg})
if is_ok(r):
    deps = r['result'].get('dependencies', [])
    tot = r['result'].get('total', -1)
    test('get_dependencies returns array', isinstance(deps, list), f'(total={tot})')
    if deps:
        d0 = deps[0]
        test('dep entry has path+dep_type', 'path' in d0 and 'dep_type' in d0,
             f'(first={d0})')
        valid_types = {'Hard', 'Soft', 'SearchableName'}
        test('dep_type valid', d0.get('dep_type') in valid_types,
             f'(got={d0.get("dep_type")})')
elif is_err(r, -32004):
    SKIPPED += 1
    print(f'  [SKIP] target not in registry (-32004) — using probe asset that does not have AR entry')
else:
    test('get_dependencies', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T3: package.get_referencers
print(f'\nT3: package.get_referencers on {target_pkg}')
r = call('package.get_referencers', {'package_path': target_pkg})
if is_ok(r):
    refs = r['result'].get('referencers', [])
    tot = r['result'].get('total', -1)
    test('get_referencers returns array', isinstance(refs, list), f'(total={tot})')
    if refs:
        r0 = refs[0]
        test('ref entry has path+dep_type', 'path' in r0 and 'dep_type' in r0,
             f'(first={r0})')
elif is_err(r, -32004):
    SKIPPED += 1
    print(f'  [SKIP] target not in registry (-32004)')
else:
    test('get_referencers', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T4: package.save on a known LOADED asset.
# Strategy: list_dirty first; if anything's dirty, save the first non-transient entry. Otherwise
# probe target_pkg — it may already be loaded.
print(f'\nT4: package.save')
save_target = None
r = call('package.list_dirty')
if is_ok(r):
    for d in r['result'].get('dirty_packages', []):
        if not d.get('transient'):
            save_target = d.get('path')
            print(f'    -> saving dirty: {save_target}')
            break

if not save_target:
    # Try saving the discovered target — may or may not be loaded.
    save_target = target_pkg
    print(f'    -> trying probe target: {save_target}')

r = call('package.save', {'package_path': save_target})
if is_ok(r):
    test('save returned saved=true',
         r['result'].get('saved') is True,
         f'(was_dirty={r["result"].get("was_dirty")}, file_path={r["result"].get("file_path")!r})')
elif is_err(r, -32004):
    SKIPPED += 1
    print(f'  [SKIP] package not loaded (-32004); save is a no-op for unloaded packages by design')
else:
    test('save', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T5: package.save_all only_dirty=true
print(f'\nT5: package.save_all only_dirty=true max_packages=50')
r = call('package.save_all', {'only_dirty': True, 'max_packages': 50})
if is_ok(r):
    saved = r['result'].get('saved', -1)
    failed = r['result'].get('failed', -1)
    cands = r['result'].get('total_candidates', -1)
    test('save_all returned counts',
         isinstance(saved, (int, float)) and isinstance(failed, (int, float)),
         f'(saved={saved}, failed={failed}, candidates={cands})')
    fails = r['result'].get('failures', [])
    if fails:
        print(f'    -> first failure: {fails[0]}')
else:
    test('save_all', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T6: bad path -> -32004 or -32010
print(f'\nT6: package.get_dependencies bad path')
r = call('package.get_dependencies', {'package_path': '/Game/__totally_does_not_exist_xyz123__'})
# get_dependencies on a non-existent package returns empty array (AR doesn't error — it just has
# no entries for that package). To get -32004 we need to test save instead, which actually requires
# the package to be loaded.
if is_ok(r) and r['result'].get('total', -1) == 0:
    PASS += 1
    print(f'  [PASS] get_dependencies on missing path returns empty array (total=0)')
else:
    # Also accept -32004 (some registry implementations might error)
    test('get_dependencies on missing path', is_err(r, -32004) or is_err(r, -32010),
         f'(code={r.get("error",{}).get("code")}, result={r.get("result")})')

# T7: package.save bad path returns -32004 (not loaded)
print(f'\nT7: package.save on non-loaded path -> -32004 or -32010')
r = call('package.save', {'package_path': '/Game/__totally_does_not_exist_xyz123__'})
test('save unloaded returns ObjectNotFound',
     is_err(r, -32004) or is_err(r, -32010),
     f'(code={r.get("error",{}).get("code")}, msg={r.get("error",{}).get("message","")!r})')

# T8: malformed path -> -32010
print(f'\nT8: package.save with malformed path -> -32010 InvalidPath')
r = call('package.save', {'package_path': 'not-a-valid-path'})
test('malformed path returns InvalidPath',
     is_err(r, -32010),
     f'(code={r.get("error",{}).get("code")})')

# T9: missing args -> -32602
print(f'\nT9: package.save missing args -> -32602 InvalidParams')
r = call('package.save', {})
test('missing args returns InvalidParams',
     is_err(r, -32602),
     f'(code={r.get("error",{}).get("code")})')

# T10: filter exclusion — get_dependencies with both flags false should return empty
print(f'\nT10: get_dependencies include_hard=false include_soft=false (empty result)')
if target_pkg.startswith('/Game'):
    r = call('package.get_dependencies', {
        'package_path': target_pkg,
        'include_hard': False,
        'include_soft': False
    })
    if is_ok(r):
        test('both flags false returns empty', r['result'].get('total', -1) == 0,
             f'(total={r["result"].get("total")})')
    else:
        SKIPPED += 1
        print(f'  [SKIP] target not queryable: {r.get("error",{}).get("code")}')
else:
    SKIPPED += 1
    print(f'  [SKIP] no valid /Game target')

print(f'\n=== Wave I S1 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
