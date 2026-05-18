# MCPTools.tools — individual tool implementations.
# Each module in this package registers one or more tools via @tool from MCPTools.registry.
#
# Modules imported here register their @tool entries at import time, so listing them here
# makes them discoverable after `from MCPTools import tools` runs in the bootstrap.

# Phase 1 — smoke / ping / version reflection tools.
from MCPTools.tools import smoke_tools  # noqa: F401

# Phase 2 — Category D Python composites (find_unused, size_report, batch_metadata, etc.).
# asset_tools.py exposes shared helpers only — no @tool decorators — but importing it here
# keeps the symbol table populated for asset_composites.
from MCPTools.tools import asset_tools  # noqa: F401
from MCPTools.tools import asset_composites  # noqa: F401

# Phase 3 Days 11-14 — Category D Python composites (level.full_actor_dump,
# level.find_actors_with_class, actor.batch_spawn / batch_destroy / batch_set_property). All
# 5 wrappers are async-only — they call dispatch_internal('..._internal', args) and return the
# {job_id} envelope; AI client polls job.status / job.result externally.
from MCPTools.tools import level_composites  # noqa: F401
