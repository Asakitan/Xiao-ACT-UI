# -*- coding: utf-8 -*-
from __future__ import annotations
import struct
from typing import List, Optional, Tuple
from mem_probe import rt_io

_kv8, _kv4, _kvb = rt_io._r1_v8, rt_io._r1_v4, lambda va, n: rt_io._r1_r(va, n, 2)


class PageResolver:

    def __init__(self) -> None:
        self._cr3 = self._pid = self._ep = 0

    def find_process_by_name(self, name: str) -> Optional[Tuple[int, int]]:
        if not rt_io.ensure_loaded(rt_io.ENGINE_READONLY):
            return None
        if not rt_io._resolve_ep_offsets():
            return None
        off_pid, off_apl = rt_io._OFF_PID, rt_io._OFF_APL
        off_dtb, off_name = rt_io._OFF_DTB, rt_io._OFF_NAME
        if not (off_pid and off_apl and off_name):
            return None
        ps_ptr = rt_io._r1_fp()
        if not ps_ptr:
            return None
        system_ep = _kv8(ps_ptr)
        if not system_ep:
            return None
        target = name.lower()
        target_base = target.rsplit(".exe", 1)[0] if target.endswith(".exe") else target
        flink = _kv8(system_ep + off_apl)
        visited: set[int] = set()
        while flink and flink not in visited:
            visited.add(flink)
            ep = flink - off_apl
            raw = _kvb(ep + off_name, 15)
            if raw:
                img = raw.split(b"\x00", 1)[0].decode("ascii", "ignore").lower()
                if img and (img == target or img.startswith(target_base)):
                    pid, cr3 = _kv4(ep + off_pid), _kv8(ep + off_dtb)
                    if pid and cr3:
                        self._pid, self._cr3, self._ep = pid, cr3, ep
                        return (pid, cr3)
            flink = _kv8(flink)
            if len(visited) > 2000:
                break
        return None

    def enumerate_processes(self) -> List[Tuple[str, int]]:
        if not rt_io.ensure_loaded(rt_io.ENGINE_READONLY):
            return []
        if not rt_io._resolve_ep_offsets():
            return []
        off_pid, off_apl = rt_io._OFF_PID, rt_io._OFF_APL
        off_name = rt_io._OFF_NAME
        if not (off_pid and off_apl and off_name):
            return []
        ps_ptr = rt_io._r1_fp()
        if not ps_ptr:
            return []
        system_ep = _kv8(ps_ptr)
        if not system_ep:
            return []
        result: List[Tuple[str, int]] = []
        flink = _kv8(system_ep + off_apl)
        visited: set[int] = set()
        while flink and flink not in visited:
            visited.add(flink)
            ep = flink - off_apl
            raw = _kvb(ep + off_name, 15)
            if raw:
                img = raw.split(b"\x00", 1)[0].decode("ascii", "ignore")
                pid = _kv4(ep + off_pid)
                if img and pid and pid > 0:
                    result.append((img, pid))
            flink = _kv8(flink)
            if len(visited) > 4000:
                break
        return result

    def read_memory(self, cr3: int, addr: int, size: int) -> Optional[bytes]:
        if size <= 0:
            return b""
        result = bytearray()
        remaining, cur = size, addr
        while remaining > 0:
            pa = rt_io._r1_w(cr3, cur)
            if pa is None or pa < 0x100000 or 0xFEC00000 <= pa <= 0xFEE00000:
                return None
            chunk = min(remaining, 0x1000 - (cur & 0xFFF))
            data = rt_io._r1_r(pa, chunk, 1)
            if data is None:
                return None
            result.extend(data)
            remaining -= chunk
            cur += chunk
        return bytes(result)

    def enumerate_modules(self, pid: int) -> List[Tuple[str, int, int]]:
        if not rt_io._OFF_PEB:
            rt_io._resolve_ep_offsets()
        if not rt_io._OFF_PEB:
            return []
        ep, cr3 = rt_io._r1_fe(pid)
        if not ep or not cr3:
            return []
        peb_va = _kv8(ep + rt_io._OFF_PEB)
        if not peb_va:
            return []
        d = self.read_memory(cr3, peb_va + 0x18, 8)
        if not d:
            return []
        ldr = struct.unpack("<Q", d)[0]
        if not ldr:
            return []
        d = self.read_memory(cr3, ldr + 0x10, 8)
        if not d:
            return []
        list_head = ldr + 0x10
        flink = struct.unpack("<Q", d)[0]
        modules: List[Tuple[str, int, int]] = []
        visited: set[int] = set()
        while flink and flink != list_head and flink not in visited:
            visited.add(flink)
            entry = self.read_memory(cr3, flink, 0x70)
            if not entry or len(entry) < 0x70:
                break
            dll_base = struct.unpack_from("<Q", entry, 0x30)[0]
            img_size = struct.unpack_from("<I", entry, 0x40)[0]
            name_len = struct.unpack_from("<H", entry, 0x58)[0]
            name_buf = struct.unpack_from("<Q", entry, 0x60)[0]
            mod_name = ""
            if name_len and name_buf:
                raw = self.read_memory(cr3, name_buf, min(name_len, 520))
                if raw:
                    mod_name = raw.decode("utf-16-le", errors="ignore")
            if dll_base:
                modules.append((mod_name, dll_base, img_size))
            nd = self.read_memory(cr3, flink, 8)
            if not nd:
                break
            flink = struct.unpack("<Q", nd)[0]
            if len(visited) > 1024:
                break
        return modules

    def as_game_process(self, pid: int):
        ep, cr3 = rt_io._r1_fe(pid)
        if not ep or not cr3:
            raise RuntimeError(f"pid {pid}")
        return _MP(pid, cr3, self)


class _MP:

    def __init__(self, pid: int, cr3: int, sp) -> None:
        self._pid, self._cr3, self._sp = pid, cr3, sp

    @property
    def pid(self) -> int:
        return self._pid

    @property
    def handle(self) -> int:
        return 0

    @property
    def name(self) -> str:
        return ""

    @property
    def memory_tier(self) -> str:
        return rt_io.memory_tier()

    def read_bytes(self, addr: int, n: int) -> Optional[bytes]:
        return b"" if n <= 0 else self._sp.read_memory(self._cr3, addr, n)

    def read_bytes_into(self, addr: int, buf, n: Optional[int] = None) -> int:
        want = len(buf) if n is None else int(n)
        data = self.read_bytes(addr, want)
        if data is None:
            return 0
        buf[:len(data)] = data
        return len(data)

    def read_u32(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 4)
        return struct.unpack("<I", b)[0] if b and len(b) == 4 else None

    def read_i32(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 4)
        return struct.unpack("<i", b)[0] if b and len(b) == 4 else None

    def read_u64(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 8)
        return struct.unpack("<Q", b)[0] if b and len(b) == 8 else None

    def read_i64(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 8)
        return struct.unpack("<q", b)[0] if b and len(b) == 8 else None
