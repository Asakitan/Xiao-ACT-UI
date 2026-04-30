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
