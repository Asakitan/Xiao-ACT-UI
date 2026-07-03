# -*- coding: utf-8 -*-
"""mem_viewer — read-only raw memory viewer over the Process Selector's
cached engine-A handle (mem_probe._pm._core._MP).

Standalone module: does not touch game-specific state (no plugin owner,
no MemStateBridge). Consumers just call status()/read_bytes()/read_value()
after a user has attached a process via the Process Selector panel
(gui_modules.sao_gui_process_selector).
"""
from __future__ import annotations

import struct
from typing import Any, Dict

#: Hard cap on a single read to keep RPC/tool payloads small and bounded.
MAX_READ_BYTES = 4096

_VALUE_FORMATS = {
    "u8": ("<B", 1), "i8": ("<b", 1),
    "u16": ("<H", 2), "i16": ("<h", 2),
    "u32": ("<I", 4), "i32": ("<i", 4),
    "u64": ("<Q", 8), "i64": ("<q", 8),
    "f32": ("<f", 4), "f64": ("<d", 8),
}


def _get_gp():
    from gui_modules.sao_gui_process_selector import get_cached_gp
    return get_cached_gp()


def status() -> Dict[str, Any]:
    """Attach status for the cached engine-A process handle."""
    from gui_modules.sao_gui_process_selector import get_cached_process_info
    return get_cached_process_info()


def _parse_address(address: Any) -> int:
    if isinstance(address, int):
        return address
    text = str(address or "").strip()
    if not text:
        raise ValueError("address is required")
    return int(text, 16) if text.lower().startswith("0x") else int(text, 16)


def read_bytes(address: Any, length: int) -> Dict[str, Any]:
    """Read raw bytes at `address`, returned as hex + printable-ASCII dump."""
    gp = _get_gp()
    if gp is None:
        return {"ok": False, "error": "no process attached (use Process Selector)"}
    try:
        addr = _parse_address(address)
    except Exception as exc:
        return {"ok": False, "error": f"bad address: {exc}"}
    n = max(1, min(int(length or 0), MAX_READ_BYTES))
    try:
        data = gp.read_bytes(addr, n)
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
    if data is None:
        return {"ok": False, "error": "read failed (unmapped or inaccessible address)"}
    ascii_repr = "".join(chr(b) if 32 <= b < 127 else "." for b in data)
    return {
        "ok": True,
        "address": f"0x{addr:X}",
        "length": len(data),
        "hex": data.hex(),
        "ascii": ascii_repr,
    }


def read_value(address: Any, dtype: str = "u32") -> Dict[str, Any]:
    """Read one typed scalar (u8/i8/u16/i16/u32/i32/u64/i64/f32/f64) at `address`."""
    gp = _get_gp()
    if gp is None:
        return {"ok": False, "error": "no process attached (use Process Selector)"}
    dtype = str(dtype or "u32").strip().lower()
    fmt = _VALUE_FORMATS.get(dtype)
    if fmt is None:
        return {"ok": False, "error": f"unsupported dtype: {dtype}",
                "supported": sorted(_VALUE_FORMATS)}
    try:
        addr = _parse_address(address)
    except Exception as exc:
        return {"ok": False, "error": f"bad address: {exc}"}
    pack_fmt, size = fmt
    try:
        data = gp.read_bytes(addr, size)
    except Exception as exc:
        return {"ok": False, "error": str(exc)}
    if not data or len(data) != size:
        return {"ok": False, "error": "read failed (unmapped or inaccessible address)"}
    value = struct.unpack(pack_fmt, data)[0]
    return {"ok": True, "address": f"0x{addr:X}", "dtype": dtype, "value": value}
