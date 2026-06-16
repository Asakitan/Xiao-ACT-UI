"""Top-level mem_probe package — generic memory scanning infrastructure.

Exposes the read-only memory probe stack used by the SAO ACT runtime:

  - :mod:`mem_probe.process` — game process handle (name from config, admin required)
  - :mod:`mem_probe.cy_memscan` — Python facade over ``_sao_cy_memscan`` (AVX2)
  - :mod:`mem_probe.scanner` — multi-frame memory value search
  - :mod:`mem_probe.unified_source` — TCP/memory hybrid data source

Game-specific bridges (e.g. IL2CPP field resolvers) are injected by plugins
at runtime via ``unified_source.set_bridge_classes()``.  The package itself
has no game-specific imports.

All readers rely on ``PROCESS_VM_READ``; admin shell is mandatory.
Write path requires driver backend (Tier B+); see ``StarProcess.write_bytes``.
"""
from __future__ import annotations

# Re-export the Cython facade so ``from mem_probe import cy_memscan`` works
# whether or not the extension has been built in-place.  When the compiled
# ``_sao_cy_memscan`` extension is unavailable the import will silently fall
# back to the pure-Python implementation bundled with ``cy_memscan.py``.
from . import cy_memscan  # noqa: F401