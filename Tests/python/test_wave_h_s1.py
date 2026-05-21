"""Wave H Surface 1 — data_table.* test (4 tools)."""
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

print('=== Wave H S1 — data_table.* (4 tools) ===\n')

# T1: data_table.list
print('T1: data_table.list — enumerate DataTables')
r = call('data_table.list', {'page_size': 200})
ok = is_ok(r) and isinstance(r['result'].get('data_tables'), list)
total = r['result'].get('total_known', 0) if ok else 0
test('list returns array', ok, f'(total_known={total})')
discovered_dt = None
discovered_struct = None
discovered_count = 0
if ok and r['result']['data_tables']:
    dt0 = r['result']['data_tables'][0]
    discovered_dt = dt0.get('asset_path')
    discovered_struct = dt0.get('row_struct_path')
    discovered_count = dt0.get('row_count')
    print(f'    -> first DT: {discovered_dt}')
    print(f'    -> row_struct: {discovered_struct}')
    print(f'    -> row_count: {discovered_count}')

# Try to find a non-trivial DataTable
target_dt = None
target_count = 0
if ok:
    for dt in r['result']['data_tables']:
        cnt = dt.get('row_count', 0)
        if cnt > 0 and dt.get('row_struct_path'):
            target_dt = dt['asset_path']
            target_count = cnt
            print(f'    -> chose target: {target_dt} ({cnt} rows)')
            break
    if not target_dt and r['result']['data_tables']:
        # fallback: just use first DT even if empty
        target_dt = r['result']['data_tables'][0]['asset_path']
        target_count = r['result']['data_tables'][0].get('row_count', 0)

if not target_dt:
    print('  NO DATA TABLE FOUND IN PROJECT — creating synthetic test path')
    target_dt = '/Game/__nonexistent__/__nonexistent_dt__'
    target_count = 0

# T2: data_table.get_rows
print(f'\nT2: data_table.get_rows on {target_dt}')
r = call('data_table.get_rows', {'data_table_path': target_dt, 'page_size': 5})
if is_ok(r):
    rows = r['result'].get('rows', [])
    rstruct = r['result'].get('row_struct_path')
    test('get_rows returns rows array', isinstance(rows, list), f'(count={len(rows)}, struct={rstruct})')
    if rows:
        r0 = rows[0]
        test('first row has row_name+values', 'row_name' in r0 and 'values' in r0, f'(row[0].name={r0.get("row_name")})')
        if isinstance(r0.get('values'), dict) and r0['values']:
            keys = list(r0['values'].keys())[:3]
            print(f'    -> field sample: {keys}')
    else:
        print('    (empty DT — skipping field validation)')
