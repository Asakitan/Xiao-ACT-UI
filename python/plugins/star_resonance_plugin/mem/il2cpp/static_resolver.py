# static_resolver - 统一 API: dump.cs 偏移 + script.json RVA + 进程内存读.
#
# 核心抽象:
# sr = StaticResolver(pm, ga_base, script_index, dump_cs_index)
# klass = sr.resolve_klass("Zproto.CharSerialize")    # *(GA + RVA)
# objs  = sr.find_instances(klass)                    # heap scan
# cid   = sr.read_field(obj, "CharSerialize", "CharId")
# attr  = sr.read_ptr_field(obj, "CharSerialize", "Attr")
# hp    = sr.read_field(attr, "UserFightAttr", "CurHp")
#
# 字段类型 → 读法 (常用):
# long / ulong / Int64 / UInt64        → i64 / u64
# int  / uint  / Int32 / UInt32        → i32 / u32
# float / Single                       → f32
# bool / Boolean                       → u8 (1=true)
# 引用类型 (class)                     → ptr (u64)
# string                               → ptr → utf16 length-prefixed
from __future__ import annotations

import os
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ..process import StarProcess
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.il2cpp.dump_cs_parser import DumpCsIndex
from mem_probe import cy_memscan as _cy


# C# 基本类型 → (字节宽度, 读法)
_PRIMITIVE = {
    "bool": ("u8",), "Boolean": ("u8",),
    "byte": ("u8",), "Byte": ("u8",),
    "sbyte": ("i8",), "SByte": ("i8",),
    "short": ("i16",), "Int16": ("i16",),
    "ushort": ("u16",), "UInt16": ("u16",),
    "int": ("i32",), "Int32": ("i32",),
    "uint": ("u32",), "UInt32": ("u32",),
    "long": ("i64",), "Int64": ("i64",),
    "ulong": ("u64",), "UInt64": ("u64",),
    "float": ("f32",), "Single": ("f32",),
    "double": ("f64",), "Double": ("f64",),
    "IntPtr": ("u64",), "UIntPtr": ("u64",),
}


@dataclass
class FieldInfo:
    name: str
    type: str
    offset: int
    is_static: bool


