"""值搜索 / 多帧收敛.

核心思想:
    第 1 帧: 全内存搜索目标值 → 拿到 N 个候选地址 (通常成百上千)
    第 2 帧: 目标值变了之后, 在候选集中再搜新值 → 收敛到 < 10 个
    第 3 帧: 再变一次 → 通常剩 1 个

公开 API:
    encode_value(value, dtype) -> bytes    # 拼出 little-endian 字节串
    scan(pm, value, dtype) -> list[int]    # 全内存首次扫描
    narrow(pm, addrs, value, dtype) -> list[int]  # 在候选集中再扫

加速:
    i32/u32/i64/u64 的 scan 内层走 cy_memscan (AVX2: ~16 GB/s).
    f32/f64/utf16 仍走 bytes.find 标量路径 (调用频次极低)。
"""

from __future__ import annotations

import os
import struct
from concurrent.futures import ThreadPoolExecutor
from typing import Iterable, List, Sequence

from mem_probe import cy_memscan as _cy
from .process import StarProcess

_SCAN_WORKERS = max(2, min(os.cpu_count() or 4, 8) - 1)

# ───────────────────────── 编码 ─────────────────────────
# dtype 字符串 → struct 格式; 'utf16' 单独处理
_STRUCT_FMT = {
    "i32": "<i",
    "u32": "<I",
    "i64": "<q",
    "u64": "<Q",
    "f32": "<f",
    "f64": "<d",
}

VALID_DTYPES = tuple(_STRUCT_FMT.keys()) + ("utf16",)


def encode_value(value, dtype: str) -> bytes:
    """把一个 Python 值编码成内存里的 little-endian 字节串.

    对 i32/i64, 若 value 超出 signed 范围但落在 unsigned 范围内 (常见于
    游戏 UID 用 uint64 表示但被解析成 Python int), 自动按 unsigned 编码,
    bytes 表示一致。
    """
    if dtype == "utf16":
        if not isinstance(value, str):
            raise TypeError(f"utf16 dtype requires str, got {type(value).__name__}")
        # 不带 NUL 终结符: 子串匹配更宽容 (允许命中后接其它字符)
        return value.encode("utf-16-le")
    fmt = _STRUCT_FMT.get(dtype)
    if fmt is None:
        raise ValueError(f"unknown dtype {dtype!r}; valid: {VALID_DTYPES}")
    # 整型自适应 signed/unsigned
    if dtype in ("i32", "u32") and isinstance(value, int):
        v = value & 0xFFFFFFFF
        return v.to_bytes(4, "little", signed=False)
    if dtype in ("i64", "u64") and isinstance(value, int):
        v = value & 0xFFFFFFFFFFFFFFFF
        return v.to_bytes(8, "little", signed=False)
    try:
        return struct.pack(fmt, value)
    except struct.error as e:
        raise ValueError(f"value {value!r} not representable as {dtype}: {e}") from e


# ───────────────────────── 扫描 ─────────────────────────
def _find_all_in_chunk(buf: bytes, needle: bytes, *, align: int = 1) -> Iterable[int]:
    """在一段缓冲里找 needle 的所有偏移."""
    if not needle:
        return
    start = 0
    n = len(needle)
    while True:
        i = buf.find(needle, start)
        if i < 0:
            return
        if align == 1 or (i % align) == 0:
            yield i
        start = i + 1


def scan(
    pm: StarProcess,
    value,
    dtype: str,
    *,
    align: int = 0,
    max_hits: int = 200_000,
    max_region_size: int = 256 * 1024 * 1024,
) -> List[int]:
    """全内存首次扫描.

    返回所有匹配地址的列表。

    Parameters
    ----------
    align: 对齐字节. 0 表示 dtype 默认对齐 (i32/f32=4, i64/f64=8, u32=4 等),
           1 表示不对齐, 任意字节边界。
    max_hits: 命中数上限, 超过即停止扫描 (避免内存爆炸; 通常 utf16 短串才会触发)。
    max_region_size: 跳过过大的区域 (默认 256 MiB), 主要是 GameAssembly.dll
           的 IL2CPP 元数据段, 静态数据扫描没意义又拖慢速度。
    """
    needle = encode_value(value, dtype)
    if align == 0:
        align = _default_align(dtype)

    # ── 加速路径: 整型 + 默认对齐 走 cy_memscan AVX2 ──
    if isinstance(value, int) and align == _default_align(dtype):
        if dtype in ("i64", "u64"):
            return _scan_aligned_int(pm, value, max_hits, max_region_size, width=8)
        if dtype in ("i32", "u32"):
            return _scan_aligned_int(pm, value, max_hits, max_region_size, width=4)

    # ── 慢路径 (f32/f64/utf16/不对齐): bytes.find ──
    hits: List[int] = []
    for region in pm.iter_regions():
        if region.size > max_region_size:
            continue
        buf = pm.read_bytes(region.base, region.size)
        if buf is None:
            continue
        for off in _find_all_in_chunk(buf, needle, align=align):
            hits.append(region.base + off)
            if len(hits) >= max_hits:
                return hits
    return hits


