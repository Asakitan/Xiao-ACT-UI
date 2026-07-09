# mem_probe 命令行入口.
#
# 用法 (在 sao_auto/ 目录, 管理员 PowerShell):
# python -m tools.mem_probe.cli attach
# python -m tools.mem_probe.cli regions [--limit N]
# python -m tools.mem_probe.cli dump <addr_hex> <length>
#
# 阶段 1 验证目标:
# 1. attach 子命令能在游戏运行时输出 Star.exe 基址, 无 access denied;
# 2. 游戏未运行时友好报错;
# 3. 运行时环境限制 attach → 失败时打印明确提示, 用户据此中止整个方案。

from __future__ import annotations

import argparse
import sys

from .process import StarProcess, StarProcessError, is_admin, find_pid_by_name


def _hexdump(data: bytes, base: int, width: int = 16) -> str:
    out = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        out.append(f"  {base + i:016X}  {hex_part:<{width*3}}  {ascii_part}")
    return "\n".join(out)


def cmd_attach(args: argparse.Namespace) -> int:
    if not is_admin():
        print("[warn] 当前未以管理员身份运行, 大概率 OpenProcess 会失败。", file=sys.stderr)
    try:
        with StarProcess() as sp:
            print(f"[ok] attached: pid={sp.pid} name={sp.name} handle=0x{sp.handle:X}")
            mods = sp.list_modules()
            print(f"[ok] modules: {len(mods)} total")
            main = sp.main_module()
            print(f"[ok] main module: {main}")
            # 打印前若干个体积最大的模块, 便于辨认游戏引擎 dll
            if args.list_modules:
                top = sorted(mods, key=lambda m: -m.size)[: args.list_modules]
                print(f"[ok] top {len(top)} modules by size:")
                for m in top:
                    print(f"  {m}")
        return 0
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2


def cmd_regions(args: argparse.Namespace) -> int:
    try:
        with StarProcess() as sp:
            count = 0
            total = 0
            shown = 0
            for r in sp.iter_regions():
                count += 1
                total += r.size
                if args.limit and shown < args.limit:
                    print(
                        f"  base=0x{r.base:016X} size=0x{r.size:X} "
                        f"protect=0x{r.protect:02X} type=0x{r.type_:X}"
                    )
                    shown += 1
            print(
                f"[ok] private commit readable regions: {count}, "
                f"total={total/1024/1024:.1f} MiB"
            )
        return 0
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2


def cmd_dump(args: argparse.Namespace) -> int:
    try:
        addr = int(args.addr, 16) if args.addr.lower().startswith("0x") else int(args.addr, 16)
    except ValueError:
        print(f"[fail] addr 必须是十六进制 (如 0x7FF6_1234_5678): {args.addr}", file=sys.stderr)
        return 1
    length = max(1, min(int(args.length), 4096))
    try:
        with StarProcess() as sp:
            data = sp.read_bytes(addr, length)
            if data is None:
                print(f"[fail] read_bytes 失败 addr=0x{addr:X} len={length}", file=sys.stderr)
                return 3
            print(_hexdump(data, addr))
        return 0
    except StarProcessError as e:
        print(f"[fail] {e}", file=sys.stderr)
        return 2


def cmd_pid(args: argparse.Namespace) -> int:
    pid = find_pid_by_name(args.name)
    if pid is None:
        print(f"[fail] {args.name} 未运行", file=sys.stderr)
        return 4
    print(f"[ok] {args.name} pid={pid}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="python -m tools.mem_probe.cli")
    sub = p.add_subparsers(dest="cmd", required=True)

    p_attach = sub.add_parser("attach", help="附加 Star.exe 并打印基础信息")
    p_attach.add_argument(
        "--list-modules",
        type=int,
        default=0,
        help="额外打印前 N 个体积最大的模块 (默认 0 不打印)",
    )
    p_attach.set_defaults(func=cmd_attach)

    p_reg = sub.add_parser("regions", help="枚举 private commit 可读区域统计")
    p_reg.add_argument("--limit", type=int, default=0, help="额外打印前 N 个区域 (默认 0)")
    p_reg.set_defaults(func=cmd_regions)

    p_dump = sub.add_parser("dump", help="按地址 hex dump 一段内存")
    p_dump.add_argument("addr", help="十六进制地址, 如 0x7FF612345678")
    p_dump.add_argument("length", help="字节数 (1..4096)")
    p_dump.set_defaults(func=cmd_dump)

    p_pid = sub.add_parser("pid", help="只查 PID, 不 OpenProcess")
    p_pid.add_argument("--name", default="Star.exe")
    p_pid.set_defaults(func=cmd_pid)

    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    return int(args.func(args) or 0)


if __name__ == "__main__":
    raise SystemExit(main())
