"""Wave H Surface 2 - curve.* test (4 tools)."""
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

def call_python(expression, timeout=30):
    s = socket.create_connection((HOST, PORT), timeout=timeout)
    req = {'id': 'pyt_'+str(int(time.time()*1000)), 'kind': 'exec_python', 'args': {'expression': expression}}
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

print('=== Wave H S2 - curve.* (4 tools) ===\n')

# ---------------------------------------------------------------------------
# Test setup: create a fresh UCurveFloat for write tests.
# UCurveFloat has no modal factory (NewObject fallback used) - safe via asset.create.
# UCurveTable factory IS modal - we use exec_python NewObject path for it.
# ---------------------------------------------------------------------------
TEST_CURVE_PATH = '/Game/MCPTest/MCP_TestCurve_H2'
TEST_CURVE_OBJ_PATH = TEST_CURVE_PATH + '.MCP_TestCurve_H2'
TEST_CT_PATH = '/Game/MCPTest/MCP_TestCurveTable_H2'

# Ensure the test folder exists.
r = call('cb.create_folder', {'folder_path': '/Game/MCPTest'})
print(f'cb.create_folder /Game/MCPTest -> ok={is_ok(r)} (may already exist)')

# Clean up any leftover from prior runs.
for p in (TEST_CURVE_PATH, TEST_CT_PATH):
    r = call('cb.delete', {'path': p, 'force': True})
    print(f'cb.delete {p} -> ok={is_ok(r)} (cleanup)')

# Create a fresh UCurveFloat.
print(f'\n[setup] asset.create UCurveFloat at {TEST_CURVE_PATH}')
r = call('asset.create', {
    'class_path': '/Script/Engine.CurveFloat',
    'dest_path':  TEST_CURVE_PATH,
    'save': False,
    'allow_fallback_newobject': True,
})
created_curve = is_ok(r) and r['result'].get('created')
print(f'   -> created={created_curve} (used_factory={r.get("result", {}).get("used_factory", "<err>")})')
if not created_curve:
    print(f'   FATAL: cannot create test curve, aborting. err={r.get("error")}')
    sys.exit(2)

# ---------------------------------------------------------------------------
# T1: curve.list - enumerate curves
# ---------------------------------------------------------------------------
print('\nT1: curve.list - enumerate curves in /Game')
r = call('curve.list', {'page_size': 200})
ok = is_ok(r) and isinstance(r['result'].get('curves'), list)
total = r['result'].get('total_known', 0) if ok else 0
test('curve.list returns array', ok, f'(total_known={total})')

found_test_curve = False
if ok:
    for c in r['result']['curves']:
        if TEST_CURVE_OBJ_PATH in c.get('asset_path', ''):
            found_test_curve = True
            print(f'    -> matched test curve: {c}')
            break
test('curve.list found our test curve', found_test_curve)

# T1b: types filter
print('\nT1b: curve.list with types=["UCurveFloat"]')
r = call('curve.list', {'types': ['UCurveFloat'], 'page_size': 50})
ok = is_ok(r) and isinstance(r['result'].get('curves'), list)
all_are_floats = ok and all(c.get('curve_class') == 'UCurveFloat' for c in r['result']['curves'])
test('all returned curves are UCurveFloat', ok and all_are_floats,
     f'(count={len(r["result"]["curves"]) if ok else 0})')

# T1c: pagination round-trip
print('\nT1c: curve.list pagination round-trip')
r = call('curve.list', {'page_size': 1})
ok1 = is_ok(r)
next_tok = r['result'].get('next_page_token') if ok1 else None
test('first page', ok1, f'(next_page_token present: {bool(next_tok)})')
if next_tok:
    r2 = call('curve.list', {'page_size': 1, 'page_token': next_tok})
    ok2 = is_ok(r2)
    test('second page via token', ok2, f'(curves count={len(r2["result"].get("curves",[])) if ok2 else 0})')
else:
    SKIPPED += 1
    print(f'  [SKIP] only one curve total - no pagination to test')

