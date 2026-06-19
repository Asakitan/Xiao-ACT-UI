"""Unreal Engine SDK Dumper — extract GObjects/GNames/UClass hierarchy from live process.

Algorithm based on Dumper-7/UEDumper:
  1. Find main game module
  2. Pattern scan for GObjects (FUObjectArray) and GNames (FNamePool)
  3. Walk GObjects → UClass → UProperty/FProperty chain
  4. Resolve FName strings from GNames pool

UE4/5 key structures (x64):
  FUObjectArray:
    +0x00  int32 ObjFirstGCIndex
    +0x08  int32 ObjLastNonGCIndex
    +0x10  int32 MaxElements
    +0x14  int32 NumElements
    +0x18  FUObjectItem** Objects
  FUObjectItem:
    +0x00  UObject* Object
    +0x08  int32 Flags
    +0x18  (next = stride 0x18)
  UObject:
    +0x00  void** VTable
    +0x08  int32 ObjectFlags
    +0x0C  int32 InternalIndex
    +0x10  UClass* ClassPrivate
    +0x18  FName NamePrivate
    +0x20  UObject* OuterPrivate
  FName:
    +0x00  int32 ComparisonIndex
    +0x04  int32 Number
  FNamePool (UE4.23+):
    Block-based: Entries[BlockIndex][OffsetInBlock]
    Each entry: uint16 header (len in lower 6 bits of second byte), then UTF8/UTF16 chars
"""

from __future__ import annotations

from typing import Dict, List, Optional

from ai_editor.sdk_dumper.base import (
    MemoryReader, SDKClass, SDKDumper, SDKField, SDKMethod,
    register_engine,
)


