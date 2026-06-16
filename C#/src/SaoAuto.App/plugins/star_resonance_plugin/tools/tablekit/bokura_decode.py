"""bokura_decode - 星痕共鸣 m0.pkg "Bokura" 配置表字符串池解码器 (离线).

================================================================================
重要结论 (经穷尽逆向后实证, 见 tools/tablekit/README_bokura.md):
--------------------------------------------------------------------------------
m0.pkg 不是一个能"离线完整解码 id->中文名"的容器。实测如下:

  1. 文件 = 4713 个 Lua 5.3 字节码 chunk + 它们之间的"数据缝隙"(共 ~1.18GB)。
     缝隙才是 Bokura 表数据区 (Lua chunk 本身只占几十 MB)。
  2. 中文名以 <1字节长度><UTF-8> 紧密拼接, 堆在一个主缝隙 gap@0x124DE0AA
     (8.6MB) 里, 顺序 ~= 各表的 id 升序 (实测怪物锚点 97.8% 单调递增)。
  3. 但是: 池里【没有】与名字行序对应的 id 列, 【没有】绝对/相对偏移指针
     (833 个怪物名的绝对偏移在全文件 0 次作为 u32 出现)。
     => 每个名字 token 的"确切 id"无法离线唯一确定 (id 有跳号且未知)。
  4. 池只装了部分名字: 怪物名 94% 在池里, 但技能名仅 20% 在池里。

因此本模块能做的是【锚点对齐】: 用已知正确的 crib (id->name) 当锚点, 借助
"池按 id 升序"的性质, 把池 token 对齐回 id, 产出一个【高置信子集】(交叉验证
100% 一致)。它【不能】产出比 crib 更完整的表 —— 完整表需读运行时 ZTable
(IL2CPP, 见报告)。

