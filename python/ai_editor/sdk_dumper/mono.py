"""Mono SDK Dumper — extract classes from live Unity Mono process.

Algorithm based on frida-mono-dump and Cheat Engine mono helpers:
  1. Find mono-2.0-bdwgc.dll (or mono.dll) module
  2. Locate mono_get_root_domain export
  3. Walk domain → assemblies → images → classes
  4. For each class: name, namespace, parent, fields, methods

Mono struct layout (x64):
  MonoDomain:
    +0x00  ...
    +0xC8  GSList* domain_assemblies
  MonoAssembly:
    +0x00  ...
    +0x60  MonoImage* image
  MonoImage:
    +0x00  ...
    +0x18  const char* name
    +0x4C0 MonoClass** class_cache  (HashTable or linear)
  MonoClass:
    +0x00  ...
    +0x08  MonoClass* element_class
    +0x2C  const char* name
    +0x34  const char* name_space
    +0x100 MonoClassField* fields
    +0x108 MonoMethod** methods
    +0x10C uint32_t field_count
    +0x110 uint32_t method_count
    +0x5C  uint32_t instance_size
"""

from __future__ import annotations

from typing import List, Set

from ai_editor.sdk_dumper.base import (
    MemoryReader, SDKClass, SDKDumper, SDKField, SDKMethod,
    register_engine,
)


