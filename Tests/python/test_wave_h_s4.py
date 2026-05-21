"""Wave H Surface 4 - screenshot.* extended test (3 tools).

Tools tested:
  - screenshot.high_resolution  (Lane A)
  - screenshot.region_capture   (Lane A)
  - screenshot.diff             (Lane B)
"""
import socket, json, time, sys, os, hashlib

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


def file_md5(path):
    if not os.path.exists(path):
        return None
    with open(path, 'rb') as f:
        return hashlib.md5(f.read()).hexdigest()


print('=== Wave H S4 - screenshot.* (3 tools) ===\n')

# Cleanup any stale outputs from prior runs so we test fresh-write.
# Paths MUST live inside the sandbox whitelist (project / saved / intermediate / engine) —
# screenshot.* uses FMCPPathSandbox::Resolve which rejects D:/tmp and similar.
SAVED_DIR = 'D:/Unreal Engine Projects/FatumGame/Saved/UnrealMCP/screenshots'
os.makedirs(SAVED_DIR, exist_ok=True)
HIRES_PATH = f'{SAVED_DIR}/test_waveh4_hires.png'
REGION_PATH = f'{SAVED_DIR}/test_waveh4_region.png'
DIFF_OVERLAY_PATH = f'{SAVED_DIR}/test_waveh4_diff_overlay.png'
SECOND_HIRES_PATH = f'{SAVED_DIR}/test_waveh4_hires_second.png'
for p in (HIRES_PATH, REGION_PATH, DIFF_OVERLAY_PATH, SECOND_HIRES_PATH):
    try:
        os.remove(p)
    except FileNotFoundError:
        pass

