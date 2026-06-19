"""IL2CPP SDK Dumper — extract classes/fields/methods from live Unity IL2CPP process.

Algorithm based on Perfare/Il2CppDumper and frida-il2cpp-bridge:
  1. Find GameAssembly.dll module base
  2. Pattern scan for Il2CppMetadataRegistration / CodeRegistration
  3. Walk typeDefinitions → Il2CppClass → fields/methods
  4. For each class: read name, namespace, parent, instance_size, fields, methods

IL2CPP struct layout (x64):
  Il2CppClass:
    +0x00  Il2CppImage* image
    +0x08  void* gc_desc
    +0x10  const char* name
    +0x18  const char* namespaze
    +0x40  Il2CppClass* parent
    +0x80  FieldInfo* fields  (auto-offset resolves this)
    +0x100 uint16_t field_count
    +0x10C uint32_t instance_size
    +0x118 uint16_t method_count
    +0x130 Il2CppType* byval_arg  (type info)
"""

from __future__ import annotations

from typing import List

from ai_editor.sdk_dumper.base import (
    MemoryReader, SDKClass, SDKDumper, SDKField, SDKMethod, SDKResult,
    register_engine,
)


# IL2CPP class bitfield flags
_TYPE_ENUM = 0x08
_TYPE_VALUE = 0x10
_TYPE_INTERFACE = 0x20


