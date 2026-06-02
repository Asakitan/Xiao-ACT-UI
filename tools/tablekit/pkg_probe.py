"""pkg_probe - 摸清星痕共鸣 Star_Data/StreamingAssets/container/*.pkg 自定义容器格式.

只读探针, 不改游戏文件. 目的:
  1. 判断 info/audio/Patch 是清单还是数据
  2. 解析 .pkg 头部结构 (magic 76 20 AF E1)
  3. 定位 Lua 5.3 字节码块 (\\x1bLua\\x53 ... \\x19\\x93)
  4. 扫 ASCII 字符串找表名 (MonsterTable / DungeonTable / SkillTable ...)

用法:
    python -m tools.tablekit.pkg_probe --dir "E:\\星痕共鸣(2001991)\\Star_Data\\StreamingAssets\\container"
"""
from __future__ import annotations

import argparse
import os
import re
import struct

LUA53_SIG = b"\x1bLua\x53"           # Lua 5.3 signature
LUAC_DATA = b"\x19\x93\r\n\x1a\n"    # Lua 5.3 LUAC_DATA
PKG_MAGIC = bytes([0x76, 0x20, 0xAF, 0xE1])

TABLE_HINTS = [b"MonsterTable", b"DungeonTable", b"SkillTable", b"BuffTable",
               b"HeroDungeon", b"NormalHeroDungeon", b"Dungeon", b"Monster",
               b".bytes", b".lua", b"Table"]

_ASCII_RE = re.compile(rb"[\x20-\x7e]{4,}")


def _hexdump(b: bytes, n: int = 64) -> str:
    out = []
    for i in range(0, min(len(b), n), 16):
        chunk = b[i:i + 16]
        hx = " ".join(f"{c:02X}" for c in chunk)
        asc = "".join(chr(c) if 0x20 <= c < 0x7f else "." for c in chunk)
        out.append(f"  {i:06X}  {hx:<48}  {asc}")
    return "\n".join(out)


def probe_header(path: str, dwords: int = 16):
    with open(path, "rb") as f:
        head = f.read(256)
    print(f"\n=== {os.path.basename(path)}  size={os.path.getsize(path):,} ===")
    print("magic:", head[:4].hex(), "(expect 76 20 af e1)" if head[:4] == PKG_MAGIC else "(UNEXPECTED)")
    print("first dwords (LE):")
    for i in range(dwords):
        off = i * 4
        if off + 4 > len(head):
            break
        v = struct.unpack_from("<I", head, off)[0]
        print(f"  +0x{off:02X}: {v:>12} (0x{v:08X})")
    print("head hexdump:")
    print(_hexdump(head, 96))
    # 找第一个 Lua 块
    idx = head.find(LUA53_SIG)
    print(f"first \\x1bLua\\x53 at: {idx} (0x{idx:X})" if idx >= 0 else "no Lua sig in first 256B")


def scan_lua_and_strings(path: str, scan_bytes: int = 8 * 1024 * 1024):
    with open(path, "rb") as f:
        blob = f.read(scan_bytes)
    # Lua 块计数
    lua_positions = []
    start = 0
    while True:
        i = blob.find(LUA53_SIG, start)
        if i < 0:
            break
        lua_positions.append(i)
        start = i + 1
        if len(lua_positions) > 50:
            break
    print(f"\n[{os.path.basename(path)}] Lua5.3 chunks in first {scan_bytes//1024//1024}MB: "
          f">={len(lua_positions)} (first few offsets: {[hex(x) for x in lua_positions[:8]]})")
    # 表名命中
    hits = {}
    for h in TABLE_HINTS:
        c = blob.count(h)
        if c:
            hits[h.decode(errors='replace')] = c
    print(f"[{os.path.basename(path)}] table-name hits (first {scan_bytes//1024//1024}MB): {hits}")
    # 头部附近的字符串 (可能是条目名/路径)
    strs = [m.group().decode('latin1') for m in _ASCII_RE.finditer(blob[:4096])]
    print(f"[{os.path.basename(path)}] ASCII strings in first 4KB ({len(strs)}): {strs[:25]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--pkgs", nargs="*", default=["m0.pkg", "m1.pkg", "m10.pkg"],
                    help="要细看的 .pkg")
    args = ap.parse_args()

    d = args.dir
    print("=== container 目录条目 ===")
    for name in os.listdir(d):
        p = os.path.join(d, name)
        if os.path.isdir(p):
            kids = os.listdir(p)
            print(f"  [DIR] {name}/  ({len(kids)} entries) e.g. {kids[:5]}")
        else:
            print(f"  [FILE] {name}  {os.path.getsize(p):,} B")

    # info / Patch 若是文件, 全 dump (可能是清单)
    for manifest in ("info", "Patch", "audio"):
        mp = os.path.join(d, manifest)
        if os.path.isfile(mp) and os.path.getsize(mp) < 4096:
            with open(mp, "rb") as f:
                b = f.read()
            print(f"\n=== {manifest} (file, {len(b)}B) ===")
            print(_hexdump(b, min(len(b), 256)))

    for pk in args.pkgs:
        p = os.path.join(d, pk)
        if os.path.isfile(p):
            probe_header(p)
            scan_lua_and_strings(p)


if __name__ == "__main__":
    main()
