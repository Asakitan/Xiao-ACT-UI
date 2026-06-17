# -*- coding: utf-8 -*-
"""Build local Cython accelerators in-place.

Usage:
    python build_cython_ext.py build_ext --inplace

The generated .pyd files are ABI-specific and mandatory at runtime. The
checked-in binaries target the current release environment; other Python
versions should rebuild locally.

Layout (5.0.0 platform/plugin split):
    Platform-side accelerators live next to this script:
      _sao_cy_uihelpers.pyx           — generic UI helpers
      mem_probe/_sao_cy_memscan.pyx   — AVX2 memory scanner

    Star Resonance game accelerators live under the plugin tree:
      plugins/star_resonance_plugin/cython/_sao_cy_pixels.pyx
      plugins/star_resonance_plugin/cython/_sao_cy_combat.pyx
      plugins/star_resonance_plugin/cython/_sao_cy_packet.pyx
      plugins/star_resonance_plugin/cython/_sao_cy_skillfx.pyx
"""
from __future__ import annotations

import os

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))
_SR_CY = os.path.join(HERE, 'plugins', 'star_resonance_plugin', 'cython')

try:
    from Cython.Build import cythonize
except Exception as exc:  # noqa: BLE001
    raise SystemExit(
        'Cython is required to build _sao_cy_pixels, _sao_cy_combat, '
        '_sao_cy_packet, _sao_cy_skillfx, and _sao_cy_uihelpers. '
        'Install requirements.txt or run: python -m pip install Cython'
    ) from exc


extensions = [
    # ── Platform helpers (build output drops next to this script) ──
    Extension(
        name='_sao_cy_uihelpers',
        sources=[os.path.join(HERE, '_sao_cy_uihelpers.pyx')],
    ),
    # mem_probe: AVX2 accelerated memory scan/pattern search for the
    # mem_probe tools that import ``mem_probe.cy_memscan``.
    Extension(
        name='_sao_cy_memscan',
        sources=[os.path.join(HERE, 'mem_probe', '_sao_cy_memscan.pyx')],
        # /wd4551 + /wd4018 silence Cython runtime boilerplate noise.
        extra_compile_args=['/std:c++17', '/wd4551', '/wd4018'],
        language='c++',
    ),
    # ── Star Resonance game accelerators (build output drops into plugin/cython/) ──
    Extension(
        name='_sao_cy_pixels',
        sources=[os.path.join(_SR_CY, '_sao_cy_pixels.pyx')],
    ),
    Extension(
        name='_sao_cy_combat',
        sources=[os.path.join(_SR_CY, '_sao_cy_combat.pyx')],
    ),
    Extension(
        name='_sao_cy_packet',
        sources=[os.path.join(_SR_CY, '_sao_cy_packet.pyx')],
    ),
    Extension(
        name='_sao_cy_skillfx',
        sources=[os.path.join(_SR_CY, '_sao_cy_skillfx.pyx')],
    ),
]

# driver_backend: compile to .pyd if source exists (gitignored, local only).
_drv_src = os.path.join(HERE, 'mem_probe', 'driver_backend.py')
if os.path.isfile(_drv_src):
    extensions.append(Extension(
        name='mem_probe.driver_backend',
        sources=[_drv_src],
    ))


setup(
    name='sao-cython-accelerators',
    ext_modules=cythonize(
        extensions,
        build_dir=os.path.join(HERE, 'build', 'cython'),
        compiler_directives={
            'language_level': '3',
            'boundscheck': False,
            'wraparound': False,
            'initializedcheck': False,
            'nonecheck': False,
            'annotation_typing': False,
        },
    ),
)
