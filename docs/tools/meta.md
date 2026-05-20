---
title: Meta (tools.list, job.*, exec_python)
layout: default
parent: Tools reference
nav_order: 100
---

# Meta tools
{: .no_toc }

1. TOC
{:toc}

## `tools.list`

Probe the bridge for live state + enumerate registered handlers.

```python
r = call("tools.list")
# r["result"] = {
#   "python_ready": True,
#   "cpp_handlers": [...]   # 278+ entries
# }
```

`python_ready=true` indicates the editor's Python interpreter is initialized (otherwise Python-fallback tools return -32602).

Use as **liveness probe** — the bridge always responds with this if it's responsive, regardless of editor world state.

## Jobs (`job.*`)

For long-running operations, the bridge offers an async-job pattern:

### `job.submit`
| `method` | string | The underlying tool method |
| `args` | object | Args for that tool |
| `description` | string | Optional caller-friendly label |

Returns `{ job_id: "<uuid>" }`.

### `job.poll`
| `job_id` | string | required |

Returns one of:

```json
{ "status": "running", "progress": 0.42, "elapsed_seconds": 12.5 }
{ "status": "complete", "result": {...} }
{ "status": "failed", "error": {...} }
{ "status": "cancelled" }
```

### `job.cancel`
| `job_id` | string | required |

Cooperative cancel — bodies that check `Job->IsCancelled()` will stop ASAP; bodies that don't honour it still complete.

### `job.list`
Enumerate all known jobs in the registry.

### `job.purge`
Remove completed/failed/cancelled job records (retention is bounded).

Jobs are used by `cb.bulk_import`, `asset.batch_metadata_async`, and any tool that may take longer than a single editor tick.

## Python eval (`kind=ExecPython`)

```json
{ "id": "1", "kind": "ExecPython", "args": { "expression": "<python>" } }
```

Returns:
```json
{ "id": "1", "ok": true, "result": { "value": <repr>, "value_type": "..." } }
```

The expression runs in the editor's bundled Python interpreter, with `unreal` module pre-imported. Use for one-off introspection / scripting that doesn't warrant a dedicated C++ tool:

```python
r = send({"id": "1", "kind": "ExecPython",
          "args": {"expression": "unreal.SystemLibrary.get_engine_version()"}})
print(r["result"]["value"])
```

Returns -32603 with `{"reason": "python not initialised"}` if Python isn't up yet — wait for `python_ready=true` from `tools.list` first.

## Python registry fallback

When a `kind=call_function` method is NOT in the C++ handler map, the bridge falls back to `MCPTools.registry.get_tool(method)` (Python-side registry). This lets Python-only tools coexist with C++ ones under the same namespace.

Use cases:
- Quick experimental tools that don't need C++ performance
- Tools that wrap Python-only UE features (e.g. things only exposed via `unreal.*` Python bindings)

C++ handlers always take precedence — if `bp.add_node` is in both, the C++ one wins.
