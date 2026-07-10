"""SDK Dumper base — memory reader interface, engine detection, result model."""

from __future__ import annotations

import ctypes
import json
import logging
import os
import struct
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple


logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Memory reader interface
# ---------------------------------------------------------------------------

class MemoryReader(ABC):
    """Abstract memory reader. Subclass for different backends."""

    @abstractmethod
    def read(self, addr: int, size: int) -> bytes:
        ...

    def read_ptr(self, addr: int) -> int:
        d = self.read(addr, 8)
        return struct.unpack_from("<Q", d)[0] if len(d) == 8 else 0

    def read_u32(self, addr: int) -> int:
        d = self.read(addr, 4)
        return struct.unpack_from("<I", d)[0] if len(d) == 4 else 0

    def read_i32(self, addr: int) -> int:
        d = self.read(addr, 4)
        return struct.unpack_from("<i", d)[0] if len(d) == 4 else 0

    def read_u16(self, addr: int) -> int:
        d = self.read(addr, 2)
        return struct.unpack_from("<H", d)[0] if len(d) == 2 else 0

    def read_u8(self, addr: int) -> int:
        d = self.read(addr, 1)
        return d[0] if d else 0

    def read_f32(self, addr: int) -> float:
        d = self.read(addr, 4)
        return struct.unpack_from("<f", d)[0] if len(d) == 4 else 0.0

    def read_cstr(self, addr: int, max_len: int = 256) -> str:
        if addr == 0:
            return ""
        d = self.read(addr, max_len)
        if not d:
            return ""
        end = d.find(b"\x00")
        return d[:end].decode("utf-8", errors="replace") if end >= 0 else d.decode("utf-8", errors="replace")

    def read_wstr(self, addr: int, max_chars: int = 128) -> str:
        if addr == 0:
            return ""
        d = self.read(addr, max_chars * 2)
        if not d:
            return ""
        chars = []
        for i in range(0, len(d) - 1, 2):
            c = struct.unpack_from("<H", d, i)[0]
            if c == 0:
                break
            chars.append(chr(c))
        return "".join(chars)

    def pattern_scan(self, start: int, size: int, pattern: bytes, mask: bytes = None) -> int:
        chunk = self.read(start, size)
        if not chunk:
            return 0
        plen = len(pattern)
        for i in range(len(chunk) - plen + 1):
            match = True
            for j in range(plen):
                if mask and mask[j:j+1] == b"?":
                    continue
                if chunk[i + j] != pattern[j]:
                    match = False
                    break
            if match:
                return start + i
        return 0

    def get_module_base(self, name: str) -> Tuple[int, int]:
        """Return (base_address, size) for a module. Override in subclass."""
        return (0, 0)

    def close(self):
        return None


class ProcessReader(MemoryReader):
    """Read memory via mem_probe rt_io (Engine A)."""

    def __init__(self, pid: int) -> None:
        self.pid = pid
        self._drv = None
        self._lease = None
        self._attached = False
        self._module_cache: Dict[str, Tuple[int, int]] = {}

    def _ensure_attached(self) -> None:
        if self._attached:
            return
        lease = None
        try:
            from mem_probe import rt_io as drv
            from mem_probe.process import TargetLease
            lease = TargetLease(self.pid)
            if not lease.ensure_active():
                raise RuntimeError("driver ensure/attach returned false")
            self._drv = drv
            self._lease = lease
            self._attached = True
        except Exception as exc:
            if lease is not None:
                lease.close()
            raise RuntimeError(f"Failed to attach to pid {self.pid}: {exc}") from exc

    def read(self, addr: int, size: int) -> bytes:
        if addr == 0 or size <= 0:
            return b""
        self._ensure_attached()
        try:
            with self._lease.operation() as drv:
                data = drv.read(addr, size)
            return data if data is not None else b""
        except Exception as exc:
            logger.debug("ProcessReader.read failed at %s size=%s: %s",
                         hex(addr), size, exc)
            return b""

    def get_module_base(self, name: str) -> Tuple[int, int]:
        name_lower = name.lower()
        if name_lower in self._module_cache:
            return self._module_cache[name_lower]
        self._ensure_attached()
        try:
            with self._lease.operation():
                from mem_probe._pm._core import PageResolver
                modules = PageResolver().enumerate_modules(self.pid)
            for mod_name, base, size in modules:
                self._module_cache[str(mod_name or "").lower()] = (
                    int(base), int(size)
                )
            return self._module_cache.get(name_lower, (0, 0))
        except Exception as exc:
            logger.debug("ProcessReader.get_module_base failed for %s: %s",
                         name, exc)
            return (0, 0)

    def close(self):
        lease = self._lease
        self._lease = None
        if lease is not None:
            lease.close()
        self._attached = False
        self._drv = None


# ---------------------------------------------------------------------------
# SDK result model
# ---------------------------------------------------------------------------

@dataclass
class SDKField:
    name: str
    offset: int
    type_name: str = ""
    size: int = 0
    is_static: bool = False
    static_addr: int = 0

    def to_dict(self) -> Dict:
        d = {"name": self.name, "offset": hex(self.offset), "type": self.type_name}
        if self.size:
            d["size"] = self.size
        if self.is_static:
            d["static"] = True
            d["addr"] = hex(self.static_addr)
        return d


@dataclass
class SDKMethod:
    name: str
    address: int = 0
    return_type: str = ""
    params: List[str] = field(default_factory=list)
    is_static: bool = False
    is_virtual: bool = False
    vtable_index: int = -1

    def to_dict(self) -> Dict:
        d = {"name": self.name, "addr": hex(self.address)}
        if self.return_type:
            d["ret"] = self.return_type
        if self.params:
            d["params"] = self.params
        if self.is_static:
            d["static"] = True
        if self.is_virtual:
            d["virtual"] = True
            d["vindex"] = self.vtable_index
        return d


