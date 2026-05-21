"""Wave I Surface 3 — collision.* test (4 tools)."""
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

print('=== Wave I S3 — collision.* (4 tools) ===\n')

# ─── T1: list_channels ──────────────────────────────────────────────────────────────────────────
print('T1: collision.list_channels — enumerate trace + object channels')
r = call('collision.list_channels')
ok = is_ok(r) and isinstance(r['result'].get('trace_channels'), list) and isinstance(r['result'].get('object_channels'), list)
trace_count = len(r['result'].get('trace_channels', [])) if ok else 0
object_count = len(r['result'].get('object_channels', [])) if ok else 0
test('list_channels returns trace+object arrays', ok,
     f'(trace={trace_count}, object={object_count})')

if ok:
    # Spot-check expected engine channels
    trace_names = {e.get('name') for e in r['result']['trace_channels']}
    object_names = {e.get('name') for e in r['result']['object_channels']}
    test('Visibility in trace_channels', 'Visibility' in trace_names, f'(found={sorted(trace_names)})')
    test('Camera in trace_channels', 'Camera' in trace_names)
    test('Pawn in object_channels', 'Pawn' in object_names, f'(found={sorted(object_names)})')
    test('WorldStatic in object_channels', 'WorldStatic' in object_names)
    # Each entry should have index + name + display_name
    sample = (r['result']['trace_channels'] + r['result']['object_channels'])[0]
    test('entry has index+name+display_name',
         all(k in sample for k in ('index', 'name', 'display_name')),
         f'(sample keys={list(sample.keys())})')

# ─── T2: list_profiles ──────────────────────────────────────────────────────────────────────────
print('\nT2: collision.list_profiles — enumerate stock + custom profiles')
r = call('collision.list_profiles')
ok = is_ok(r) and isinstance(r['result'].get('profiles'), list)
profile_count = len(r['result'].get('profiles', [])) if ok else 0
test('list_profiles returns array', ok, f'(count={profile_count})')

profile_names = set()
if ok:
    for p in r['result']['profiles']:
        if 'name' in p:
            profile_names.add(p['name'])
    # UE 5.7 stock profiles
    expected_stock = ['NoCollision', 'BlockAll', 'OverlapAll', 'Pawn', 'BlockAllDynamic']
    for stock in expected_stock:
        test(f'stock profile "{stock}" present', stock in profile_names)

    # First profile should have all expected fields
    p0 = r['result']['profiles'][0]
    expected_keys = ('name', 'collision_enabled', 'object_type', 'helper_description')
    test('profile entry has all keys', all(k in p0 for k in expected_keys),
         f'(p0 keys={list(p0.keys())})')

# ─── T3: get_profile BlockAll ───────────────────────────────────────────────────────────────────
print('\nT3: collision.get_profile name="BlockAll"')
if 'BlockAll' in profile_names:
    r = call('collision.get_profile', {'profile_name': 'BlockAll'})
    ok = is_ok(r) and isinstance(r['result'].get('response_to_channels'), dict)
    test('get_profile BlockAll returns response_to_channels', ok,
         f'(channels={len(r["result"].get("response_to_channels", {}))})')
    if ok:
        responses = r['result']['response_to_channels']
        # BlockAll should have most/all channels set to Block (default for the engine profile).
        block_count = sum(1 for v in responses.values() if v == 'Block')
        test('BlockAll majority responses == Block', block_count > len(responses) / 2,
             f'(block_count={block_count}/{len(responses)})')
        # Each response must be one of the three valid strings
        invalid = [v for v in responses.values() if v not in ('Block', 'Overlap', 'Ignore')]
        test('all responses are Block/Overlap/Ignore', not invalid,
             f'(invalid={invalid[:5]})')
        # Profile-level fields present
        test('get_profile returns collision_enabled+object_type',
             'collision_enabled' in r['result'] and 'object_type' in r['result'],
             f'(enabled={r["result"].get("collision_enabled")}, obj_type={r["result"].get("object_type")})')
else:
    SKIPPED += 1
    print('  [SKIP] BlockAll profile not present — skipping get_profile test')

# ─── T4: set_profile_response — pick a writable profile ────────────────────────────────────────
# UE 5.7 stock profiles are flagged bCanModify=false. We need a profile that allows modification.
# The Custom* convention (FName "Custom" reserved) or any project-added profile works. If no
# writable profile is present, we create a temporary test via Pawn (which most projects modify).
# However, modifying engine stock profiles via TryUpdateDefaultConfigFile DOES work — bCanModify
# is enforced only by the editor UI, not the runtime API. So we proceed with "Pawn" which is the
# brief's choice.
target_profile = 'Pawn'
target_channel = 'Visibility'
print(f'\nT4: collision.set_profile_response profile="{target_profile}" channel="{target_channel}" response="Overlap"')

