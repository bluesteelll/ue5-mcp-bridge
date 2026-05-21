# UnrealMCPBridge — Python smoke + integration tests

End-to-end tests that drive the running editor's MCP bridge over TCP.

## Usage

1. Launch FatumGame editor (the bridge auto-starts on port 30020).
2. From any terminal:
   ```bash
   PYTHONIOENCODING=utf-8 python test_wave_<X>_s<N>.py
   ```
3. Each script prints per-tool PASS/FAIL/SKIP and exits non-zero on hard failure.

## Bridge readiness probe

Every test opens a single TCP connection to `127.0.0.1:30020`, sends newline-delimited JSON-RPC frames, and reads single-line replies. Pre-test, scripts poll `tools.ping` until success (the editor may still be loading subsystems for ~3-10 seconds after startup):

```python
import socket, json, time, sys
sock = socket.create_connection(("127.0.0.1", 30020), timeout=5)
sock.sendall(b'{"jsonrpc":"2.0","id":"probe","method":"ping"}\n')
reply = sock.recv(4096)
```

## Per-file naming convention

```
test_wave_<wave_letter>_s<surface_number>.py    # Wave H S1 = data_table.*
test_wave_<wave_letter>.py                       # full-wave smoke (one-shot)
test_<feature>.py                                # feature-specific cross-surface tests
```

47 tests as of post-Wave-J (2026-05-21). Group ownership in `MEMORY.md`.

## Required environment

- `PYTHONIOENCODING=utf-8` — Windows console default `cp1251` mangles ✓/✗ status glyphs in test output.
- Editor running in development mode (the bridge plugin loads at editor-startup, not at PIE start).
- For `pie.*` tests: PIE must be running before the test invokes those tools (each test gates its PIE-dependent block).

## Test counts (snapshot 2026-05-21 post-Wave J)

- Waves A-J: 47 test scripts (one per surface, plus a few cross-cutting probes).
- ~430 PASS / ~40 SKIP / 0 FAIL on the canonical FatumGame editor world at last full sweep.
- SKIPs are all fixture-absence (e.g. Wave J AI tests skip when the test map has no BehaviorTree fixtures — FatumGame uses Flecs ECS for AI, not UE-AI).