@dataclass
class SDKClass:
    name: str
    namespace: str = ""
    parent: str = ""
    address: int = 0
    instance_size: int = 0
    fields: List[SDKField] = field(default_factory=list)
    methods: List[SDKMethod] = field(default_factory=list)
    is_enum: bool = False
    is_interface: bool = False
    is_value_type: bool = False

    @property
    def full_name(self) -> str:
        return f"{self.namespace}.{self.name}" if self.namespace else self.name

    def to_dict(self) -> Dict:
        d: Dict[str, Any] = {"name": self.name, "addr": hex(self.address)}
        if self.namespace:
            d["namespace"] = self.namespace
        if self.parent:
            d["parent"] = self.parent
        if self.instance_size:
            d["size"] = self.instance_size
        if self.is_enum:
            d["enum"] = True
        if self.is_value_type:
            d["value_type"] = True
        if self.fields:
            d["fields"] = [f.to_dict() for f in self.fields]
        if self.methods:
            d["methods"] = [m.to_dict() for m in self.methods]
        return d


@dataclass
class SDKResult:
    engine: str
    process_name: str = ""
    timestamp: float = 0.0
    classes: List[SDKClass] = field(default_factory=list)
    enums: List[SDKClass] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)

    def to_dict(self) -> Dict:
        return {
            "engine": self.engine,
            "process": self.process_name,
            "timestamp": self.timestamp,
            "class_count": len(self.classes),
            "enum_count": len(self.enums),
            "classes": [c.to_dict() for c in self.classes],
            "enums": [e.to_dict() for e in self.enums],
            "errors": self.errors,
        }

    def to_json(self, indent: int = 2) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, indent=indent)

    def to_header(self) -> str:
        lines = [f"// SDK Dump — {self.engine}", f"// {self.process_name}",
                 f"// {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(self.timestamp))}",
                 f"// Classes: {len(self.classes)}, Enums: {len(self.enums)}", "", "#pragma once", ""]
        for cls in sorted(self.classes, key=lambda c: c.full_name):
            parent = f" : public {cls.parent}" if cls.parent else ""
            lines.append(f"// Size: 0x{cls.instance_size:X}")
            lines.append(f"class {cls.name}{parent} {{")
            lines.append("public:")
            for f in cls.fields:
                offset = f"0x{f.offset:04X}" if not f.is_static else "static"
                lines.append(f"    {f.type_name} {f.name}; // {offset}")
            if cls.fields and cls.methods:
                lines.append("")
            for m in cls.methods:
                params_str = ", ".join(m.params) if m.params else ""
                static = "static " if m.is_static else ""
                virtual = "virtual " if m.is_virtual else ""
                lines.append(f"    {virtual}{static}{m.return_type} {m.name}({params_str}); // {hex(m.address)}")
            lines.append("};")
            lines.append("")
        return "\n".join(lines)

    def save(self, path: str, fmt: str = "json") -> str:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        content = self.to_json() if fmt == "json" else self.to_header()
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        return path


# ---------------------------------------------------------------------------
# Dumper base class
# ---------------------------------------------------------------------------

class SDKDumper(ABC):
    ENGINE = "unknown"

    def __init__(self, reader: MemoryReader) -> None:
        self.reader = reader
        self.result = SDKResult(engine=self.ENGINE, timestamp=time.time())
        self._progress_cb = None

    def set_progress(self, cb) -> None:
        self._progress_cb = cb

    def _progress(self, pct: float, msg: str = "") -> None:
        if self._progress_cb:
            self._progress_cb(pct, msg)

    def _record_issue(self, message: str, exc: Optional[Exception] = None) -> None:
        detail = f"{message}: {exc}" if exc else message
        if detail not in self.result.errors:
            self.result.errors.append(detail)
        logger.debug("SDK dumper issue [%s]: %s", self.ENGINE, detail)

    @abstractmethod
    def dump(self) -> SDKResult:
        ...

    @staticmethod
    @abstractmethod
    def detect(reader: MemoryReader) -> bool:
        ...


# ---------------------------------------------------------------------------
# Engine registry
# ---------------------------------------------------------------------------

_ENGINES: Dict[str, type] = {}


def register_engine(cls: type) -> type:
    _ENGINES[cls.ENGINE] = cls
    return cls


def list_engines() -> List[str]:
    return sorted(_ENGINES.keys())


def detect_engine(pid: int) -> str:
    reader = ProcessReader(pid)
    try:
        for name, cls in _ENGINES.items():
            try:
                if cls.detect(reader):
                    return name
            except Exception as exc:
                logger.debug("SDK engine detection failed for %s on pid %s: %s",
                             name, pid, exc)
                continue
    finally:
        reader.close()
    return "unknown"


def create_dumper(engine: str, pid: int) -> SDKDumper:
    cls = _ENGINES.get(engine)
    if not cls:
        raise ValueError(f"Unknown engine: {engine}. Available: {list_engines()}")
    reader = ProcessReader(pid)
    return cls(reader)


# Import engine modules to trigger registration
def _load_engines():
    try:
        from ai_editor.sdk_dumper import il2cpp
    except Exception as exc:
        logger.debug("Failed to load IL2CPP SDK dumper: %s", exc)
    try:
        from ai_editor.sdk_dumper import mono
    except Exception as exc:
        logger.debug("Failed to load Mono SDK dumper: %s", exc)
    try:
        from ai_editor.sdk_dumper import unreal
    except Exception as exc:
        logger.debug("Failed to load Unreal SDK dumper: %s", exc)
    try:
        from ai_editor.sdk_dumper import source
    except Exception as exc:
        logger.debug("Failed to load Source SDK dumper: %s", exc)

_load_engines()
