# 通用进程附加与基础读取封装.
#
# 只暴露 ``GameProcess`` 一个类, 内部用驱动后端完成内存读取。
# 所有读操作均包裹 try/except, 失败返回 None 而不是抛,
# 因为内存扫描场景下触碰未映射页是常态。
#
# 进程名通过构造函数 ``process_name`` / ``process_names`` 参数指定。

from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import os
import sys
import time
from dataclasses import dataclass
from typing import Iterator, List, Optional


# ───────────────────────── Win32 常量 / 结构体 ─────────────────────────
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000
MEM_IMAGE = 0x1000000
MEM_MAPPED = 0x40000

PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100
PAGE_READONLY = 0x02
PAGE_READWRITE = 0x04
PAGE_WRITECOPY = 0x08
PAGE_EXECUTE = 0x10
PAGE_EXECUTE_READ = 0x20
PAGE_EXECUTE_READWRITE = 0x40
PAGE_EXECUTE_WRITECOPY = 0x80

_READABLE_PROTECTS = (
    PAGE_READONLY
    | PAGE_READWRITE
    | PAGE_WRITECOPY
    | PAGE_EXECUTE_READ
    | PAGE_EXECUTE_READWRITE
    | PAGE_EXECUTE_WRITECOPY
)

# optional backend
try:
    from mem_probe import rt_io as _drv
except Exception:
    _drv = None
_DRIVER_OK = False
_DRIVER_TRIED = False


class _MEMORY_RANGE_ENTRY(ctypes.Structure):
    _fields_ = [
        ("VirtualAddress", ctypes.c_void_p),
        ("NumberOfBytes", ctypes.c_size_t),
    ]


try:
    _PrefetchVM = ctypes.windll.kernel32.PrefetchVirtualMemory
    _PrefetchVM.argtypes = [
        wintypes.HANDLE, ctypes.c_size_t,
        ctypes.POINTER(_MEMORY_RANGE_ENTRY), wintypes.DWORD,
    ]
    _PrefetchVM.restype = wintypes.BOOL
except Exception:
    _PrefetchVM = None


def _mem_read(handle, addr, buf, size, p_got):
    # Read via driver backend only. No user-mode API fallback.
    global _DRIVER_OK, _DRIVER_TRIED
    if _drv is not None:
        if _DRIVER_OK:
            return _drv._rpm(handle, addr, buf, size, p_got)
        if not _DRIVER_TRIED:
            _DRIVER_TRIED = True
            try:
                _DRIVER_OK = _drv.ensure_loaded()
            except Exception:
                _DRIVER_OK = False
            if _DRIVER_OK:
                return _drv._rpm(handle, addr, buf, size, p_got)
    return False


class _MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_ulonglong),
        ("AllocationBase", ctypes.c_ulonglong),
        ("AllocationProtect", wintypes.DWORD),
        ("__alignment1", wintypes.DWORD),
        ("RegionSize", ctypes.c_ulonglong),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
        ("__alignment2", wintypes.DWORD),
    ]


class _UNICODE_STRING(ctypes.Structure):
    _fields_ = [
        ("Length", ctypes.c_ushort),
        ("MaximumLength", ctypes.c_ushort),
        ("Buffer", ctypes.c_void_p),
    ]


def _iter_process_entries_wide() -> Iterator[tuple[str, int]]:
    # Enumerate processes via NtQuerySystemInformation(SystemProcessInformation).
    #
    # Avoids CreateToolhelp32Snapshot which is commonly monitored by anti-cheat.
    _ntdll = ctypes.windll.ntdll
    _SPI = 5  # SystemProcessInformation
    buf_size = 0x100000  # 1 MB initial
    for _ in range(3):
        buf = ctypes.create_string_buffer(buf_size)
        ret_len = ctypes.c_ulong(0)
        status = _ntdll.NtQuerySystemInformation(
            _SPI, buf, buf_size, ctypes.byref(ret_len))
        if status == 0:
            break
        if status == 0xC0000004:  # STATUS_INFO_LENGTH_MISMATCH
            buf_size = int(ret_len.value) + 0x10000
            continue
        return
    else:
        return
    offset = 0
    raw = buf.raw
    total = int(ret_len.value)
    while offset < total:
        next_entry = int.from_bytes(raw[offset:offset + 4], "little")
        pid = int.from_bytes(raw[offset + 0x50:offset + 0x58], "little")
        name_us_len = int.from_bytes(raw[offset + 0x38:offset + 0x3A], "little")
        name_us_buf = int.from_bytes(raw[offset + 0x40:offset + 0x48], "little")
        name = ""
        if name_us_len > 0 and name_us_buf:
            buf_offset = name_us_buf - ctypes.addressof(buf)
            if 0 <= buf_offset <= total - name_us_len:
                try:
                    name = raw[buf_offset:buf_offset + name_us_len].decode("utf-16-le")
                except Exception:
                    pass
        if pid > 0:
            yield name, pid
        if next_entry == 0:
            break
        offset += next_entry


