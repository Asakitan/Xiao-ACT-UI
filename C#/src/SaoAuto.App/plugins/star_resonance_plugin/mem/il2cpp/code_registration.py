"""IL2CPP CodeRegistration / CodeGenModule 解析 - 拿运行时 Il2CppClass*.

CodeRegistration v31 layout (Il2CppDumper Il2CppCodeRegistration.cs):
    intptr_t reversePInvokeWrappersCount;       // +0x00
    void*    reversePInvokeWrappers;            // +0x08
    intptr_t genericMethodPointersCount;        // +0x10
    void**   genericMethodPointers;             // +0x18
    void**   genericAdjustorThunks;             // +0x20  (v22+)
    intptr_t invokerPointersCount;              // +0x28
    void**   invokerPointers;                   // +0x30
    intptr_t unresolvedVirtualCallCount;        // +0x38  (deprecated)
    void**   unresolvedVirtualCallPointers;     // +0x40
    intptr_t unresolvedInstanceCallCount;       // +0x48  (v29.1+)  ← v31 有
    void**   unresolvedInstanceCallPointers;    // +0x50
    intptr_t unresolvedStaticCallCount;         // +0x58
    void**   unresolvedStaticCallPointers;      // +0x60
    intptr_t interopDataCount;                  // +0x68
    void*    interopData;                       // +0x70
    intptr_t windowsRuntimeFactoryCount;        // +0x78
    void*    windowsRuntimeFactoryTable;        // +0x80
    intptr_t codeGenModulesCount;               // +0x88   ← 关键
    Il2CppCodeGenModule** codeGenModules;       // +0x90   ← 关键

(v31 字段顺序参考: Il2CppDumper/Il2CppDumper/Il2Cpp/Il2CppExecutor.cs +
 unity-2022.x il2cpp/libil2cpp/vm-utils/RegistrationHelper.h)

Il2CppCodeGenModule v31:
    const char* moduleName;                  // +0x00
    uint32_t  methodPointerCount;            // +0x08
    Il2CppMethodPointer* methodPointers;     // +0x10
    uint32_t  adjustorThunkCount;            // +0x18
    AdjustorThunk* adjustorThunks;           // +0x20
    int32_t*  invokerIndices;                // +0x28
    uint32_t  reversePInvokeWrapperCount;    // +0x30
    CustomAttributesCacheGenerator* customAttributes;  // +0x38
    Il2CppTokenRangePair* rgctxRanges;       // +0x40
    uint32_t  rgctxRangesCount;              // +0x48
    Il2CppRGCTXDefinition* rgctxs;           // +0x50
    uint32_t  rgctxsCount;                   // +0x58
    Il2CppDebuggerMetadataRegistration* debuggerMetadataRegistration;  // +0x60
    Il2CppMethodPointer moduleInitializer;   // +0x68
    void*     staticConstructorTypeIndices;  // +0x70  (i32*)
    Il2CppMetadataRange* metadataRangeArray; // +0x78
    Il2CppClass** pTypeInfoTable;            // +0x80   ← 真正的 klass 表 (按 typeIdx 索引)

注意: pTypeInfoTable 是按 module-local typeDefIndex 索引的, 不是全局 typeIndex.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.process import StarProcess

# ─────────── 已知常量 ───────────

GAME_ASSEMBLY = "GameAssembly.dll"
CODE_REGISTRATION_RVA = 0x76E8C40

# Il2CppClass v31 关键 offset
CLASS_NAME_OFF = 0x10
CLASS_NAMESPACE_OFF = 0x18
CLASS_PARENT_OFF = 0x58


# ─────────── CodeRegistration ───────────


@dataclass
class CodeRegistration:
    addr: int
    code_gen_modules_count: int
    code_gen_modules_ptr: int  # Il2CppCodeGenModule** array


def _find_module(pm: StarProcess, name: str):
    name_lc = name.lower()
    for m in pm.list_modules():
        if m.name.lower() == name_lc:
            return m
    raise RuntimeError(f"模块未找到: {name}")


