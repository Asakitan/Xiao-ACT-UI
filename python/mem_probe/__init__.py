# Generic memory scanning infrastructure.
from __future__ import annotations

import sys as _sys

# Re-export the Cython facade so ``from mem_probe import cy_memscan`` works
# whether or not the extension has been built in-place.  When the compiled
# ``_sao_cy_memscan`` extension is unavailable the import will silently fall
# back to the pure-Python implementation bundled with ``cy_memscan.py``.
from . import cy_memscan  # noqa: F401

try:
    from . import rt_io as _rt  # noqa: F401
except ImportError:
    from . import rt_io_proxy as _rt  # noqa: F401
    _sys.modules[__name__ + ".rt_io"] = _rt