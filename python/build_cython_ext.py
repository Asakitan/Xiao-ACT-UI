# -*- coding: utf-8 -*-
# Build local Cython accelerators in-place.
#
# Usage:
# python build_cython_ext.py build_ext --inplace
#
# The generated .pyd files are ABI-specific and mandatory at runtime. The
# checked-in binaries target the current release environment; other Python
# versions should rebuild locally.
#
# Layout (5.0.0 platform/plugin split):
# Platform accelerators live next to this script:
# _sao_cy_uihelpers.pyx           — generic UI helpers
# mem_probe/_sao_cy_memscan.pyx   — AVX2 memory scanner
#
# Plugin accelerators are discovered dynamically from:
# plugins/*/cython/_sao_cy*.pyx
from __future__ import annotations

import os
from glob import glob

from setuptools import Extension, setup

HERE = os.path.dirname(os.path.abspath(__file__))

import pyx_vault
pyx_vault.sync()


def _selected_extension_names() -> set[str]:
    raw = os.environ.get('SAO_CY_ONLY', '').strip()
    if not raw:
        return set()
    return {item.strip() for item in raw.split(',') if item.strip()}


def _filter_extensions(selected: set[str], items: list[Extension]) -> list[Extension]:
    if not selected:
        return items
    out = [item for item in items if item.name in selected]
    found = {item.name for item in out}
    missing = sorted(selected - found)
    if missing:
        raise SystemExit(f'Unknown SAO_CY_ONLY target(s): {", ".join(missing)}')
    print('Building selected Cython extensions:', ', '.join(sorted(found)))
    return out


def _plugin_cython_extensions() -> list[Extension]:
    out: list[Extension] = []
    for src in sorted(glob(os.path.join(HERE, 'plugins', '*', 'cython', '_sao_cy*.pyx'))):
        name = os.path.splitext(os.path.basename(src))[0]
        if not name:
            continue
        out.append(Extension(name=name, sources=[src]))
    return out


def _relocate_plugin_pyds() -> None:
    # Move plugin .pyd from source root back to their plugin cython/ dirs.
    import sysconfig
    suffix = sysconfig.get_config_var('EXT_SUFFIX') or '.pyd'
    plugin_srcs = glob(os.path.join(HERE, 'plugins', '*', 'cython', '_sao_cy*.pyx'))
    plugin_names = {os.path.splitext(os.path.basename(s))[0] for s in plugin_srcs}
    for name in sorted(plugin_names):
        root_pyd = os.path.join(HERE, f'{name}{suffix}')
        if not os.path.isfile(root_pyd):
            continue
        for src in plugin_srcs:
            if os.path.splitext(os.path.basename(src))[0] == name:
                dst = os.path.join(os.path.dirname(src), f'{name}{suffix}')
                try:
                    os.replace(root_pyd, dst)
                    print(f'  relocated {name}{suffix} -> {os.path.relpath(dst, HERE)}')
                except Exception as e:
                    print(f'  relocate {name} failed: {e}')
                break

try:
    from Cython.Build import cythonize
except Exception as exc:  # noqa: BLE001
    raise SystemExit(
        'Cython is required to build platform and plugin accelerators. '
        'Install requirements.txt or run: python -m pip install Cython'
    ) from exc


_wnd_src = os.path.join(HERE, '_sao_cy_wnd.pyx')
_pixels_src = os.path.join(HERE, '_sao_cy_pixels.pyx')

extensions = [
    # ── Platform helpers (build output drops next to this script) ──
    Extension(
        name='_sao_cy_uihelpers',
        sources=[os.path.join(HERE, '_sao_cy_uihelpers.pyx')],
    ),
    Extension(
        name='_sao_cy_pixels',
        sources=[_pixels_src],
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

if os.path.isfile(_wnd_src):
    extensions.append(Extension(
        name='_sao_cy_wnd',
        sources=[_wnd_src],
    ))

_drv_src = os.path.join(HERE, 'mem_probe', 'rt_io.py')
if os.path.isfile(_drv_src):
    extensions.append(Extension(
        name='mem_probe.rt_io',
        sources=[_drv_src],
    ))

_drv_proxy_src = os.path.join(HERE, 'mem_probe', 'rt_io_proxy.py')
if os.path.isfile(_drv_proxy_src):
    extensions.append(Extension(
        name='mem_probe.rt_io_proxy',
        sources=[_drv_proxy_src],
    ))

extensions = _filter_extensions(_selected_extension_names(), extensions)


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

if 'build_ext' in os.sys.argv and '--inplace' in os.sys.argv:
    _relocate_plugin_pyds()
