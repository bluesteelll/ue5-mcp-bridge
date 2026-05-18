"""Type marshalling between JSON wire values and Unreal Python types.

Two tiers per blueprint v2 §C2 / §6.1:

**Tier 1 (this module)** — schema-driven walk over KNOWN Python ``unreal.*`` types.
Converts values to/from JSON-encodable dicts tagged with a ``_kind`` discriminator
so the round-trip is unambiguous on both sides. Supports:

* Vector, Rotator, Transform, LinearColor, Quat
* Name, SoftObjectPath
* UObject references (emitted as ``ObjectRef`` with path + class)
* list / tuple / unreal.Array → JSON array
* dict / unreal.Map → JSON object
* unreal.Set → JSON array
* unreal.StructBase (unknown USTRUCTs) → partial marker (Tier 2 reflective walker
  produces full content via C++ ``marshall.read_property``)

**Tier 2 (C++ helper)** — generic reflective walker in
``FMCPMarshalling.cpp`` (registered as ``marshall.list_properties`` /
``marshall.read_property`` / ``marshall.write_property`` /
``marshall.describe_struct``). Required because ``unreal.UStruct.get_properties()``
doesn't exist in the Python bindings (verified — see
D:/tmp/mcp_unreal_spike_results.md).

The two tiers SHARE the discriminator format (``{"_kind":"Vector",...}`` etc.) so
results from C++ ``marshall.read_property`` and Python ``py_to_json`` are
interchangeable on the wire.

This module is auto-applied by :class:`FMCPPythonEval::CallPythonTool` on every
Python tool invocation:

* Args are passed through :func:`json_to_py` (shallow — depth 1; tools opt into
  deeper coercion via explicit ``target_type``) before the tool body runs.
* The tool's return value is passed through :func:`py_to_json` before JSON
  encoding so tools can freely return ``unreal.Vector`` etc. without manual
  conversion.
"""

from __future__ import annotations

import math
from typing import Any, Callable, Dict, Optional, Tuple

try:
    import unreal  # type: ignore[import-not-found]
    _UNREAL_AVAILABLE = True
except ImportError:
    # Running outside the editor — type checks degrade to None-comparisons.
    unreal = None  # type: ignore[assignment]
    _UNREAL_AVAILABLE = False


# -------------------------------------------------------------------------------------------------
# Defaults
# -------------------------------------------------------------------------------------------------

#: Maximum recursion depth for :func:`py_to_json`. Prevents pathological cycles in
#: actor-component graphs from blowing the stack. 10 covers >99 % of real tools.
DEFAULT_MAX_DEPTH = 10


# -------------------------------------------------------------------------------------------------
# Extension registry — game-specific types can register custom converters
# -------------------------------------------------------------------------------------------------

_CUSTOM_MARSHALLERS: Dict[type, Tuple[Callable[[Any], Any], Callable[[Any], Any]]] = {}


def register_marshaller(
    type_class: type,
    py_to_json_fn: Callable[[Any], Any],
    json_to_py_fn: Callable[[Any], Any],
) -> None:
    """Register a custom converter pair for ``type_class``.

    Used by game-specific code that wants to extend the wire format with project
    types (e.g. ``FInventoryItem``). The converters MUST be JSON-compatible —
    ``py_to_json_fn`` produces a JSON-encodable value and ``json_to_py_fn`` is
    its inverse.

    Re-registration overwrites silently (last-writer-wins) — caller is responsible
    for avoiding double-registration of conflicting converters.
    """

    _CUSTOM_MARSHALLERS[type_class] = (py_to_json_fn, json_to_py_fn)


# -------------------------------------------------------------------------------------------------
# Tier 1: py_to_json
# -------------------------------------------------------------------------------------------------


def _is_unreal_type(value: Any, type_name: str) -> bool:
    """True if ``value`` is an instance of ``unreal.<type_name>`` (safe if unreal missing)."""
    if not _UNREAL_AVAILABLE:
        return False
    cls = getattr(unreal, type_name, None)
    return cls is not None and isinstance(value, cls)


