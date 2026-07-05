# -*- coding: utf-8 -*-
# plugin_deps — 平台级「插件外置依赖引导」(从 midi_piano bootstrap 上提为通用能力)。
#
# 任意插件都可声明 ``requirements.txt``；本模块按下列顺序让这些依赖**存在于插件
# 目录内并从那里 import**，不污染主程序全局 site-packages：
#
# 1. libs/    —— ``pip install --target libs``（dev 联网态自动拉取；冻结态跳过）
# 2. vendor/  —— 插件随包自带的纯 Python 副本（冻结 / 离线兜底）
# 3. site     —— 以上都没有时，最后退回主程序环境（已带的依赖；记为 fallback）
#
# 要点：把插件目录内的 ``engine/`` ``libs/`` ``vendor/`` 前插到 ``sys.path``，使插件
# 本地副本优先；返回的记录供 ``restore_paths`` 在卸载时还原，避免主进程残留。
#
# 冻结态 (PyInstaller onedir) 没有 pip，故 ``pip --target`` 跳过 → 作者必须把纯
# Python 依赖随包 vendor。主程序已经带的依赖（cv2 / numpy / config / utils ...）
# 插件直接 import 即可，无需在此声明。参见 :mod:`act_platform.plugin_install`。

from __future__ import annotations

import importlib
import importlib.util
import os
import re
import sys
from typing import Any, Callable, Optional

#: dist 名 → import 名（不同名时在此补充，例如 ``Pillow`` → ``PIL``）。
_IMPORT_NAME: dict[str, str] = {
    "pillow": "PIL",
    "pyyaml": "yaml",
    "beautifulsoup4": "bs4",
}


def _new_record() -> dict[str, Any]:
    return {"added": [], "deps": {}}


def _log(log: Optional[Callable[[str], None]], message: str) -> None:
    if callable(log):
        try:
            log(str(message))
        except Exception:
            pass


def _prepend_path(path: str, rec: dict[str, Any]) -> None:
    path = os.path.abspath(path)
    if not os.path.isdir(path):
        return
    while path in sys.path:
        sys.path.remove(path)
    idx = _safe_plugin_insert_index(path)
    sys.path.insert(idx, path)
    if path not in rec["added"]:
        rec["added"].append(path)


def _safe_plugin_insert_index(plugin_path: str) -> int:
    # Find an insertion index that will NOT shadow packages already
    # importable from the host environment.
    #
    # Walk ``sys.path`` entries; if an entry contains a top-level package
    # directory whose name also exists inside *plugin_path*, skip past it
    # so the host copy wins.  Falls back to ``0`` (prepend) when nothing
    # collides.
    colliders: set[str] = set()
    try:
        for name in os.listdir(plugin_path):
            pkg_dir = os.path.join(plugin_path, name)
            if os.path.isdir(pkg_dir) and os.path.isfile(
                    os.path.join(pkg_dir, '__init__.py')):
                colliders.add(name)
    except OSError:
        return 0
    if not colliders:
        return 0
    last_host = -1
    for i, sp in enumerate(sys.path):
        try:
            for c in colliders:
                if os.path.isdir(os.path.join(sp, c)):
                    last_host = i
                    break
        except OSError:
            continue
    return last_host + 1 if last_host >= 0 else 0


def _parse_requirements(req_file: str) -> list[str]:
    out: list[str] = []
    try:
        with open(req_file, "r", encoding="utf-8") as fp:
            for line in fp:
                s = line.strip()
                if not s or s.startswith("#") or s.startswith("-"):
                    continue
                name = re.split(r"[<>=!~;\[ ]", s, 1)[0].strip()
                if name:
                    out.append(name)
    except FileNotFoundError:
        pass
    except Exception:
        pass
    return out


def _module_origin(mod_name: str) -> Optional[str]:
    # 返回模块文件路径（未加载也能查），找不到返回 None。
    try:
        spec = importlib.util.find_spec(mod_name)
    except Exception:
        return None
    if spec is None:
        return None
    if spec.origin and spec.origin not in ("built-in", "frozen", "namespace"):
        return os.path.abspath(spec.origin)
    locs = list(getattr(spec, "submodule_search_locations", []) or [])
    return os.path.abspath(locs[0]) if locs else None


def _is_inside(path: str, root: str) -> bool:
    if not path:
        return False
    try:
        return os.path.commonpath([os.path.abspath(path), os.path.abspath(root)]) == os.path.abspath(root)
    except ValueError:
        return False


