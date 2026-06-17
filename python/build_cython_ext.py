# -*- coding: utf-8 -*-
"""Build local Cython accelerators in-place.

Usage:
    python build_cython_ext.py build_ext --inplace

The generated .pyd files are ABI-specific and mandatory at runtime. The
checked-in binaries target the current release environment; other Python
versions should rebuild locally.

Layout (5.0.0 platform/plugin split):
        Platform accelerators live next to this script:
      _sao_cy_uihelpers.pyx           — generic UI helpers
      mem_probe/_sao_cy_memscan.pyx   — AVX2 memory scanner

        Plugin accelerators are discovered dynamically from:
            plugins/*/cython/_sao_cy*.pyx
"""
from __future__ import annotations

import os
from glob import glob

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))


def _plugin_cython_extensions() -> list[Extension]:
    out: list[Extension] = []
    for src in sorted(glob(os.path.join(HERE, 'plugins', '*', 'cython', '_sao_cy*.pyx'))):
        name = os.path.splitext(os.path.basename(src))[0]
        if not name:
            continue
        out.append(Extension(name=name, sources=[src]))
    return out

try:
    from Cython.Build import cythonize
except Exception as exc:  # noqa: BLE001
    raise SystemExit(
        'Cython is required to build platform and plugin accelerators. '
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
]

extensions.extend(_plugin_cython_extensions())

_drv_src = os.path.join(HERE, 'mem_probe', 'rt_io.py')
if os.path.isfile(_drv_src):
    extensions.append(Extension(
        name='mem_probe.rt_io',
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
