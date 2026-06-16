# -*- coding: utf-8 -*-
"""一键回归门：运行 tools/ 下全部 ``*_selftest.py`` 并汇总通过/失败。

为什么需要它：各 selftest 既要 ``import _bootstrap``（在 tools/ 内，靠脚本
自身目录进 sys.path 自动满足），又有进程隔离用例会 spawn
``python -m act_platform.parser_worker``（需要 **仓库根** 在 cwd / 可导入）。
因此每个子测试必须以 ``python tools/<name>_selftest.py`` 形式、cwd=仓库根
运行——直接 ``python <name>.py``（cwd=tools/）会让进程隔离子进程找不到
``act_platform`` 而假阳性。本 runner 固定该正确姿势，避免每次手搓时踩坑。

用法（在 sao_auto 仓库根）：

    python tools/run_selftests.py            # 跑全部
    python tools/run_selftests.py act_       # 只跑名字含 act_ 的
    python tools/run_selftests.py --timeout 90

退出码 0=全绿，1=有失败/超时，便于当 CI / 提交门。
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS)
PY = sys.executable


def discover(filt: str | None) -> list[str]:
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(TOOLS, "*_selftest.py")))
    if filt:
        names = [n for n in names if filt in n]
    return names


def main() -> int:
    ap = argparse.ArgumentParser(description="Run all tools/*_selftest.py from the repo root.")
    ap.add_argument("filter", nargs="?", default=None,
                    help="只跑文件名包含该子串的自测（缺省=全部）")
    ap.add_argument("--timeout", type=int, default=90, help="单测超时秒数（默认 90）")
    args = ap.parse_args()

    names = discover(args.filter)
    if not names:
        print("没有匹配的 *_selftest.py")
        return 1

    passed: list[str] = []
    failed: list[tuple[str, int, str]] = []
    errored: list[tuple[str, str]] = []

    for name in names:
        rel = os.path.join("tools", name)
        try:
            r = subprocess.run([PY, rel], cwd=REPO_ROOT, capture_output=True,
                               text=True, timeout=args.timeout)
            if r.returncode == 0:
                passed.append(name)
                print(f"  ok   {name}")
            else:
                tail = (r.stderr or r.stdout or "")[-500:]
                failed.append((name, r.returncode, tail))
                print(f"  FAIL {name} (rc={r.returncode})")
        except subprocess.TimeoutExpired:
            errored.append((name, f"TIMEOUT>{args.timeout}s"))
            print(f"  ERR  {name} (timeout)")
        except Exception as exc:  # noqa: BLE001
            errored.append((name, repr(exc)))
            print(f"  ERR  {name} ({exc!r})")

    total = len(names)
    print(f"\n===== {len(passed)}/{total} passed =====")
    for name, rc, tail in failed:
        print(f"\n[FAIL rc={rc}] {name}\n{tail}")
    for name, why in errored:
        print(f"[ERR] {name}: {why}")
    if not failed and not errored:
        print("ALL GREEN")
    return 1 if (failed or errored) else 0


if __name__ == "__main__":
    raise SystemExit(main())
