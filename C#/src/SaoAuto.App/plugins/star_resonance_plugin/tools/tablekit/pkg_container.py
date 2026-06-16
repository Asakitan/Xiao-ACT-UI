"""pkg_container - 解析星痕共鸣 container/*.pkg, 列出内含的 Lua 脚本清单.

m0.pkg / m1.pkg 内部 = 一串定制 Lua 5.3 字节码 chunk (format=0x01, size_t=4).
每个 chunk 头后跟源码名字符串, 形如:
  @D:/panda/panda-client/../panda-ab/Standalone/container/lua/zcontainer/<name>.lua

本工具只读, mmap + 正则扫描 (正则引擎在 C, 不在 Python 内层循环), 一次性离线用.

用法:
    python -m tools.tablekit.pkg_container --pkg "...\\container\\m0.pkg"
    python -m tools.tablekit.pkg_container --pkg "...\\m0.pkg" --filter table dungeon monster skill buff
"""
from __future__ import annotations

import argparse
import mmap
import os
import re

LUA53_SIG = re.compile(rb"\x1bLua\x53")          # chunk 起点
# 源码名: @ 开头, 到 .lua 结束 (路径里只含可打印 ASCII)
SRC_NAME = re.compile(rb"@[\x20-\x7e]{1,300}?\.lua")


def iter_lua_chunks(path: str):
    """yield (chunk_offset, source_name_str). 用 mmap 流式扫, 不全读进 RAM."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        try:
            for m in LUA53_SIG.finditer(mm):
                off = m.start()
                # 源码名就在 header(~0x21B) 之后, 取一个 512B 窗口找 @.../*.lua
                window = mm[off: off + 512]
                nm = SRC_NAME.search(window)
                name = nm.group().decode("latin1") if nm else "<no-name>"
                yield off, name
        finally:
            mm.close()


_STR_RE = re.compile(rb"[\x20-\x7e]{4,}")


def dump_chunk_at(path: str, at: int, max_window: int = 8 * 1024 * 1024):
    """从 at(chunk 起点)读到下一个 LUA53_SIG, 打印结构 + ASCII 字符串."""
    with open(path, "rb") as f:
        f.seek(at)
        blob = f.read(max_window)
    # chunk 结束 = 下一个 \x1bLuaS (跳过自身起点)
    nxt = LUA53_SIG.search(blob, 4)
    end = nxt.start() if nxt else len(blob)
    chunk = blob[:end]
    print(f"chunk @0x{at:X}  size={len(chunk):,}B")
    markers = {}
    for kw in (b"blob_reader", b".bytes", b"__data__", b"ReadInt32", b"ReadInt64",
               b"ReadString", b"ReadFloat", b"ReadBool", b"require", b"setmetatable"):
        c = chunk.count(kw)
        if c:
            markers[kw.decode()] = c
    print(f"markers: {markers}")
    strs = [m.group().decode("latin1") for m in _STR_RE.finditer(chunk)]
    # 去重保序
    seen = set(); uniq = []
    for s in strs:
        if s not in seen:
            seen.add(s); uniq.append(s)
    print(f"ASCII strings ({len(uniq)} unique, first 80):")
    for s in uniq[:80]:
        print(f"  {s}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", required=True)
    ap.add_argument("--filter", nargs="*", default=None,
                    help="只显示名字含这些关键字(小写匹配)的 chunk")
    ap.add_argument("--limit", type=int, default=0, help="最多打印 N 条 (0=全部)")
    ap.add_argument("--at", default=None, help="dump 指定 chunk 起点(hex, 如 0x19A389E8)的字符串/结构")
    args = ap.parse_args()

    if args.at is not None:
        dump_chunk_at(args.pkg, int(args.at, 16))
        return

    print(f"=== {os.path.basename(args.pkg)}  ({os.path.getsize(args.pkg):,} B) ===")
    total = 0
    shown = 0
    name_counter = {}
    filt = [s.lower() for s in (args.filter or [])]
    hits = []
    for off, name in iter_lua_chunks(args.pkg):
        total += 1
        # 取 lua 文件 basename (去掉 @.../ 前缀)
        base = name.rsplit("/", 1)[-1] if "/" in name else name
        name_counter[base] = name_counter.get(base, 0) + 1
        if filt:
            low = name.lower()
            if not any(k in low for k in filt):
                continue
        hits.append((off, base, name))

    print(f"Lua chunks total: {total}")
    if filt:
        print(f"matched filter {filt}: {len(hits)}")
        for off, base, name in hits[: (args.limit or len(hits))]:
            print(f"  0x{off:09X}  {base:40s}  {name}")
    else:
        # 列出去重后的脚本名 (按出现次数)
        uniq = sorted(name_counter.items(), key=lambda kv: (-kv[1], kv[0]))
        print(f"unique script basenames: {len(uniq)}")
        for base, cnt in uniq[: (args.limit or len(uniq))]:
            print(f"  x{cnt:<4} {base}")


if __name__ == "__main__":
    main()