class StaticResolver:
    def __init__(self, pm: StarProcess, ga_base: int,
                 script_index: ScriptIndex, dump_cs_index: DumpCsIndex):
        self.pm = pm
        self.ga = ga_base
        self.si = script_index
        self.dci = dump_cs_index
        self._klass_cache: Dict[str, int] = {}
        self._live_index: Optional[Dict[str, int]] = None  # in-memory fallback (lazy)

    # ---------- klass / instance ----------

    def _klass_name(self, kp: int) -> str:
        try:
            np = self.pm.read_u64(kp + 0x10)
            if not (0x10000 <= (np or 0) <= 0x7FFFFFFFFFFF):
                return ""
            b = self.pm.read_bytes(np, 48)
            return b.split(b"\x00", 1)[0].decode("utf-8", "replace") if b else ""
        except Exception:
            return ""

    # Critical classes resolved in one in-memory pass when the bundle is stale/missing.
    _LIVE_CLASSES = {
        "Zproto.CharSerialize", "Zproto.UserFightAttr", "Zproto.CharBaseInfo",
        "Zproto.RoleLevel", "Zproto.ProfessionList", "Zproto.EnergyItem",
        "Zproto.SceneData", "Zproto.SkillCDInfo",
        "Zproto.UserFightAttrContainerArchive", "Zproto.CharSerializeContainerArchive",
        "Panda.ZGame.ZEntityMgr", "Panda.ZGame.ZEntity",
    }

    def _live_resolve(self, class_name: str) -> int:
        # In-memory klass-by-name fallback (version-robust, no dump, onedir-safe).
        if self._live_index is None:
            try:
                # Shared process index: one GA scan for the union of critical
                # classes, warm-started from the persisted per-version RVAs.
                from plugins.star_resonance_plugin.mem.il2cpp.klass_index import resolve_klasses
                want = set(self._LIVE_CLASSES) | {class_name}
                self._live_index = resolve_klasses(self.pm, want, time_budget_s=40)
            except Exception:
                self._live_index = {}
        kp = int(self._live_index.get(class_name, 0) or 0)
        if kp:
            return kp
        short = class_name.rsplit(".", 1)[-1]
        for full, p in self._live_index.items():
            if full.rsplit(".", 1)[-1] == short:
                return int(p)
        return 0

    def resolve_klass(self, class_name: str) -> Optional[int]:
        if class_name in self._klass_cache:
            return self._klass_cache[class_name]
        short = class_name.rsplit(".", 1)[-1]
        # 1) bundle/script.json klass_rva, VALIDATED by live klass name (catches a
        #    stale bundle from a previous game version -> self-heal via live locator).
        rva = self.si.find_klass(class_name)
        if rva is not None:
            kp = self.pm.read_u64(self.ga + rva)
            if kp and self._klass_name(kp) == short:
                self._klass_cache[class_name] = kp
                return kp
        # 2) in-memory live locator (no dump / onedir / new game version)
        kp = self._live_resolve(class_name)
        if kp and self._klass_name(kp) == short:
            self._klass_cache[class_name] = kp
            return kp
        return None

    def find_instances(self, klass_ptr: int, max_region: Optional[int] = None,
                       max_hits: int = 4096, progress: bool = False) -> List[int]:
        # 全私有读区扫描 *(addr) == klass_ptr.
        #
        # 内层走 cy_memscan.find_aligned_u64 (AVX2: ~16 GB/s).
        hits: List[int] = []
        t0 = time.time()
        bytes_scanned = 0
        klass = klass_ptr & 0xFFFFFFFFFFFFFFFF
        chunk = 16 * 1024 * 1024
        read_into = getattr(self.pm, "read_bytes_into", None)
        scratch = bytearray(chunk)            # one reused buffer for the whole scan
        for r in self.pm.iter_regions(only_readable=True, only_private=True):
            if max_region is not None and r.size > max_region:
                continue
            off = 0
            while off < r.size:
                n = min(chunk, r.size - off)
                if read_into is not None:
                    got = read_into(r.base + off, scratch, n)
                    if got <= 0:
                        break
                    buf = memoryview(scratch)[:got]
                else:
                    blob = self.pm.read_bytes(r.base + off, n)
                    if blob is None:
                        break
                    buf = blob
                    got = n
                bytes_scanned += got
                remaining = max_hits - len(hits)
                if remaining <= 0:
                    return hits
                offs = _cy.find_aligned_u64(buf, klass, max_hits=remaining)
                for o in offs:
                    hits.append(r.base + off + o)
                if len(hits) >= max_hits:
                    return hits
                off += n
            if progress:
                print(f"  [scan] {bytes_scanned/1e9:.2f}GB "
                      f"hits={len(hits)} t={time.time()-t0:.1f}s", file=sys.stderr)
        return hits

    def find_self(self, class_name: str, sentinel_field: str,
                  sentinel_class: str, max_hits: int = 2048) -> List[tuple]:
        # 找 class 实例, 用 sentinel_field@offset 解引用必须指向 sentinel_class.
        #
        # 返回 [(obj, sentinel_obj), ...]. 通常 sentinel 验证后只剩 1 个真实对象.
        kp = self.resolve_klass(class_name)
        skp = self.resolve_klass(sentinel_class)
        if kp is None or skp is None:
            return []
        sf_off = self.dci.field_offset(class_name, sentinel_field)
        if sf_off is None:
            return []
        cands = self.find_instances(kp, max_hits=max_hits)
        out = []
        for obj in cands:
            sp = self.pm.read_u64(obj + sf_off)
            if sp and self.pm.read_u64(sp) == skp:
                out.append((obj, sp))
        return out

    # ---------- field read ----------

    def _field(self, class_name: str, field_name: str) -> Optional[FieldInfo]:
        c = self.dci.find_class(class_name)
        if not c:
            return None
        for f in c["fields"]:
            if f["name"] == field_name and not f["is_static"]:
                return FieldInfo(f["name"], f["type"], f["offset"], False)
        return None

    def read_field(self, obj: int, class_name: str, field_name: str):
        # 根据字段类型自动选择读法. 引用类型返回 ptr.
        f = self._field(class_name, field_name)
        if f is None or obj == 0:
            return None
        addr = obj + f.offset
        prim = _PRIMITIVE.get(f.type.split("<")[0].strip("[] "))
        if prim:
            method = getattr(self.pm, f"read_{prim[0]}", None)
            if method is None:
                # fall back via bytes
                width = int(prim[0][1:]) // 8
                blob = self.pm.read_bytes(addr, width)
                if blob is None:
                    return None
                signed = prim[0].startswith("i")
                return int.from_bytes(blob, "little", signed=signed)
            return method(addr)
        if f.type == "string" or f.type == "String":
            return self.read_string(self.pm.read_u64(addr))
        # 默认: 引用类型 → ptr
        return self.pm.read_u64(addr)

    def read_ptr_field(self, obj: int, class_name: str, field_name: str) -> Optional[int]:
        f = self._field(class_name, field_name)
        if f is None or obj == 0:
            return None
        return self.pm.read_u64(obj + f.offset)

    def read_string(self, str_obj: int) -> Optional[str]:
        # Il2CppString: +0x10=length(int), +0x14=utf16 chars.
        if not str_obj:
            return None
        ln = self.pm.read_i32(str_obj + 0x10)
        if ln is None or ln < 0 or ln > 4096:
            return None
        return self.pm.read_utf16(str_obj + 0x14, ln)

    # ---------- collections (Google.Protobuf RepeatedField + IL2CPP arrays) ----------

    # Confirmed via runtime probe (UserFightAttr.CdInfo @ 2025-04 dump):
    #   RepeatedField<T> instance: +0x10 = T[] array ptr, +0x18 = int count
    #   IL2CPP array (T[]):       +0x18 = max_length (uint32), +0x20 = elements
    REPEATED_ARRAY_OFF = 0x10
    REPEATED_COUNT_OFF = 0x18
    ARRAY_LENGTH_OFF = 0x18
    ARRAY_ELEMS_OFF = 0x20

    def read_repeated_count(self, rf_obj: int) -> int:
        # RepeatedField<T> 元素个数. 0 表示空/无效.
        if not rf_obj:
            return 0
        c = self.pm.read_u32(rf_obj + self.REPEATED_COUNT_OFF) or 0
        return c if 0 <= c <= 65535 else 0

    def read_repeated_elements(self, rf_obj: int, elem_size: int = 8,
                                max_count: int = 1024) -> List[int]:
        # 读 RepeatedField<T> 的所有元素地址 (引用类型) 或值 (基本类型).
        #
        # elem_size:
        # 8 → ref/T*/long/ulong/double  (read_u64)
        # 4 → int/uint/float            (read_u32)
        # 2 → short/ushort              (read_u16)
        # 1 → byte/sbyte/bool           (read_u8)
        # 返回值类型由调用者解读. 引用类型: 拿到的是 T 实例地址.
        cnt = self.read_repeated_count(rf_obj)
        if cnt == 0:
            return []
        cnt = min(cnt, max_count)
        arr = self.pm.read_u64(rf_obj + self.REPEATED_ARRAY_OFF)
        if not arr:
            return []
        max_len = self.pm.read_u32(arr + self.ARRAY_LENGTH_OFF) or 0
        cnt = min(cnt, max_len)
        if cnt <= 0:
            return []
        base = arr + self.ARRAY_ELEMS_OFF
        if elem_size == 8:
            reader = self.pm.read_u64
        elif elem_size == 4:
            reader = self.pm.read_u32
        elif elem_size == 2:
            reader = self.pm.read_u16
        elif elem_size == 1:
            reader = self.pm.read_u8
        else:
            raise ValueError(f"unsupported elem_size {elem_size}")
        out: List[int] = []
        for i in range(cnt):
            v = reader(base + i * elem_size)
            out.append(v if v is not None else 0)
        return out

    def read_repeated_field(self, obj: int, class_name: str, field_name: str,
                             elem_size: int = 8, max_count: int = 1024) -> List[int]:
        # 便捷: obj.field 是 RepeatedField<T>, 一次读所有元素.
        rf = self.read_ptr_field(obj, class_name, field_name)
        if not rf:
            return []
        return self.read_repeated_elements(rf, elem_size=elem_size, max_count=max_count)

    def read_repeated_struct_array(self, rf_obj: int, struct_size: int,
                                    max_count: int = 1024) -> List[bytes]:
        # RepeatedField<T> 包含 struct in-place, 不是引用.
        #
        # 常见于 Google.Protobuf RepeatedField<Primitive> / RepeatedField<SmallMessage>
        # (IL2CPP 不会把每个元素独立 new). 返回的元素 bytes 长度 == struct_size.
        cnt = self.read_repeated_count(rf_obj)
        if cnt == 0 or struct_size <= 0:
            return []
        cnt = min(cnt, max_count)
        arr = self.pm.read_u64(rf_obj + self.REPEATED_ARRAY_OFF)
        if not arr:
            return []
        max_len = self.pm.read_u32(arr + self.ARRAY_LENGTH_OFF) or 0
        cnt = min(cnt, max_len)
        if cnt <= 0:
            return []
        base = arr + self.ARRAY_ELEMS_OFF
        # Try one big read; fall back to per-element reads if it fails.
        total = cnt * struct_size
        big = self.pm.read_bytes(base, total)
        if big is not None and len(big) == total:
            return [big[i * struct_size:(i + 1) * struct_size] for i in range(cnt)]
        out: List[bytes] = []
        for i in range(cnt):
            blob = self.pm.read_bytes(base + i * struct_size, struct_size)
            if blob is None:
                out.append(b"\x00" * struct_size)
            else:
                out.append(blob)
        return out


