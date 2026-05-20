---
title: debug.* (Visual overlay)
layout: default
parent: Tools reference
nav_order: 2
---

# debug.* — Visual debug overlay (6 tools)

Exposes UE's `DrawDebugHelpers` for live visual debugging via MCP. Lifetime-based overlays work in both editor world and PIE world.

## Constraints

- All Lane A (game thread)
- **NOT PIE-guarded** — drawing works in both editor + PIE; tool picks the appropriate world automatically via `GEditor->PlayWorld ?? GetEditorWorld()`
- No transactions (overlays aren't asset state, not undoable)

## Tools

### `debug.draw_line`
| Arg | Type | Default |
|---|---|---|
| `start` | [x,y,z] | required |
| `end` | [x,y,z] | required |
| `color` | [r,g,b,a] | `[1,1,1,1]` white |
| `thickness` | number | 0 |
| `lifetime` | number | -1 (persistent if `persistent=true`) |
| `persistent` | bool | false |

Calls `DrawDebugLine(World, Start, End, Color, persistent, lifetime, 0, thickness)`.

### `debug.draw_sphere`
| `center` | [x,y,z] | required |
| `radius` | number | required |
| `segments` | int | 16 |

### `debug.draw_box`
| `center` | [x,y,z] | required |
| `extent` | [x,y,z] | required |
| `rotation` | [p,y,r] | `[0,0,0]` |

### `debug.draw_arrow`
| `start` | [x,y,z] | required |
| `end` | [x,y,z] | required |
| `arrow_size` | number | 40 |

### `debug.draw_text`
| `location` | [x,y,z] | required |
| `text` | string | required |
| `draw_shadow` | bool | true |
| `font_scale` | number | 1.0 |

DrawDebugString takes Duration directly (no bPersistent flag). lifetime=-1 means permanent.

### `debug.clear`
No args. Calls `FlushPersistentDebugLines(World) + FlushDebugStrings(World)`.

## Color format

Colors are `[r, g, b, a]` floats 0..1. Default white if missing or wrong shape. Internally converted via `LinearColor.ToFColor(true)`.

## Response shape

All tools return:
```json
{ "ok": true, "result": { "drawn": true, "world": "editor"|"pie" } }
```

debug.clear returns:
```json
{ "ok": true, "result": { "cleared": true, "world": "editor"|"pie" } }
```

## Example

```python
# Draw a coordinate axis indicator at origin
call("debug.draw_arrow", {"start": [0,0,0], "end": [100,0,0], "color": [1,0,0,1], "lifetime": 60})  # X red
call("debug.draw_arrow", {"start": [0,0,0], "end": [0,100,0], "color": [0,1,0,1], "lifetime": 60})  # Y green
call("debug.draw_arrow", {"start": [0,0,0], "end": [0,0,100], "color": [0,0,1,1], "lifetime": 60})  # Z blue
call("debug.draw_text", {"location": [0, 0, 150], "text": "WORLD ORIGIN", "lifetime": 60})

# Box around an actor
import math
center = [500, 200, 100]
half = [50, 50, 100]
call("debug.draw_box", {"center": center, "extent": half, "color": [1, 1, 0, 1], "lifetime": 30})

# Clear everything
call("debug.clear")
```
