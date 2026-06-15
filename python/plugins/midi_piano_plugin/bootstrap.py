# -*- coding: utf-8 -*-
"""bootstrap — 插件自带「外置 requirements」依赖引导。

设计目标：依赖完全自包含在**插件自己的目录**里，并从那里 import，
不依赖、不污染主程序的全局 site-packages。

满足顺序（优先级从高到低，全部落在插件目录内）：
  1. libs/    —— `pip install --target libs` 装到插件本地（开发态联网时自动拉取）
  2. vendor/  —— 插件随包自带的纯 Python 副本（冻结态 / 离线兜底）
  （仅当插件目录内都没有、且本地安装失败时，才最后退回主程序环境，避免直接报错）

实现要点：
  * 把 engine/、libs/、vendor/ 放到 sys.path 最前 → 插件目录内的副本优先于 site-packages；
  * import 后校验 __file__ 确实落在插件目录内，否则视作“非本地”，触发本地安装；
  * 返回记录供 on_unload 还原 sys.path，避免主进程残留。
"""

from __future__ import annotations

import importlib
import importlib.util
import os
import re
import sys


_IMPORT_NAME = {  # dist 名 → import 名（不同名时在此补充）
    "mido": "mido",
}


def _record():
    return {"added": [], "deps": {}}


def _prepend_path(path: str, rec: dict):
    path = os.path.abspath(path)
    if not os.path.isdir(path):
        return
    # 放到最前；若已存在先移除再前插，确保插件本地优先级最高
    while path in sys.path:
        sys.path.remove(path)
    sys.path.insert(0, path)
    if path not in rec["added"]:
        rec["added"].append(path)


def _parse_requirements(req_file: str):
    out = []
    try:
        with open(req_file, "r", encoding="utf-8") as f:
            for line in f:
                s = line.strip()
                if not s or s.startswith("#"):
                    continue
                name = re.split(r"[<>=!~;\[ ]", s, 1)[0].strip()
                if name:
                    out.append(name)
    except FileNotFoundError:
        pass
    return out


def _module_origin(mod_name: str):
    """返回模块文件路径（未加载也能查），找不到返回 None。"""
    try:
        spec = importlib.util.find_spec(mod_name)
    except Exception:
        return None
    if spec is None:
        return None
    if spec.origin and spec.origin not in ("built-in", "frozen", "namespace"):
        return os.path.abspath(spec.origin)
    # 命名空间包等：取首个 search location
    locs = list(getattr(spec, "submodule_search_locations", []) or [])
    return os.path.abspath(locs[0]) if locs else None


def _is_inside(path: str, root: str) -> bool:
    if not path:
        return False
    try:
        return os.path.commonpath([os.path.abspath(path), os.path.abspath(root)]) == os.path.abspath(root)
    except Exception:
        return False


def _fresh_import(mod_name: str) -> bool:
    """清掉缓存后重新解析+导入，确保从当前 sys.path（插件本地）加载。"""
    for k in list(sys.modules):
        if k == mod_name or k.startswith(mod_name + "."):
            del sys.modules[k]
    importlib.invalidate_caches()
    try:
        importlib.import_module(mod_name)
        return True
    except Exception:
        return False


def _pip_install_target(req_file: str, libs_dir: str, ctx=None) -> bool:
    """`pip install --target <插件>/libs -r requirements.txt`；冻结态/无 pip → False。"""
    if getattr(sys, "frozen", False):
        return False
    import subprocess
    os.makedirs(libs_dir, exist_ok=True)
    cmd = [sys.executable, "-m", "pip", "install", "--no-input",
           "--disable-pip-version-check", "--target", libs_dir, "-r", req_file]
    try:
        if ctx is not None:
            ctx.log("[deps] pip install --target <plugin>/libs -r requirements.txt …")
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if proc.returncode != 0 and ctx is not None:
            ctx.log(f"[deps] pip 失败 rc={proc.returncode}: {(proc.stderr or '')[-300:]}")
        return proc.returncode == 0
    except Exception as exc:
        if ctx is not None:
            ctx.log(f"[deps] pip 不可用: {exc}")
        return False


def ensure_requirements(plugin_dir: str, ctx=None, install: bool = True) -> dict:
    """确保 requirements.txt 的依赖**存在于插件目录内并从那里 import**。

    返回 {"added":[paths...], "deps":{name:'libs'|'vendor'|'pip→libs'|'site(fallback)'|'missing'}}
    """
    plugin_dir = os.path.abspath(plugin_dir)
    engine_dir = os.path.join(plugin_dir, "engine")
    libs_dir = os.path.join(plugin_dir, "libs")
    vendor_dir = os.path.join(plugin_dir, "vendor")
    req_file = os.path.join(plugin_dir, "requirements.txt")

    rec = _record()
    # 插件目录内目录全部前插：engine 最先；libs 优先于 vendor（最终顺序 engine, libs, vendor, …）
    _prepend_path(vendor_dir, rec)
    _prepend_path(libs_dir, rec)
    _prepend_path(engine_dir, rec)

    for dist in _parse_requirements(req_file):
        mod = _IMPORT_NAME.get(dist, dist)
        origin = _module_origin(mod)

        # 1) 已能从插件目录内解析（libs 或 vendor）→ 直接用
        if origin and _is_inside(origin, plugin_dir):
            if _fresh_import(mod):
                rec["deps"][dist] = "libs" if _is_inside(origin, libs_dir) else "vendor"
                continue

        # 2) 插件目录内没有 → 按要求装到插件本地 libs/，再从本地 import
        if install and _pip_install_target(req_file, libs_dir, ctx):
            _prepend_path(libs_dir, rec)  # 确保 libs 在最前
            if _fresh_import(mod) and _is_inside(_module_origin(mod) or "", plugin_dir):
                rec["deps"][dist] = "pip→libs"
                continue

        # 3) vendor 兜底（纯 Python 副本，已在 path 上）
        origin = _module_origin(mod)
        if origin and _is_inside(origin, vendor_dir) and _fresh_import(mod):
            rec["deps"][dist] = "vendor"
            continue

        # 4) 最后退路：主程序环境里有就用（不报错），但记为 fallback 提醒
        if _fresh_import(mod):
            rec["deps"][dist] = "site(fallback)"
        else:
            rec["deps"][dist] = "missing"

    if ctx is not None:
        summary = ", ".join(f"{k}={v}" for k, v in rec["deps"].items()) or "(none)"
        ctx.log(f"[deps] requirements 满足情况(插件本地优先): {summary}")
    return rec


def restore_paths(rec: dict):
    """on_unload 还原 sys.path，移除本插件加入的目录。"""
    if not rec:
        return
    for p in rec.get("added", []):
        try:
            while p in sys.path:
                sys.path.remove(p)
        except Exception:
            pass
