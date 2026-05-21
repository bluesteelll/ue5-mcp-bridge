"""Wave I Surface 5 — sequencer_ext.* test (3 tools).

Validates the per-binding track/section/possessable creation surface that completes the
Sequencer CRUD triangle started by Wave C Tier 5a (create_sequence / add_master_track /
add_camera_cut / add_keyframe / set_section_range).

Test plan:
  T1: sequencer_ext.add_possessable on a test LevelSequence + spawned actor → possessable_guid
  T2: sequencer_ext.add_track binding=T1.guid track_class="Transform" → track_class_path set
  T3: sequencer_ext.add_section binding=T1.guid track_class="Transform" → section_index >= 0
  T4: bad sequence_path → -32004 ObjectNotFound (or -32010 InvalidPath, depending on form)
  T5: bad track_class string → -32602 InvalidParams
  T6: bad binding_guid format → -32602 InvalidParams

If no fixture LevelSequence + actor pair is discoverable, T1-T3 SKIP with a note. T4-T6 are
self-contained (synthetic inputs) and always run.
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

print('=== Wave I S5 — sequencer_ext.* (3 tools) ===\n')

# ─── Fixture discovery ────────────────────────────────────────────────────────────────────
# Locate a ULevelSequence asset to test against. Prefer one in /Game; fall back to creating a
# fresh one if none exist (uses sequencer.create_sequence from Wave C).
fixture_sequence = None
fixture_actor = None
created_fresh = False

print('Discovering test fixtures...')
r = call('sequencer.list_cinematics', {'scope_paths': ['/Game']})
if is_ok(r) and r['result'].get('sequences'):
    for seq in r['result']['sequences']:
        p = seq.get('path')
        if p:
            fixture_sequence = p
            print(f'  -> found existing sequence: {fixture_sequence}')
            break

# If no existing sequence, create one for testing.
if not fixture_sequence:
    print('  -> no existing LevelSequence found; creating /Game/MCPTest/SEQ_MCP_TestWaveI')
    r = call('sequencer.create_sequence', {
        'dest_path': '/Game/MCPTest/SEQ_MCP_TestWaveI',
        'save': False
    })
    if is_ok(r) and r['result'].get('created'):
        # asset_path comes back as the object path "/Game/.../Foo.Foo" — strip the trailing
        # ".Foo" duplicate to get the package-path form that SEQX_LoadLevelSequenceByPath
        # accepts in either form (it tries the object-path fallback internally).
        ap = r['result'].get('asset_path', '')
        fixture_sequence = ap.split('.')[0] if '.' in ap else ap
        created_fresh = True
        print(f'  -> created fresh sequence: {fixture_sequence}')
    else:
        err = r.get('error', {}) if not is_ok(r) else {}
        print(f'  -> could not create fresh sequence: code={err.get("code")} msg={err.get("message")}')

# Discover an actor to bind. Try StaticMeshActor first (most common in test maps), then fall
# back to a generic AActor search if none found. actor.find_by_class is paginated.
if fixture_sequence:
    candidates = [
        ('/Script/Engine.StaticMeshActor', True),
        ('/Script/Engine.Actor',           True),
    ]
    for cls, sub in candidates:
        r = call('actor.find_by_class', {
            'class_path':        cls,
            'search_subclasses': sub,
            'page_size':         20,
        })
        if is_ok(r) and r['result'].get('actors'):
            for a in r['result']['actors']:
                ap = a.get('actor_path')
                ac = a.get('class', '')
                # Skip the obvious "infrastructure" actors that don't belong in cinematics.
                if ap and 'WorldSettings' not in ac and 'Brush' not in ac and 'HUD' not in ac \
                   and 'PlayerStart' not in ac and 'SkyAtmosphere' not in ac:
                    fixture_actor = ap
                    print(f'  -> selected fixture actor (class={cls}): {fixture_actor}')
                    break
            if not fixture_actor and r['result']['actors']:
                a = r['result']['actors'][0]
                fixture_actor = a.get('actor_path')
                print(f'  -> fallback fixture actor (class={cls}): {fixture_actor}')
            if fixture_actor:
                break

# ─── T1: add_possessable ──────────────────────────────────────────────────────────────────
print(f'\nT1: sequencer_ext.add_possessable')
possessable_guid = None
if fixture_sequence and fixture_actor:
    r = call('sequencer_ext.add_possessable', {
        'sequence_path': fixture_sequence,
        'actor_path':    fixture_actor,
        'label':         'MCP_TestBinding_WaveI'
    })
    if is_ok(r):
        possessable_guid = r['result'].get('possessable_guid')
        ok = bool(possessable_guid) and isinstance(possessable_guid, str) and len(possessable_guid) >= 32
        test('add_possessable returned possessable_guid', ok,
             f'(guid={possessable_guid}, obj_class={r["result"].get("object_class")})')
    else:
        err = r.get('error', {})
        test('add_possessable succeeded', False,
             f'(code={err.get("code")} msg={err.get("message")})')
else:
    skip('T1: no fixture sequence+actor available')

# ─── T2: add_track ────────────────────────────────────────────────────────────────────────
print(f'\nT2: sequencer_ext.add_track track_class="Transform"')
if possessable_guid:
    r = call('sequencer_ext.add_track', {
        'sequence_path': fixture_sequence,
        'binding_guid':  possessable_guid,
        'track_class':   'Transform'
    })
    if is_ok(r):
        cls_path = r['result'].get('track_class_path', '')
        ok = bool(cls_path) and ('Transform' in cls_path)
        test('add_track returned track_class_path with Transform', ok,
             f'(class={cls_path}, track_index={r["result"].get("track_index")})')
    else:
        err = r.get('error', {})
        test('add_track succeeded', False,
             f'(code={err.get("code")} msg={err.get("message")})')
else:
    skip('T2: no possessable_guid from T1')

# ─── T3: add_section ──────────────────────────────────────────────────────────────────────
print(f'\nT3: sequencer_ext.add_section track_class="Transform"')
if possessable_guid:
    r = call('sequencer_ext.add_section', {
        'sequence_path': fixture_sequence,
        'binding_guid':  possessable_guid,
        'track_class':   'Transform',
        'start_frame':   0,
        'end_frame':     120
    })
    if is_ok(r):
        section_idx = r['result'].get('section_index', -1)
        ok = isinstance(section_idx, int) and section_idx >= 0
        test('add_section returned section_index >= 0', ok,
             f'(idx={section_idx}, class={r["result"].get("section_class")})')
    else:
        err = r.get('error', {})
        test('add_section succeeded', False,
             f'(code={err.get("code")} msg={err.get("message")})')
else:
    skip('T3: no possessable_guid from T1')

# ─── T4: bad sequence_path ────────────────────────────────────────────────────────────────
print(f'\nT4: bad sequence_path -> -32004 (or -32010)')
r = call('sequencer_ext.add_possessable', {
    'sequence_path': '/Game/__totally_does_not_exist__/__no_sequence__',
    'actor_path':    'AnyActor'
})
ok = is_err(r, -32004) or is_err(r, -32010)
test('bad sequence_path returns ObjectNotFound or InvalidPath', ok,
     f'(code={r.get("error",{}).get("code")})')

# ─── T5: bad track_class ──────────────────────────────────────────────────────────────────
print(f'\nT5: bad track_class -> -32602')
# Use a plausible fixture sequence for the path so we get past the path check; if no fixture
# was discoverable we synthesise a path that will fail earlier with -32004 — still acceptable
# coverage since the assertion is "track_class validation triggers".
probe_seq = fixture_sequence or '/Game/MCPTest/__nonexistent__'
probe_guid = possessable_guid or '12345678-1234-1234-1234-123456789012'
r = call('sequencer_ext.add_track', {
    'sequence_path': probe_seq,
    'binding_guid':  probe_guid,
    'track_class':   'TotallyBogusTrackClass'
})
test('bad track_class returns -32602', is_err(r, -32602),
     f'(code={r.get("error",{}).get("code")})')

# ─── T6: bad binding_guid format ──────────────────────────────────────────────────────────
print(f'\nT6: bad binding_guid format -> -32602')
r = call('sequencer_ext.add_track', {
    'sequence_path': probe_seq,
    'binding_guid':  'not-a-real-guid-format',
    'track_class':   'Transform'
})
test('bad binding_guid format returns -32602', is_err(r, -32602),
     f'(code={r.get("error",{}).get("code")})')

print(f'\n=== Wave I S5 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
if created_fresh:
    print('(Note: created fresh test sequence /Game/MCPTest/SEQ_MCP_TestWaveI — '
          'delete manually if cleanup desired)')
sys.exit(0 if FAIL == 0 else 1)
