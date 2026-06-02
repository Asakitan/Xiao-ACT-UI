"""Top-level mem_probe package.

Exposes the read-only memory probe stack used by the SAO ACT runtime:

  - :mod:`mem_probe.process` — minimal Star.exe process handle (admin required)
  - :mod:`mem_probe.cy_memscan` — Python facade over ``_sao_cy_memscan`` (AVX2)
  - :mod:`mem_probe.il2cpp` — script.json / dump.cs / static resolvers

All readers rely on ``PROCESS_VM_READ``; running them from an admin shell is
mandatory.  Nothing in this package mutates the target process.
"""
from __future__ import annotations

# Re-export the Cython facade so ``from mem_probe import cy_memscan`` works
# whether or not the extension has been built in-place.  When the compiled
# ``_sao_cy_memscan`` extension is unavailable the import will silently fall
# back to the pure-Python implementation bundled with ``cy_memscan.py``.
from . import cy_memscan  # noqa: F401