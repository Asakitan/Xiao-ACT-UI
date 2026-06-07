"""一键流水线: 抓 metadata → 调 Il2CppDumper → 检查输出.

步骤:
  1. 调 mem_dump_metadata 抓 global-metadata.dat 到 out/<sha8>/
  2. 调 Il2CppDumper.exe 输入 (磁盘 GameAssembly.dll, 内存 dump 出的 metadata)
     输出 dump.cs / script.json / il2cpp.h / DummyDll/ 到同一 out/<sha8>/dumper_out/
  3. 报告各文件 size, 提示下一步跑 metadata_builder

CLI:
    python -m tools.mem_probe.il2cpp.dump_tool
    python -m tools.mem_probe.il2cpp.dump_tool --skip-mem-dump   # metadata 已存在
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from typing import List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp import mem_dump_metadata as mdm

DUMPER_EXE = os.path.join(_HERE, "bin", "Il2CppDumper.exe")
EXPECTED_OUTPUTS = ("dump.cs", "script.json", "il2cpp.h")


def _run_dumper(game_assembly: str, metadata: str, out_dir: str) -> int:
    if not os.path.isfile(DUMPER_EXE):
        print(f"[fail] Il2CppDumper.exe 不存在: {DUMPER_EXE}", file=sys.stderr)
        print("    先跑: python -m tools.mem_probe.il2cpp.setup_dumper", file=sys.stderr)
        return 10
    os.makedirs(out_dir, exist_ok=True)
    cmd = [DUMPER_EXE, game_assembly, metadata, out_dir]
    print(f"[exec] {' '.join(cmd)}")
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.stdout:
        print(proc.stdout)
    if proc.stderr:
        print(proc.stderr, file=sys.stderr)
    # Il2CppDumper finishes with "Press any key to exit..." -> Console.ReadKey(),
    # which throws (non-zero exit) under redirected stdin AFTER all artifacts are
    # already written. Treat the run as successful when the outputs exist so the
    # pipeline isn't aborted by that cosmetic crash.
    produced = (os.path.isfile(os.path.join(out_dir, "script.json"))
                and os.path.isfile(os.path.join(out_dir, "dump.cs")))
    if proc.returncode != 0 and produced:
        print("[warn] Il2CppDumper exited non-zero on the 'press any key' prompt "
              "(redirected stdin); artifacts present -> treating as success.")
        return 0
    return proc.returncode


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="One-shot IL2CPP dump pipeline")
    ap.add_argument("--game-dir", default=r"E:\星痕共鸣(2001991)")
    ap.add_argument("--skip-mem-dump", action="store_true",
                    help="跳过 mem dump (假设 metadata 已存在)")
    args = ap.parse_args(argv)

    game_assembly = os.path.join(args.game_dir, "GameAssembly.dll")
    if not os.path.isfile(game_assembly):
        print(f"[fail] 找不到 {game_assembly}", file=sys.stderr)
        return 1

    out_dir = mdm._default_out_dir(args.game_dir)
    metadata_path = os.path.join(out_dir, "global-metadata.dat")
    dumper_out = os.path.join(out_dir, "dumper_out")

    # Step 1: mem dump
    if not args.skip_mem_dump or not os.path.isfile(metadata_path):
        print("=" * 60)
        print("Step 1/2: 从 Star.exe 内存抓 global-metadata.dat")
        print("=" * 60)
        rc = mdm.main(["--game-dir", args.game_dir, "--out", metadata_path])
        if rc != 0:
            print(f"[fail] mem_dump_metadata 失败 (rc={rc})", file=sys.stderr)
            return rc
    else:
        print(f"[skip] 复用现有 {metadata_path}")

    # Step 2: Il2CppDumper
    print("=" * 60)
    print("Step 2/2: 离线调 Il2CppDumper.exe")
    print("=" * 60)
    rc = _run_dumper(game_assembly, metadata_path, dumper_out)
    if rc != 0:
        print(f"[fail] Il2CppDumper 退出码 {rc}", file=sys.stderr)
        return rc

    # 校验输出
    print("=" * 60)
    print("输出校验")
    print("=" * 60)
    missing = []
    for name in EXPECTED_OUTPUTS:
        p = os.path.join(dumper_out, name)
        if os.path.isfile(p):
            sz = os.path.getsize(p)
            print(f"  [ok] {name:<14} {sz/1024:.1f}KB")
        else:
            print(f"  [missing] {name}")
            missing.append(name)
    if missing:
        return 4
    print()
    print(f"输出目录: {dumper_out}")
    print()
    print("下一步:")
    print(f"  python -m tools.mem_probe.il2cpp.metadata_builder --dumper-out {dumper_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