def py_to_json(value: Any, max_depth: int = DEFAULT_MAX_DEPTH) -> Any:
    """Convert a Python value (possibly an ``unreal.*`` type) into a JSON-encodable form.

    Recurses into containers (list/tuple/dict/unreal.Array/Map/Set). Unknown types
    fall through to the ``{"_kind":"Unsupported", ...}`` marker so the wire payload
    is always JSON-encodable — never raises on type-mismatch, never returns a
    Python-only object.

    On ``max_depth=0``: emits a ``{"_kind":"Truncated"}`` sentinel and stops.
    """

    if max_depth <= 0:
        return {"_kind": "Truncated", "reason": "max depth"}

    # Custom extension types take precedence over all built-in conversions.
    for type_cls, (to_json, _from_json) in _CUSTOM_MARSHALLERS.items():
        if isinstance(value, type_cls):
            return to_json(value)

    # Primitives pass through unchanged. bool first because bool is an int subclass.
    if value is None or isinstance(value, (bool, int, str)):
        return value
    if isinstance(value, float):
        # JSON doesn't support NaN / Inf — coerce to None so the wire stays valid.
        if math.isnan(value) or math.isinf(value):
            return None
        return value

    if _UNREAL_AVAILABLE:
        # Math / colour structs — emit with _kind discriminator.
        if _is_unreal_type(value, "Vector"):
            return {"_kind": "Vector", "x": float(value.x), "y": float(value.y), "z": float(value.z)}
        if _is_unreal_type(value, "Vector2D"):
            return {"_kind": "Vector2D", "x": float(value.x), "y": float(value.y)}
        if _is_unreal_type(value, "Vector4"):
            return {
                "_kind": "Vector4",
                "x": float(value.x),
                "y": float(value.y),
                "z": float(value.z),
                "w": float(value.w),
            }
        if _is_unreal_type(value, "Rotator"):
            return {
                "_kind": "Rotator",
                "pitch": float(value.pitch),
                "yaw": float(value.yaw),
                "roll": float(value.roll),
            }
        if _is_unreal_type(value, "Quat"):
            return {
                "_kind": "Quat",
                "x": float(value.x),
                "y": float(value.y),
                "z": float(value.z),
                "w": float(value.w),
            }
        if _is_unreal_type(value, "Transform"):
            return {
                "_kind": "Transform",
                "translation": py_to_json(value.translation, max_depth - 1),
                "rotation": py_to_json(value.rotation, max_depth - 1),
                "scale": py_to_json(value.scale3d, max_depth - 1),
            }
        if _is_unreal_type(value, "LinearColor"):
            return {
                "_kind": "LinearColor",
                "r": float(value.r),
                "g": float(value.g),
                "b": float(value.b),
                "a": float(value.a),
            }
        if _is_unreal_type(value, "Color"):
            return {
                "_kind": "Color",
                "r": int(value.r),
                "g": int(value.g),
                "b": int(value.b),
                "a": int(value.a),
            }
        if _is_unreal_type(value, "Name"):
            return {"_kind": "Name", "value": str(value)}
        if _is_unreal_type(value, "Text"):
            return {"_kind": "Text", "value": str(value)}
        if _is_unreal_type(value, "SoftObjectPath"):
            return {"_kind": "SoftObjectPath", "value": str(value)}
        if _is_unreal_type(value, "SoftClassPath"):
            return {"_kind": "SoftClassPath", "value": str(value)}

        # UObject reference — emit class + path so clients can rebind.
        if isinstance(value, unreal.Object):
            try:
                path = value.get_path_name()
                cls = value.get_class().get_name()
                return {"_kind": "ObjectRef", "path": path, "class": cls}
            except Exception as exc:  # pragma: no cover — defensive against null-init objects
                return {"_kind": "ObjectRef", "path": "", "class": "", "error": str(exc)}

        # unreal.Array / Map / Set — duck-typed because they don't subclass list/dict.
        if hasattr(unreal, "Array") and isinstance(value, unreal.Array):
            return [py_to_json(v, max_depth - 1) for v in value]
        if hasattr(unreal, "Map") and isinstance(value, unreal.Map):
            # Map keys aren't always JSON-string-keyable (e.g. Name keys). Emit as pair list.
            return {
                "_kind": "Map",
                "pairs": [
                    {"key": py_to_json(k, max_depth - 1), "value": py_to_json(v, max_depth - 1)}
                    for k, v in value.items()
                ],
            }
        if hasattr(unreal, "Set") and isinstance(value, unreal.Set):
            return [py_to_json(v, max_depth - 1) for v in value]

        # StructBase catch-all — for any USTRUCT the bridge doesn't have an explicit branch for.
        # Tier 2's marshall.read_property gives the full content; here we just stamp the type so
        # the client can decide whether to round-trip via C++.
        if hasattr(unreal, "StructBase") and isinstance(value, unreal.StructBase):
            try:
                struct_name = value.__class__.__name__
            except Exception:
                struct_name = "Unknown"
            return {
                "_kind": "Struct",
                "type": struct_name,
                "_partial": True,
                "_hint": "Use marshall.read_property for full field access",
            }

    # Native Python containers — recurse.
    if isinstance(value, (list, tuple)):
        return [py_to_json(v, max_depth - 1) for v in value]
    if isinstance(value, set):
        return [py_to_json(v, max_depth - 1) for v in value]
    if isinstance(value, dict):
        # Coerce keys to str (JSON requires string keys). Non-string keys become repr().
        out = {}
        for k, v in value.items():
            key_str = k if isinstance(k, str) else str(k)
            out[key_str] = py_to_json(v, max_depth - 1)
        return out

    # Fallback — unrecognised type. Emit a marker so the wire payload stays JSON-valid.
    try:
        repr_str = str(value)[:200]
    except Exception:
        repr_str = "<unrepresentable>"
    return {"_kind": "Unsupported", "type": type(value).__name__, "repr": repr_str}