def _find_pid_by_name_wide(process_name: str) -> Optional[int]:
    target = os.path.basename(str(process_name or "")).casefold()
    if not target:
        return None
    try:
        for exe_name, pid in _iter_process_entries_wide():
            if os.path.basename(str(exe_name or "")).casefold() == target:
                return int(pid)
    except Exception:
        return None
    return None


# ───────────────────────── 数据类 ─────────────────────────
@dataclass(frozen=True)
class ModuleInfo:
    name: str
    base: int
    size: int

    def __repr__(self) -> str:
        return f"<Module {self.name} base=0x{self.base:016X} size=0x{self.size:X}>"


@dataclass(frozen=True)
class MemoryRegion:
    base: int
    size: int
    protect: int
    type_: int

    @property
    def is_private(self) -> bool:
        return bool(self.type_ & MEM_PRIVATE)

    @property
    def is_image(self) -> bool:
        return bool(self.type_ & MEM_IMAGE)


# ───────────────────────── 主类 ─────────────────────────
class GameProcessError(RuntimeError):
    pass


class GameProcess:
    # 对目标进程的只读包装 (进程名由调用方显式指定).

    def __init__(self, process_name: Optional[str] = None,
                 process_names: Optional[List[str]] = None) -> None:
        try:
            import pymem  # noqa: F401  延迟 import, 主程序不强依赖
        except ImportError as e:
            raise GameProcessError(
                "pymem 未安装。请在源码运行环境执行: pip install pymem"
            ) from e

        import pymem.process
        from pymem import Pymem
        from pymem.exception import CouldNotOpenProcess

        # ── Step 1: 只找 PID (NtQuerySystemInformation, 无句柄, 不触发 ObRegisterCallbacks) ──
        candidates: List[str] = []
        if process_name:
            candidates.append(process_name)
        if process_names:
            for name in process_names:
                if name and name not in candidates:
                    candidates.append(name)
        found_pid: Optional[int] = None
        attached_name: Optional[str] = None
        last_err: Optional[Exception] = None
        for name in candidates:
            if not name:
                continue
            pid = find_pid_by_name(name)
            if pid is not None:
                found_pid = pid
                attached_name = name
                break
            last_err = GameProcessError(f"E_NOT_FOUND")
        if found_pid is None:
            raise GameProcessError(
                f"E_PROCESS ({len(candidates)})"
            )

        # ── Step 2: 驱动优先 (在 OpenProcess 之前, 不产生游戏进程句柄) ──
        global _DRIVER_OK, _DRIVER_TRIED
        drv_attached = False
        if _drv is not None:
            if not _DRIVER_TRIED:
                try:
                    _DRIVER_OK = _drv.ensure_loaded()
                except Exception:
                    _DRIVER_OK = False
                _DRIVER_TRIED = True
            if _DRIVER_OK:
                if _drv.attach(found_pid):
                    drv_attached = True
                    try:
                        from mem_probe import cy_memscan as _cy
                        _cy.driver_attach(found_pid)
                    except Exception:
                        pass

        # ── Step 3: OpenProcess — 只要 QUERY (不含 VM_READ, 不触发降权) ──
        access = PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION
        try:
            handle = pymem.process.open(
                found_pid, debug=False, process_access=access)
            if not handle:
                raise CouldNotOpenProcess(found_pid)
            self._pm = Pymem()
            self._pm.process_id = found_pid
            self._pm.process_handle = handle
        except CouldNotOpenProcess as e:
            if drv_attached:
                self._pm = Pymem()
                self._pm.process_id = found_pid
                self._pm.process_handle = 0
            else:
                raise GameProcessError(f"E_ACCESS ({e})") from e
        self._attached_name = attached_name
        self._pid = int(found_pid)
        self._handle = int(self._pm.process_handle)
        self._region_cache: Optional[List[MemoryRegion]] = None
        self._region_cache_time: float = 0.0

    # ───── 基本属性 ─────
    @property
    def pid(self) -> int:
        return self._pid

    @property
    def handle(self) -> int:
        return self._handle

    @property
    def name(self) -> str:
        return self._attached_name or ""

    @property
    def memory_tier(self) -> str:
        if _drv is not None and _DRIVER_OK:
            return _drv.memory_tier()
        return "S"

    @property
    def memory_tier_desc(self) -> str:
        if _drv is not None and _DRIVER_OK:
            return _drv.TIER_DESC.get(_drv.memory_tier(), "")
        return "无驱动"

    # ───── 模块 ─────
    def list_modules(self) -> List[ModuleInfo]:
        if _drv is not None and _DRIVER_OK:
            mods = self._list_modules_via_driver()
            if mods is not None:
                return mods
        try:
            mods = list(self._pm.list_modules())
        except Exception as e:
            raise GameProcessError(f"E_MODULES ({e})") from e
        out: List[ModuleInfo] = []
        for m in mods:
            try:
                out.append(
                    ModuleInfo(
                        name=os.path.basename(str(m.name)),
                        base=int(m.lpBaseOfDll),
                        size=int(m.SizeOfImage),
                    )
                )
            except Exception:
                continue
        return out

    def _list_modules_via_driver(self) -> Optional[List[ModuleInfo]]:
        try:
            pbi = (ctypes.c_byte * 48)()
            ret_len = ctypes.c_ulong(0)
            _ntqip = ctypes.windll.ntdll.NtQueryInformationProcess
            st = _ntqip(ctypes.c_void_p(self._handle), 0, pbi, 48, ctypes.byref(ret_len))
            if st < 0:
                return None
            peb_addr = int.from_bytes(bytes(pbi[8:16]), "little")
            if not peb_addr:
                return None

            peb_data = _drv.read(peb_addr, 0x20)
            if not peb_data or len(peb_data) < 0x20:
                return None
            ldr_addr = int.from_bytes(peb_data[0x18:0x20], "little")
            if not ldr_addr:
                return None

            ldr_data = _drv.read(ldr_addr, 0x30)
            if not ldr_data or len(ldr_data) < 0x20:
                return None
            head = ldr_addr + 0x10
            flink = int.from_bytes(ldr_data[0x10:0x18], "little")

            out: List[ModuleInfo] = []
            visited = set()
            while flink and flink != head and flink not in visited:
                visited.add(flink)
                if len(visited) > 1024:
                    break
                entry = _drv.read(flink, 0x78)
                if not entry or len(entry) < 0x78:
                    break
                dll_base = int.from_bytes(entry[0x20:0x28], "little")
                size_of_image = int.from_bytes(entry[0x40:0x44], "little")
                name_len = int.from_bytes(entry[0x58:0x5A], "little")
                name_buf = int.from_bytes(entry[0x60:0x68], "little")
                name = ""
                if name_buf and name_len:
                    raw = _drv.read(name_buf, min(name_len, 520))
                    if raw:
                        try:
                            name = raw.decode("utf-16-le").rstrip("\x00")
                            name = os.path.basename(name)
                        except Exception:
                            name = ""
                if dll_base and name:
                    out.append(ModuleInfo(name=name, base=dll_base, size=size_of_image))
                flink = int.from_bytes(entry[0x00:0x08], "little")
            return out if out else None
        except Exception:
            return None

    def main_module(self) -> ModuleInfo:
        target = (self._attached_name or "").lower()
        for m in self.list_modules():
            if m.name.lower() == target:
                return m
        raise GameProcessError(f"E_MAIN_MODULE")

    # ───── 内存区域 ─────
    def iter_regions(
        self,
        *,
        only_readable: bool = True,
        only_private: bool = True,
    ) -> Iterator[MemoryRegion]:
        # 遍历目标进程的虚拟地址空间.
        #
        # 默认只返回可读 + private commit 的区域 (排除 image / mapped /
        # guard / noaccess), 这是值搜索常用的"堆数据"集合。
        VirtualQueryEx = ctypes.windll.kernel32.VirtualQueryEx
        VirtualQueryEx.argtypes = [
            wintypes.HANDLE,
            wintypes.LPCVOID,
            ctypes.POINTER(_MEMORY_BASIC_INFORMATION64),
            ctypes.c_size_t,
        ]
        VirtualQueryEx.restype = ctypes.c_size_t

        mbi = _MEMORY_BASIC_INFORMATION64()
        addr = 0
        # 用户态地址上限 (x64 Windows 7FFF_FFFF_FFFF), 留点余量
        max_addr = 0x7FFFFFFFFFFF
        size = ctypes.sizeof(mbi)
        while addr < max_addr:
            ret = VirtualQueryEx(self._handle, addr, ctypes.byref(mbi), size)
            if ret == 0:
                break
            region_base = int(mbi.BaseAddress)
            region_size = int(mbi.RegionSize)
            if region_size == 0:
                break
            next_addr = region_base + region_size
            if mbi.State == MEM_COMMIT:
                protect = int(mbi.Protect)
                type_ = int(mbi.Type)
                ok = True
                if only_readable:
                    if protect & PAGE_GUARD:
                        ok = False
                    elif protect & PAGE_NOACCESS:
                        ok = False
                    elif not (protect & _READABLE_PROTECTS):
                        ok = False
                if ok and only_private and not (type_ & MEM_PRIVATE):
                    ok = False
                if ok:
                    yield MemoryRegion(region_base, region_size, protect, type_)
            if next_addr <= addr:
                break
            addr = next_addr

    # ───── 安全读取 ─────
    def read_bytes(self, addr: int, n: int) -> Optional[bytes]:
        if n <= 0:
            return b""
        try:
            buf = ctypes.create_string_buffer(n)
            got = ctypes.c_size_t(0)
            ok = _mem_read(self._handle, int(addr), buf, n, ctypes.byref(got))
            if ok and got.value > 0:
                return buf.raw[: int(got.value)]
            return None
        except Exception:
            return None

    def read_bytes_into(self, addr: int, buf, n: Optional[int] = None) -> int:
        # 把目标进程内存直接读进调用方提供的可写缓冲 (bytearray/memoryview).
        #
        # 跳过 pymem 的 create_string_buffer + .raw 两次拷贝, 扫堆循环配合一块
        # 复用缓冲可把每 chunk 的分配/拷贝开销整段去掉。返回实际读到的字节数
        # (失败返回 0; 与 read_bytes 一样, RPM 碰到不可读页时整次失败)。
        want = len(buf) if n is None else int(n)
        if want <= 0:
            return 0
        try:
            c_buf = (ctypes.c_char * want).from_buffer(buf)
        except (TypeError, ValueError):
            return 0
        got = ctypes.c_size_t(0)
        try:
            ok = _mem_read(self._handle, int(addr), c_buf, want, ctypes.byref(got))
        except Exception:
            return 0
        return int(got.value) if ok else 0

    def read_i32(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 4)
        if b is None:
            return None
        return int.from_bytes(b, "little", signed=True)

    def read_u32(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 4)
        if b is None:
            return None
        return int.from_bytes(b, "little", signed=False)

    def read_i64(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 8)
        if b is None:
            return None
        return int.from_bytes(b, "little", signed=True)

    def read_u64(self, addr: int) -> Optional[int]:
        b = self.read_bytes(addr, 8)
        if b is None:
            return None
        return int.from_bytes(b, "little", signed=False)

    def read_batch(self, requests) -> list:
        if not requests:
            return []
        if _drv is not None and _DRIVER_OK and hasattr(_drv, 'read_batch'):
            try:
                return _drv.read_batch([(int(a), int(s)) for a, s in requests])
            except Exception:
                pass
        return [self.read_bytes(a, s) for a, s in requests]

    # ───── 批量读 (跨进程 0 延迟优化) ─────
    def read_u64_many(self, addrs) -> list:
        # Batch-read 8-byte words at each address -> list (None on fail).
        #
        # Uses the Cython nogil batch RPM (one GIL release for the whole batch,
        # no per-read ctypes/pymem overhead); falls back to a direct-ctypes loop.
        return self._read_words_many(addrs, 8)

    def read_u32_many(self, addrs) -> list:
        return self._read_words_many(addrs, 4)

    def _read_words_many(self, addrs, word_size: int) -> list:
        addrs = list(addrs)
        if not addrs:
            return []
        try:
            from mem_probe import cy_memscan as _cy
            res = _cy.read_words_many(self._handle, addrs, word_size)
            if res is not None:
                return res
        except Exception:
            pass
        # direct-ctypes fallback
        h = self._handle
        buf = (ctypes.c_uint64 if word_size == 8 else ctypes.c_uint32)()
        got = ctypes.c_size_t()
        pbuf = ctypes.byref(buf)
        pgot = ctypes.byref(got)
        out: list = []
        for a in addrs:
            try:
                ok = _mem_read(h, int(a), pbuf, word_size, pgot)
                if ok and got.value == word_size:
                    out.append(int(buf.value))
                else:
                    out.append(None)
            except Exception:
                out.append(None)
        return out

    def read_ptr(self, addr: int) -> Optional[int]:
        # 读 64-bit 指针 (x64 Windows 用户态地址).
        return self.read_u64(addr)

    def read_cstr(self, addr: int, max_len: int = 256) -> Optional[str]:
        # 读 ASCII/UTF-8 0-终止字符串.
        b = self.read_bytes(addr, max_len)
        if b is None:
            return None
        end = b.find(b"\x00")
        if end < 0:
            end = len(b)
        try:
            return b[:end].decode("utf-8", errors="replace")
        except Exception:
            return None

    def read_f32(self, addr: int) -> Optional[float]:
        import struct
        b = self.read_bytes(addr, 4)
        if b is None:
            return None
        try:
            return struct.unpack("<f", b)[0]
        except struct.error:
            return None

    def read_utf16(self, addr: int, max_chars: int = 64) -> Optional[str]:
        b = self.read_bytes(addr, max_chars * 2)
        if b is None:
            return None
        # 以 NUL 结尾
        end = len(b)
        for i in range(0, len(b) - 1, 2):
            if b[i] == 0 and b[i + 1] == 0:
                end = i
                break
        try:
            return b[:end].decode("utf-16-le", errors="replace")
        except Exception:
            return None

    # ───── 写入 (仅驱动后端, Tier B+ 时可用) ─────
    def write_bytes(self, addr: int, data: bytes) -> bool:
        if not data or _drv is None or not _DRIVER_OK:
            return False
        return _drv.write(int(addr), data)

    def can_write(self) -> bool:
        if _drv is None or not _DRIVER_OK:
            return False
        caps = _drv.ENGINE_CAPS.get(_drv._engine, 0)
        return bool(caps & _drv.CAP_WRITE)

    # ───── 区域缓存 / 预取 / 批量 slab ─────
    def cached_regions(
        self, *, ttl: float = 5.0, only_readable: bool = True, only_private: bool = True,
    ) -> List[MemoryRegion]:
        # Return cached region list (refreshed every *ttl* seconds).
        now = time.monotonic()
        if self._region_cache is not None and now - self._region_cache_time < ttl:
            return self._region_cache
        self._region_cache = list(
            self.iter_regions(only_readable=only_readable, only_private=only_private)
        )
        self._region_cache_time = now
        return self._region_cache

    def invalidate_region_cache(self) -> None:
        self._region_cache = None
        self._region_cache_time = 0.0

    def prefetch_regions(self, regions, *, max_entries: int = 64) -> None:
        # Hint the OS to page-in target memory before batch reads (Win8+).
        if _PrefetchVM is None or not regions:
            return
        n = min(len(regions), max_entries)
        entries = (_MEMORY_RANGE_ENTRY * n)()
        for i in range(n):
            r = regions[i]
            entries[i].VirtualAddress = ctypes.c_void_p(r.base)
            entries[i].NumberOfBytes = r.size
        try:
            _PrefetchVM(self._handle, n, entries, 0)
        except Exception:
            pass

    def read_slab_many(self, base_addrs, offsets, word_size: int = 8) -> list:
        # For each base addr, read one block covering all offsets, extract values.
        #
        # Returns flat list of ``len(base_addrs) * len(offsets)`` values (None on fail).
        # Cython-accelerated when available; fallback to individual reads.
        try:
            from mem_probe import cy_memscan as _cy
            res = _cy.read_slab_many(self._handle, list(base_addrs), list(offsets), word_size)
            if res is not None:
                return res
        except Exception:
            pass
        out: list = []
        read_fn = self.read_u64 if word_size == 8 else self.read_u32
        for base in base_addrs:
            for off in offsets:
                out.append(read_fn(int(base) + int(off)))
        return out

    # ───── 关闭 ─────
    def close(self) -> None:
        try:
            if _drv is not None and _DRIVER_OK:
                _drv.detach()
        except Exception:
            pass
        try:
            self._pm.close_process()
        except Exception:
            pass

    def __enter__(self) -> "GameProcess":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# ───────────────────────── 工具函数 ─────────────────────────
def is_admin() -> bool:
    # 当前 Python 进程是否拥有管理员权限. 非管理员下大概率 attach 失败.
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def find_pid_by_name(process_name: str) -> Optional[int]:
    # 轻量探测: 不开进程, 仅枚举 PID; 用于 attach 前预检.
    #
    # Uses the Unicode Win32 process enumeration path so a non-UTF-8 executable
    # name from any unrelated process cannot abort game process discovery.
    return _find_pid_by_name_wide(process_name)


if __name__ == "__main__":  # 简易自测
    print(f"is_admin={is_admin()} python={sys.executable}")
    try:
        with GameProcess() as sp:
            print(f"attached: pid={sp.pid} name={sp.name}")
            print(sp.main_module())
            n = 0
            total = 0
            for r in sp.iter_regions():
                n += 1
                total += r.size
            print(f"private commit regions: {n}, total={total/1024/1024:.1f} MiB")
    except GameProcessError as e:
        print(f"[err] {e}")
