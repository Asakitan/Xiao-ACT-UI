# -*- coding: utf-8 -*-
"""打包 onedir 构建产物为一个 full-package zip，用于重大更新（含启动器 exe 本身）。

zip 内部布局与客户端目录完全一致（以 BASE_DIR 为根）:
  XiaoACTUI.exe
    update.exe
    web/...
    assets/...
    proto/...
    runtime/...

用法:
  python build_full_package.py --version 3.0.0
"""

from __future__ import annotations

import argparse
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--src", default=os.path.join(HERE, "dist", "XiaoACTUI"),
                        help="onedir 构建输出目录（含 XiaoACTUI.exe + update.exe + web/assets/proto/runtime）")
    parser.add_argument("--out-dir", default=os.path.join(HERE, "dist", "full"))
    args = parser.parse_args()

    if not os.path.isdir(args.src):
        print(f"[build_full] 源目录不存在: {args.src}", file=sys.stderr)
        print("[build_full] 请先运行: pyinstaller --clean --noconfirm XiaoACTUI.spec", file=sys.stderr)
        return 1

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, f"XiaoACTUI-{args.version}-full-package.zip")
    written = 0
    total_bytes = 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _dirs, files in os.walk(args.src):
            for name in files:
                fp = os.path.join(root, name)
                rel = os.path.relpath(fp, args.src).replace("\\", "/")
                zf.write(fp, rel)
                written += 1
                try:
                    total_bytes += os.path.getsize(fp)
                except Exception:
                    pass
    print(f"[build_full] 写入 {written} 个文件 ({total_bytes/1024/1024:.1f} MB 未压缩) -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