# ---------- factory ----------

def open_resolver(dump_id: str = "ef9ef95a") -> StaticResolver:
    # 便捷工厂: 自动加载当前 dump 的 script.json + dump_cs_index.json.
    here = os.path.dirname(os.path.abspath(__file__))
    sj = os.path.join(here, "out", dump_id, "dumper_out", "script.json")
    dci_json = os.path.join(here, "out", dump_id, "dump_cs_index.json")
    si = ScriptIndex.load(sj)
    dci = DumpCsIndex.load_from_json(dci_json)
    pm = StarProcess()
    ga = next(m for m in pm.list_modules() if m.name.lower() == "gameassembly.dll")
    return StaticResolver(pm, ga.base, si, dci)


# ---------- self-test ----------

def _selftest():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--dump-id", default="ef9ef95a")
    p.add_argument("--known-uid", type=int, default=36668136)
    p.add_argument("--known-hp", type=int, default=None)
    args = p.parse_args()

    print("[init] loading resolver...")
    sr = open_resolver(args.dump_id)
    print(f"[init] GA=0x{sr.ga:X}")

    cs_klass = sr.resolve_klass("Zproto.CharSerialize")
    ufa_klass = sr.resolve_klass("Zproto.UserFightAttr")
    print(f"[klass] CharSerialize=0x{cs_klass:X}  UserFightAttr=0x{ufa_klass:X}")

    print("[scan] finding self CharSerialize (Attr deref validates)...")
    t0 = time.time()
    hits = sr.find_self("Zproto.CharSerialize", "Attr", "Zproto.UserFightAttr")
    print(f"[scan] {time.time()-t0:.1f}s -> {len(hits)} valid instances")

    for obj, attr in hits:
        cid = sr.read_field(obj, "Zproto.CharSerialize", "CharId")
        cur = sr.read_field(attr, "Zproto.UserFightAttr", "CurHp")
        mx = sr.read_field(attr, "Zproto.UserFightAttr", "MaxHp")
        tag = " *** SELF ***" if cid == args.known_uid else ""
        print(f"   obj=0x{obj:X}  CharId={cid}  HP={cur}/{mx}{tag}")

    self_hits = [h for h in hits if sr.read_field(h[0], "Zproto.CharSerialize", "CharId") == args.known_uid]
    if not self_hits:
        print("[FAIL] self not found")
        return 1
    if args.known_hp is not None:
        obj, attr = self_hits[0]
        hp = sr.read_field(attr, "Zproto.UserFightAttr", "CurHp")
        ok = hp == args.known_hp
        print(f"[verify HP={args.known_hp}] {'OK' if ok else 'FAIL'} (got {hp})")
    return 0


if __name__ == "__main__":
    raise SystemExit(_selftest())