# ---------------------------------------------------------------------------
# T2: curve.get_data on empty curve (newly created - 0 keys expected)
# ---------------------------------------------------------------------------
print(f'\nT2: curve.get_data on empty {TEST_CURVE_PATH}')
r = call('curve.get_data', {'curve_path': TEST_CURVE_PATH})
ok = is_ok(r)
keys_arr = r['result'].get('keys', []) if ok else None
curve_class = r['result'].get('curve_class') if ok else None
test('get_data returns keys array (empty initially)', ok and isinstance(keys_arr, list) and len(keys_arr) == 0,
     f'(curve_class={curve_class}, len(keys)={len(keys_arr) if keys_arr is not None else "<err>"})')

# ---------------------------------------------------------------------------
# T3: curve.add_key - append a key
# ---------------------------------------------------------------------------
print(f'\nT3: curve.add_key at t=0 v=0 Cubic')
r = call('curve.add_key', {
    'curve_path': TEST_CURVE_PATH,
    'time': 0.0,
    'value': 0.0,
})
ok = is_ok(r)
added = r['result'].get('added') if ok else False
new_count = r['result'].get('new_key_count') if ok else 0
test('add_key first key added=true new_key_count=1', ok and added and new_count == 1,
     f'(added={added}, was_replaced={r["result"].get("was_replaced") if ok else "<err>"}, new_count={new_count})')

# Add a second key.
r = call('curve.add_key', {
    'curve_path': TEST_CURVE_PATH,
    'time': 1.0,
    'value': 100.0,
    'interp_mode': 'Linear',
})
ok = is_ok(r)
new_count = r['result'].get('new_key_count') if ok else 0
test('add_key second key (Linear) new_key_count=2', ok and new_count == 2,
     f'(new_count={new_count})')

# Add at same time t=0 - should REPLACE not add.
r = call('curve.add_key', {
    'curve_path': TEST_CURVE_PATH,
    'time': 0.0,
    'value': 5.0,  # different value
})
ok = is_ok(r)
was_replaced = r['result'].get('was_replaced') if ok else None
new_count = r['result'].get('new_key_count') if ok else 0
test('add_key at existing time was_replaced=true count still 2', ok and was_replaced and new_count == 2,
     f'(was_replaced={was_replaced}, new_count={new_count})')

# Verify via get_data.
r = call('curve.get_data', {'curve_path': TEST_CURVE_PATH})
ok = is_ok(r)
keys_after_add = r['result'].get('keys', []) if ok else []
test('post-add get_data shows 2 keys', ok and len(keys_after_add) == 2,
     f'(keys={[(k.get("time"), k.get("value"), k.get("interp_mode")) for k in keys_after_add]})')

# ---------------------------------------------------------------------------
# T4: curve.set_data - replace entire key set
# ---------------------------------------------------------------------------
print(f'\nT4: curve.set_data replace with 4 keys')
new_keys = [
    {'time': 0.0,  'value': 0.0,  'interp_mode': 'Linear'},
    {'time': 0.25, 'value': 25.0, 'interp_mode': 'Cubic'},
    {'time': 0.75, 'value': 75.0, 'interp_mode': 'Constant'},
    {'time': 1.0,  'value': 100.0, 'interp_mode': 'Linear'},
]
r = call('curve.set_data', {
    'curve_path': TEST_CURVE_PATH,
    'keys': new_keys,
})
ok = is_ok(r)
prior = r['result'].get('prior_key_count') if ok else None
new_count_resp = r['result'].get('new_key_count') if ok else None
test('set_data prior=2 new=4', ok and prior == 2 and new_count_resp == 4,
     f'(prior={prior}, new={new_count_resp})')

# Verify via get_data.
r = call('curve.get_data', {'curve_path': TEST_CURVE_PATH})
ok = is_ok(r)
keys_after_set = r['result'].get('keys', []) if ok else []
test('post-set get_data shows 4 keys', ok and len(keys_after_set) == 4,
     f'(times={[k.get("time") for k in keys_after_set]})')
