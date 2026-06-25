# -*- coding: utf-8 -*-
"""一键回归门：运行 tools/ 下全部 ``*_selftest.py`` 并汇总通过/失败。

为什么需要它：各 selftest 既要 ``import _bootstrap``（在 tools/ 内，靠脚本
自身目录进 sys.path 自动满足），又有进程隔离用例会 spawn
``python -m act_platform.parser_worker``（需要 **仓库根** 在 cwd / 可导入）。
因此每个子测试必须以脚本绝对路径形式、cwd=python 源码根
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
PY_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(TOOLS)))
PY = sys.executable
_FAILURE_DETAIL_LINE_LIMIT = 80
_FINAL_FAILURE_POINT_HEADING = "FAILED CHECK POINTS (final):"
_RECORDED_FAILURE_NOTE = "failure points are listed last"


def discover(filt: str | None) -> list[str]:
    names = sorted(os.path.basename(p) for p in glob.glob(os.path.join(TOOLS, "*_selftest.py")))
    if filt:
        names = [n for n in names if filt in n]
    return names


def _failure_detail_tail(detail: str) -> list[str]:
    lines = str(detail or "").splitlines()
    if len(lines) <= _FAILURE_DETAIL_LINE_LIMIT:
        return lines
    omitted = len(lines) - _FAILURE_DETAIL_LINE_LIMIT
    return [f"... omitted {omitted} earlier detail lines ...", *lines[-_FAILURE_DETAIL_LINE_LIMIT:]]


def _process_detail(stdout: str, stderr: str) -> str:
    chunks: list[str] = []
    if stderr:
        chunks.append("[stderr]")
        chunks.append(stderr.rstrip())
    if stdout:
        chunks.append("[stdout]")
        chunks.append(stdout.rstrip())
    return "\n".join(chunks).strip()


def _failure_point_reason(detail: str) -> str:
    fallback = ""
    for line in str(detail or "").splitlines():
        text = line.strip()
        if not text or text in {"[stdout]", "[stderr]"}:
            continue
        if set(text) <= {"F", "E", "s", "S", "x", "X", "."}:
            continue
        if set(text) <= {"=", "-"}:
            continue
        if text.startswith(("FAIL:", "ERROR:", "AssertionError:", "ModuleNotFoundError:")):
            return text[:177] + "..." if len(text) > 180 else text
        if not fallback:
            fallback = text
    if fallback:
        return fallback[:177] + "..." if len(fallback) > 180 else fallback
    return "no detail recorded"


def _print_final_failed_check_points(
    failed: list[tuple[str, int, str]],
    errored: list[tuple[str, str]],
) -> None:
    print()
    print("=" * 50)
    print(_FINAL_FAILURE_POINT_HEADING)
    index = 1
    for name, rc, detail in failed:
        print(f"  {index}. {name} (rc={rc}): {_failure_point_reason(detail)}")
        detail_lines = _failure_detail_tail(detail)
        if detail_lines:
            print("     detail:")
            for line in detail_lines:
                print(f"       {line}")
        index += 1
    for name, why in errored:
        print(f"  {index}. {name}: {why}")
        index += 1


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
    child_env = os.environ.copy()
    child_env["PYTHONPATH"] = os.pathsep.join(
        part for part in (PY_ROOT, child_env.get("PYTHONPATH", "")) if part
    )

    for name in names:
        script = os.path.join(TOOLS, name)
        try:
            r = subprocess.run([PY, script], cwd=PY_ROOT, capture_output=True,
                               text=True, timeout=args.timeout, env=child_env)
            if r.returncode == 0:
                passed.append(name)
                print(f"  ok   {name}")
            else:
                failed.append((name, r.returncode, _process_detail(r.stdout or "", r.stderr or "")))
                print(f"  ✗ failure #{len(failed) + len(errored)} recorded; {_RECORDED_FAILURE_NOTE}")
        except subprocess.TimeoutExpired:
            errored.append((name, f"TIMEOUT>{args.timeout}s"))
            print(f"  ✗ failure #{len(failed) + len(errored)} recorded; {_RECORDED_FAILURE_NOTE}")
        except Exception as exc:  # noqa: BLE001
            errored.append((name, repr(exc)))
            print(f"  ✗ failure #{len(failed) + len(errored)} recorded; {_RECORDED_FAILURE_NOTE}")

    total = len(names)
    print(f"\n===== {len(passed)}/{total} passed =====")
    if not failed and not errored:
        print("ALL GREEN")
    else:
        _print_final_failed_check_points(failed, errored)
    return 1 if (failed or errored) else 0


if __name__ == "__main__":
    raise SystemExit(main())
