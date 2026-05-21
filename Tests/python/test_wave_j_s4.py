"""Wave J Surface 4 — ai.eqs.* test (3 tools).

Tests EQS inspection + execution tools:
  T1: ai.eqs.list_queries           → enumerate UEnvQuery assets (FatumGame may have none)
  T2: ai.eqs.get_query_info         → options + generator + tests structure
  T3: ai.eqs.run_query              → single_best mode, valid querier, status set
  T4: ai.eqs.get_query_info bad     → -32004 ObjectNotFound
  T5: ai.eqs.run_query bad mode     → -32602 InvalidParams

If no UEnvQuery in project → T1 passes with empty array; T2/T3 SKIP.
If no actor available for querier → T3 SKIPs.
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

print('=== Wave J S4 — ai.eqs.* (3 tools) ===\n')

# T1: ai.eqs.list_queries
print('T1: ai.eqs.list_queries — enumerate UEnvQuery assets')
r = call('ai.eqs.list_queries')
ok = is_ok(r) and isinstance(r['result'].get('queries'), list)
queries = r['result'].get('queries', []) if ok else []
test('list_queries returns queries array', ok,
     f'(count={len(queries)} total_known={r["result"].get("total_known") if ok else "?"})')

discovered_query = None
if ok and queries:
    first = queries[0]
    required = ('asset_path', 'options_count')
    missing = [k for k in required if k not in first]
    if missing:
        test('list_queries entry shape', False, f'missing fields: {missing}')
    else:
        shape_ok = (isinstance(first['asset_path'], str)
                    and isinstance(first['options_count'], int))
        test('list_queries entry shape', shape_ok,
             f"asset_path={first['asset_path'][:60]}... options_count={first['options_count']}")
        if shape_ok:
            discovered_query = first['asset_path']
else:
    print('  (no UEnvQuery assets in project — T2/T3 will SKIP)')

# T2: ai.eqs.get_query_info on discovered query
print('\nT2: ai.eqs.get_query_info on discovered query')
if discovered_query:
    r = call('ai.eqs.get_query_info', {'query_path': discovered_query})
    if is_ok(r):
        res = r['result']
        required = ('query_path', 'options_count', 'options')
        missing = [k for k in required if k not in res]
        if missing:
            test('get_query_info shape', False, f'missing fields: {missing}')
        else:
            options = res['options']
            opts_ok = isinstance(options, list) and all(
                isinstance(o, dict)
                and 'option_name' in o
                and 'generator_class' in o
                and isinstance(o.get('tests'), list)
                for o in options)
            test('get_query_info shape', opts_ok,
                 f"options_count={res['options_count']} "
                 f"first_gen={options[0].get('generator_class') if options else None} "
                 f"first_test_count={len(options[0].get('tests', [])) if options else 0}")
    else:
        test('get_query_info', False,
             f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:80]}')
else:
    skip('get_query_info', '(no UEnvQuery in project)')

# T3: ai.eqs.run_query — needs a discovered query AND a valid actor in the current world
print('\nT3: ai.eqs.run_query with single_best mode')
querier_path = None
# Find any actor in the current world to act as querier. StaticMeshActor is the most reliable
# baseline (every PIE/editor world has a few).
r = call('actor.find_by_class', {
    'class_path': '/Script/Engine.StaticMeshActor',
    'page_size': 5,
})
if is_ok(r):
    actors = r['result'].get('actors', [])
    for a in actors:
        querier_path = a.get('actor_path')
        if querier_path:
            break

if discovered_query and querier_path:
    r = call('ai.eqs.run_query', {
        'query_path': discovered_query,
        'querier_actor_path': querier_path,
        'mode': 'single_best',
    })
    # Either Success (returns up to 1 result for single_best) or query-level failure (-32015).
    # Both are valid outcomes — we're testing the wire shape, not the EQS semantics.
    if is_ok(r):
        res = r['result']
        required = ('status', 'mode', 'items_count', 'results')
        missing = [k for k in required if k not in res]
        if missing:
            test('run_query shape', False, f'missing fields: {missing}')
        else:
            results = res['results']
            results_ok = (isinstance(results, list)
                          and len(results) == res['items_count']
                          and all(isinstance(it, dict)
                                  and isinstance(it.get('location'), list)
                                  and len(it['location']) == 3
                                  and isinstance(it.get('score'), (int, float))
                                  for it in results))
            single_best_ok = (res['mode'] != 'single_best' or res['items_count'] <= 1)
            test('run_query shape', results_ok and single_best_ok,
                 f"status={res['status']} mode={res['mode']} items={res['items_count']}")
    elif is_err(r, -32015):
        # Query failed at the EQS level (e.g. generator returned no items, owner mismatch) —
        # this still proves the wire path works.
        test('run_query (query-level failure surfaced as -32015)', True,
             f'(msg: {r.get("error",{}).get("message","")[:100]})')
    else:
        test('run_query', False,
             f'error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")[:100]}')
else:
    if not discovered_query:
        skip('run_query', '(no UEnvQuery in project)')
    else:
        skip('run_query', '(no StaticMeshActor available as querier)')

# T4: ai.eqs.get_query_info with bad path → -32004
print('\nT4: ai.eqs.get_query_info with bad path → -32004')
r = call('ai.eqs.get_query_info', {'query_path': '/Game/__nonexistent_eqs_query_xyz__'})
test('bad query_path → -32004', is_err(r, -32004),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

# T5: ai.eqs.run_query bad mode → -32602
print('\nT5: ai.eqs.run_query bad mode → -32602')
# Use whatever query_path / querier we have — the mode check fires first so failure of either
# load doesn't matter here. If no querier we still get -32602 because mode is validated up front.
test_query  = discovered_query or '/Game/__placeholder__'
test_actor  = querier_path or '/Game/__placeholder__'
r = call('ai.eqs.run_query', {
    'query_path': test_query,
    'querier_actor_path': test_actor,
    'mode': 'random_garbage_mode',
})
test('bad mode → -32602', is_err(r, -32602),
     f'(got code={r.get("error",{}).get("code")} msg={r.get("error",{}).get("message","")[:80]})')

print(f'\n{"="*60}')
print(f'WAVE J SURFACE 4 TEST: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP')
print(f'{"="*60}')
sys.exit(0 if FAIL == 0 else 1)
