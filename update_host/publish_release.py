# -*- coding: utf-8 -*-
"""发布脚本: 把一个目录或 zip 包注册为某 channel/target 的最新版本.

用法:
  python publish_release.py --version 2.1.0 \
      --package path/to/update-2.1.0.zip \
      --type runtime-delta \
      [--minimum 2.0.1] [--force] [--notes "修复..."] \
      [--channel stable] [--target windows-x64] \
      [--release-dir releases]

会:
  1. 把 zip 复制到 <release-dir>/<channel>/<target>/update-<version>-<type>.zip
  2. 计算 SHA256
  3. 写入 <release-dir>/<channel>/<target>/manifest.json
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import sys


def sha256_file(path: str) -> tuple:
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(64 * 1024)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--package", required=True, help="本地 zip 包路径")
    parser.add_argument("--type", default="runtime-delta", choices=["runtime-delta", "full-package"])
    parser.add_argument("--minimum", default="", help="minimum_version (低于则强制升级)")
    parser.add_argument("--force", action="store_true", help="设置 force_update=true")
    parser.add_argument("--notes", default="")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--target", default="windows-x64")
    parser.add_argument("--release-dir", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "releases"))
    args = parser.parse_args()

    pkg = os.path.abspath(args.package)
    if not os.path.exists(pkg):
        print(f"[publish] 包不存在: {pkg}", file=sys.stderr)
        return 1

    target_dir = os.path.join(args.release_dir, args.channel, args.target)
    os.makedirs(target_dir, exist_ok=True)
    fname = f"update-{args.version}-{args.type}{os.path.splitext(pkg)[1] or '.zip'}"
    dst = os.path.join(target_dir, fname)
    shutil.copy2(pkg, dst)
    digest, size = sha256_file(dst)

    manifest = {
        "version": args.version,
        "minimum_version": args.minimum,
        "force_update": bool(args.force),
        "package_type": args.type,
        "target": args.target,
        "channel": args.channel,
        "download_url": f"/downloads/{args.channel}/{args.target}/{fname}",
        "sha256": digest,
        "size": size,
        "notes": args.notes,
        "published_at": datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"),
    }
    manifest_path = os.path.join(target_dir, "manifest.json")
    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    print(f"[publish] 已发布 v{args.version} -> {dst}")
    print(f"[publish] manifest: {manifest_path}")
    print(f"[publish] sha256:   {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