else:
    test('get_rows', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

# T3: pagination round-trip
print(f'\nT3: pagination round-trip on {target_dt}')
if target_count > 1:
    r = call('data_table.get_rows', {'data_table_path': target_dt, 'page_size': 1})
    ok1 = is_ok(r) and 'next_page_token' in r['result']
    tok = r['result'].get('next_page_token') if ok1 else None
    test('first page has next_page_token', ok1, f'(token={tok!r})')
    if tok:
        r2 = call('data_table.get_rows', {'data_table_path': target_dt, 'page_size': 1, 'page_token': tok})
        ok2 = is_ok(r2) and r2['result'].get('rows')
        test('second page returns rows', ok2, f'(rows={len(r2["result"].get("rows", [])) if ok2 else 0})')
else:
    SKIPPED += 1
    print(f'  [SKIP] not enough rows ({target_count}) for pagination test')

# T4-T6: write tests on a copy of target DT, or skip if no DT available
# We need to know what the row struct fields look like for set_row.
# Approach: read first row's field shape from existing DT, then set a NEW row with matching shape.
synthetic_row = 'MCP_TEST_ROW_H1'

# Discover field shape
field_shape = {}
if target_count > 0:
    r = call('data_table.get_rows', {'data_table_path': target_dt, 'page_size': 1})
    if is_ok(r) and r['result'].get('rows'):
        field_shape = r['result']['rows'][0].get('values', {})
        print(f'\n    -> discovered field shape: {list(field_shape.keys())}')

# T4: set_row with create_if_missing=true
print(f'\nT4: data_table.set_row create_if_missing on {target_dt}')
if field_shape:
    r = call('data_table.set_row', {
        'data_table_path': target_dt,
        'row_name': synthetic_row,
        'create_if_missing': True,
        'values': field_shape  # copy shape from first row
    })
    if is_ok(r):
        was_created = r['result'].get('was_created', False)
        written = r['result'].get('written', False)
        test('set_row create returned was_created=true', was_created and written,
             f'(written={written}, was_created={was_created}, fields_updated={r["result"].get("fields_updated")})')
    else:
        test('set_row create succeeded', False,
             f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')
else:
    SKIPPED += 1
    print(f'  [SKIP] no field shape — cannot do write tests')

# T5: set_row update existing
print(f'\nT5: data_table.set_row update existing (create_if_missing=false)')
if field_shape:
    # mutate one numeric field value if possible
    mutated = dict(field_shape)
    # pick first numeric-looking field
    target_field = None
    for k, v in field_shape.items():
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            target_field = k
            mutated[k] = (v if v != 0 else 1) * 2
            break
    if target_field:
        r = call('data_table.set_row', {
            'data_table_path': target_dt,
            'row_name': synthetic_row,
            'create_if_missing': False,
            'values': {target_field: mutated[target_field]}
        })
        if is_ok(r):
            test('set_row update fields_updated>=1', r['result'].get('fields_updated', 0) >= 1 and not r['result'].get('was_created', True),
                 f'(was_created={r["result"].get("was_created")}, fields_updated={r["result"].get("fields_updated")})')
        else:
            test('set_row update', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')
    else:
        SKIPPED += 1
        print(f'  [SKIP] no numeric field to mutate')
else:
    SKIPPED += 1
    print(f'  [SKIP] no field shape')

# T6: delete_row
print(f'\nT6: data_table.delete_row {synthetic_row}')
if field_shape:
    r = call('data_table.delete_row', {
        'data_table_path': target_dt,
        'row_name': synthetic_row
    })
    if is_ok(r):
        test('delete_row deleted=true', r['result'].get('deleted', False),
             f'(deleted={r["result"].get("deleted")}, remaining={r["result"].get("remaining_row_count")})')

        # Verify removed
        r2 = call('data_table.get_rows', {'data_table_path': target_dt, 'page_size': 1000})
        if is_ok(r2):
            still_exists = any(row.get('row_name') == synthetic_row for row in r2['result'].get('rows', []))
            test('post-delete: row no longer in get_rows', not still_exists)
    else:
        test('delete_row', False, f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')
else:
    SKIPPED += 1
    print(f'  [SKIP] no row to delete')

# T7: bad data_table_path
print(f'\nT7: bad data_table_path -> -32004')
r = call('data_table.get_rows', {'data_table_path': '/Game/__totally_does_not_exist__'})
test('bad path returns ObjectNotFound', is_err(r, -32004) or is_err(r, -32010),
     f'(code={r.get("error",{}).get("code")})')

# T8: non-DataTable asset -> -32011
print(f'\nT8: non-DataTable path -> -32011')
# Use the bridge itself's path or a known non-DT asset
r = call('data_table.get_rows', {'data_table_path': '/Game/MCPTest/BP_MCPTest'})
# If that doesn't exist, try a common asset
if is_err(r, -32004):
    r = call('asset.search_by_class', {'class_path': '/Script/Engine.Texture2D', 'page_size': 1})
    if is_ok(r) and r['result'].get('assets'):
        tex_path = r['result']['assets'][0].get('asset_path') or r['result']['assets'][0].get('object_path')
        r = call('data_table.get_rows', {'data_table_path': tex_path})
        test('non-DT returns WrongClass', is_err(r, -32011),
             f'(probed={tex_path}, code={r.get("error",{}).get("code")})')
    else:
        SKIPPED += 1
        print(f'  [SKIP] no Texture2D found to test WrongClass')
else:
    test('non-DT returns WrongClass', is_err(r, -32011),
         f'(code={r.get("error",{}).get("code")})')

print(f'\n=== Wave H S1 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