# -------------------------------------------------------------------------------------------------
# Tier 1: json_to_py
# -------------------------------------------------------------------------------------------------


def _coerce_to_vector(value: Any) -> Any:
    """Build an ``unreal.Vector`` from a dict, list/tuple, or already-vector value."""
    if not _UNREAL_AVAILABLE:
        return value
    if isinstance(value, unreal.Vector):
        return value
    if isinstance(value, dict):
        x = float(value.get("x", value.get("X", 0.0)))
        y = float(value.get("y", value.get("Y", 0.0)))
        z = float(value.get("z", value.get("Z", 0.0)))
        return unreal.Vector(x, y, z)
    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return unreal.Vector(float(value[0]), float(value[1]), float(value[2]))
    raise ValueError(f"json_to_py: cannot coerce {value!r} → unreal.Vector")


def _coerce_to_rotator(value: Any) -> Any:
    if not _UNREAL_AVAILABLE:
        return value
    if isinstance(value, unreal.Rotator):
        return value
    if isinstance(value, dict):
        # Roll/Pitch/Yaw — Python ctor signature is (pitch, yaw, roll).
        pitch = float(value.get("pitch", value.get("Pitch", 0.0)))
        yaw = float(value.get("yaw", value.get("Yaw", 0.0)))
        roll = float(value.get("roll", value.get("Roll", 0.0)))
        return unreal.Rotator(pitch, yaw, roll)
    if isinstance(value, (list, tuple)) and len(value) >= 3:
        return unreal.Rotator(float(value[0]), float(value[1]), float(value[2]))
    raise ValueError(f"json_to_py: cannot coerce {value!r} → unreal.Rotator")


def _coerce_to_quat(value: Any) -> Any:
    if not _UNREAL_AVAILABLE:
        return value
    if isinstance(value, unreal.Quat):
        return value
    if isinstance(value, dict):
        x = float(value.get("x", value.get("X", 0.0)))
        y = float(value.get("y", value.get("Y", 0.0)))
        z = float(value.get("z", value.get("Z", 0.0)))
        w = float(value.get("w", value.get("W", 1.0)))
        return unreal.Quat(x, y, z, w)
    if isinstance(value, (list, tuple)) and len(value) >= 4:
        return unreal.Quat(float(value[0]), float(value[1]), float(value[2]), float(value[3]))
    raise ValueError(f"json_to_py: cannot coerce {value!r} → unreal.Quat")


def _coerce_to_transform(value: Any) -> Any:
    if not _UNREAL_AVAILABLE:
        return value
    if isinstance(value, unreal.Transform):
        return value
    if not isinstance(value, dict):
        raise ValueError(f"json_to_py: cannot coerce {value!r} → unreal.Transform")
    loc_payload = value.get("translation", value.get("location", {}))
    rot_payload = value.get("rotation", {})
    scale_payload = value.get("scale", value.get("scale3d", {"x": 1.0, "y": 1.0, "z": 1.0}))
    loc = _coerce_to_vector(loc_payload)
    # Rotator OR Quat — accept either.
    if isinstance(rot_payload, dict) and ("pitch" in rot_payload or "Pitch" in rot_payload):
        rot = _coerce_to_rotator(rot_payload)
    else:
        rot = _coerce_to_quat(rot_payload) if rot_payload else unreal.Quat()
    scale = _coerce_to_vector(scale_payload)
    t = unreal.Transform()
    t.translation = loc
    if isinstance(rot, unreal.Rotator):
        t.rotation = rot.quaternion()
    else:
        t.rotation = rot
    t.scale3d = scale
    return t


