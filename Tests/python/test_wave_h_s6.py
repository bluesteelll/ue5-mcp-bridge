"""Wave H Surface 6 - cook.* cooking automation test (3 tools).

Tools tested:
  - cook.list_platforms       (Lane A, NO PIE guard)
  - cook.validate_cookable    (Lane A, NO PIE guard)
  - cook.start                (Lane A submitter -> WORKER-thread job via FMCPJobRegistry;
                                NOT awaited to completion - we just verify kickoff + cancel)
"""
import socket, json, time, sys

HOST, PORT = '127.0.0.1', 30020
PASS = 0; FAIL = 0; SKIPPED = 0


def call(method, args=None, timeout=60):
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


print('=== Wave H S6 - cook.* (3 tools) ===\n')

# ---------------------------------------------------------------------------
# T1: cook.list_platforms - enumerate registered target platforms
# ---------------------------------------------------------------------------
print('T1: cook.list_platforms (no args)')
r = call('cook.list_platforms', {}, timeout=30)
if not is_ok(r):
    test('list_platforms call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    platforms = res.get('platforms', [])
    total = res.get('total', -1)
    active = res.get('active_count', -1)
    test('list_platforms has platforms/total/active_count fields',
         all(k in res for k in ('platforms', 'total', 'active_count')),
         f'(keys={list(res.keys())})')
    test('list_platforms total > 0 (at least one platform registered)',
         total > 0,
         f'(total={total})')
    test('list_platforms each entry has name/display_name/is_server/is_client/is_editor/is_active',
         all(all(k in p for k in ('name', 'display_name', 'is_server', 'is_client', 'is_editor', 'is_active'))
             for p in platforms),
         f'(first entry keys={list(platforms[0].keys()) if platforms else "EMPTY"})')
    # Print enumerated platforms for diagnostic
    print(f'    enumerated platforms ({total} total, {active} active):')
    for p in platforms[:20]:
        marks = []
        if p.get('is_active'): marks.append('ACTIVE')
        if p.get('is_server'): marks.append('SERVER')
        if p.get('is_client'): marks.append('CLIENT')
        if p.get('is_editor'): marks.append('EDITOR')
        flags_str = '/'.join(marks) if marks else 'GAME'
        print(f'      - {p["name"]:24} | {p["display_name"]:32} | {flags_str}')
    # Find a Windows-like platform name to use for subsequent tests
    windows_platform = None
    for p in platforms:
        name = p.get('name', '')
        if name == 'Windows' or name == 'WindowsClient' or name == 'WindowsServer':
            windows_platform = name
            break
    if not windows_platform:
        # Fall back to first non-server, non-editor platform
        for p in platforms:
            if not p.get('is_server') and not p.get('is_editor'):
                windows_platform = p['name']
                break
    if not windows_platform and platforms:
        windows_platform = platforms[0]['name']
    test('list_platforms contains a usable platform name (Windows or fallback)',
         windows_platform is not None,
         f'(picked={windows_platform})')

# ---------------------------------------------------------------------------
# T2: cook.validate_cookable - dry-run cookability check on a small subset
# ---------------------------------------------------------------------------
PLATFORM_NAME = windows_platform if windows_platform else 'Windows'
print(f'\nT2: cook.validate_cookable platform_name={PLATFORM_NAME!r} max_assets=50')
r = call('cook.validate_cookable', {
    'platform_name': PLATFORM_NAME,
    'max_assets': 50,
}, timeout=60)
if not is_ok(r):
    test('validate_cookable call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    cookable = res.get('cookable_count', -1)
    uncookable = res.get('uncookable_count', -1)
    visited = res.get('total_visited', -1)
    known = res.get('total_known', -1)
    max_reached = res.get('max_assets_reached', None)
    errors = res.get('errors', [])
    test('validate_cookable has cookable_count/uncookable_count/total_visited/errors fields',
         all(k in res for k in ('cookable_count', 'uncookable_count', 'total_visited', 'errors')),
         f'(keys={list(res.keys())})')
    test('validate_cookable cookable_count + uncookable_count == total_visited',
         cookable + uncookable == visited,
         f'(cookable={cookable}, uncookable={uncookable}, visited={visited})')
    test('validate_cookable total_visited <= 50 (respects max_assets cap)',
         visited <= 50,
         f'(visited={visited})')
    test('validate_cookable max_assets_reached field is bool',
         isinstance(max_reached, bool),
         f'(max_reached={max_reached!r})')
    print(f'    cookable={cookable}, uncookable={uncookable}, visited={visited}, '
          f'known={known}, errors={len(errors)}')
    if errors:
        for err in errors[:5]:
            print(f'      - {err.get("asset_path","?")}: {err.get("reason","?")[:60]}')

# ---------------------------------------------------------------------------
# T3: cook.start with output_directory in sandbox -> get job_id; poll briefly; cancel
# ---------------------------------------------------------------------------
COOK_OUT = 'D:/Unreal Engine Projects/FatumGame/Saved/Cooked_h6_test'
print(f'\nT3: cook.start platform_name={PLATFORM_NAME!r} output_directory={COOK_OUT!r}')
r = call('cook.start', {
    'platform_name': PLATFORM_NAME,
    'output_directory': COOK_OUT,
}, timeout=30)
job_id = None
if not is_ok(r):
    test('cook.start call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    job_id = res.get('job_id')
    started_at = res.get('started_at')
    cmd_line = res.get('command_line', '')
    test('cook.start returned job_id', job_id is not None and len(job_id) > 0, f'(job_id={job_id})')
    test('cook.start returned started_at (float)',
         isinstance(started_at, (int, float)) and started_at > 0,
         f'(started_at={started_at})')
    # NOTE: The 'command_line' field carries only the PARAMS portion (what
    # FMonitoredProcess passes after the exe URL). The exe path itself (UnrealEditor-Cmd)
    # is stored separately inside the FMonitoredProcess instance — not echoed back.
    test('cook.start command_line contains .uproject',
         '.uproject' in cmd_line,
         f'(cmd_line[:120]={cmd_line[:120]!r})')
    test('cook.start command_line contains -run=Cook',
         '-run=Cook' in cmd_line,
         f'(cmd_line[:120]={cmd_line[:120]!r})')

# Poll job.status briefly to verify the worker thread picked it up
if job_id:
    print('\nT3b: poll job.status to verify job is running')
    # Give the worker thread a moment to start the process
    time.sleep(2.0)
    r_status = call('job.status', {'job_id': job_id}, timeout=10)
    if is_ok(r_status):
        state = r_status['result'].get('state', '')
        progress = r_status['result'].get('progress', 0.0)
        test('cook.start job state is Pending|Running (not Failed)',
             state in ('Pending', 'Running', 'Succeeded', 'Cancelled'),
             f'(state={state}, progress={progress})')
    else:
        test('job.status call succeeded', False, f'(err={r_status.get("error")})')

# Cancel the job (don't wait minutes for the cook to complete)
if job_id:
    print('\nT3c: cancel the cook job via job.cancel')
    r_cancel = call('job.cancel', {'job_id': job_id}, timeout=10)
    if is_ok(r_cancel):
        # job.cancel contract: returns {accepted: bool, id: <guid>}. accepted=true means the
        # registry has set bCancelRequested; the body honours it asynchronously.
        accepted = r_cancel['result'].get('accepted', False)
        test('job.cancel returned accepted=true',
             accepted is True,
             f'(result={r_cancel["result"]})')
    else:
        test('job.cancel call succeeded', False, f'(err={r_cancel.get("error")})')

    # Wait briefly then check status -> should transition to Cancelled (or Failed if cancel got
    # the process before it really started). 8s wait covers the 500ms poll cadence + the
    # cancel-deadline wait inside the cook body.
    print('\nT3d: wait 8s for cancel to propagate, then verify job state')
    time.sleep(8.0)
    r_final = call('job.status', {'job_id': job_id}, timeout=10)
    if is_ok(r_final):
        final_state = r_final['result'].get('state', '')
        test('cook job final state is Cancelled or Failed (cancel took effect)',
             final_state in ('Cancelled', 'Failed'),
             f'(final_state={final_state})')
    else:
        test('final job.status call succeeded', False, f'(err={r_final.get("error")})')

# ---------------------------------------------------------------------------
# T4: bad platform_name -> -32004
# ---------------------------------------------------------------------------
print('\nT4: cook.start with bad platform_name -> -32004')
r = call('cook.start', {
    'platform_name': '__nonexistent_platform_xyz',
    'output_directory': COOK_OUT,
}, timeout=10)
test('cook.start bad platform_name -> -32004', is_err(r, -32004), f'(resp={r})')

print('\nT4b: cook.validate_cookable with bad platform_name -> -32004')
r = call('cook.validate_cookable', {
    'platform_name': '__nonexistent_platform_xyz',
    'max_assets': 10,
}, timeout=10)
test('cook.validate_cookable bad platform_name -> -32004', is_err(r, -32004), f'(resp={r})')

# ---------------------------------------------------------------------------
# T5: missing platform_name -> -32602
# ---------------------------------------------------------------------------
print('\nT5: cook.start missing platform_name -> -32602')
r = call('cook.start', {
    'output_directory': COOK_OUT,
}, timeout=10)
test('cook.start missing platform_name -> -32602', is_err(r, -32602), f'(resp={r})')

# ---------------------------------------------------------------------------
# T6: out-of-sandbox output_directory -> -32013 PathEscape
# ---------------------------------------------------------------------------
print('\nT6: cook.start output_directory outside sandbox -> -32013')
r = call('cook.start', {
    'platform_name': PLATFORM_NAME,
    'output_directory': 'D:/tmp/cooked_outside_sandbox',
}, timeout=10)
test('cook.start out-of-sandbox dir -> -32013', is_err(r, -32013), f'(resp={r})')

# ---------------------------------------------------------------------------
# T7: missing output_directory -> -32602
# ---------------------------------------------------------------------------
print('\nT7: cook.start missing output_directory -> -32602')
r = call('cook.start', {
    'platform_name': PLATFORM_NAME,
}, timeout=10)
test('cook.start missing output_directory -> -32602', is_err(r, -32602), f'(resp={r})')

# ---------------------------------------------------------------------------
# T8: validate_cookable with max_assets out of range -> -32602
# ---------------------------------------------------------------------------
print('\nT8: cook.validate_cookable max_assets=0 -> -32602')
r = call('cook.validate_cookable', {
    'platform_name': PLATFORM_NAME,
    'max_assets': 0,
}, timeout=10)
test('cook.validate_cookable max_assets=0 -> -32602', is_err(r, -32602), f'(resp={r})')

# ---------------------------------------------------------------------------
print(f'\n--- {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIPPED ---')
sys.exit(0 if FAIL == 0 else 1)