# Capture the pre-change response for verification + revert.
original_response = None
if target_profile in profile_names:
    rg = call('collision.get_profile', {'profile_name': target_profile})
    if is_ok(rg):
        original_response = rg['result'].get('response_to_channels', {}).get(target_channel)
        print(f'    -> pre-change response for {target_profile}/{target_channel}: {original_response}')

if target_profile in profile_names and original_response is not None:
    # Pick a new response different from the original.
    new_response = 'Overlap' if original_response != 'Overlap' else 'Ignore'
    r = call('collision.set_profile_response', {
        'profile_name': target_profile,
        'channel_name': target_channel,
        'response': new_response
    })
    if is_ok(r):
        updated = r['result'].get('updated', False)
        prior = r['result'].get('prior_response')
        persisted = r['result'].get('persisted_to_ini')
        test('set_profile_response returned updated=true', updated,
             f'(updated={updated}, prior={prior}, persisted={persisted})')
        test('prior_response matches pre-change value',
             prior == original_response,
             f'(reported_prior={prior}, expected={original_response})')
        test('persisted_to_ini is true', persisted is True,
             f'(persisted={persisted})')
    else:
        test('set_profile_response', False,
             f'(error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')

    # ─── T5: round-trip via get_profile ─────────────────────────────────────────────────────────
    print(f'\nT5: collision.get_profile {target_profile} — verify {target_channel} round-trip')
    r = call('collision.get_profile', {'profile_name': target_profile})
    if is_ok(r):
        actual = r['result'].get('response_to_channels', {}).get(target_channel)
        test('round-trip: channel reflects new response',
             actual == new_response,
             f'(actual={actual}, expected={new_response})')
    else:
        test('round-trip get_profile', False,
             f'(error: {r.get("error",{}).get("code")})')

    # ─── Cleanup: revert to original ────────────────────────────────────────────────────────────
    print(f'\nCleanup: revert {target_profile}/{target_channel} → {original_response}')
    r = call('collision.set_profile_response', {
        'profile_name': target_profile,
        'channel_name': target_channel,
        'response': original_response
    })
    if is_ok(r):
        print(f'    -> revert OK (updated={r["result"].get("updated")}, persisted={r["result"].get("persisted_to_ini")})')
    else:
        print(f'    -> revert FAILED (error: {r.get("error",{}).get("code")} {r.get("error",{}).get("message","")})')
else:
    SKIPPED += 1
    print(f'  [SKIP] cannot test set_profile_response — target profile "{target_profile}" not found or original_response unknown')

# ─── T6: bad profile_name → -32004 ─────────────────────────────────────────────────────────────
print('\nT6: get_profile bad profile_name -> -32004')
r = call('collision.get_profile', {'profile_name': '__totally_does_not_exist_profile__'})
test('bad profile returns ObjectNotFound', is_err(r, -32004),
     f'(code={r.get("error",{}).get("code")})')

print('\nT6b: set_profile_response bad profile -> -32004')
r = call('collision.set_profile_response', {
    'profile_name': '__totally_does_not_exist_profile__',
    'channel_name': 'Visibility',
    'response': 'Block'
})
test('bad profile (set) returns ObjectNotFound', is_err(r, -32004),
     f'(code={r.get("error",{}).get("code")})')

print('\nT6c: set_profile_response bad channel -> -32004')
if 'Pawn' in profile_names:
    r = call('collision.set_profile_response', {
        'profile_name': 'Pawn',
        'channel_name': '__totally_does_not_exist_channel__',
        'response': 'Block'
    })
    test('bad channel returns ObjectNotFound', is_err(r, -32004),
         f'(code={r.get("error",{}).get("code")})')
else:
    SKIPPED += 1
    print('  [SKIP] no Pawn profile for bad-channel test')

# ─── T7: bad response value → -32602 ───────────────────────────────────────────────────────────
print('\nT7: set_profile_response bad response value -> -32602')
if 'Pawn' in profile_names:
    r = call('collision.set_profile_response', {
        'profile_name': 'Pawn',
        'channel_name': 'Visibility',
        'response': 'BadValue'
    })
    test('bad response returns InvalidParams', is_err(r, -32602),
         f'(code={r.get("error",{}).get("code")})')

    # Missing required field
    r = call('collision.set_profile_response', {
        'profile_name': 'Pawn',
        'channel_name': 'Visibility'
    })
    test('missing response field returns InvalidParams', is_err(r, -32602),
         f'(code={r.get("error",{}).get("code")})')
else:
    SKIPPED += 1
    print('  [SKIP] no Pawn profile for bad-value test')

print(f'\n=== Wave I S3 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
