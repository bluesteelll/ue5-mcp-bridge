"""Type marshalling between JSON wire values and Unreal Python types.

Two tiers as specified in blueprint v2 §C2 / §6.1:

**Tier 1 (this module, Python-side)** — schema-driven walk over known property
paths using ``UObject.get_editor_property`` / ``set_editor_property``. Suitable
when the caller knows the exact field names (declarative tools). Limitations:

* Returned struct values are COPIES — mutating ``transform.translation.x`` does
  NOT persist. Pattern: read full struct → mutate → ``set_editor_property``.
* ``BlueprintReadOnly`` UPROPERTY raises on write — Tier 1 propagates as-is.
* No generic "list all properties of unknown UObject" — that's Tier 2.

**Tier 2 (C++ helper, ``UnrealMCPBridge`` module)** — reflective property walker
via ``FProperty::ContainerPtrToValuePtr`` + ``FStructProperty`` recursive descent.
Exposed as bridge tools ``marshall.read_property`` / ``write_property`` /
``list_properties`` / ``describe_struct``. Lives in C++ because
``unreal.UStruct.get_properties()`` does NOT exist in the Python bindings
(verified empirically — see D:/tmp/mcp_unreal_spike_results.md).

Phase 1 Day 1: signatures only. Real implementations land alongside the Tier 2
C++ helper in Day 4-5.
"""

from __future__ import annotations

from typing import Any, Optional


def py_to_json(value: Any) -> Any:
    """Convert a Python value (possibly an ``unreal.*`` type) into a JSON-encodable form.

    Will handle ``unreal.Vector`` / ``Rotator`` / ``Transform`` / ``LinearColor`` /
    ``Name`` / ``SoftObjectPath`` / containers thereof, plus all native JSON types.
    See blueprint v2 §C2 Tier 3 type-coercion table for the full matrix.
    """

    raise NotImplementedError("py_to_json: Phase 1 Day 4 work — see blueprint v2 §6.1 Tier 1")


def json_to_py(value: Any, target_type: Optional[Any] = None) -> Any:
    """Convert a JSON-decoded value into the requested Unreal Python type.

    ``target_type`` is an optional hint — typically the destination ``FProperty``'s
    Python class (e.g. ``unreal.Vector``). When omitted the inverse mapping of
    :func:`py_to_json` is applied (best-effort, fail-fast on ambiguous cases).
    """

    raise NotImplementedError("json_to_py: Phase 1 Day 4 work — see blueprint v2 §6.1 Tier 1")
