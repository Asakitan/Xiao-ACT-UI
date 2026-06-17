"""IL2CPP MetadataRegistration 解析器 (走 typeInfoTable / types[] 路径).

GameAssembly.dll 内有两个全局结构: Il2CppCodeRegistration, Il2CppMetadataRegistration.
本文件用 MetadataRegistration.types[] (即 Il2CppType** 全局表) 来枚举所有类型.

v31 layout (from Il2CppDumper Il2CppMetadataRegistration.cs):
    intptr_t genericClassesCount;            // +0x00
    void**   genericClasses;                 // +0x08
    intptr_t genericInstsCount;              // +0x10
    void**   genericInsts;                   // +0x18
    intptr_t genericMethodTableCount;        // +0x20
    void*    genericMethodTable;             // +0x28
    intptr_t typesCount;                     // +0x30   ← 关键
    Il2CppType** types;                      // +0x38   ← 关键
    intptr_t methodSpecsCount;               // +0x40
    void*    methodSpecs;                    // +0x48
    intptr_t fieldOffsetsCount;              // +0x50
    int32_t**fieldOffsets;                   // +0x58
    intptr_t typeDefinitionsSizesCount;      // +0x60
    void**   typeDefinitionsSizes;           // +0x68
    size_t   metadataUsagesCount;            // +0x70
    void***  metadataUsages;                 // +0x78

Il2CppType (16 字节):
    void* data;        // +0x00  对 TYPE_CLASS/VALUETYPE: 运行时是 Il2CppClass*
    uint32_t bits;     // +0x08  低 16 位 attrs, 16-23 type, 24-29 num_mods, 30 byref, 31 pinned

Type enum (relevant):
    0x11 VALUETYPE
    0x12 CLASS
    0x15 GENERICINST
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass
from typing import Dict, Iterator, List, Optional, Tuple

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ..process import StarProcess, ModuleInfo

# ─────────── 已知常量 ───────────

GAME_ASSEMBLY = "GameAssembly.dll"
METADATA_REGISTRATION_RVA = 0x881D7E0  # 来自 dumper output script.json
CODE_REGISTRATION_RVA = 0x76E8C40

TYPE_VALUETYPE = 0x11
TYPE_CLASS = 0x12
TYPE_GENERICINST = 0x15

# Il2CppClass key offsets (来自 dumper il2cpp.h, v31)
CLASS_NAME_OFF = 0x10
CLASS_NAMESPACE_OFF = 0x18
CLASS_PARENT_OFF = 0x58


# ─────────── MetadataRegistration ───────────


@dataclass
class MetadataRegistration:
    addr: int
    types_count: int
    types_ptr: int  # Il2CppType** array
    type_defs_sizes_count: int
    field_offsets_count: int


def _find_module(pm: StarProcess, name: str) -> ModuleInfo:
    name_lc = name.lower()
    for m in pm.list_modules():
        if m.name.lower() == name_lc:
            return m
    raise RuntimeError(f"模块未找到: {name}")


def parse_metadata_registration(pm: StarProcess) -> MetadataRegistration:
    mod = _find_module(pm, GAME_ASSEMBLY)
    addr = mod.base + METADATA_REGISTRATION_RVA
    blob = pm.read_bytes(addr, 0x80)
    if blob is None:
        raise RuntimeError(f"读 MetadataRegistration @ 0x{addr:X} 失败")
    qs = [int.from_bytes(blob[i : i + 8], "little") for i in range(0, 0x80, 8)]
    mr = MetadataRegistration(
        addr=addr,
        types_count=qs[6],
        types_ptr=qs[7],
        type_defs_sizes_count=qs[12],
        field_offsets_count=qs[10],
    )
    print(f"[MR] @ 0x{addr:X} (GameAssembly + 0x{METADATA_REGISTRATION_RVA:X})")
    print(f"     genericClassesCount   = {qs[0]}")
    print(f"     genericInstsCount     = {qs[2]}")
    print(f"     genericMethodTableCnt = {qs[4]}")
    print(f"     typesCount            = {mr.types_count}    (types @ 0x{mr.types_ptr:X})")
    print(f"     fieldOffsetsCount     = {mr.field_offsets_count}")
    print(f"     typeDefinitionsSizes  = {mr.type_defs_sizes_count}")
    return mr


# ─────────── 类型枚举 ───────────


@dataclass
class TypeEntry:
    type_idx: int
    type_addr: int      # Il2CppType*
    data_value: int     # 运行时: Il2CppClass* (对 CLASS/VALUETYPE)
    type_enum: int      # 0x12=CLASS, 0x11=VALUETYPE, ...
    name: Optional[str] = None
    namespace: Optional[str] = None
    parent: int = 0


def _read_cstring(pm: StarProcess, addr: int, max_len: int = 256) -> Optional[str]:
    if addr == 0:
        return None
    buf = pm.read_bytes(addr, max_len)
    if not buf:
        return None
    end = buf.find(b"\x00")
    if end < 0:
        end = len(buf)
    try:
        return buf[:end].decode("utf-8")
    except UnicodeDecodeError:
        return None


def iter_types(pm: StarProcess, mr: MetadataRegistration,
               *, only_class: bool = True) -> Iterator[TypeEntry]:
    """枚举 MetadataRegistration.types[] 中的所有类型."""
    # 一次读完整张 Il2CppType** 表 (8 字节指针 × count)
    ptrs_blob = pm.read_bytes(mr.types_ptr, mr.types_count * 8)
    if ptrs_blob is None:
        raise RuntimeError(f"读 types 表 @ 0x{mr.types_ptr:X} 失败")
    type_ptrs = [int.from_bytes(ptrs_blob[i : i + 8], "little")
                 for i in range(0, len(ptrs_blob), 8)]

    # 批量读 Il2CppType (16 字节) - 但地址不连续, 必须逐个
    # 优化: 大多数 Il2CppType 应该集中在 GameAssembly 的某个 .data 段, 试着批量
    # 简单起见, 还是逐个读 (缓存 page)
    for idx, tp_addr in enumerate(type_ptrs):
        if tp_addr == 0:
            continue
        b = pm.read_bytes(tp_addr, 16)
        if not b or len(b) < 16:
            continue
        data_v = int.from_bytes(b[0:8], "little")
        bits = int.from_bytes(b[8:12], "little")
        type_enum = (bits >> 16) & 0xFF
        if only_class and type_enum not in (TYPE_CLASS, TYPE_VALUETYPE):
            continue
        yield TypeEntry(
            type_idx=idx, type_addr=tp_addr,
            data_value=data_v, type_enum=type_enum,
        )


def build_class_index(pm: StarProcess, mr: MetadataRegistration,
                      *, progress_every: int = 5000) -> Dict[str, List[TypeEntry]]:
    """枚举所有 CLASS/VALUETYPE 类型, 解析 name+namespace, 构建 fullname → TypeEntry list."""
    index: Dict[str, List[TypeEntry]] = {}
    t0 = time.time()
    n_total = 0
    n_resolved = 0
    for entry in iter_types(pm, mr, only_class=True):
        n_total += 1
        klass = entry.data_value
        # data_value 应该是 Il2CppClass*. 简单合理性检查: 不能为 0, 应该可读
        if klass == 0:
            continue
        # 读 Il2CppClass.name, namespaze, parent
        head = pm.read_bytes(klass, 0x60)
        if not head or len(head) < 0x60:
            continue
        name_ptr = int.from_bytes(head[CLASS_NAME_OFF : CLASS_NAME_OFF + 8], "little")
        ns_ptr = int.from_bytes(head[CLASS_NAMESPACE_OFF : CLASS_NAMESPACE_OFF + 8], "little")
        parent = int.from_bytes(head[CLASS_PARENT_OFF : CLASS_PARENT_OFF + 8], "little")
        name = _read_cstring(pm, name_ptr)
        ns = _read_cstring(pm, ns_ptr) if ns_ptr else ""
        if not name:
            continue
        entry.name = name
        entry.namespace = ns or ""
        entry.parent = parent
        full = f"{ns}.{name}" if ns else name
        index.setdefault(full, []).append(entry)
        n_resolved += 1
        if n_resolved % progress_every == 0:
            print(f"  [progress] {n_resolved}/{n_total} ({time.time()-t0:.1f}s)")
    print(f"[build_class_index] {n_resolved}/{n_total} 个 class 类型, "
          f"{len(index)} 个唯一全名 ({time.time()-t0:.1f}s)")
    return index


# ─────────── CLI ───────────


def cmd_dump_mr(_args) -> int:
    pm = StarProcess()
    try:
        parse_metadata_registration(pm)
        return 0
    finally:
        pm.close()


def cmd_sample_types(args) -> int:
    pm = StarProcess()
    try:
        mr = parse_metadata_registration(pm)
        n = 0
        for entry in iter_types(pm, mr, only_class=True):
            klass = entry.data_value
            head = pm.read_bytes(klass, 0x60)
            if not head:
                continue
            name_ptr = int.from_bytes(head[CLASS_NAME_OFF : CLASS_NAME_OFF + 8], "little")
            ns_ptr = int.from_bytes(head[CLASS_NAMESPACE_OFF : CLASS_NAMESPACE_OFF + 8], "little")
            name = _read_cstring(pm, name_ptr) or "?"
            ns = _read_cstring(pm, ns_ptr) if ns_ptr else ""
            full = f"{ns}.{name}" if ns else name
            print(f"  [{entry.type_idx:5d}] type@0x{entry.type_addr:X} klass=0x{klass:X}  {full}")
            n += 1
            if n >= args.limit:
                break
        return 0
    finally:
        pm.close()


def cmd_find(args) -> int:
    pm = StarProcess()
    try:
        mr = parse_metadata_registration(pm)
        index = build_class_index(pm, mr)
        # 模糊匹配
        q = args.name
        matches = [k for k in index if q in k]
        matches.sort(key=lambda k: (len(k), k))
        for k in matches[: args.limit]:
            entries = index[k]
            for e in entries:
                print(f"  klass=0x{e.data_value:X}  parent=0x{e.parent:X}  {k}")
        if not matches:
            print(f"  (无匹配 '{q}')")
            return 3
        print(f"[total] {len(matches)} 个匹配 (显示前 {min(args.limit, len(matches))})")
        return 0
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="IL2CPP MetadataRegistration 解析")
    sp = ap.add_subparsers(dest="cmd", required=True)

    p = sp.add_parser("dump-mr", help="只 dump MetadataRegistration 头信息")
    p.set_defaults(func=cmd_dump_mr)

    p = sp.add_parser("sample", help="抽样列出前 N 个 class 类型")
    p.add_argument("--limit", type=int, default=20)
    p.set_defaults(func=cmd_sample_types)

    p = sp.add_parser("find", help="构建全索引并模糊查找类名")
    p.add_argument("name")
    p.add_argument("--limit", type=int, default=30)
    p.set_defaults(func=cmd_find)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

