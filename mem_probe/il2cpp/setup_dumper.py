"""自动下载并解压 Il2CppDumper 到 bin/.

CLI:
    python -m tools.mem_probe.il2cpp.setup_dumper
    python -m tools.mem_probe.il2cpp.setup_dumper --version v6.7.46
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import urllib.request
import zipfile
from typing import List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_BIN_DIR = os.path.join(_HERE, "bin")
_GH_LATEST = "https://api.github.com/repos/Perfare/Il2CppDumper/releases/latest"


def _fetch_release(version: Optional[str]) -> dict:
    if version:
        url = f"https://api.github.com/repos/Perfare/Il2CppDumper/releases/tags/{version}"
    else:
        url = _GH_LATEST
    req = urllib.request.Request(url, headers={"User-Agent": "sao-auto-setup"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _pick_asset(release: dict) -> dict:
    """挑 net6 windows 版本."""
    candidates = []
    for a in release.get("assets", []):
        n = a["name"].lower()
        if n.endswith(".zip") and "net6" in n:
            candidates.append(a)
    if not candidates:
        # 退化: 任意 .zip
        for a in release.get("assets", []):
            if a["name"].lower().endswith(".zip"):
                candidates.append(a)
    if not candidates:
        raise RuntimeError("未找到合适的 release asset (.zip)")
    return candidates[0]


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Download Il2CppDumper into bin/")
    ap.add_argument("--version", default=None, help="指定 release tag (默认 latest)")
    ap.add_argument("--force", action="store_true", help="强制重下")
    args = ap.parse_args(argv)

    exe = os.path.join(_BIN_DIR, "Il2CppDumper.exe")
    if os.path.isfile(exe) and not args.force:
        print(f"[ok] 已存在: {exe}")
        print("    使用 --force 强制重下")
        return 0

    print("[info] 查询 GitHub release ...")
    try:
        rel = _fetch_release(args.version)
    except Exception as e:
        print(f"[fail] 查询失败: {e}", file=sys.stderr)
        print("    请手动下载: https://github.com/Perfare/Il2CppDumper/releases", file=sys.stderr)
        return 2

    tag = rel.get("tag_name", "?")
    asset = _pick_asset(rel)
    name = asset["name"]
    url = asset["browser_download_url"]
    size = asset.get("size", 0)
    print(f"[info] tag={tag}  asset={name}  size={size/1024/1024:.2f}MB")
    print(f"[info] url={url}")

    print("[info] 下载中 ...")
    req = urllib.request.Request(url, headers={"User-Agent": "sao-auto-setup"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = resp.read()
    print(f"[ok] 下载完成 ({len(data)/1024/1024:.2f}MB)")

    print(f"[info] 解压到 {_BIN_DIR}")
    os.makedirs(_BIN_DIR, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        zf.extractall(_BIN_DIR)

    if not os.path.isfile(exe):
        # 可能解压到子目录, 找一下
        for root, _, files in os.walk(_BIN_DIR):
            if "Il2CppDumper.exe" in files:
                src = os.path.join(root, "Il2CppDumper.exe")
                print(f"[info] 发现于子目录: {src}")
                exe = src
                break

    if os.path.isfile(exe):
        print(f"[ok] Il2CppDumper.exe 就绪: {exe}")
        return 0
    print("[fail] 解压后未找到 Il2CppDumper.exe", file=sys.stderr)
    return 3


if __name__ == "__main__":
    sys.exit(main())
