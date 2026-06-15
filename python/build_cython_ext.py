# -*- coding: utf-8 -*-
"""Build local Cython accelerators in-place.

Usage:
    python build_cython_ext.py build_ext --inplace

The generated .pyd files are ABI-specific and mandatory at runtime. The
checked-in binaries target the current release environment; other Python
versions should rebuild locally.
"""
from __future__ import annotations

import os

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))

try:
    from Cython.Build import cythonize
except Exception as exc:  # noqa: BLE001
    raise SystemExit(
        'Cython is required to build _sao_cy_pixels, _sao_cy_combat, '
        '_sao_cy_packet, and _sao_cy_skillfx. '
        'Install requirements.txt or run: python -m pip install Cython'
    ) from exc


extensions = [
    Extension(
        name='_sao_cy_pixels',
        sources=[os.path.join(HERE, '_sao_cy_pixels.pyx')],
    ),
    Extension(
        name='_sao_cy_combat',
        sources=[os.path.join(HERE, '_sao_cy_combat.pyx')],
    ),
    Extension(
        name='_sao_cy_packet',
        sources=[os.path.join(HERE, '_sao_cy_packet.pyx')],
    ),
    Extension(
        name='_sao_cy_skillfx',
        sources=[os.path.join(HERE, '_sao_cy_skillfx.pyx')],
    ),
    Extension(
        name='_sao_cy_uihelpers',
        sources=[os.path.join(HERE, '_sao_cy_uihelpers.pyx')],
    ),
    # mem_probe: AVX2 accelerated memory scan/pattern search for the
    # mem_probe tools that import ``mem_probe.cy_memscan``.  Source lives at
    # the repo root under ``mem_probe/`` so the import name continues to
    # work and the .pyd gets dropped next to the .pyx via ``build_ext --inplace``.
    Extension(
        name='_sao_cy_memscan',
        sources=[os.path.join(HERE, 'mem_probe', '_sao_cy_memscan.pyx')],
        # /wd4551 + /wd4018 silence Cython runtime boilerplate noise
        # ((void)__Pyx_*; casts and generated signed/unsigned comparisons),
        # not anything in our source.
        extra_compile_args=['/std:c++17', '/wd4551', '/wd4018'],
        language='c++',
    ),
]


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