def _fresh_import(mod_name: str) -> bool:
    # Validate that *mod_name* is importable; reload from current
    # ``sys.path`` if it was not previously loaded.
    #
    # **Never** evict modules that are already in ``sys.modules`` — doing
    # so breaks C-extension packages like numpy/PIL whose ``.pyd`` files
    # cannot be loaded twice in one process.
    existing = sys.modules.get(mod_name)
    if existing is not None:
        return True
    importlib.invalidate_caches()
    try:
        importlib.import_module(mod_name)
        return True
    except Exception:
        return False


def _pip_install_target(req_file: str, libs_dir: str, log: Optional[Callable[[str], None]]) -> bool:
    # ``pip install --target <plugin>/libs -r requirements.txt``；冻结态 / 无 pip → False。
    if getattr(sys, "frozen", False):
        return False
    import subprocess
    os.makedirs(libs_dir, exist_ok=True)
    cmd = [sys.executable, "-m", "pip", "install", "--no-input",
           "--disable-pip-version-check", "--target", libs_dir, "-r", req_file]
    try:
        _log(log, "[deps] pip install --target <plugin>/libs -r requirements.txt …")
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if proc.returncode != 0:
            _log(log, f"[deps] pip 失败 rc={proc.returncode}: {(proc.stderr or '')[-300:]}")
        return proc.returncode == 0
    except Exception as exc:
        _log(log, f"[deps] pip 不可用: {exc}")
        return False


def ensure_requirements(plugin_dir: str, log: Optional[Callable[[str], None]] = None,
                        install: bool = True) -> dict[str, Any]:
    # 确保插件 ``requirements.txt`` 的依赖存在于插件目录内并从那里 import。
    #
    # 返回 ``{"added":[paths...], "deps":{name:'libs'|'vendor'|'pip→libs'|'site(fallback)'|'missing'}}``。
    # 总是把 ``engine/`` ``libs/`` ``vendor/`` 前插 sys.path（即使没有 requirements），
    # 这样作者只需把纯 Python 依赖丢进 ``vendor/`` 即可被插件直接 import。
    plugin_dir = os.path.abspath(str(plugin_dir or ""))
    engine_dir = os.path.join(plugin_dir, "engine")
    libs_dir = os.path.join(plugin_dir, "libs")
    vendor_dir = os.path.join(plugin_dir, "vendor")
    req_file = os.path.join(plugin_dir, "requirements.txt")

    rec = _new_record()
    # 最终顺序：engine, libs, vendor, …（engine 最先；libs 优先于 vendor）。
    _prepend_path(vendor_dir, rec)
    _prepend_path(libs_dir, rec)
    _prepend_path(engine_dir, rec)

    for dist in _parse_requirements(req_file):
        mod = _IMPORT_NAME.get(dist.lower(), dist)
        origin = _module_origin(mod)

        # 1) 已能从插件目录内解析（libs 或 vendor）→ 直接用
        if origin and _is_inside(origin, plugin_dir) and _fresh_import(mod):
            rec["deps"][dist] = "libs" if _is_inside(origin, libs_dir) else "vendor"
            continue

        # 2) 插件目录内没有 → 装到插件本地 libs/，再从本地 import（dev 联网态）
        if install and _pip_install_target(req_file, libs_dir, log):
            _prepend_path(libs_dir, rec)
            if _fresh_import(mod) and _is_inside(_module_origin(mod) or "", plugin_dir):
                rec["deps"][dist] = "pip→libs"
                continue

        # 3) vendor 兜底（纯 Python 副本，已在 path 上）
        origin = _module_origin(mod)
        if origin and _is_inside(origin, vendor_dir) and _fresh_import(mod):
            rec["deps"][dist] = "vendor"
            continue

        # 4) 最后退路：主程序环境里有就用（不报错），记为 fallback 提醒
        if _fresh_import(mod):
            rec["deps"][dist] = "site(fallback)"
        else:
            rec["deps"][dist] = "missing"

    summary = ", ".join(f"{k}={v}" for k, v in rec["deps"].items()) or "(none)"
    _log(log, f"[deps] requirements 满足情况(插件本地优先): {summary}")
    return rec


def restore_paths(rec: Optional[dict[str, Any]]) -> None:
    # 卸载时还原 sys.path，移除本插件加入的目录。
    if not rec:
        return
    for path in rec.get("added", []) or []:
        try:
            while path in sys.path:
                sys.path.remove(path)
        except Exception:
            pass


__all__ = ["ensure_requirements", "restore_paths"]
