# -*- coding: utf-8 -*-
"""构建 runtime-delta zip 包（模块化顶层布局）。

发布后客户端目录:
  XiaoACTUI/
    XiaoACTUI.exe
    update.exe
    web/   assets/   proto/   runtime/   ...

delta zip 内路径与客户端目录一致（以 BASE_DIR 为根）:
  runtime/sao_gui.py
  web/menu.html
  assets/sounds/x.wav

用法:
  python build_delta.py --version 2.1.0 --files runtime/sao_gui.py web/menu.html
  python build_delta.py --version 2.1.0 --from-list changes.txt
"""

from __future__ import annotations

import argparse
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def _resolve_source(rel: str) -> str:
    """将 zip 内相对路径映射到源码中的实际路径。

    优先顺序:
        1) dist/XiaoACTUI/<path>             (如果已构建 onedir 产物)
        2) HERE/<path>                       (源码 .py / web / assets ...)
        3) HERE/<path 去掉 runtime/ 前缀>    (runtime/sao_gui.py -> sao_gui.py)
    """
    rel_norm = rel.replace("\\", "/").lstrip("/")
    candidates = [
        os.path.join(HERE, "dist", "XiaoACTUI", rel_norm),
        os.path.join(HERE, rel_norm),
    ]
    if rel_norm.startswith("runtime/"):
        candidates.append(os.path.join(HERE, rel_norm[len("runtime/"):]))
    for c in candidates:
        if os.path.exists(c):
            return c
    return candidates[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--files", nargs="*", default=[],
                        help="zip 内路径列表，例如: runtime/sao_gui.py web/menu.html")
    parser.add_argument("--from-list", default="",
                        help="从文件读取（每行一个路径）")
    parser.add_argument("--out-dir", default=os.path.join(HERE, "dist", "delta"))
    args = parser.parse_args()

    rels = list(args.files)
    if args.from_list and os.path.exists(args.from_list):
        with open(args.from_list, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    rels.append(line)
    if not rels:
        print("[build_delta] 未指定文件", file=sys.stderr)
        return 1

    os.makedirs(args.out_dir, exist_ok=True)
    out_path = os.path.join(args.out_dir, f"update-{args.version}-runtime-delta.zip")
    missing = []
    written = 0
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
        for rel in rels:
            src = _resolve_source(rel)
            if not os.path.exists(src):
                missing.append((rel, src))
                continue
            zf.write(src, rel)
            written += 1
    print(f"[build_delta] 写入 {written} 个文件 -> {out_path}")
    if missing:
        print("[build_delta] 缺失文件:")
        for rel, src in missing:
            print(f"  {rel}  (expected at {src})")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
