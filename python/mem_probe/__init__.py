"""Top-level mem_probe package — generic memory scanning infrastructure.

Exposes the read-only memory probe stack used by the SAO ACT runtime:

  - :mod:`mem_probe.process` — process handle (name injected by plugin)
  - :mod:`mem_probe.cy_memscan` — Python facade over ``_sao_cy_memscan`` (AVX2)
  - :mod:`mem_probe.scanner` — multi-frame memory value search

Game-specific modules (mem_access, unified_source) are provided by plugins
and registered into this namespace at load time via ``sys.modules``.

All readers rely on ``PROCESS_VM_READ``; admin shell is mandatory.
Write path requires driver backend (Tier B+); see ``GameProcess.write_bytes``.
"""
from __future__ import annotations

# Re-export the Cython facade so ``from mem_probe import cy_memscan`` works
# whether or not the extension has been built in-place.  When the compiled
# ``_sao_cy_memscan`` extension is unavailable the import will silently fall
# back to the pure-Python implementation bundled with ``cy_memscan.py``.
from . import cy_memscan  # noqa: F401