# Verify interp_mode round-trip
if ok and len(keys_after_set) == 4:
    interp_round_trip_ok = (
        keys_after_set[0].get('interp_mode') == 'Linear' and
        keys_after_set[1].get('interp_mode') == 'Cubic' and
        keys_after_set[2].get('interp_mode') == 'Constant' and
        keys_after_set[3].get('interp_mode') == 'Linear'
    )
    test('interp_mode round-trip Linear/Cubic/Constant/Linear', interp_round_trip_ok,
         f'(modes={[k.get("interp_mode") for k in keys_after_set]})')

# ---------------------------------------------------------------------------
# T5: UCurveTable workflow via exec_python (UCurveTableFactory is MODAL).
# ---------------------------------------------------------------------------
print(f'\nT5: UCurveTable workflow (created via exec_python to avoid modal factory)')
# Bridge's exec_python uses EvaluateStatement (single expression only). Wrap multi-line logic
# inside exec(compile(...)) so we can run a full script as one expression that returns a marker.
# Populate the table via CreateTableFromCSVString (works without UFUNCTION wrapper because we go
# through Python's bound method on the object - except CreateTableFromCSVString isn't bound either.
# Fallback: use CSV import via AssetTools.import_assets_with_dialog? No - that's modal too.
#
# UE 5.7 Python's unreal.CurveTable exposes ONLY UFUNCTION/UPROPERTY members. Engine-side
# AddRichCurve and CreateTableFromCSVString are plain C++ - not reachable. We attempt creation;
# if AddRichCurve isn't reachable through python, the row-mutation subtests are skipped (we still
# verify the error paths -32004 missing-row and -32602 missing-key on a freshly-created empty CT).
SCRIPT_BODY = (
    'import unreal\n'
    f'pkg_path = "{TEST_CT_PATH}"\n'
    'asset_name = pkg_path.rsplit("/", 1)[1]\n'
    'folder_path = pkg_path.rsplit("/", 1)[0]\n'
    'if unreal.EditorAssetLibrary.does_asset_exist(pkg_path):\n'
    '    unreal.EditorAssetLibrary.delete_asset(pkg_path)\n'
    'ct = unreal.AssetToolsHelpers.get_asset_tools().create_asset(asset_name, folder_path, unreal.CurveTable, None)\n'
    'globals()["_MCP_CT_RESULT"] = "FAIL_CT_NOT_CREATED"\n'
    'if ct is not None:\n'
    '    has_arc = hasattr(ct, "add_rich_curve")\n'
    '    if has_arc:\n'
    '        rich = ct.add_rich_curve("TestRow")\n'
    '        rich.add_key(0.0, 10.0)\n'
    '        rich.add_key(1.0, 20.0)\n'
    '    unreal.EditorAssetLibrary.save_loaded_asset(ct)\n'
    '    globals()["_MCP_CT_RESULT"] = "OK_CT_WITH_ROW" if has_arc else "OK_CT_EMPTY"\n'
)
SCRIPT_REPR = repr(SCRIPT_BODY)
PY_CREATE_CT = (
    f'(exec(compile({SCRIPT_REPR}, "<mcp_h2>", "exec"), globals()) or globals().get("_MCP_CT_RESULT", "FAIL_NO_MARKER"))'
)
ct_pyres = call_python(PY_CREATE_CT, timeout=30)
print(f'  exec_python(create_ct) -> {ct_pyres.get("result", ct_pyres.get("error"))}')
res_str = str(ct_pyres.get('result', ''))
ct_created = 'OK_CT_WITH_ROW' in res_str or 'OK_CT_EMPTY' in res_str
ct_has_row = 'OK_CT_WITH_ROW' in res_str