# ---------------------------------------------------------------------------
# T1: screenshot.high_resolution multiplier=2.0 -> file written at ~2x native viewport size
# ---------------------------------------------------------------------------
print('T1: screenshot.high_resolution multiplier=2.0')
r = call('screenshot.high_resolution', {
    'resolution_multiplier': 2.0,
    'output_path': HIRES_PATH,
})
if not is_ok(r):
    test('high_resolution call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    saved = res.get('saved_path')
    w = res.get('width', 0)
    h = res.get('height', 0)
    mult = res.get('multiplier', 0)
    nw = res.get('native_width', 0)
    nh = res.get('native_height', 0)
    bytes_written = res.get('bytes', 0)
    file_exists = saved and os.path.exists(saved)
    expected_w = int(nw * mult) if nw else 0
    expected_h = int(nh * mult) if nh else 0
    test('high_resolution returns saved_path that exists', file_exists,
         f'(path={saved!r}, exists={file_exists})')
    test('high_resolution width/height >= 2x native',
         w >= expected_w * 0.95 and h >= expected_h * 0.95,
         f'(native={nw}x{nh}, returned={w}x{h}, expected~{expected_w}x{expected_h}, mult={mult})')
    test('high_resolution bytes > 0', bytes_written > 0,
         f'(bytes={bytes_written})')

# ---------------------------------------------------------------------------
# T1b: high_resolution multiplier out of range -> -32602
# ---------------------------------------------------------------------------
print('\nT1b: high_resolution multiplier=10.0 (out of [1.0, 8.0]) -> -32602')
r = call('screenshot.high_resolution', {'resolution_multiplier': 10.0})
test('bad multiplier returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error", {}).get("code")})')

# ---------------------------------------------------------------------------
# T2: screenshot.region_capture - need an actor to target. Use any actor in the level
#     (probably some default light/floor). Resolve via actor.spawn then capture, then
#     leave the actor in place (cleanup not strictly necessary for the test).
# ---------------------------------------------------------------------------
print('\nT2: screenshot.region_capture on a spawned StaticMeshActor')
spawn_label = f'WaveH4Region_{int(time.time() * 1000) % 1000000}'
sr = call('actor.spawn', {
    'class_path': '/Script/Engine.StaticMeshActor',
    'location': {'x': 0.0, 'y': 0.0, 'z': 200.0},
    'rotation': {'pitch': 0.0, 'yaw': 0.0, 'roll': 0.0},
    'name': spawn_label,
})
if not is_ok(sr):
    print(f'  (could not spawn test actor: {sr.get("error")})')
    print('  (skipping T2 region_capture proper, retrying with first actor in scene)')
    # Use actor.list to find any actor
    al = call('actor.list', {'max_actors': 1})
    if is_ok(al) and al['result'].get('actors'):
        actor_path = al['result']['actors'][0].get('path')
    else:
        actor_path = None
        SKIPPED += 1
        print(f'  [SKIP] no actor available for region_capture')
else:
    actor_path = sr['result'].get('actor_path')

if actor_path:
    r = call('screenshot.region_capture', {
        'actor_path': actor_path,
        'padding': 150.0,
        'output_path': REGION_PATH,
        'resolution': [800, 600],
    }, timeout=30)
    if not is_ok(r):
        test('region_capture call succeeded', False,
             f'(actor={actor_path!r}, err={r.get("error")})')
    else:
        res = r['result']
        saved = res.get('saved_path')
        w = res.get('width', 0)
        h = res.get('height', 0)
        bytes_written = res.get('bytes', 0)
        bounds = res.get('actor_bounds', {})
        captured_res = res.get('captured_resolution', [])
        file_exists = saved and os.path.exists(saved)
        test('region_capture returns saved_path that exists', file_exists,
             f'(path={saved!r})')
        test('region_capture returned w=800 h=600',
             w == 800 and h == 600, f'(w={w}, h={h})')
        test('region_capture returns actor_bounds', 'origin' in bounds and 'extent' in bounds,
             f'(bounds={bounds})')
        test('region_capture returns captured_resolution',
             isinstance(captured_res, list) and len(captured_res) == 2,
             f'(captured_resolution={captured_res})')
        test('region_capture bytes > 0', bytes_written > 0,
             f'(bytes={bytes_written})')

# ---------------------------------------------------------------------------
# T3: screenshot.diff - compare a file with itself -> identical=true, difference_pct≈0
# ---------------------------------------------------------------------------
print('\nT3: screenshot.diff(same_file, same_file) -> identical=true')
if os.path.exists(HIRES_PATH):
    r = call('screenshot.diff', {
        'image_a_path': HIRES_PATH,
        'image_b_path': HIRES_PATH,
    })
    if not is_ok(r):
        test('diff(same, same) call succeeded', False, f'(err={r.get("error")})')
    else:
        res = r['result']
        identical = res.get('identical', False)
        pct = res.get('difference_pct', -1)
        differing = res.get('differing_pixels', -1)
        total = res.get('total_pixels', -1)
        test('diff(same, same) identical=true', identical,
             f'(identical={identical}, pct={pct}, diff_px={differing}/{total})')
        test('diff(same, same) difference_pct ~ 0', pct < 0.01,
             f'(pct={pct})')
else:
    SKIPPED += 2
    print(f'  [SKIP] HIRES_PATH not available - 2 subtests skipped')

# ---------------------------------------------------------------------------
# T4: screenshot.diff(file_a, file_b) where they differ -> identical=false, pct > 0
# Capture a fresh hi-res shot AFTER spawning the actor (so the scene differs by the new actor)
# ---------------------------------------------------------------------------
print('\nT4: screenshot.diff(file_a, file_b) - DIFFERENT scenes')
# Take another hi-res screenshot but with a different camera position via viewport.set_camera
# This guarantees a different image even if the scene is static.
# Get current camera, move it slightly, take a second shot, restore.
cam = call('viewport.get_camera', {'viewport_index': 0})
if is_ok(cam):
    cam_res = cam['result']
    orig_loc = cam_res.get('camera_location', [0, 0, 0])
    orig_rot = cam_res.get('camera_rotation', [0, 0, 0])
    # We need to use viewport_index 1 (perspective, active) — index 0 is an unrealised ortho.
    # Detect which viewport is active by listing.
    vl = call('viewport.list')
    perspective_vp_index = 0
    if is_ok(vl):
        for v in vl['result']['viewports']:
            if v.get('viewport_type') == 'perspective' and v.get('is_active'):
                perspective_vp_index = v['viewport_index']
                break
    print(f'  (using perspective viewport index {perspective_vp_index} for camera moves)')
    # Re-read camera at that specific index
    cam2 = call('viewport.get_camera', {'viewport_index': perspective_vp_index})
    if is_ok(cam2):
        orig_loc = cam2['result'].get('camera_location', orig_loc)
        orig_rot = cam2['result'].get('camera_rotation', orig_rot)
    # Move camera to a *significantly* different location for shot B
    new_loc = [orig_loc[0] + 2000.0, orig_loc[1] + 2000.0, orig_loc[2] + 500.0]
    new_rot = [orig_rot[0] + 30.0, orig_rot[1] + 90.0, orig_rot[2]]
    sc1 = call('viewport.set_camera', {
        'viewport_index': perspective_vp_index,
        'location': new_loc,
        'rotation': new_rot,
    })
    print(f'  (set_camera ok={is_ok(sc1)}; new_loc={new_loc} new_rot={new_rot})')
    # Verify the change took effect by reading back
    cam_after = call('viewport.get_camera', {'viewport_index': perspective_vp_index})
    if is_ok(cam_after):
        print(f'  (camera read-back: loc={cam_after["result"].get("camera_location")} rot={cam_after["result"].get("camera_rotation")})')
    time.sleep(1.0)  # give editor a tick to redraw
    # Take second shot — pass the same viewport_index so screenshot operates on the moved camera
    r2 = call('screenshot.high_resolution', {
        'viewport_index': perspective_vp_index,
        'resolution_multiplier': 2.0,
        'output_path': SECOND_HIRES_PATH,
    })
    # Restore camera regardless of capture outcome
    call('viewport.set_camera', {
        'viewport_index': perspective_vp_index,
        'location': orig_loc,
        'rotation': orig_rot,
    })
    if not is_ok(r2):
        print(f'  (could not take second screenshot: {r2.get("error")})')
        SKIPPED += 2
    else:
        # Now diff the two. Use a tight threshold (0.001 = 0.1%) so the camera
        # change registers as "not identical" — the default 5% threshold would
        # absorb small viewport shifts.
        r = call('screenshot.diff', {
            'image_a_path': HIRES_PATH,
            'image_b_path': SECOND_HIRES_PATH,
            'threshold': 0.001,
            'diff_output_path': DIFF_OVERLAY_PATH,
        })
        if not is_ok(r):
            test('diff(file_a, file_b) call succeeded', False, f'(err={r.get("error")})')
        else:
            res = r['result']
            identical = res.get('identical', True)
            pct = res.get('difference_pct', 0)
            differing = res.get('differing_pixels', 0)
            diff_image = res.get('diff_image_path')
            test('diff(different) identical=false (threshold=0.1%)', not identical,
                 f'(identical={identical}, pct={pct:.2f}%, diff_px={differing})')
            test('diff(different) difference_pct > 0', pct > 0.01,
                 f'(pct={pct:.2f}%)')
            # Bonus: was diff overlay written?
            if diff_image and os.path.exists(diff_image):
                test('diff overlay written', True,
                     f'(diff_image_path={diff_image!r}, size={os.path.getsize(diff_image)})')
            else:
                test('diff overlay written', False,
                     f'(diff_image_path={diff_image!r})')
else:
    print(f'  (could not read camera state: {cam.get("error")})')
    SKIPPED += 3

# ---------------------------------------------------------------------------
# T5: screenshot.region_capture with bad actor_path -> -32004
# ---------------------------------------------------------------------------
print('\nT5: region_capture bad actor_path -> -32004')
r = call('screenshot.region_capture', {
    'actor_path': '__NoSuchActor_WaveH4__',
    'output_path': f'{SAVED_DIR}/test_bad_region.png',
})
test('bad actor_path returns ObjectNotFound', is_err(r, -32004),
     f'(code={r.get("error", {}).get("code")})')

# ---------------------------------------------------------------------------
# T6: bad output_path (outside sandbox) -> -32013
# ---------------------------------------------------------------------------
print('\nT6: bad output_path outside sandbox -> -32013')
r = call('screenshot.high_resolution', {
    'resolution_multiplier': 2.0,
    'output_path': 'C:/Windows/System32/__test_escape__.png',
})
test('output_path outside sandbox returns PathEscape', is_err(r, -32013),
     f'(code={r.get("error", {}).get("code")})')

# T6b: bad image path for diff (file not found) -> -32004
print('\nT6b: diff with missing file -> -32004')
r = call('screenshot.diff', {
    'image_a_path': f'{SAVED_DIR}/__missing_waveh4_a.png',
    'image_b_path': f'{SAVED_DIR}/__missing_waveh4_b.png',
})
test('missing image file returns ObjectNotFound', is_err(r, -32004),
     f'(code={r.get("error", {}).get("code")})')

# T6c: diff missing required field -> -32602
print('\nT6c: diff without image_a_path -> -32602')
r = call('screenshot.diff', {'image_b_path': HIRES_PATH})
test('missing image_a_path returns InvalidParams', is_err(r, -32602),
     f'(code={r.get("error", {}).get("code")})')

print(f'\n=== Wave H S4 RESULTS: {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIP ===')
sys.exit(0 if FAIL == 0 else 1)