@register_engine
class IL2CPPDumper(SDKDumper):
    ENGINE = "il2cpp"

    # Offsets vary by Unity version. These are common 2021-2024 x64 defaults.
    # Auto-detection adjusts them at runtime.
    _CLASS_NAME     = 0x10
    _CLASS_NS       = 0x18
    _CLASS_PARENT   = 0x40
    _CLASS_FIELDS   = 0x80
    _CLASS_FCOUNT   = 0x100
    _CLASS_SIZE     = 0x10C
    _CLASS_MCOUNT   = 0x118
    _CLASS_METHODS  = 0x98
    _CLASS_IMAGE    = 0x00
    _CLASS_FLAGS    = 0x114

    _FIELD_NAME     = 0x00
    _FIELD_OFFSET   = 0x18
    _FIELD_PARENT   = 0x10  # Il2CppClass* back-pointer
    _FIELD_STRIDE   = 0x20

    _METHOD_NAME    = 0x00
    _METHOD_PTR     = 0x08
    _METHOD_CLASS   = 0x10
    _METHOD_RET     = 0x18
    _METHOD_PCOUNT  = 0x2E
    _METHOD_FLAGS   = 0x30
    _METHOD_STRIDE  = 0x38

    _IMAGE_NAME     = 0x00

    def dump(self) -> SDKResult:
        r = self.reader
        ga_base, ga_size = r.get_module_base("gameassembly.dll")
        if not ga_base:
            self.result.errors.append("GameAssembly.dll not found")
            return self.result
        self._progress(0.05, "GameAssembly.dll found")

        classes = self._find_all_classes(ga_base, ga_size)
        self._progress(0.2, f"Found {len(classes)} class pointers")

        total = len(classes)
        for i, klass_ptr in enumerate(classes):
            if i % 200 == 0:
                self._progress(0.2 + 0.7 * (i / max(total, 1)),
                               f"Dumping class {i}/{total}")
            try:
                cls = self._dump_class(klass_ptr)
                if cls and cls.name and not cls.name.startswith("<"):
                    if cls.is_enum:
                        self.result.enums.append(cls)
                    else:
                        self.result.classes.append(cls)
            except Exception as exc:
                self._record_issue(f"IL2CPP class dump failed at {hex(klass_ptr)}", exc)
                continue

        self._progress(1.0, f"Done: {len(self.result.classes)} classes, {len(self.result.enums)} enums")
        return self.result

    def _find_all_classes(self, base: int, size: int) -> List[int]:
        """Scan for Il2CppClass pointers via assembly/image walking."""
        r = self.reader
        classes = []

        # Strategy: scan for Il2CppImage pointers, then walk their class arrays
        # Fallback: pattern scan for class vtable signatures
        # Most reliable: find s_Il2CppImages array or iterate type definitions

        # Try pattern: reference to "Assembly-CSharp" string
        asm_csharp = self._find_string_ref(base, size, b"Assembly-CSharp")
        if asm_csharp:
            classes.extend(self._walk_image_classes(asm_csharp))

        # Also scan for other assemblies
        for asm_name in [b"mscorlib", b"UnityEngine", b"UnityEngine.CoreModule"]:
            ref = self._find_string_ref(base, size, asm_name)
            if ref:
                classes.extend(self._walk_image_classes(ref))

        if not classes:
            # Brute force: scan memory for Il2CppClass-like structures
            classes = self._brute_scan_classes(base, size)

        return list(set(classes))

    def _find_string_ref(self, base: int, size: int, needle: bytes) -> int:
        """Find a string in the module, then find a pointer to it."""
        r = self.reader
        chunk_size = min(size, 0x2000000)  # 32MB chunks
        for offset in range(0, size, chunk_size):
            chunk = r.read(base + offset, min(chunk_size, size - offset))
            if not chunk:
                continue
            idx = chunk.find(needle + b"\x00")
            if idx >= 0:
                str_addr = base + offset + idx
                # Now scan for pointers to this string
                for ptr_off in range(0, min(size, 0x4000000), 0x1000):
                    block = r.read(base + ptr_off, 0x1000)
                    if not block:
                        continue
                    import struct as _st
                    for j in range(0, len(block) - 7, 8):
                        val = _st.unpack_from("<Q", block, j)[0]
                        if val == str_addr:
                            return base + ptr_off + j
        return 0

    def _walk_image_classes(self, image_name_ptr: int) -> List[int]:
        """Given a pointer to an image name, try to find the Il2CppImage and walk its classes."""
        r = self.reader
        classes = []
        # The Il2CppImage struct has name at offset 0, typeStart/typeCount further in
        # Walk backwards to find the Image struct base
        # Image pointer is at class+0x00, so scan for pointers to this image
        for nearby in range(image_name_ptr - 0x100, image_name_ptr + 0x8, 8):
            ptr = r.read_ptr(nearby)
            if ptr == image_name_ptr:
                # This might be Il2CppImage.name field → image base = nearby
                image_base = nearby
                # Read typeCount and walk
                type_count = r.read_u32(image_base + 0x1C)
                type_start = r.read_u32(image_base + 0x18)
                if 0 < type_count < 100000:
                    # Find the type definition table
                    # For now just collect what we can
                    break
        return classes

    def _brute_scan_classes(self, base: int, size: int) -> List[int]:
        """Brute force scan for Il2CppClass structures by signature."""
        r = self.reader
        classes = []
        # Il2CppClass has name pointer at +0x10 and namespace at +0x18
        # Both should point to readable ASCII strings in GameAssembly.dll
        scan_size = min(size, 0x8000000)  # 128MB max
        chunk = 0x10000
        for off in range(0, scan_size, chunk):
            block = r.read(base + off, chunk)
            if not block or len(block) < chunk:
                continue
            import struct as _st
            for j in range(0, len(block) - 0x120, 8):
                name_ptr = _st.unpack_from("<Q", block, j + self._CLASS_NAME)[0]
                ns_ptr = _st.unpack_from("<Q", block, j + self._CLASS_NS)[0]
                if not (base <= name_ptr <= base + size):
                    continue
                if ns_ptr != 0 and not (base <= ns_ptr <= base + size):
                    continue
                name = r.read_cstr(name_ptr, 64)
                if not name or not name.isascii() or len(name) < 2:
                    continue
                # Check instance_size is reasonable
                isize = _st.unpack_from("<I", block, j + self._CLASS_SIZE)[0]
                if isize > 0x10000:
                    continue
                classes.append(base + off + j)
                if len(classes) >= 50000:
                    return classes
        return classes

    def _dump_class(self, klass: int) -> SDKClass:
        r = self.reader
        name = r.read_cstr(r.read_ptr(klass + self._CLASS_NAME), 128)
        ns = r.read_cstr(r.read_ptr(klass + self._CLASS_NS), 128)
        parent_ptr = r.read_ptr(klass + self._CLASS_PARENT)
        parent = ""
        if parent_ptr:
            parent = r.read_cstr(r.read_ptr(parent_ptr + self._CLASS_NAME), 128)
        isize = r.read_u32(klass + self._CLASS_SIZE)
        flags = r.read_u32(klass + self._CLASS_FLAGS)

        cls = SDKClass(
            name=name, namespace=ns, parent=parent,
            address=klass, instance_size=isize,
            is_enum=bool(flags & _TYPE_ENUM),
            is_value_type=bool(flags & _TYPE_VALUE),
            is_interface=bool(flags & _TYPE_INTERFACE),
        )

        # Fields
        fcount = r.read_u16(klass + self._CLASS_FCOUNT)
        fields_ptr = r.read_ptr(klass + self._CLASS_FIELDS)
        if fields_ptr and 0 < fcount < 500:
            for fi in range(fcount):
                fa = fields_ptr + fi * self._FIELD_STRIDE
                fname = r.read_cstr(r.read_ptr(fa + self._FIELD_NAME), 128)
                foffset = r.read_i32(fa + self._FIELD_OFFSET)
                if fname:
                    cls.fields.append(SDKField(name=fname, offset=foffset))

        # Methods
        mcount = r.read_u16(klass + self._CLASS_MCOUNT)
        methods_ptr = r.read_ptr(klass + self._CLASS_METHODS)
        if methods_ptr and 0 < mcount < 500:
            for mi in range(mcount):
                # methods is an array of pointers to MethodInfo
                minfo_ptr = r.read_ptr(methods_ptr + mi * 8)
                if not minfo_ptr:
                    continue
                mname = r.read_cstr(r.read_ptr(minfo_ptr + self._METHOD_NAME), 128)
                maddr = r.read_ptr(minfo_ptr + self._METHOD_PTR)
                if mname:
                    cls.methods.append(SDKMethod(name=mname, address=maddr))

        return cls

    @staticmethod
    def detect(reader: MemoryReader) -> bool:
        base, size = reader.get_module_base("gameassembly.dll")
        return base > 0 and size > 0
