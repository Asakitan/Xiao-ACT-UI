# 半自动锚点定位.
#
# 工作模式 (CLI 交互):
#
# python -m tools.mem_probe.triangulate i32        # 找一个 int32 (如 HP)
# python -m tools.mem_probe.triangulate i64        # 找一个 int64 (如 UID)
# python -m tools.mem_probe.triangulate utf16      # 找一个字符串 (如 角色名)
#
# 步骤:
# 1. 在游戏内查看当前值 (如 HP=12345), 输入到脚本
# 2. 脚本全内存扫描该值, 报候选数
# 3. 在游戏内改变该值 (如挨一下 HP=12200), 输入新值
# 4. 脚本在候选集中再扫
# 5. 重复直到收敛 (通常 2~4 帧)
# 6. 输出最终地址 + RVA + 周边 hex dump
#
# 为什么不直接调 packet_bridge:
# triangulate 是离线诊断工具, 不依赖主程序运行时, 也不引入 Npcap/protobuf
# 重型依赖。一旦内存方案投入使用, reader.py 会引用 GameStateManager
# 做对比, 但定位过程本身只需要 ReadProcessMemory + 用户眼睛。

from __future__ import annotations

import argparse
import sys
import time
from typing import List

from .process import StarProcess, StarProcessError, is_admin
from .scanner import VALID_DTYPES, encode_value, narrow, scan


def _parse_value(raw: str, dtype: str):
    if dtype == "utf16":
        return raw
    if dtype in ("f32", "f64"):
        return float(raw)
    # 支持 0x 前缀
    if raw.lower().startswith("0x"):
        return int(raw, 16)
    return int(raw)


def _format_addr(pm: StarProcess, addr: int) -> str:
    try:
        for m in pm.list_modules():
            if m.base <= addr < m.base + m.size:
                rva = addr - m.base
                return f"0x{addr:016X} ({m.name}+0x{rva:X})"
    except Exception:
        pass
    return f"0x{addr:016X} (heap)"


def _hex_context(pm: StarProcess, addr: int, before: int = 32, after: int = 32) -> str:
    start = max(0, addr - before)
    n = before + after
    data = pm.read_bytes(start, n)
    if data is None:
        return "  (read failed)"
    out = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        marker = " ←" if start + i <= addr < start + i + 16 else ""
        out.append(f"  {start + i:016X}  {hex_part:<48}  {ascii_part}{marker}")
    return "\n".join(out)


def run(dtype: str, *, max_iterations: int = 8) -> int:
    if dtype not in VALID_DTYPES:
        print(f"[fail] dtype must be one of {VALID_DTYPES}", file=sys.stderr)
        return 1
    if not is_admin():
        print("[warn] 未以管理员身份运行, OpenProcess 可能失败。", file=sys.stderr)

    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2

    print(f"[ok] attached pid={pm.pid} name={pm.name}")
    print(f"[info] dtype={dtype}; 输入 'q' 中止, 'r' 重新开始, 'd' dump 当前候选")

    candidates: List[int] = []
    iteration = 0
    try:
        while iteration < max_iterations:
            iteration += 1
            prompt = (
                f"[frame {iteration}] 输入当前 {dtype} 值"
                f"{' (字符串)' if dtype == 'utf16' else ''}: "
            )
            try:
                raw = input(prompt).strip()
            except EOFError:
                print("\n[abort] EOF")
                return 130
            if not raw:
                print("[skip] 空输入")
                iteration -= 1
                continue
            if raw.lower() == "q":
                print("[abort] user quit")
                return 130
            if raw.lower() == "r":
                candidates = []
                iteration = 0
                print("[reset] 候选集已清空")
                continue
            if raw.lower() == "d":
                iteration -= 1
                if not candidates:
                    print("[info] 候选集为空")
                else:
                    print(f"[info] 当前 {len(candidates)} 个候选:")
                    for a in candidates[:20]:
                        print(f"   {_format_addr(pm, a)}")
                    if len(candidates) > 20:
                        print(f"   ... ({len(candidates) - 20} more)")
                continue

            try:
                value = _parse_value(raw, dtype)
            except ValueError as e:
                print(f"[err] 解析失败: {e}")
                iteration -= 1
                continue

            t0 = time.time()
            if not candidates:
                # 第 1 帧: 全内存扫描
                # utf16 字符串扫描: 默认 align=2; i64 默认 8。
                candidates = scan(pm, value, dtype)
            else:
                # 后续帧: 仅缩小
                candidates = narrow(pm, candidates, value, dtype)
            dt = time.time() - t0

            n = len(candidates)
            print(f"[scan] {n} 个候选 (耗时 {dt:.2f}s)")

            if n == 0:
                print("[fail] 候选集已空 → 该值可能未直接以原始形式存内存,"
                      " 或扫描错过。建议 'r' 重置, 换一个明显变化的值再试。")
                continue
            if n == 1:
                addr = candidates[0]
                print(f"\n[converged] 唯一地址: {_format_addr(pm, addr)}\n")
                print("周边 64 字节:")
                print(_hex_context(pm, addr))
                return 0
            if n <= 8:
                print(f"[hint] 候选已很少, 再变 1~2 次值即可收敛:")
                for a in candidates:
                    print(f"   {_format_addr(pm, a)}")

        print(f"[stop] 达到 {max_iterations} 帧上限, 仍剩 {len(candidates)} 个候选")
        for a in candidates[:20]:
            print(f"   {_format_addr(pm, a)}")
        return 4
    finally:
        pm.close()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.triangulate")
    p.add_argument("dtype", choices=VALID_DTYPES, help="目标值类型")
    p.add_argument("--max-iter", type=int, default=8, help="最多扫描帧数 (默认 8)")
    args = p.parse_args(argv)
    return run(args.dtype, max_iterations=args.max_iter)


if __name__ == "__main__":
    raise SystemExit(main())