def _coerce_to_linear_color(value: Any) -> Any:
    if not _UNREAL_AVAILABLE:
        return value
    if isinstance(value, unreal.LinearColor):
        return value
    if isinstance(value, dict):
        r = float(value.get("r", value.get("R", 0.0)))
        g = float(value.get("g", value.get("G", 0.0)))
        b = float(value.get("b", value.get("B", 0.0)))
        a = float(value.get("a", value.get("A", 1.0)))
        return unreal.LinearColor(r, g, b, a)
    if isinstance(value, (list, tuple)) and len(value) >= 3:
        a = float(value[3]) if len(value) >= 4 else 1.0
        return unreal.LinearColor(float(value[0]), float(value[1]), float(value[2]), a)
    raise ValueError(f"json_to_py: cannot coerce {value!r} → unreal.LinearColor")


# Dispatch table — _kind discriminator → coercion function.
_KIND_COERCERS: Dict[str, Callable[[Any], Any]] = {}


def _init_kind_coercers() -> None:
    if not _UNREAL_AVAILABLE:
        return

    _KIND_COERCERS["Vector"] = _coerce_to_vector
    _KIND_COERCERS["Rotator"] = _coerce_to_rotator
    _KIND_COERCERS["Quat"] = _coerce_to_quat
    _KIND_COERCERS["Transform"] = _coerce_to_transform
    _KIND_COERCERS["LinearColor"] = _coerce_to_linear_color
    _KIND_COERCERS["Name"] = lambda v: unreal.Name(v["value"]) if isinstance(v, dict) else unreal.Name(str(v))
    _KIND_COERCERS["Text"] = lambda v: unreal.Text(v["value"]) if isinstance(v, dict) else unreal.Text(str(v))
    _KIND_COERCERS["SoftObjectPath"] = (
        lambda v: unreal.SoftObjectPath(v["value"]) if isinstance(v, dict) else unreal.SoftObjectPath(str(v))
    )
    _KIND_COERCERS["ObjectRef"] = (
        lambda v: unreal.load_object(None, v["path"]) if isinstance(v, dict) and v.get("path") else None
    )


_init_kind_coercers()


def json_to_py(value: Any, target_type: Optional[Any] = None) -> Any:
    """Convert a JSON-decoded value into the requested Unreal Python type.

    Behaviour matrix:
        target_type=None
            * Dict with ``_kind`` → looked up in :data:`_KIND_COERCERS` and dispatched.
            * Plain primitive / list / dict → returned unchanged.
        target_type=unreal.Vector (or any registered unreal.* class)
            * Coerces via the matching ``_coerce_to_*`` function.
        target_type=<custom registered type>
            * Dispatches through :func:`register_marshaller`.

    Raises ValueError on coercion failure when a target_type is supplied. Without a
    target_type, the function NEVER raises — returns value unchanged on miss so
    tool code can recursively unwrap layered payloads.
    """

    # Custom registered types win over discriminator dispatch.
    if target_type is not None:
        for type_cls, (_to_json, from_json) in _CUSTOM_MARSHALLERS.items():
            if type_cls is target_type:
                return from_json(value)
        if _UNREAL_AVAILABLE:
            for kind, coerce in _KIND_COERCERS.items():
                cls = getattr(unreal, kind, None)
                if cls is not None and target_type is cls:
                    return coerce(value)
        # Fallback: target_type known but no coercer — try identity.
        return value

    # No target_type — discriminator-driven dispatch.
    if isinstance(value, dict):
        kind = value.get("_kind")
        if isinstance(kind, str):
            # Custom user-registered _kind first (case-sensitive lookup).
            for type_cls, (_to_json, from_json) in _CUSTOM_MARSHALLERS.items():
                if getattr(type_cls, "__name__", None) == kind:
                    return from_json(value)
            coercer = _KIND_COERCERS.get(kind)
            if coercer is not None:
                try:
                    return coercer(value)
                except Exception:
                    # Coercion failed; fall through to identity (caller may not need a UE type).
                    return value
            # _kind=Struct partial / Unsupported / Truncated — leave as-is.
            return value
        # Plain dict (no _kind) — recurse into values to surface any embedded markers.
        return {k: json_to_py(v) for k, v in value.items()}

    if isinstance(value, list):
        return [json_to_py(v) for v in value]

    # Primitives.
    return value