def _scan_one_region(pm, region_base, region_size, v, find_fn, chunk, max_per):
    """Worker: scan one region, return list of absolute addresses."""
    hits: List[int] = []
    read_into = getattr(pm, "read_bytes_into", None)
    scratch = bytearray(chunk) if read_into is not None else None
    off = 0
    while off < region_size:
        n = min(chunk, region_size - off)
        if read_into is not None:
            got = read_into(region_base + off, scratch, n)
            if got <= 0:
                break
            buf = memoryview(scratch)[:got]
        else:
            blob = pm.read_bytes(region_base + off, n)
            if blob is None:
                break
            buf = blob
        remaining = max_per - len(hits)
        if remaining <= 0:
            break
        for o in find_fn(buf, v, max_hits=remaining):
            hits.append(region_base + off + o)
        if len(hits) >= max_per:
            break
        off += n
    return hits


def _scan_aligned_int(
    pm: StarProcess,
    value: int,
    max_hits: int,
    max_region_size: int,
    *,
    width: int,
) -> List[int]:
    """对齐 i32/u32/i64/u64 的 cy_memscan 加速路径.

    分块 + read_bytes_into 复用 scratch 零拷贝喂内核 (取代每 region 整块 read_bytes
    的两次拷贝), 与 fingerprint_v2.locate_v2 同款姿势。
    多 region 时并行扫描 + PrefetchVirtualMemory 预取。
    """
    if width == 8:
        v = value & 0xFFFFFFFFFFFFFFFF
        find_fn = _cy.find_aligned_u64
    else:
        v = value & 0xFFFFFFFF
        find_fn = _cy.find_aligned_u32
    chunk = 16 * 1024 * 1024
    # region cache + prefetch
    if hasattr(pm, "cached_regions"):
        regions = [r for r in pm.cached_regions() if r.size <= max_region_size]
    else:
        regions = [r for r in pm.iter_regions() if r.size <= max_region_size]
    if not regions:
        return []
    if hasattr(pm, "prefetch_regions"):
        pm.prefetch_regions(regions)
    # parallel scan when enough regions
    if len(regions) >= 4 and _SCAN_WORKERS >= 2:
        all_hits: List[int] = []
        max_per = max(max_hits // max(len(regions), 1) + 1, 1000)
        with ThreadPoolExecutor(max_workers=_SCAN_WORKERS) as pool:
            futures = [
                pool.submit(_scan_one_region, pm, r.base, r.size,
                            v, find_fn, chunk, max_per)
                for r in regions
            ]
            for f in futures:
                try:
                    all_hits.extend(f.result(timeout=60))
                    if len(all_hits) >= max_hits:
                        break
                except Exception:
                    continue
        return all_hits[:max_hits]
    # sequential fallback
    hits: List[int] = []
    for region in regions:
        region_hits = _scan_one_region(pm, region.base, region.size,
                                       v, find_fn, chunk, max_hits - len(hits))
        hits.extend(region_hits)
        if len(hits) >= max_hits:
            return hits[:max_hits]
    return hits


def narrow(
    pm: StarProcess,
    addrs: Sequence[int],
    value,
    dtype: str,
) -> List[int]:
    """在已有候选集中再搜目标值, 返回仍然匹配的子集.

    第 1 帧 scan 后候选集可达 max_hits (~2e5-3e5) 个地址; 逐个 read_bytes 会发
    同样多次跨进程 RPC。对定宽整型/浮点 (4/8 字节, 按原始位比较), 改为一次
    read_words_many 批量 nogil 读 + 进程内比较 (N 次 RPC → 1 次)。utf16/变长仍走
    逐地址回退。
    """
    needle = encode_value(value, dtype)
    n = len(needle)
    addrs = list(addrs)
    if not addrs:
        return []
    # 快路径: 定宽 4/8 字节 word 一次批量读, 进程内按位比较 (浮点也按原始位)
    if n in (4, 8) and dtype in ("i32", "u32", "i64", "u64", "f32", "f64"):
        target = int.from_bytes(needle, "little", signed=False)
        try:
            words = pm._read_words_many(addrs, n)
        except Exception:
            words = None
        if words is not None and len(words) == len(addrs):
            return [a for a, w in zip(addrs, words) if w is not None and w == target]
    # 回退 / 变长 (utf16): 逐地址读 + 字节比较
    out: List[int] = []
    for addr in addrs:
        b = pm.read_bytes(addr, n)
        if b is None:
            continue
        if b == needle:
            out.append(addr)
    return out


def _default_align(dtype: str) -> int:
    if dtype in ("i32", "u32", "f32"):
        return 4
    if dtype in ("i64", "u64", "f64"):
        return 8
    if dtype == "utf16":
        return 2
    return 1