if ct_created:
    # Always-runnable tests (no rows needed):
    print('\n  T5b: curve.get_data on missing row -> -32004')
    r = call('curve.get_data', {'curve_path': TEST_CT_PATH, 'key': 'NoSuchRow'})
    test('get_data missing row returns ObjectNotFound', is_err(r, -32004),
         f'(code={r.get("error",{}).get("code") if "error" in r else "<ok>"})')

    print('\n  T5c: curve.get_data without key on UCurveTable -> -32602')
    r = call('curve.get_data', {'curve_path': TEST_CT_PATH})
    test('get_data UCurveTable without key returns InvalidParams', is_err(r, -32602),
         f'(code={r.get("error",{}).get("code") if "error" in r else "<ok>"})')

    if ct_has_row:
        # Row-population worked - exercise the full read/write path on UCurveTable rows.
        print('\n  T5a: curve.get_data on UCurveTable + key=TestRow')
        r = call('curve.get_data', {'curve_path': TEST_CT_PATH, 'key': 'TestRow'})
        ok = is_ok(r)
        keys = r['result'].get('keys', []) if ok else []
        raw_err = r.get('error') if not ok else None
        test('UCurveTable row returns 2 keys', ok and len(keys) == 2,
             f'(class={r["result"].get("curve_class") if ok else "<err>"}, keys={[(k.get("time"),k.get("value")) for k in keys]}, err={raw_err})')

        print('\n  T5d: curve.add_key on UCurveTable row TestRow')
        r = call('curve.add_key', {
            'curve_path': TEST_CT_PATH,
            'key': 'TestRow',
            'time': 0.5,
            'value': 15.0,
            'interp_mode': 'Cubic',
        })
        ok = is_ok(r)
        new_count = r['result'].get('new_key_count') if ok else 0
        test('add_key on UCurveTable row new_key_count=3', ok and new_count == 3,
             f'(new_count={new_count}, added={r["result"].get("added") if ok else "<err>"})')
    else:
        SKIPPED += 2
        print('   [SKIP] add_rich_curve not exposed to Python - 2 row-mutation subtests')
else:
    SKIPPED += 4
    print(f'   [SKIP] CurveTable not creatable - 4 UCurveTable path tests')

# ---------------------------------------------------------------------------
# T6: bad path -> -32004
# ---------------------------------------------------------------------------
print(f'\nT6: bad curve_path -> -32004 or -32010')
r = call('curve.get_data', {'curve_path': '/Game/__totally_does_not_exist__'})
test('bad path returns ObjectNotFound or InvalidPath',
     is_err(r, -32004) or is_err(r, -32010),
     f'(code={r.get("error",{}).get("code")})')

# ---------------------------------------------------------------------------
# T7: non-curve asset -> -32011
# ---------------------------------------------------------------------------
print(f'\nT7: non-curve asset (Texture2D) -> -32011')
r = call('asset.search_by_class', {'class_path': '/Script/Engine.Texture2D', 'page_size': 1})
matches = r['result'].get('matches') if is_ok(r) else None
if matches:
    tex_path = matches[0].get('asset_path') or matches[0].get('object_path')
    r = call('curve.get_data', {'curve_path': tex_path})
    test('non-curve returns WrongClass', is_err(r, -32011),
         f'(probed={tex_path}, code={r.get("error",{}).get("code")})')
else:
    SKIPPED += 1
    print(f'  [SKIP] no Texture2D found to test WrongClass')

# ---------------------------------------------------------------------------
# T8: missing required field (no keys array on set_data) -> -32602
# ---------------------------------------------------------------------------
print(f'\nT8: missing keys array on set_data -> -32602')
r = call('curve.set_data', {'curve_path': TEST_CURVE_PATH})
test('set_data without keys returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error",{}).get("code")})')

# ---------------------------------------------------------------------------
# Cleanup
# ---------------------------------------------------------------------------
print('\n=== cleanup ===')
for p in (TEST_CURVE_PATH, TEST_CT_PATH):
    r = call('cb.delete', {'path': p, 'force': True})
    print(f'cb.delete {p} -> ok={is_ok(r)}')

print(f'\n=== Wave H S2 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