本模块仍是可复用的: 它给出 gap 地图、字符串池 token 化、crib 锚点对齐三件套,
可用于任何"有 crib + 名字在池中按 id 升序"的表。
================================================================================
"""
from __future__ import annotations

import bisect
import json
import mmap
import os
import re
import struct
from typing import Dict, List, Tuple

PKG_DEFAULT = r"E:\星痕共鸣(2001991)\Star_Data\StreamingAssets\container\m0.pkg"
_LUA_SIG = re.compile(rb"\x1bLua\x53")


# ---------------------------------------------------------------------------
# 1) Lua 5.3 chunk 精确长度解析 -> 数据缝隙(gap)地图
# ---------------------------------------------------------------------------
class _R:
    __slots__ = ("mm", "p")

    def __init__(self, mm, off):
        self.mm = mm
        self.p = off

    def u8(self):
        v = self.mm[self.p]
        self.p += 1
        return v

    def u32(self):
        v = struct.unpack_from("<I", self.mm, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from("<q", self.mm, self.p)[0]
        self.p += 8
        return v


def _rd_string(r: _R):
    n = r.u8()
    if n == 0:
        return
    if n == 0xFF:
        n = r.u64()
    r.p += n - 1


def _parse_function(r: _R):
    """解析一个 Lua 5.3 Proto (header 已知: int=4,size_t=4,inst=4,luaint=8,luanum=8)."""
    _rd_string(r)               # source
    r.u32(); r.u32()            # linedefined, lastlinedefined
    r.u8(); r.u8(); r.u8()      # numparams, is_vararg, maxstacksize
    nc = r.u32(); r.p += 4 * nc           # code
    nk = r.u32()                          # constants
    for _ in range(nk):
        t = r.u8()
        if t == 0:        # nil
            pass
        elif t == 1:      # bool
            r.u8()
        elif t in (3, 0x13):   # float / int (both 8 bytes here)
            r.p += 8
        elif t in (4, 0x14):   # short / long string
            _rd_string(r)
        else:
            raise ValueError("bad const tag 0x%X @0x%X" % (t, r.p))
    nu = r.u32(); r.p += 2 * nu            # upvalues
    npr = r.u32()                          # protos
    for _ in range(npr):
        _parse_function(r)
    nli = r.u32(); r.p += 4 * nli          # lineinfo
    nlv = r.u32()                          # locvars
    for _ in range(nlv):
        _rd_string(r); r.u32(); r.u32()
    nun = r.u32()                          # upvalue names
    for _ in range(nun):
        _rd_string(r)


def _chunk_end(mm, off: int) -> int:
    r = _R(mm, off)
    r.p = off + 0x21          # 固定 header 长度
    r.u8()                    # sizeupvalues
    _parse_function(r)
    return r.p


def build_gap_map(mm) -> List[Tuple[int, int]]:
    """返回 [(gap_start, gap_end), ...] —— Lua chunk 之间的纯数据缝隙."""
    size = mm.size()
    offs = [m.start() for m in _LUA_SIG.finditer(mm)]
    ends = []
    for o in offs:
        try:
            ends.append((o, _chunk_end(mm, o)))
        except Exception:
            ends.append((o, o + 0x21))
    ends.sort()
    gaps = []
    for i, (o, e) in enumerate(ends):
        nxt = ends[i + 1][0] if i + 1 < len(ends) else size
        if nxt - e > 16:
            gaps.append((e, nxt))
    return gaps


# ---------------------------------------------------------------------------
# 2) 字符串池 token 化: <1字节长度><UTF-8> 紧密序列
# ---------------------------------------------------------------------------
def tokenize_gap(mm, gs: int, ge: int, only_cjk: bool = False) -> List[Tuple[int, str]]:
    """把一个 gap 解析成 [(file_offset, text), ...] 的 <len><utf8> token 流."""
    data = mm[gs:ge]
    out = []
    i = 0
    N = len(data)
    while i < N:
        ln = data[i]
        if ln == 0 or ln > 0x40:
            i += 1
            continue
        if i + 1 + ln > N:
            break
        seg = data[i + 1:i + 1 + ln]
        if only_cjk and (seg[0] < 0x80):
            i += 1
            continue
        try:
            t = seg.decode("utf-8")
        except UnicodeDecodeError:
            i += 1
            continue
        if all(ord(c) >= 0x20 for c in t) and t:
            out.append((gs + i, t))
            i += 1 + ln
        else:
            i += 1
    return out


def main_pool_gap(gaps: List[Tuple[int, int]]) -> Tuple[int, int]:
    """主名字池缝隙: 经实测固定为 gap@0x124DE0AA..0x12D41D0D。
    若结构变化, 回退为含该地址的 gap, 否则取最大 gap。"""
    target = 0x124DE0AA
    for gs, ge in gaps:
        if gs <= target < ge:
            return (gs, ge)
    return max(gaps, key=lambda g: g[1] - g[0])


# ---------------------------------------------------------------------------
# 3) crib 锚点对齐: 池 token 按 id 升序 <-> crib(id->name) 升序
# ---------------------------------------------------------------------------
def _lis_indices(vals: List[int]) -> List[int]:
    """最长严格递增子序列, 返回选中的下标 (用于剔除非单调离群锚点)."""
    tails: List[int] = []
    tails_idx: List[int] = []
    prev = [-1] * len(vals)
    for i, x in enumerate(vals):
        j = bisect.bisect_left(tails, x)
        if j == len(tails):
            tails.append(x)
            tails_idx.append(i)
        else:
            tails[j] = x
            tails_idx[j] = i
        prev[i] = tails_idx[j - 1] if j > 0 else -1
    if not tails_idx:
        return []
    out = []
    k = tails_idx[-1]
    while k != -1:
        out.append(k)
        k = prev[k]
    out.reverse()
    return out


def align_table(tokens: List[Tuple[int, str]],
                crib: Dict[int, str]) -> Tuple[Dict[int, str], dict]:
    """用 crib 锚点把池 tokens 对齐到 id。

    返回 (result{id:name}, stats)。result 仅含【高置信】条目:
      - LIS 单调清洗后的唯一名锚点 (本身就是 crib 的 id, 100% 准);
      - 锚点区间内, 当 池子段名字序列 与 crib 对应 id 段名字序列【逐一相等】时,
        填入该段 (零容错, 任何不符就整段跳过)。
    这样保证产出每条都与 crib 一致 (交叉验证 100%), 不引入瞎猜。
    """
    from collections import defaultdict
    name2ids = defaultdict(list)
    for i, nm in crib.items():
        if nm:
            name2ids[nm].append(i)
    nameset = set(name2ids)
    ids_sorted = sorted(crib)
    anchors = {nm: v[0] for nm, v in name2ids.items() if len(v) == 1}

    pool = [(off, t) for off, t in tokens if t in nameset]
    ap = [(idx, anchors[t]) for idx, (off, t) in enumerate(pool) if t in anchors]
    keep = _lis_indices([a for _, a in ap])
    clean = [ap[i] for i in keep]

    result: Dict[int, str] = {}
    for p, a in clean:
        result[a] = pool[p][1]

    span_filled = 0
    for (p0, a0), (p1, a1) in zip(clean, clean[1:]):
        pslice = [pool[x][1] for x in range(p0, p1 + 1)]
        islice = [i for i in ids_sorted if a0 <= i <= a1]
        if len(pslice) == len(islice) and all(
                crib[i] == nm for i, nm in zip(islice, pslice)):
            for i, nm in zip(islice, pslice):
                if i not in result:
                    result[i] = nm
                    span_filled += 1

    inter = [i for i in result if i in crib]
    agree = sum(1 for i in inter if crib[i] == result[i])
    stats = {
        "pool_tokens_matching_crib": len(pool),
        "anchors_unique": len(ap),
        "anchors_after_lis": len(clean),
        "span_filled": span_filled,
        "result": len(result),
        "crosscheck_agree": agree,
        "crosscheck_total": len(inter),
        "crosscheck_rate": round(100 * agree / max(len(inter), 1), 2),
    }
    return result, stats


# ---------------------------------------------------------------------------
# 便捷入口
# ---------------------------------------------------------------------------
def open_pkg(path: str = PKG_DEFAULT):
    f = open(path, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    return f, mm


def load_pool_tokens(path: str = PKG_DEFAULT) -> List[Tuple[int, str]]:
    """打开 m0.pkg, 建 gap 地图, token 化主名字池, 返回 token 流."""
    f, mm = open_pkg(path)
    try:
        gaps = build_gap_map(mm)
        gs, ge = main_pool_gap(gaps)
        return tokenize_gap(mm, gs, ge)
    finally:
        mm.close()
        f.close()


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--pkg", default=PKG_DEFAULT)
    args = ap.parse_args()
    f, mm = open_pkg(args.pkg)
    try:
        gaps = build_gap_map(mm)
        gs, ge = main_pool_gap(gaps)
        print("gaps: %d, total %d bytes" % (len(gaps), sum(b - a for a, b in gaps)))
        print("main name pool gap: 0x%X..0x%X (%d KB)" % (gs, ge, (ge - gs) // 1024))
        toks = tokenize_gap(mm, gs, ge)
        print("pool tokens: %d (%d unique)" % (len(toks), len(set(t for _, t in toks))))
    finally:
        mm.close()
        f.close()
