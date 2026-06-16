"""IL2CPP runtime resolver - 类名字符串锚定方案.

核心思路:
    1. 已 dump 的 metadata blob 中 stringSection 包含所有类名字符串 (稳定不变)
    2. 在 Star.exe 内存里找到 metadata blob 的当前地址 (mem_dump_metadata)
    3. 在 stringSection 中找目标字符串 "PlayerMng\\0" → string_addr
    4. 全堆扫指向 string_addr 的 8 字节指针 → Il2CppClass.name 字段位置
    5. 校验 (klass_addr + 0x78) == klass_addr (Il2CppClass.klass 自指针)
    6. 拿到 klass_ptr, 全堆扫首 8 字节 == klass_ptr → 该类实例
    7. + dump.cs 中的 field offset → 字段值

CLI 子命令:
    list <name-substr>            列出 metadata 中所有匹配的类名字符串
    locate-class <name>           定位一个类的 Il2CppClass*
    locate-instance <name>        定位类的所有实例
    read <name> <field>           读取实例字段 (找到 1 个实例时)
    quick <name>                  一键 class→instance→所有非空字段 dump
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time
from typing import Dict, List, Optional, Tuple

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.process import StarProcess, StarProcessError
from plugins.star_resonance_plugin.mem.il2cpp.mem_dump_metadata import (
    find_metadata_in_process, MAGIC, _default_out_dir
)

# ─────────── Il2CppGlobalMetadataHeader v31 layout ───────────
# 前 8 字节: sanity (u32), version (i32)
# 后续若干 (offset:i32, size:i32) 对, 每对描述一个 section.
# v31 中 stringLiteral 是第 1 对, string 是第 3 对 (字符串section).
#
# 完整 v31 header (来自 unity-technologies/il2cpp 公开):
#   uint32_t sanity;
#   int32_t version;
#   int32_t stringLiteralOffset, stringLiteralSize;
#   int32_t stringLiteralDataOffset, stringLiteralDataSize;
#   int32_t stringOffset, stringSize;                         ← 类/字段/方法名 (我们要的)
#   int32_t eventsOffset, eventsSize;
#   int32_t propertiesOffset, propertiesSize;
#   int32_t methodsOffset, methodsSize;
#   int32_t parameterDefaultValuesOffset, ...Size;
#   int32_t fieldDefaultValuesOffset, ...Size;
#   int32_t fieldAndParameterDefaultValueDataOffset, ...Size;
#   int32_t fieldMarshaledSizesOffset, ...Size;
#   int32_t parametersOffset, ...Size;
#   int32_t fieldsOffset, ...Size;
#   int32_t genericParametersOffset, ...Size;
#   int32_t genericParameterConstraintsOffset, ...Size;
#   int32_t genericContainersOffset, ...Size;
#   int32_t nestedTypesOffset, ...Size;
#   int32_t interfacesOffset, ...Size;
#   int32_t vtableMethodsOffset, ...Size;
#   int32_t interfaceOffsetsOffset, ...Size;
#   int32_t typeDefinitionsOffset, ...Size;
#   int32_t imagesOffset, ...Size;
#   int32_t assembliesOffset, ...Size;
#   int32_t fieldRefsOffset, ...Size;
#   int32_t referencedAssembliesOffset, ...Size;
#   int32_t attributeDataOffset, ...Size;
#   int32_t attributeDataRangeOffset, ...Size;
#   int32_t unresolvedIndirectCallParameterTypesOffset, ...Size;
#   int32_t unresolvedIndirectCallParameterRangesOffset, ...Size;
#   int32_t windowsRuntimeTypeNamesOffset, ...Size;
#   int32_t windowsRuntimeStringsOffset, ...Size;
#   int32_t exportedTypeDefinitionsOffset, ...Size;

# 对我们而言, 只需要 stringOffset/stringSize (第 3 对, 即 header[16:24]).
_HDR_STRING_PAIR = 3  # 0=stringLiteral, 1=stringLiteralData, 2=stringInfo? 3=string ...
# 其实 v31 中 stringOffset 是 (offset+size) 第 3 对索引, 即字节偏移 8 + 3*8 = 0x20.
# 验证方式: stringSection 解析后能找到正常 ASCII 类名.

# 经实测各 dumper version, v31 在 header[8 + 16] = 0x18 是 stringOffset.
# 但为保险, 我们暴力扫描 header 找所有 (offset,size) 对, 看哪一段最像字符串区:
#   - 大量 ASCII printable
#   - 包含 \0 分隔
#   - 含已知名字 (如 "Object", "String", "Int32")


def _find_string_section(blob: bytes) -> Tuple[int, int]:
    """从 metadata blob 找 stringSection 的 (offset, size).
    
    策略: 扫 header 的所有 (offset, size) 对, 取一段抽样判定为 ASCII 字符串区.
    """
    sanity = int.from_bytes(blob[:4], "little")
    if sanity != MAGIC:
        raise ValueError(f"sanity mismatch: {sanity:#x}")
    version = int.from_bytes(blob[4:8], "little", signed=True)
    pairs: List[Tuple[int, int, int]] = []  # (header_off, sec_off, sec_size)
    # header 长度大致 256, pair 从 +8 起
    for i in range(8, 256, 8):
        if i + 8 > len(blob):
            break
        po, ps = struct.unpack_from("<II", blob, i)
        if po == 0 or ps == 0:
            continue
        if po + ps > len(blob):
            continue
        pairs.append((i, po, ps))

    # 评分: 抽样 256 字节, 计算 ASCII printable 比例 + null 数 + 是否含 "Object"
    best = None
    best_score = -1
    for h_off, s_off, s_size in pairs:
        sample = blob[s_off : s_off + min(4096, s_size)]
        if not sample:
            continue
        printable = sum(1 for b in sample if 0x20 <= b <= 0x7E or b == 0)
        nulls = sample.count(0)
        bonus = 0
        if b"Object\x00" in sample:
            bonus += 100
        if b"String\x00" in sample:
            bonus += 50
        if b"Int32\x00" in sample:
            bonus += 50
        score = printable + nulls + bonus
        if score > best_score:
            best_score = score
            best = (h_off, s_off, s_size)
    if best is None:
        raise RuntimeError("找不到字符串 section")
    h_off, s_off, s_size = best
    print(f"[meta] string section: header_off={h_off:#x}  data=[{s_off:#x}, +{s_size:#x}]")
    return s_off, s_size


# ─────────── Il2CppClass v31 关键 offset ───────────

CLASS_NAME_OFF = 0x10
CLASS_NAMESPACE_OFF = 0x18
CLASS_PARENT_OFF = 0x58
CLASS_KLASS_SELFPTR_OFF = 0x78  # 校验锚: 指向自己
CLASS_FIELDS_OFF = 0x80
CLASS_METHODS_OFF = 0x98
CLASS_STATIC_FIELDS_OFF = 0xB8

OBJECT_KLASS_OFF = 0x00
OBJECT_HEADER_SIZE = 0x10  # klass + monitor


# ─────────── 主类 ───────────


class Il2CppResolver:
    def __init__(self, pm: StarProcess, *, max_region_size: int = 256 * 1024 * 1024):
        self.pm = pm
        self._max_region_size = max_region_size
        self._metadata_addr: Optional[int] = None
        self._metadata_size: Optional[int] = None
        self._string_section_addr: Optional[int] = None
        self._string_section_size: Optional[int] = None

    # ── metadata 定位 ──
    def locate_metadata(self) -> Tuple[int, int]:
        """定位 metadata blob, 并把整块作为 "字符串搜索域" 使用.
        
        我们不解析 header section 表 (太脆弱), 而是直接在整个 blob 里
        查找类名字符串. blob 内除 stringSection 外, 其他段是数值/索引,
        几乎不可能误命中 "PlayerMng\0" 这样的可读字符串.
        """
        if self._metadata_addr is not None:
            return self._metadata_addr, self._metadata_size  # type: ignore[return-value]
        candidates = find_metadata_in_process(self.pm, verbose=False)
        if not candidates:
            raise RuntimeError("找不到 metadata 魔数")
        addr, version, total = candidates[0]
        print(f"[meta] @ 0x{addr:X}  version={version}  size={total/1024/1024:.2f}MB")
        self._metadata_addr = addr
        self._metadata_size = total
        # 把整块 blob 当作字符串搜索域
        self._string_section_addr = addr
        self._string_section_size = total
        return addr, total

    # ── 字符串查找 ──
    def find_string(self, name: str) -> List[int]:
        """返回 stringSection 内 "name\\0" 的所有出现地址."""
        self.locate_metadata()
        assert self._string_section_addr is not None and self._string_section_size is not None
        section = self.pm.read_bytes(self._string_section_addr, self._string_section_size)
        if section is None:
            raise RuntimeError("读 string section 失败")
        needle = name.encode("utf-8") + b"\x00"
        results: List[int] = []
        pos = 0
        while True:
            i = section.find(needle, pos)
            if i < 0:
                break
            # 必须是 \0 之后开始 (字符串边界)
            if i == 0 or section[i - 1] == 0:
                results.append(self._string_section_addr + i)
            pos = i + 1
        return results

    def list_strings(self, substring: str, limit: int = 50) -> List[Tuple[int, str]]:
        """枚举 stringSection 中所有含 substring 的字符串 (用于探查类名)."""
        self.locate_metadata()
        assert self._string_section_addr is not None and self._string_section_size is not None
        section = self.pm.read_bytes(self._string_section_addr, self._string_section_size)
        if section is None:
            return []
        needle = substring.encode("utf-8")
        results: List[Tuple[int, str]] = []
        pos = 0
        while pos < len(section) and len(results) < limit:
            i = section.find(needle, pos)
            if i < 0:
                break
            # 找此字符串边界: 向前找上一个 \0
            start = section.rfind(b"\x00", 0, i) + 1
            end = section.find(b"\x00", i)
            if end < 0:
                break
            try:
                s = section[start:end].decode("utf-8")
                results.append((self._string_section_addr + start, s))
            except UnicodeDecodeError:
                pass
            pos = end + 1
        return results

    # ── 找指针 ──
    def _scan_for_pointer(self, target: int, *, aligned: bool = True,
                          max_hits: int = 1000, only_private: bool = True) -> List[int]:
        from mem_probe import cy_memscan as _cy
        target_u64 = int(target) & 0xFFFFFFFFFFFFFFFF
        # cy 内核只支持对齐扫; 调用方要求非对齐时退化到 bytes.find
        if not aligned:
            needle = target_u64.to_bytes(8, "little")
        hits: List[int] = []
        for region in self.pm.iter_regions(only_readable=True,
                                           only_private=only_private):
            if region.size > self._max_region_size:
                continue
            chunk = 16 * 1024 * 1024
            offset = 0
            while offset < region.size:
                n = min(chunk, region.size - offset)
                blob = self.pm.read_bytes(region.base + offset, n)
                if blob is None:
                    break
                if aligned:
                    remaining = max_hits - len(hits)
                    if remaining <= 0:
                        return hits
                    for i in _cy.find_aligned_u64(blob, target_u64,
                                                  max_hits=remaining):
                        hits.append(region.base + offset + i)
                    if len(hits) >= max_hits:
                        return hits
                else:
                    pos = 0
                    while True:
                        i = blob.find(needle, pos)
                        if i < 0:
                            break
                        hits.append(region.base + offset + i)
                        if len(hits) >= max_hits:
                            return hits
                        pos = i + 1
                if offset + n < region.size:
                    offset += n - 7
                else:
                    offset += n
        return hits

    # ── class 定位 ──
    def locate_class(self, name: str, *, debug: bool = False,
                     include_mapped: bool = False) -> List[int]:
        """返回所有该 class 的 Il2CppClass* (一般 1 个)."""
        # 1. 找名字字符串地址
        str_addrs = self.find_string(name)
        if not str_addrs:
            print(f"[locate_class] 未找到字符串 '{name}'")
            return []
        if len(str_addrs) > 1:
            print(f"[locate_class] '{name}' 在 string section 出现 {len(str_addrs)} 次")

        # 2. 扫指向这些字符串的 8 字节指针
        all_klass: List[int] = []
        for sa in str_addrs:
            for only_private in ([True] if not include_mapped else [True, False]):
                t0 = time.time()
                ptr_hits = self._scan_for_pointer(sa, aligned=True,
                                                  max_hits=2000,
                                                  only_private=only_private)
                tag = "PRIVATE" if only_private else "ALL"
                print(f"[locate_class] 字符串 0x{sa:X} [{tag}]: "
                      f"指针引用 {len(ptr_hits)} 处 ({time.time()-t0:.1f}s)")
                # 3. 校验: 每个候选 = name 字段位置, klass = 候选 - 0x10
                for ptr_addr in ptr_hits:
                    klass = ptr_addr - CLASS_NAME_OFF
                    # dump 候选 0x300 字节诊断
                    dump = self.pm.read_bytes(klass, 0x300)
                    if debug and dump:
                        self._diag_dump_klass(klass, dump, sa)
                    self_off = self._find_self_ptr_offset(klass, dump)
                    if self_off is None:
                        if debug:
                            print(f"  [skip] klass=0x{klass:X} 无自指针")
                        continue
                    if debug:
                        print(f"  [pass] klass=0x{klass:X} 自指针 @ +0x{self_off:X}")
                    ns_ptr = self.pm.read_u64(klass + CLASS_NAMESPACE_OFF)
                    if ns_ptr is None:
                        continue
                    if ns_ptr != 0:
                        if not (self._string_section_addr <=
                                ns_ptr <
                                self._string_section_addr + self._string_section_size):  # type: ignore[operator]
                            if debug:
                                print(f"  [skip] ns_ptr=0x{ns_ptr:X} 不在 metadata 内")
                            continue
                    if klass not in all_klass:
                        all_klass.append(klass)
                if all_klass and not include_mapped:
                    break  # PRIVATE 已找到, 不必再扫 ALL
        print(f"[locate_class] '{name}' → {len(all_klass)} 个 Il2CppClass* (校验通过)")
        return all_klass

    def _find_self_ptr_offset(self, klass: int, dump: Optional[bytes]) -> Optional[int]:
        """在 klass dump 中搜索指向自己 (即 == klass) 的 8 字节指针, 返回偏移.
        
        范围扩到 0x40 - 0x300 (覆盖各种 IL2CPP layout 变种).
        """
        if not dump:
            return None
        needle = klass.to_bytes(8, "little")
        for off in range(0x40, len(dump) - 8, 8):
            if dump[off : off + 8] == needle:
                return off
        return None

    def _diag_dump_klass(self, klass: int, dump: bytes, name_str_addr: int) -> None:
        print(f"  [diag] klass candidate @ 0x{klass:X}")
        for off in range(0, min(0x300, len(dump)), 8):
            v = int.from_bytes(dump[off : off + 8], "little")
            mark = ""
            if v == klass:
                mark = " ← SELF"
            elif v == name_str_addr:
                mark = " ← name str"
            elif v != 0 and (self._string_section_addr or 0) <= v < (self._string_section_addr or 0) + (self._string_section_size or 0):  # type: ignore[operator]
                mark = " ← in metadata"
            elif v != 0 and 0x70_0000_0000 <= v <= 0x7F_FFFF_FFFF:
                mark = " ← heap-like"
            print(f"    +0x{off:03X}: 0x{v:016X}{mark}")

    # ── 实例定位 ──
    def locate_instances(self, klass_ptr: int, *, max_hits: int = 200) -> List[int]:
        """返回所有 Il2CppObject 实例 (首 8 字节 == klass_ptr)."""
        return self._scan_for_pointer(klass_ptr, aligned=True, max_hits=max_hits)


# ─────────── CLI ───────────


def cmd_list(args) -> int:
    pm = StarProcess()
    try:
        r = Il2CppResolver(pm)
        items = r.list_strings(args.substr, limit=args.limit)
        for addr, s in items:
            print(f"  0x{addr:X}  {s}")
        print(f"[total] {len(items)}")
        return 0
    finally:
        pm.close()


def cmd_locate_class(args) -> int:
    pm = StarProcess()
    try:
        r = Il2CppResolver(pm)
        klasses = r.locate_class(args.name, debug=args.debug,
                                 include_mapped=args.include_mapped)
        for k in klasses:
            name_ptr = pm.read_u64(k + CLASS_NAME_OFF)
            ns_ptr = pm.read_u64(k + CLASS_NAMESPACE_OFF)
            parent_ptr = pm.read_u64(k + CLASS_PARENT_OFF)
            print(f"  klass=0x{k:X}  name_ptr=0x{name_ptr:X}  ns_ptr=0x{ns_ptr:X}  parent=0x{parent_ptr:X}")
        return 0 if klasses else 3
    finally:
        pm.close()


def cmd_locate_instance(args) -> int:
    pm = StarProcess()
    try:
        r = Il2CppResolver(pm)
        klasses = r.locate_class(args.name)
        if not klasses:
            return 3
        if len(klasses) > 1:
            print(f"[warn] {len(klasses)} 个 klass, 用第一个 0x{klasses[0]:X}")
        klass = klasses[0]
        t0 = time.time()
        instances = r.locate_instances(klass)
        print(f"[locate_instances] {len(instances)} 个 ({time.time()-t0:.1f}s)")
        for inst in instances[: min(20, len(instances))]:
            head = pm.read_bytes(inst, 64)
            head_hex = head.hex() if head else "READ_FAIL"
            print(f"  inst=0x{inst:X}  head[0:64]={head_hex}")
        return 0
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="IL2CPP runtime resolver")
    sp = ap.add_subparsers(dest="cmd", required=True)

    p = sp.add_parser("list", help="枚举 metadata 中含 substr 的字符串")
    p.add_argument("substr")
    p.add_argument("--limit", type=int, default=50)
    p.set_defaults(func=cmd_list)

    p = sp.add_parser("locate-class", help="定位某个类的 Il2CppClass*")
    p.add_argument("name")
    p.add_argument("--debug", action="store_true", help="打印候选 klass dump")
    p.add_argument("--include-mapped", action="store_true",
                   help="也扫 MEM_MAPPED 区 (慢, 但 Il2CppClass 可能在那)")
    p.set_defaults(func=cmd_locate_class)

    p = sp.add_parser("locate-instance", help="定位某个类的所有实例")
    p.add_argument("name")
    p.set_defaults(func=cmd_locate_instance)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

