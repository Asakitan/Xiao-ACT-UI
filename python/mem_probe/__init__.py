"""Generic memory scanning infrastructure."""
from __future__ import annotations

# Re-export the Cython facade so ``from mem_probe import cy_memscan`` works
# whether or not the extension has been built in-place.  When the compiled
# ``_sao_cy_memscan`` extension is unavailable the import will silently fall
# back to the pure-Python implementation bundled with ``cy_memscan.py``.
from . import cy_memscan  # noqa: F401