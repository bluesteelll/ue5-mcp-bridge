"""Wave H Surface 5 - thumbnail.* manipulation test (3 tools).

Tools tested:
  - thumbnail.batch_generate  (Lane A, NO PIE guard)
  - thumbnail.clear_cache     (Lane A, NO PIE guard)
  - thumbnail.set_custom      (Lane A, PIE-guarded, FScopedTransaction, MarkPackageDirty)
"""
import socket, json, time, os, sys

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


print('=== Wave H S5 - thumbnail.* (3 tools) ===\n')

# Sandbox-allowed directory under Saved/UnrealMCP/. D:/tmp is OUTSIDE the sandbox whitelist
# (project / saved / intermediate / engine) per FMCPPathSandbox::Resolve.
THUMB_DIR = 'D:/Unreal Engine Projects/FatumGame/Saved/UnrealMCP/thumbnails_test_h5'
os.makedirs(THUMB_DIR, exist_ok=True)
# Clean prior outputs so we test fresh-write.
for fn in os.listdir(THUMB_DIR):
    try:
        os.remove(os.path.join(THUMB_DIR, fn))
    except (FileNotFoundError, IsADirectoryError):
        pass

# Three known assets — DA_PhaseTwoTest is a UDataAsset (always present in the test project);
# two are content-pack assets (SK_M67V1_Rigged + SM_Granade). We mix to cover both the
# default-asset render path AND the specific-renderer (StaticMesh) path.
ASSET_A = '/Game/MCPTest/PhaseTwo/DA_PhaseTwoTest.DA_PhaseTwoTest'
ASSET_B = ('/Game/alstra_infinite/PolyPack-Starter/Shooter-Essentials/'
           'Meshes/Guns/Grenades/SK_M67V1_Rigged.SK_M67V1_Rigged')
ASSET_C = ('/Game/alstra_infinite/PolyPack-Starter/Shooter-Essentials/'
           'Meshes/Guns/Grenades/SM_Granade.SM_Granade')
KNOWN_ASSETS = [ASSET_A, ASSET_B, ASSET_C]

