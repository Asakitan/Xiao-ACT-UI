# -*- coding: utf-8 -*-
# pack_plugin — 把一个插件目录打成可「一键导入」的 ``.zip`` 包（作者工具）。
#
# 用法:
# python tools/pack_plugin.py plugins/midi_piano_plugin
# python tools/pack_plugin.py path/to/my_plugin -o dist/my_plugin.zip
# python tools/pack_plugin.py path/to/my_plugin --include-libs
#
# 校验 ``plugin.json`` / 入口存在后，把整个目录压成 ``<id>-<version>.zip``（清单落在
# 压缩包根部）。用户在「插件管理 → 导入」选这个 zip 即可装入 ``user_plugins/`` 并启用
# 即用，无需编译（纯 Python 插件）。
#
# 打包约定（与 :mod:`act_platform.plugin_install` / :mod:`act_platform.plugin_deps` 对齐）：
# * 纯 Python：直接打包，运行时 import 原始 .py（dev + onedir 冻结态都行）。
# * 第三方依赖：写进 ``requirements.txt`` 并把**纯 Python 副本** vendor 进 ``vendor/``
# （冻结态没有 pip，必须 vendor）；加载器会自动把 ``vendor/``/``libs/`` 前插 sys.path。
# * 原生扩展(.pyd / Cython)：由作者**预编译**匹配目标 ``cp3xx`` + ``win_amd64`` 后随包，
# 用户端不编译；建议同时留纯 Python 回退。
# * 默认**排除** ``libs/``（dev 期 pip --target 的本地缓存）、``__pycache__``、``.git`` 等；
# ``--include-libs`` 可强制带上 ``libs/``。

from __future__ import annotations

import argparse
import json
import os
import sys
import zipfile

MANIFEST_FILE = "plugin.json"
_DEFAULT_SKIP_DIRS = {"__pycache__", ".git", ".idea", ".vscode", "libs", ".import_"}
_DEFAULT_SKIP_EXT = {".pyc", ".pyo"}


def _safe_id(value: str) -> str:
    text = str(value or "").strip().replace(" ", "_").lower()
    return "".join(ch for ch in text if ch.isalnum() or ch in ("_", "-", ".")).strip("._-")


def pack(plugin_dir: str, out_path: str = "", *, include_libs: bool = False) -> str:
    plugin_dir = os.path.abspath(plugin_dir)
    if not os.path.isdir(plugin_dir):
        raise SystemExit(f"不是目录: {plugin_dir}")
    manifest_path = os.path.join(plugin_dir, MANIFEST_FILE)
    if not os.path.isfile(manifest_path):
        raise SystemExit(f"缺少 {MANIFEST_FILE}: {plugin_dir}")
    with open(manifest_path, "r", encoding="utf-8") as fp:
        manifest = json.load(fp)
    if not isinstance(manifest, dict):
        raise SystemExit("plugin.json 必须是一个 JSON 对象")

    plugin_id = _safe_id(manifest.get("id") or os.path.basename(plugin_dir))
    if not plugin_id:
        raise SystemExit("plugin.json 缺少有效 id")
    version = str(manifest.get("version") or "0.0.0")
    entry = str(manifest.get("entry") or "plugin.py").strip() or "plugin.py"
    if not os.path.isfile(os.path.join(plugin_dir, entry)):
        raise SystemExit(f"入口文件缺失: {entry}")

    skip_dirs = set(_DEFAULT_SKIP_DIRS)
    if include_libs:
        skip_dirs.discard("libs")

    if not out_path:
        out_path = os.path.abspath(f"{plugin_id}-{version}.zip")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)) or ".", exist_ok=True)

    count = 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for base, dirs, files in os.walk(plugin_dir):
            dirs[:] = [d for d in dirs if d not in skip_dirs and not d.startswith(".import_")]
            for name in files:
                if os.path.splitext(name)[1].lower() in _DEFAULT_SKIP_EXT:
                    continue
                full = os.path.join(base, name)
                arc = os.path.relpath(full, plugin_dir)  # 清单落在 zip 根部
                zf.write(full, arc)
                count += 1

    print(f"[pack] {plugin_id} v{version}: {count} 个文件 → {out_path}")
    has_req = os.path.isfile(os.path.join(plugin_dir, "requirements.txt"))
    has_vendor = os.path.isdir(os.path.join(plugin_dir, "vendor"))
    if has_req and not has_vendor:
        print("[pack] ⚠ 有 requirements.txt 但无 vendor/：冻结态(onedir)没有 pip，"
              "若主程序未自带这些依赖，请把纯 Python 副本 vendor 进 vendor/。")
    return out_path


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="打包一个 SAO ACT 插件目录为可一键导入的 .zip")
    ap.add_argument("plugin_dir", help="插件目录(含 plugin.json)")
    ap.add_argument("-o", "--out", default="", help="输出 .zip 路径(默认 <id>-<version>.zip)")
    ap.add_argument("--include-libs", action="store_true", help="把 dev 期 libs/ 一并打入")
    args = ap.parse_args(argv)
    pack(args.plugin_dir, args.out, include_libs=args.include_libs)
    return 0


if __name__ == "__main__":
    sys.exit(main())