def parse_code_registration(pm: StarProcess, *, debug: bool = False) -> CodeRegistration:
    mod = _find_module(pm, GAME_ASSEMBLY)
    addr = mod.base + CODE_REGISTRATION_RVA
    blob = pm.read_bytes(addr, 0x100)
    if blob is None:
        raise RuntimeError(f"读 CodeRegistration @ 0x{addr:X} 失败")
    qs = [int.from_bytes(blob[i : i + 8], "little") for i in range(0, 0x100, 8)]
    if debug:
        print(f"[CR raw] @ 0x{addr:X}")
        for i, v in enumerate(qs):
            mark = ""
            if 0 < v < 1_000_000:
                mark = " ← small (count?)"
            elif mod.base <= v < mod.base + mod.size:
                mark = f" ← in GameAssembly (+0x{v - mod.base:X})"
            print(f"  +0x{i*8:03X} qs[{i:2d}]: 0x{v:016X}{mark}")
    cr = CodeRegistration(
        addr=addr,
        code_gen_modules_count=qs[15],   # +0x78
        code_gen_modules_ptr=qs[16],     # +0x80
    )
    print(f"[CR] @ 0x{addr:X} (GameAssembly + 0x{CODE_REGISTRATION_RVA:X})")
    print(f"     codeGenModulesCount       = {cr.code_gen_modules_count}")
    print(f"     codeGenModules            @ 0x{cr.code_gen_modules_ptr:X}")
    return cr


# ─────────── CodeGenModule ───────────


@dataclass
class CodeGenModule:
    addr: int
    name: str
    p_type_info_table: int
    type_info_count: int = 0  # 待估算


def parse_code_gen_modules(pm: StarProcess, cr: CodeRegistration) -> List[CodeGenModule]:
    n = cr.code_gen_modules_count
    if n == 0 or cr.code_gen_modules_ptr == 0:
        return []
    arr = pm.read_bytes(cr.code_gen_modules_ptr, n * 8)
    if arr is None:
        raise RuntimeError("读 codeGenModules 数组失败")
    mods: List[CodeGenModule] = []
    for i in range(n):
        m_addr = int.from_bytes(arr[i * 8 : i * 8 + 8], "little")
        if m_addr == 0:
            continue
        mod_blob = pm.read_bytes(m_addr, 0x90)
        if not mod_blob or len(mod_blob) < 0x88:
            continue
        name_ptr = int.from_bytes(mod_blob[0:8], "little")
        method_count = int.from_bytes(mod_blob[8:12], "little")
        p_type_info_table = int.from_bytes(mod_blob[0x80:0x88], "little")
        # 读 module name
        name_buf = pm.read_bytes(name_ptr, 256) if name_ptr else None
        if name_buf:
            end = name_buf.find(b"\x00")
            mname = name_buf[: end if end > 0 else 0].decode("utf-8", errors="replace")
        else:
            mname = "?"
        mods.append(CodeGenModule(
            addr=m_addr, name=mname,
            p_type_info_table=p_type_info_table,
        ))
        if i < 3 or "Bokura" in mname or "Panda" in mname:
            print(f"  [{i:3d}] {mname:40s}  module@0x{m_addr:X}  "
                  f"methods={method_count}  pTypeInfo=0x{p_type_info_table:X}")
    return mods


# ─────────── 类索引 (扫所有 module 的 pTypeInfoTable) ───────────


@dataclass
class ClassEntry:
    klass: int
    name: str
    namespace: str
    parent: int
    module: str
    local_idx: int


def _read_cstring(pm: StarProcess, addr: int, max_len: int = 256) -> Optional[str]:
    if addr == 0:
        return None
    buf = pm.read_bytes(addr, max_len)
    if not buf:
        return None
    end = buf.find(b"\x00")
    return buf[: end if end >= 0 else len(buf)].decode("utf-8", errors="replace")


