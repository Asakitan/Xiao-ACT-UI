"""从运行中的 Star.exe 抓取解密后的 IL2CPP global metadata.

腾讯 ACE 把磁盘上的 global-metadata.dat 掏空 (0 字节), 真实 metadata 在游戏启动时
解密到内存中。本工具:
  1. 扫描 Star.exe 全部可读 PRIVATE 区域, 找魔数 0xFAB11BAF (Il2CppGlobalMetadataHeader.sanity)
  2. 对每个候选解析 header, 确定 metadata 版本与总长度
  3. 校验所有 (offset, size) 对都在合理范围内, 选最大的有效候选
  4. dump 到磁盘 (默认: tools/mem_probe/il2cpp/out/<game_sha8>/global-metadata.dat)

CLI:
    python -m tools.mem_probe.il2cpp.mem_dump_metadata
    python -m tools.mem_probe.il2cpp.mem_dump_metadata --out custom/path.dat
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
import time
from typing import List, Optional, Tuple

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ..process import StarProcess, StarProcessError
from mem_probe import cy_memscan as _cy

MAGIC = 0xFAB11BAF
MAGIC_BYTES = MAGIC.to_bytes(4, "little")

# Il2CppGlobalMetadataHeader 结构 (Unity 2018+, version 16~31):
# 共 ~120 字节, 由若干 (offset:i32, size:i32) 对组成.
# 前 8 字节固定: sanity (u32), version (i32)
# 后续 28~30 个 (offset, size) 对, 字段名随 version 略有不同, 但布局兼容.

# 已知合理范围
_VALID_VERSIONS = set(range(16, 35))  # Unity 2017 ~ Unity 2023
_MAX_METADATA_SIZE = 256 * 1024 * 1024  # 256MB 上限
_MIN_METADATA_SIZE = 256 * 1024         # 256KB 下限
_MAX_REGION_SIZE = 512 * 1024 * 1024


def _parse_header(blob: bytes, off: int) -> Optional[Tuple[int, int]]:
    """解析 metadata header, 返回 (version, total_size). 失败返 None."""
    if off + 8 > len(blob):
        return None
    sanity, version = struct.unpack_from("<Ii", blob, off)
    if sanity != MAGIC or version not in _VALID_VERSIONS:
        return None

    # header 长度因 version 不同, 但所有字段都是 (i32 offset, i32 size) 对.
    # 我们扫描前 256 字节内所有形如 (offset, size) 对, 取最大 offset+size 作为总长.
    header_max = 256
    avail = min(header_max, len(blob) - off)
    pairs = (avail - 8) // 8  # header 后续多少对
    if pairs < 8:
        return None

    max_end = 0
    for i in range(pairs):
        p_off, p_size = struct.unpack_from("<II", blob, off + 8 + i * 8)
        # 必须满足: offset >= header_size, size 合理, offset+size 不溢出
        if p_off == 0 or p_size == 0:
            continue
        if p_off > _MAX_METADATA_SIZE or p_size > _MAX_METADATA_SIZE:
            continue
        end = p_off + p_size
        if end > _MAX_METADATA_SIZE:
            continue
        if end > max_end:
            max_end = end

    if max_end < _MIN_METADATA_SIZE:
        return None
    # 8 字节对齐
    total = (max_end + 7) & ~7
    return version, total


def find_metadata_in_process(pm: StarProcess, *, verbose: bool = True) -> List[Tuple[int, int, int]]:
    """扫整个进程, 返回 [(addr, version, total_size), ...] 按 size 降序."""
    candidates: List[Tuple[int, int, int]] = []
    region_count = 0
    scanned = 0
    t0 = time.time()
    for region in pm.iter_regions(only_readable=True, only_private=True):
        if region.size > _MAX_REGION_SIZE:
            continue
        region_count += 1
        # 大块分片
        chunk_size = 32 * 1024 * 1024
        offset = 0
        while offset < region.size:
            n = min(chunk_size, region.size - offset)
            blob = pm.read_bytes(region.base + offset, n)
            if blob is None:
                break
            scanned += len(blob)
            # 4 字节对齐扫 magic (cy_memscan AVX2: ~14 GB/s)
            for i in _cy.find_aligned_u32(blob, MAGIC):
                parsed = _parse_header(blob, i)
                if parsed is not None:
                    version, total = parsed
                    addr = region.base + offset + i
                    candidates.append((addr, version, total))
                    if verbose:
                        print(f"   候选: 0x{addr:X}  version={version}  size={total/1024/1024:.2f}MB")
            # chunk 重叠 (header < 256B)
            if offset + n < region.size:
                offset += n - 256
            else:
                offset += n
    if verbose:
        print(f"[scan] regions={region_count} scanned≈{scanned/1e9:.2f}GB ({time.time()-t0:.1f}s) -> {len(candidates)} 候选")
    candidates.sort(key=lambda c: c[2], reverse=True)
    return candidates


def dump_metadata(pm: StarProcess, addr: int, total: int, out_path: str) -> bool:
    """从 addr 读 total 字节, 写到 out_path. 分块读以容忍部分页失败."""
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    chunk = 4 * 1024 * 1024
    with open(out_path, "wb") as f:
        written = 0
        while written < total:
            n = min(chunk, total - written)
            blob = pm.read_bytes(addr + written, n)
            if blob is None:
                # 尝试更小粒度
                small = 4096
                got = bytearray()
                for s in range(0, n, small):
                    sb = pm.read_bytes(addr + written + s, min(small, n - s))
                    if sb is None:
                        sb = b"\x00" * min(small, n - s)
                    got.extend(sb)
                blob = bytes(got)
            f.write(blob)
            written += n
    return True


def _game_assembly_sha8(game_dir: str) -> str:
    """取 GameAssembly.dll 前 64MB 的 sha256 前 8 字符 (避免 213MB 全文件 hash)."""
    p = os.path.join(game_dir, "GameAssembly.dll")
    if not os.path.isfile(p):
        return "unknown"
    h = hashlib.sha256()
    with open(p, "rb") as f:
        h.update(f.read(64 * 1024 * 1024))
    return h.hexdigest()[:8]


def _default_out_dir(game_dir: str) -> str:
    sha8 = _game_assembly_sha8(game_dir)
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(here, "out", sha8)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Dump decrypted IL2CPP global-metadata from Star.exe")
    ap.add_argument("--game-dir", default=r"E:\星痕共鸣(2001991)", help="游戏安装目录 (含 GameAssembly.dll)")
    ap.add_argument("--out", default=None, help="输出 metadata.dat 路径 (默认: il2cpp/out/<sha8>/global-metadata.dat)")
    ap.add_argument("--no-dump", action="store_true", help="只扫描不 dump")
    args = ap.parse_args(argv)

    try:
        pm = StarProcess()
    except StarProcessError as e:
        print(f"[fail] attach Star.exe 失败: {e}", file=sys.stderr)
        return 2

    try:
        print(f"[info] PID={pm.pid}  Game={args.game_dir}")
        candidates = find_metadata_in_process(pm)
        if not candidates:
            print("[fail] 未找到 metadata 魔数; 游戏可能未完全加载, 或 ACE 进一步加密", file=sys.stderr)
            return 3

        addr, version, total = candidates[0]
        print(f"[ok] 选用: addr=0x{addr:X}  version={version}  size={total/1024/1024:.2f}MB")
        if args.no_dump:
            return 0

        out_path = args.out or os.path.join(_default_out_dir(args.game_dir), "global-metadata.dat")
        print(f"[dump] -> {out_path}")
        dump_metadata(pm, addr, total, out_path)
        actual = os.path.getsize(out_path)
        print(f"[ok] wrote {actual/1024/1024:.2f}MB")

        # 校验落地文件 magic
        with open(out_path, "rb") as f:
            head = f.read(8)
        s, v = struct.unpack("<Ii", head)
        if s != MAGIC or v != version:
            print(f"[warn] 落地文件 header 校验失败: sanity=0x{s:X} version={v}", file=sys.stderr)
            return 4
        print(f"[ok] header 校验通过, version={v}")
        return 0
    finally:
        pm.close()


if __name__ == "__main__":
    sys.exit(main())

