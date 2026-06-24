# -*- coding: utf-8 -*-
"""render — GPU 叠加窗口/合成器/渲染器/帧调度等通用渲染管线模块。

功能子包：保持空 __init__（不 re-export），各模块用完整点路径引用，
例如 `from render import gpu_renderer`，以避免汇聚式 __init__ 引入循环依赖。
（2026-06-02 目录重组：从根目录下沉至此。）
"""

from pathlib import Path


def _newer_build_render_dir() -> Path | None:
    here = Path(__file__).resolve().parent
    source_pyds = sorted(here.glob("model3d_software*.pyd"))
    source_mtime = 0
    if source_pyds:
        try:
            source_mtime = max(int(path.stat().st_mtime_ns) for path in source_pyds)
        except Exception:
            source_mtime = 0
    best_dir: Path | None = None
    best_mtime = source_mtime
    candidates = list(here.parent.glob("build/lib.win-amd64-cpython-*/render"))
    candidates.extend(here.parent.glob("build/*/render"))
    for candidate in sorted(candidates):
        pyds = sorted(candidate.glob("model3d_software*.pyd"))
        if not pyds:
            continue
        try:
            mtime = max(int(path.stat().st_mtime_ns) for path in pyds)
        except Exception:
            continue
        if mtime > best_mtime:
            best_dir = candidate
            best_mtime = mtime
    return best_dir


_build_render_dir = _newer_build_render_dir()
if _build_render_dir is not None:
    build_path = str(_build_render_dir)
    if build_path not in __path__:
        __path__.insert(0, build_path)