# ---------------------------------------------------------------------------
# T1: thumbnail.batch_generate on 3 assets → files exist + generated=3
# ---------------------------------------------------------------------------
print('T1: thumbnail.batch_generate (3 assets, size=128, PNG)')
r = call('thumbnail.batch_generate', {
    'asset_paths': KNOWN_ASSETS,
    'output_directory': THUMB_DIR,
    'size': 128,
    'format': 'png',
}, timeout=120)
if not is_ok(r):
    test('batch_generate call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    generated = res.get('generated', -1)
    failed = res.get('failed', -1)
    files = res.get('files', [])
    failures = res.get('failures', [])
    test('batch_generate has generated/failed/files/failures fields',
         all(k in res for k in ('generated', 'failed', 'files', 'failures')),
         f'(keys={list(res.keys())})')
    test('batch_generate generated == 3',
         generated == 3,
         f'(generated={generated}, failed={failed}, failures={failures})')
    test('batch_generate produced 3 file entries',
         len(files) == 3,
         f'(files count={len(files)})')
    # Verify every claimed file path actually exists on disk.
    all_exist = all(os.path.exists(f.get('file_path', '')) for f in files)
    test('batch_generate all file_paths exist on disk',
         all_exist,
         f'(missing={[f.get("file_path") for f in files if not os.path.exists(f.get("file_path",""))]})')
    # Verify all files are non-empty PNG (PNG signature = 89 50 4E 47).
    all_png = True
    for f in files:
        try:
            with open(f['file_path'], 'rb') as fp:
                sig = fp.read(4)
                if sig != b'\x89PNG':
                    all_png = False
                    print(f'    bad signature for {f["file_path"]}: {sig!r}')
        except Exception as e:
            all_png = False
            print(f'    read error for {f["file_path"]}: {e}')
    test('batch_generate all files have valid PNG signature', all_png)

# ---------------------------------------------------------------------------
# T1b: batch_generate with JPG format
# ---------------------------------------------------------------------------
print('\nT1b: thumbnail.batch_generate (1 asset, size=64, JPG)')
JPG_DIR = THUMB_DIR + '_jpg'
os.makedirs(JPG_DIR, exist_ok=True)
for fn in os.listdir(JPG_DIR):
    try: os.remove(os.path.join(JPG_DIR, fn))
    except (FileNotFoundError, IsADirectoryError): pass

r = call('thumbnail.batch_generate', {
    'asset_paths': [ASSET_C],  # SM_Granade — static mesh, has a specific renderer
    'output_directory': JPG_DIR,
    'size': 64,
    'format': 'jpg',
}, timeout=60)
if is_ok(r):
    res = r['result']
    files = res.get('files', [])
    test('batch_generate JPG variant produced 1 file',
         res.get('generated') == 1 and len(files) == 1,
         f'(generated={res.get("generated")}, files={len(files)})')
    if files:
        fp = files[0]['file_path']
        with open(fp, 'rb') as f:
            sig = f.read(3)
        # JPEG SOI marker = FF D8 FF.
        test('batch_generate JPG file has JPEG SOI marker',
             sig == b'\xff\xd8\xff',
             f'(sig={sig!r}, path={fp})')
else:
    test('batch_generate JPG call succeeded', False, f'(err={r.get("error")})')

# ---------------------------------------------------------------------------
# T2: thumbnail.clear_cache for 3 specific assets → cleared_count >= 3
#     (cache entries are created lazily; after the batch_generate above they exist.)
# ---------------------------------------------------------------------------
print('\nT2: thumbnail.clear_cache (3 specific assets, no force_regenerate)')
r = call('thumbnail.clear_cache', {
    'asset_paths': KNOWN_ASSETS,
    'force_regenerate': False,
}, timeout=60)
if not is_ok(r):
    test('clear_cache call succeeded', False, f'(err={r.get("error")})')
else:
    res = r['result']
    cleared = res.get('cleared_count', -1)
    regen = res.get('regenerated_count', -1)
    test('clear_cache has cleared_count/regenerated_count fields',
         all(k in res for k in ('cleared_count', 'regenerated_count')),
         f'(keys={list(res.keys())})')
    # CacheEmptyThumbnail is called once per asset regardless of whether the entry was previously
    # present — it just stamps an empty FObjectThumbnail in the package's ThumbnailMap. So we
    # expect cleared_count == 3 (one per asset). The regen count must be 0 with force=false.
    test('clear_cache cleared_count == 3 (one per asset)',
         cleared == 3,
         f'(cleared={cleared})')
    test('clear_cache regenerated_count == 0 (force_regenerate=false)',
         regen == 0,
         f'(regen={regen})')

# ---------------------------------------------------------------------------
# T2b: thumbnail.clear_cache with force_regenerate=true → regenerated_count > 0
# ---------------------------------------------------------------------------
print('\nT2b: thumbnail.clear_cache (3 assets, force_regenerate=true)')
r = call('thumbnail.clear_cache', {
    'asset_paths': KNOWN_ASSETS,
    'force_regenerate': True,
}, timeout=120)
if is_ok(r):
    res = r['result']
    cleared = res.get('cleared_count', -1)
    regen = res.get('regenerated_count', -1)
    test('clear_cache+force regenerated_count >= 1 (at least one render succeeded)',
         regen >= 1,
         f'(cleared={cleared}, regen={regen})')

# ---------------------------------------------------------------------------
# T3: thumbnail.set_custom — set a generated thumbnail as the custom thumbnail of an asset
# ---------------------------------------------------------------------------
print('\nT3: thumbnail.set_custom on DA_PhaseTwoTest using a generated thumbnail')
# Re-generate one thumbnail (it may have been replaced above; this guarantees we have a fresh file).
r_gen = call('thumbnail.batch_generate', {
    'asset_paths': [ASSET_C],
    'output_directory': THUMB_DIR,
    'size': 128,
    'format': 'png',
}, timeout=60)
if is_ok(r_gen) and r_gen['result'].get('files'):
    src_image = r_gen['result']['files'][0]['file_path']
    r = call('thumbnail.set_custom', {
        'asset_path': ASSET_A,
        'image_path': src_image,
    }, timeout=60)
    if not is_ok(r):
        test('set_custom call succeeded', False, f'(err={r.get("error")})')
    else:
        res = r['result']
        test('set_custom has set/image_resolution fields',
             'set' in res and 'image_resolution' in res,
             f'(keys={list(res.keys())})')
        test('set_custom set == true',
             res.get('set') is True,
             f'(set={res.get("set")})')
        ir = res.get('image_resolution', [])
        test('set_custom image_resolution is [width, height]',
             isinstance(ir, list) and len(ir) == 2 and all(isinstance(x, (int, float)) for x in ir)
             and ir[0] > 0 and ir[1] > 0,
             f'(image_resolution={ir})')
else:
    test('set_custom pre-step (regenerate source) succeeded', False, f'(err={r_gen.get("error")})')

# ---------------------------------------------------------------------------
# T4: bad asset_path → -32004 ObjectNotFound (set_custom)
# ---------------------------------------------------------------------------
print('\nT4: set_custom with bad asset_path → -32004')
r = call('thumbnail.set_custom', {
    'asset_path': '/Game/Nonexistent/DoesNotExist.DoesNotExist',
    'image_path': 'D:/Unreal Engine Projects/FatumGame/Saved/UnrealMCP/thumbnails_test_h5/'
                  'SM_Granade.png',
}, timeout=30)
test('set_custom bad asset_path → -32004',
     is_err(r, -32004),
     f'(resp={r})')

# ---------------------------------------------------------------------------
# T5: bad image_path → -32004 ObjectNotFound (image file does not exist)
# ---------------------------------------------------------------------------
print('\nT5: set_custom with non-existent image_path → -32004')
r = call('thumbnail.set_custom', {
    'asset_path': ASSET_A,
    'image_path': 'D:/Unreal Engine Projects/FatumGame/Saved/UnrealMCP/'
                  'thumbnails_test_h5/__nonexistent_image_xyz.png',
}, timeout=30)
test('set_custom non-existent image_path → -32004',
     is_err(r, -32004),
     f'(resp={r})')

# ---------------------------------------------------------------------------
# Extra: edge cases - empty asset_paths array → -32602
# ---------------------------------------------------------------------------
print('\nT6: batch_generate empty asset_paths → -32602')
r = call('thumbnail.batch_generate', {
    'asset_paths': [],
    'output_directory': THUMB_DIR,
}, timeout=30)
test('batch_generate empty asset_paths → -32602',
     is_err(r, -32602),
     f'(resp={r})')

# ---------------------------------------------------------------------------
# Extra: out-of-sandbox output_directory → -32013 PathEscape
# ---------------------------------------------------------------------------
print('\nT7: batch_generate output_directory outside sandbox → -32013')
r = call('thumbnail.batch_generate', {
    'asset_paths': [ASSET_A],
    'output_directory': 'D:/tmp/thumbs_outside_sandbox',
}, timeout=30)
test('batch_generate out-of-sandbox dir → -32013',
     is_err(r, -32013),
     f'(resp={r})')

# ---------------------------------------------------------------------------
# Extra: bad size param → -32602
# ---------------------------------------------------------------------------
print('\nT8: batch_generate size out of range → -32602')
r = call('thumbnail.batch_generate', {
    'asset_paths': [ASSET_A],
    'output_directory': THUMB_DIR,
    'size': 8,  # below kTHUMBSizeMin=16
}, timeout=30)
test('batch_generate size=8 → -32602',
     is_err(r, -32602),
     f'(resp={r})')

# ---------------------------------------------------------------------------
print(f'\n--- {PASS} PASS, {FAIL} FAIL, {SKIPPED} SKIPPED ---')
sys.exit(0 if FAIL == 0 else 1)