@register_engine
class MonoDumper(SDKDumper):
    ENGINE = "mono"

    # Offsets for Mono runtime (Unity 2019-2022 x64 typical)
    _DOMAIN_ASSEMBLIES = 0xC8
    _ASSEMBLY_IMAGE    = 0x60
    _ASSEMBLY_NEXT     = 0x00

    _IMAGE_NAME        = 0x18
    _IMAGE_CLASS_CACHE = 0x4C0
    _IMAGE_CLASS_COUNT = 0x18

    _CLASS_NAME        = 0x2C
    _CLASS_NS          = 0x34
    _CLASS_PARENT      = 0x20
    _CLASS_FIELDS      = 0x100
    _CLASS_FCOUNT      = 0x10C
    _CLASS_METHODS     = 0x108
    _CLASS_MCOUNT      = 0x110
    _CLASS_SIZE        = 0x5C
    _CLASS_FLAGS       = 0x24
    _CLASS_NESTED      = 0x68

    _FIELD_NAME        = 0x00
    _FIELD_TYPE        = 0x08
    _FIELD_OFFSET      = 0x18
    _FIELD_STRIDE      = 0x20

    _METHOD_NAME       = 0x00
    _METHOD_ADDR       = 0x08
    _METHOD_FLAGS      = 0x14
    _METHOD_STRIDE     = 0x28

    def dump(self) -> SDKResult:
        r = self.reader

        mono_base, mono_size = self._find_mono_module()
        if not mono_base:
            self.result.errors.append("Mono runtime DLL not found")
            return self.result
        self._progress(0.05, "Mono module found")

        root_domain = self._find_root_domain(mono_base, mono_size)
        if not root_domain:
            self.result.errors.append("mono_get_root_domain not found")
            return self.result
        self._progress(0.1, f"Root domain: {hex(root_domain)}")

        images = self._walk_assemblies(root_domain)
        self._progress(0.2, f"Found {len(images)} assemblies")

        seen: Set[int] = set()
        total_images = len(images)
        for idx, (image_ptr, image_name) in enumerate(images):
            self._progress(0.2 + 0.7 * (idx / max(total_images, 1)),
                           f"Dumping {image_name}")
            classes = self._walk_image_classes(image_ptr)
            for klass in classes:
                if klass in seen:
                    continue
                seen.add(klass)
                try:
                    cls = self._dump_class(klass)
                    if cls and cls.name:
                        if cls.is_enum:
                            self.result.enums.append(cls)
                        else:
                            self.result.classes.append(cls)
                except Exception as exc:
                    self._record_issue(f"Mono class dump failed at {hex(klass)}", exc)
                    continue

        self._progress(1.0, f"Done: {len(self.result.classes)} classes")
        return self.result

    def _find_mono_module(self):
        for name in ["mono-2.0-bdwgc.dll", "mono.dll", "libmonobdwgc-2.0.so"]:
            base, size = self.reader.get_module_base(name)
            if base:
                return base, size
        return 0, 0

    def _find_root_domain(self, base: int, size: int) -> int:
        r = self.reader
        # Pattern scan for mono_get_root_domain — typically:
        #   48 8B 05 xx xx xx xx  (mov rax, [rip+disp])  → rax = domain ptr
        #   C3                    (ret)
        pattern = bytes([0x48, 0x8B, 0x05])
        for off in range(0, min(size, 0x400000), 0x1000):
            chunk = r.read(base + off, 0x1000)
            if not chunk:
                continue
            idx = 0
            while idx < len(chunk) - 10:
                idx = chunk.find(pattern, idx)
                if idx < 0:
                    break
                # Check if followed by C3 (ret) within a few bytes
                ret_range = chunk[idx+7:idx+16]
                if b"\xC3" in ret_range:
                    import struct
                    disp = struct.unpack_from("<i", chunk, idx + 3)[0]
                    domain_ptr_addr = base + off + idx + 7 + disp
                    domain = r.read_ptr(domain_ptr_addr)
                    if domain > 0x10000:
                        return domain
                idx += 1
        return 0

    def _walk_assemblies(self, domain: int) -> List:
        r = self.reader
        images = []
        # GSList* at domain + _DOMAIN_ASSEMBLIES
        glist = r.read_ptr(domain + self._DOMAIN_ASSEMBLIES)
        visited = 0
        while glist and visited < 500:
            assembly = r.read_ptr(glist)
            if assembly:
                image = r.read_ptr(assembly + self._ASSEMBLY_IMAGE)
                if image:
                    name = r.read_cstr(r.read_ptr(image + self._IMAGE_NAME), 128)
                    if name:
                        images.append((image, name))
            glist = r.read_ptr(glist + 8)  # GSList.next
            visited += 1
        return images

    def _walk_image_classes(self, image: int) -> List[int]:
        r = self.reader
        classes = []
        # Walk the internal class cache — varies by Mono version
        # Try: MonoImage has a class_cache (GHashTable or sorted array)
        cache_ptr = r.read_ptr(image + self._IMAGE_CLASS_CACHE)
        if not cache_ptr:
            return classes
        # GHashTable: size at +0x18, buckets at +0x20
        table_size = r.read_u32(cache_ptr + 0x18)
        buckets = r.read_ptr(cache_ptr + 0x20)
        if not buckets or table_size > 100000:
            return classes
        for i in range(min(table_size, 50000)):
            entry = r.read_ptr(buckets + i * 8)
            depth = 0
            while entry and depth < 20:
                klass = r.read_ptr(entry + 0x10)  # GHashNode.value
                if klass and klass > 0x10000:
                    classes.append(klass)
                entry = r.read_ptr(entry)  # GHashNode.next
                depth += 1
        return classes

    def _dump_class(self, klass: int) -> SDKClass:
        r = self.reader
        # Mono stores name/namespace as direct char* (not pointer to pointer)
        name_ptr = r.read_ptr(klass + self._CLASS_NAME)
        name = r.read_cstr(name_ptr, 128) if name_ptr else ""
        ns_ptr = r.read_ptr(klass + self._CLASS_NS)
        ns = r.read_cstr(ns_ptr, 128) if ns_ptr else ""

        parent_ptr = r.read_ptr(klass + self._CLASS_PARENT)
        parent = ""
        if parent_ptr:
            pname_ptr = r.read_ptr(parent_ptr + self._CLASS_NAME)
            parent = r.read_cstr(pname_ptr, 128) if pname_ptr else ""

        isize = r.read_u32(klass + self._CLASS_SIZE)
        flags = r.read_u32(klass + self._CLASS_FLAGS)

        cls = SDKClass(
            name=name, namespace=ns, parent=parent,
            address=klass, instance_size=isize,
            is_enum=(flags & 0x100) != 0,
            is_value_type=(flags & 0x200) != 0,
        )

        # Fields
        fcount = r.read_u32(klass + self._CLASS_FCOUNT)
        fields_ptr = r.read_ptr(klass + self._CLASS_FIELDS)
        if fields_ptr and 0 < fcount < 500:
            for i in range(fcount):
                fa = fields_ptr + i * self._FIELD_STRIDE
                fname_ptr = r.read_ptr(fa + self._FIELD_NAME)
                fname = r.read_cstr(fname_ptr, 128) if fname_ptr else ""
                foffset = r.read_i32(fa + self._FIELD_OFFSET)
                if fname:
                    cls.fields.append(SDKField(name=fname, offset=foffset))

        # Methods
        mcount = r.read_u32(klass + self._CLASS_MCOUNT)
        methods_ptr = r.read_ptr(klass + self._CLASS_METHODS)
        if methods_ptr and 0 < mcount < 500:
            for i in range(mcount):
                minfo = r.read_ptr(methods_ptr + i * 8)
                if not minfo:
                    continue
                mname_ptr = r.read_ptr(minfo + self._METHOD_NAME)
                mname = r.read_cstr(mname_ptr, 128) if mname_ptr else ""
                maddr = r.read_ptr(minfo + self._METHOD_ADDR)
                if mname:
                    cls.methods.append(SDKMethod(name=mname, address=maddr))

        return cls

    @staticmethod
    def detect(reader: MemoryReader) -> bool:
        for name in ["mono-2.0-bdwgc.dll", "mono.dll"]:
            base, size = reader.get_module_base(name)
            if base > 0:
                return True
        return False
