"""SAO-UI mem_probe — production-grade memory locator + accelerators.

This package is the home for **production** mem_probe code:
  * `_sao_cy_memscan` — Cython AVX2 scan kernels (.pyd)
  * `cy_memscan`      — Python facade with pure-Python fallback
  * `locator`         — SmartLocator (TCP-anchored first_run / warm_run)
  * `test_cy_memscan` — correctness + benchmark suite

Research / diagnostic CLIs (auto_locate, refine, fingerprint, pointer_chain,
triangulate, il2cpp/*) remain under `tools.mem_probe.*` and call into this
package for the heavy lifting.
"""

__all__ = ["cy_memscan"]