@register_engine
class UnrealDumper(SDKDumper):
    ENGINE = "unreal"

    _GOBJECTS_PATTERNS = [
        # UE4 FUObjectArray pattern (48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1)
        (b"\x48\x8B\x05", b"\xFF\xFF\xFF\x00\x00\x00\x00"),
    ]

    _GNAMES_PATTERNS = [
        # FNamePool access pattern
        (b"\x48\x8D\x0D", b"\xFF\xFF\xFF\x00\x00\x00\x00"),
    ]

    _UOBJECT_ITEM_STRIDE = 0x18
    _UOBJECT_CLASS       = 0x10
    _UOBJECT_NAME        = 0x18
    _UOBJECT_OUTER       = 0x20

    _USTRUCT_SUPER        = 0x40
    _USTRUCT_CHILDREN     = 0x48
    _USTRUCT_CHILDREN_PROPS = 0x50
    _USTRUCT_SIZE         = 0x58

    _FPROPERTY_NEXT       = 0x20
    _FPROPERTY_NAME       = 0x28
    _FPROPERTY_OFFSET     = 0x4C

    _UFIELD_NEXT          = 0x28
    _UFIELD_CLASS         = 0x10

    def __init__(self, reader: MemoryReader) -> None:
        super().__init__(reader)
        self._gnames_addr = 0
        self._gobjects_addr = 0
        self._name_cache: Dict[int, str] = {}
        self._obj_cache: Dict[int, int] = {}

    def dump(self) -> SDKResult:
        r = self.reader

        game_module = self._find_game_module()
        if not game_module:
            self.result.errors.append("No game module found")
            return self.result
        mod_base, mod_size, mod_name = game_module
        self.result.process_name = mod_name
        self._progress(0.05, f"Module: {mod_name}")

        self._gobjects_addr = self._find_gobjects(mod_base, mod_size)
        self._gnames_addr = self._find_gnames(mod_base, mod_size)

        if not self._gobjects_addr:
            self.result.errors.append("GObjects not found")
            return self.result
        if not self._gnames_addr:
            self.result.errors.append("GNames not found")
            return self.result
        self._progress(0.15, f"GObjects={hex(self._gobjects_addr)} GNames={hex(self._gnames_addr)}")

        num_objects = r.read_i32(self._gobjects_addr + 0x14)
        objects_ptr = r.read_ptr(self._gobjects_addr + 0x18)
        if not objects_ptr or num_objects <= 0:
            self.result.errors.append("GObjects array empty")
            return self.result
        self._progress(0.2, f"{num_objects} objects in GObjects")

        uclasses = self._collect_uclasses(objects_ptr, num_objects)
        self._progress(0.5, f"Found {len(uclasses)} UClass objects")

        for i, (cls_addr, cls_name) in enumerate(uclasses):
            if i % 100 == 0:
                self._progress(0.5 + 0.45 * (i / max(len(uclasses), 1)),
                               f"Dumping {i}/{len(uclasses)}")
            try:
                cls = self._dump_uclass(cls_addr, cls_name)
                if cls:
                    self.result.classes.append(cls)
            except Exception as exc:
                self._record_issue(f"Unreal class dump failed for {cls_name} at {hex(cls_addr)}", exc)
                continue

        self._progress(1.0, f"Done: {len(self.result.classes)} classes")
        return self.result

    def _find_game_module(self):
        r = self.reader
        # Try common game exe names, or just use the first large module
        for name in ["gameassembly.dll", ""]:
            if name:
                base, size = r.get_module_base(name)
                if base:
                    return (base, size, name)
        # Fallback: try to find the main exe module
        try:
            from mem_probe.process import enum_modules
            reader = self.reader
            if hasattr(reader, 'pid'):
                for mod in enum_modules(reader.pid):
                    if mod.get("name", "").lower().endswith(".exe"):
                        return (mod["base"], mod["size"], mod["name"])
                    if mod.get("size", 0) > 50_000_000:
                        return (mod["base"], mod["size"], mod["name"])
        except Exception as exc:
            self._record_issue("Failed to enumerate modules for Unreal detection", exc)
        return None

    def _find_gobjects(self, base: int, size: int) -> int:
        r = self.reader
        import struct
        for off in range(0, min(size, 0x4000000), 0x1000):
            chunk = r.read(base + off, 0x1000)
            if not chunk:
                continue
            # Search for: 48 8B 05 xx xx xx xx (mov rax, [rip+disp32])
            # followed by array access patterns
            idx = 0
            while idx < len(chunk) - 10:
                idx = chunk.find(b"\x48\x8B\x05", idx)
                if idx < 0:
                    break
                disp = struct.unpack_from("<i", chunk, idx + 3)[0]
                target = base + off + idx + 7 + disp
                # Validate: target should have reasonable NumElements at +0x14
                num = r.read_i32(target + 0x14)
                ptr = r.read_ptr(target + 0x18)
                if 1000 < num < 10_000_000 and ptr > 0x10000:
                    return target
                idx += 1
        return 0

    def _find_gnames(self, base: int, size: int) -> int:
        r = self.reader
        import struct
        for off in range(0, min(size, 0x4000000), 0x1000):
            chunk = r.read(base + off, 0x1000)
            if not chunk:
                continue
            idx = 0
            while idx < len(chunk) - 10:
                idx = chunk.find(b"\x48\x8D\x0D", idx)
                if idx < 0:
                    break
                disp = struct.unpack_from("<i", chunk, idx + 3)[0]
                target = base + off + idx + 7 + disp
                # FNamePool: first block pointer should be valid
                block0 = r.read_ptr(target + 0x10)
                if block0 > 0x10000:
                    test_name = self._read_fname_from_pool(target, 0)
                    if test_name == "None":
                        return target
                idx += 1
        return 0

    def _read_fname(self, index: int) -> str:
        if index in self._name_cache:
            return self._name_cache[index]
        name = self._read_fname_from_pool(self._gnames_addr, index)
        self._name_cache[index] = name
        return name

    def _read_fname_from_pool(self, pool: int, index: int) -> str:
        r = self.reader
        block_idx = index >> 16
        offset_in_block = index & 0xFFFF
        block_ptr = r.read_ptr(pool + 0x10 + block_idx * 8)
        if not block_ptr:
            return ""
        entry_addr = block_ptr + offset_in_block * 2
        header = r.read_u16(entry_addr)
        length = header >> 6
        if length <= 0 or length > 256:
            return ""
        raw = r.read(entry_addr + 2, length)
        return raw.decode("utf-8", errors="replace") if raw else ""

    def _collect_uclasses(self, objects_ptr: int, num: int) -> List:
        r = self.reader
        classes = []
        # GObjects is chunked: each chunk is 64K elements
        chunk_size = 0x10000
        for chunk_idx in range(0, (num + chunk_size - 1) // chunk_size):
            chunk_ptr = r.read_ptr(objects_ptr + chunk_idx * 8)
            if not chunk_ptr:
                continue
            start = chunk_idx * chunk_size
            end = min(start + chunk_size, num)
            for i in range(start, end):
                local_idx = i - start
                item_addr = chunk_ptr + local_idx * self._UOBJECT_ITEM_STRIDE
                obj = r.read_ptr(item_addr)
                if not obj:
                    continue
                obj_class = r.read_ptr(obj + self._UOBJECT_CLASS)
                if obj_class == obj:
                    # This object IS its own class → it's a UClass
                    name_idx = r.read_i32(obj + self._UOBJECT_NAME)
                    name = self._read_fname(name_idx)
                    if name:
                        classes.append((obj, name))
        return classes

    def _dump_uclass(self, cls_addr: int, cls_name: str) -> Optional[SDKClass]:
        r = self.reader
        super_ptr = r.read_ptr(cls_addr + self._USTRUCT_SUPER)
        parent = ""
        if super_ptr:
            pname_idx = r.read_i32(super_ptr + self._UOBJECT_NAME)
            parent = self._read_fname(pname_idx)

        struct_size = r.read_i32(cls_addr + self._USTRUCT_SIZE)

        cls = SDKClass(
            name=cls_name, parent=parent,
            address=cls_addr, instance_size=struct_size,
        )

        # Walk FProperty chain (UE4.25+ uses ChildProperties, older uses Children)
        prop = r.read_ptr(cls_addr + self._USTRUCT_CHILDREN_PROPS)
        visited = 0
        while prop and visited < 500:
            name_idx = r.read_i32(prop + self._FPROPERTY_NAME)
            fname = self._read_fname(name_idx)
            offset = r.read_i32(prop + self._FPROPERTY_OFFSET)
            if fname:
                cls.fields.append(SDKField(name=fname, offset=offset))
            prop = r.read_ptr(prop + self._FPROPERTY_NEXT)
            visited += 1

        # Walk UFunction children for methods
        child = r.read_ptr(cls_addr + self._USTRUCT_CHILDREN)
        visited = 0
        while child and visited < 500:
            child_class = r.read_ptr(child + self._UFIELD_CLASS)
            cname_idx = r.read_i32(child + self._UOBJECT_NAME)
            cname = self._read_fname(cname_idx)
            if child_class and cname:
                # Check if this is a UFunction
                ccls_name_idx = r.read_i32(child_class + self._UOBJECT_NAME)
                ccls_name = self._read_fname(ccls_name_idx)
                if "Function" in ccls_name:
                    func_addr = r.read_ptr(child + 0xB0)  # UFunction.Func
                    cls.methods.append(SDKMethod(name=cname, address=func_addr or 0))
            child = r.read_ptr(child + self._UFIELD_NEXT)
            visited += 1

        return cls

    @staticmethod
    def detect(reader: MemoryReader) -> bool:
        # UE games typically have UE4/UE5 specific DLLs or patterns
        for name in ["ue4-win64-shipping.exe", "unrealengine.dll"]:
            base, _ = reader.get_module_base(name)
            if base:
                return True
        # Check for common UE module names in loaded modules
        try:
            from mem_probe.process import enum_modules
            if hasattr(reader, 'pid'):
                names = [m.get("name", "").lower() for m in enum_modules(reader.pid)]
                ue_indicators = ["d3d11.dll", "d3d12.dll", "xaudio2_9.dll"]
                # Check if main exe is very large (typical for UE shipping builds)
                for m in enum_modules(reader.pid):
                    if m.get("name", "").lower().endswith(".exe") and m.get("size", 0) > 100_000_000:
                        return True
        except Exception:
            return False
        return False