def build_class_index(pm: StarProcess, mods: List[CodeGenModule],
                      *, max_per_module: int = 100000) -> Dict[str, List[ClassEntry]]:
    """扫所有 module 的 pTypeInfoTable, 读 klass 的 name/namespace, 建索引."""
    index: Dict[str, List[ClassEntry]] = {}
    t0 = time.time()
    n_total = 0
    n_resolved = 0
    for mod in mods:
        if mod.p_type_info_table == 0:
            continue
        # pTypeInfoTable 是变长的: 我们不知道 count, 但可以估算: 一个 module
        # 通常 < 几万 typeDef. 读 max_per_module*8 字节, 遇到非 klass 指针就停.
        # 更稳: 一次读 8KB 看到大量 0 就截断
        chunk_size = max_per_module * 8
        buf = pm.read_bytes(mod.p_type_info_table, chunk_size)
        if not buf:
            continue
        # 估算 count: 找连续非零指针段
        local_count = 0
        for i in range(0, len(buf), 8):
            klass = int.from_bytes(buf[i : i + 8], "little")
            if klass == 0:
                # 允许少量空槽
                continue
            # 简单合理性: klass 应该在 GameAssembly 范围或堆上
            if klass < 0x10000:
                break
            local_count += 1
            if local_count > max_per_module:
                break
            n_total += 1
            head = pm.read_bytes(klass, 0x60)
            if not head or len(head) < 0x60:
                continue
            name_ptr = int.from_bytes(head[CLASS_NAME_OFF : CLASS_NAME_OFF + 8], "little")
            ns_ptr = int.from_bytes(head[CLASS_NAMESPACE_OFF : CLASS_NAMESPACE_OFF + 8], "little")
            parent = int.from_bytes(head[CLASS_PARENT_OFF : CLASS_PARENT_OFF + 8], "little")
            name = _read_cstring(pm, name_ptr)
            if not name:
                continue
            ns = _read_cstring(pm, ns_ptr) if ns_ptr else ""
            full = f"{ns}.{name}" if ns else name
            index.setdefault(full, []).append(ClassEntry(
                klass=klass, name=name, namespace=ns or "",
                parent=parent, module=mod.name, local_idx=i // 8,
            ))
            n_resolved += 1
    print(f"[build_class_index] {n_resolved}/{n_total} 个 klass, "
          f"{len(index)} 个唯一全名 ({time.time()-t0:.1f}s)")
    return index


# ─────────── CLI ───────────


def cmd_dump_cr(args) -> int:
    pm = StarProcess()
    try:
        cr = parse_code_registration(pm, debug=args.debug)
        if cr.code_gen_modules_count == 0 or cr.code_gen_modules_count > 1000:
            print("[!] codeGenModulesCount 异常, 字段顺序可能错")
            return 2
        parse_code_gen_modules(pm, cr)
        return 0
    finally:
        pm.close()


def cmd_find(args) -> int:
    pm = StarProcess()
    try:
        cr = parse_code_registration(pm)
        mods = parse_code_gen_modules(pm, cr)
        index = build_class_index(pm, mods)
        # 模糊匹配
        q = args.name
        matches = [k for k in index if q.lower() in k.lower()]
        matches.sort(key=lambda k: (len(k), k))
        for k in matches[: args.limit]:
            for e in index[k]:
                print(f"  klass=0x{e.klass:X}  parent=0x{e.parent:X}  "
                      f"[{e.module}/{e.local_idx}]  {k}")
        if not matches:
            print(f"  (无匹配 '{q}')")
            return 3
        print(f"[total] {len(matches)} 个匹配")
        return 0
    finally:
        pm.close()


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="IL2CPP CodeRegistration 解析")
    sp = ap.add_subparsers(dest="cmd", required=True)

    p = sp.add_parser("dump-cr", help="dump CodeRegistration + 列出 modules")
    p.add_argument("--debug", action="store_true")
    p.set_defaults(func=cmd_dump_cr)

    p = sp.add_parser("find", help="查找类名 (扫所有 pTypeInfoTable)")
    p.add_argument("name")
    p.add_argument("--limit", type=int, default=30)
    p.set_defaults(func=cmd_find)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

