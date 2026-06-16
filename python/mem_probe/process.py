"""通用进程附加与基础读取封装.

只暴露 ``GameProcess`` 一个类, 内部用 pymem 完成 OpenProcess +
ReadProcessMemory. 所有读操作均包裹 try/except, 失败返回 None 而不是抛,
因为内存扫描场景下触碰未映射页是常态。

进程名通过构造函数 ``process_name`` 参数或 ``GAME_PROCESS_NAMES``
(插件通过 ``set_game_process_names()`` 注入) 指定。

依赖: pymem>=1.13 (PoC 可选依赖, 未在打包 spec 中)
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import os
import sys
import time
from dataclasses import dataclass
from typing import Iterator, List, Optional

GAME_PROCESS_NAMES: list = []


def set_game_process_names(names: list) -> None:
    """Plugin injection: set the process-name candidates at runtime."""
    global GAME_PROCESS_NAMES
    GAME_PROCESS_NAMES = list(names)


# ───────────────────────── Win32 常量 / 结构体 ─────────────────────────
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

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

# 模块级缓存 ReadProcessMemory 原型 (argtypes 只配置一次, 避免每次调用重设)。
# 用私有 WinDLL 实例, 不动 ctypes.windll.kernel32 共享缓存上的函数原型。
_RPM_DIRECT = ctypes.WinDLL("kernel32").ReadProcessMemory
_RPM_DIRECT.argtypes = [
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]
_RPM_DIRECT.restype = wintypes.BOOL

# NtReadVirtualMemory from ntdll — bypasses kernel32 parameter validation layer
try:
    _NTRVM = ctypes.WinDLL("ntdll").NtReadVirtualMemory
    _NTRVM.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    _NTRVM.restype = ctypes.c_long  # NTSTATUS
except Exception:
    _NTRVM = None

# optional backend
try:
    from mem_probe import driver_backend as _drv
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
    """Unified read — driver / NtReadVirtualMemory / ReadProcessMemory fallback.

    Driver is probed lazily on first call. If driver read fails or isn't
    available, falls through to NtReadVirtualMemory then ReadProcessMemory.
    """
    global _DRIVER_OK, _DRIVER_TRIED
    if _drv is not None:
        if _DRIVER_OK:
            if _drv.driver_mem_read(handle, addr, buf, size, p_got):
                return True
        elif not _DRIVER_TRIED:
            _DRIVER_TRIED = True
            try:
                _DRIVER_OK = _drv.ensure_loaded()
            except Exception:
                _DRIVER_OK = False
            if _DRIVER_OK:
                if _drv.driver_mem_read(handle, addr, buf, size, p_got):
                    return True
    if _NTRVM is not None:
        return _NTRVM(
            ctypes.c_void_p(handle), ctypes.c_void_p(addr),
            buf, ctypes.c_size_t(size), p_got,
        ) >= 0
    return bool(_RPM_DIRECT(
        ctypes.c_void_p(handle), ctypes.c_void_p(addr),
        buf, ctypes.c_size_t(size), p_got,
    ))


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


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * wintypes.MAX_PATH),
    ]


def _iter_process_entries_wide() -> Iterator[tuple[str, int]]:
    """Enumerate process names via the Unicode Toolhelp API.

    ``pymem.process.process_from_name`` decodes ``PROCESSENTRY32.szExeFile``
    with ``locale.getpreferredencoding()``.  On Windows machines configured for
    UTF-8, unrelated processes with ANSI bytes in their executable name can make
    that helper raise ``UnicodeDecodeError`` before it ever reaches the target.
    The W-suffixed Toolhelp APIs return UTF-16 strings directly, avoiding that
    locale-sensitive decode path.
    """
    kernel32 = ctypes.windll.kernel32
    CreateToolhelp32Snapshot = kernel32.CreateToolhelp32Snapshot
    Process32FirstW = kernel32.Process32FirstW
    Process32NextW = kernel32.Process32NextW
    CloseHandle = kernel32.CloseHandle

    CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
    Process32FirstW.restype = wintypes.BOOL
    Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
    Process32NextW.restype = wintypes.BOOL
    CloseHandle.argtypes = [wintypes.HANDLE]
    CloseHandle.restype = wintypes.BOOL

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if int(snap) == int(INVALID_HANDLE_VALUE):
        return
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
        ok = Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            yield str(entry.szExeFile), int(entry.th32ProcessID)
            ok = Process32NextW(snap, ctypes.byref(entry))
    finally:
        CloseHandle(snap)


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


StarProcessError = GameProcessError


class GameProcess:
    """对目标游戏进程的只读包装 (进程名由 config 或参数指定)."""

    def __init__(self, process_name: Optional[str] = None) -> None:
        try:
            import pymem  # noqa: F401  延迟 import, 主程序不强依赖
        except ImportError as e:
            raise GameProcessError(
                "pymem 未安装。请在源码运行环境执行: pip install pymem"
            ) from e

        import pymem.process
        from pymem import Pymem
        from pymem.exception import CouldNotOpenProcess

        # ── Step 1: 只找 PID (CreateToolhelp32Snapshot, 无句柄, 不触发 ObRegisterCallbacks) ──
        candidates = [process_name] if process_name else list(GAME_PROCESS_NAMES)
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
            last_err = GameProcessError(f"process not found: {name}")
        if found_pid is None:
            raise GameProcessError(
                f"未找到游戏进程 (尝试候选: {candidates})。请确认目标进程正在运行。"
                + (f" 最后错误: {last_err}" if last_err else "")
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
                    print(f"[GameProcess] driver backend attached (pid={found_pid})")
                    try:
                        from mem_probe import cy_memscan as _cy
                        if _cy.driver_attach(found_pid):
                            print(f"[GameProcess] cython driver fast-path activated")
                    except Exception:
                        pass

        # ── Step 3: OpenProcess — 驱动在线时只要 QUERY (不含 VM_READ, 不触发降权) ──
        access = PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION
        if not drv_attached:
            access |= PROCESS_VM_READ
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
                raise GameProcessError(
                    f"找到进程 {attached_name} 但无法 OpenProcess; "
                    f"可能被反作弊保护或需要管理员权限。原始错误: {e}"
                ) from e
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
        return "系统 API (NtRVM/RPM, 无驱动)"

    # ───── 模块 ─────
    def list_modules(self) -> List[ModuleInfo]:
        try:
            mods = list(self._pm.list_modules())
        except Exception as e:
            raise GameProcessError(f"list_modules 失败: {e}") from e
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

    def main_module(self) -> ModuleInfo:
        target = (self._attached_name or "").lower()
        for m in self.list_modules():
            if m.name.lower() == target:
                return m
        raise GameProcessError(f"主模块 {target} 在模块列表中未找到")

    # ───── 内存区域 ─────
    def iter_regions(
        self,
        *,
        only_readable: bool = True,
        only_private: bool = True,
    ) -> Iterator[MemoryRegion]:
        """遍历目标进程的虚拟地址空间.

        默认只返回可读 + private commit 的区域 (排除 image / mapped /
        guard / noaccess), 这是值搜索常用的"堆数据"集合。
        """
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
        """把目标进程内存直接读进调用方提供的可写缓冲 (bytearray/memoryview).

        跳过 pymem 的 create_string_buffer + .raw 两次拷贝, 扫堆循环配合一块
        复用缓冲可把每 chunk 的分配/拷贝开销整段去掉。返回实际读到的字节数
        (失败返回 0; 与 read_bytes 一样, RPM 碰到不可读页时整次失败)。
        """
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

    # ───── 批量读 (跨进程 0 延迟优化) ─────
    def read_u64_many(self, addrs) -> list:
        """Batch-read 8-byte words at each address -> list (None on fail).

        Uses the Cython nogil batch RPM (one GIL release for the whole batch,
        no per-read ctypes/pymem overhead); falls back to a direct-ctypes loop.
        """
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
        # direct-ctypes fallback using NtRVM/RPM
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
        """读 64-bit 指针 (x64 Windows 用户态地址)."""
        return self.read_u64(addr)

    def read_cstr(self, addr: int, max_len: int = 256) -> Optional[str]:
        """读 ASCII/UTF-8 0-终止字符串."""
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
        """Return cached region list (refreshed every *ttl* seconds)."""
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
        """Hint the OS to page-in target memory before batch reads (Win8+)."""
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
        """For each base addr, read one block covering all offsets, extract values.

        Returns flat list of ``len(base_addrs) * len(offsets)`` values (None on fail).
        Cython-accelerated when available; fallback to individual reads.
        """
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
            self._pm.close_process()
        except Exception:
            pass

    def __enter__(self) -> "GameProcess":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


# ───────────────────────── 工具函数 ─────────────────────────
def is_admin() -> bool:
    """当前 Python 进程是否拥有管理员权限. 非管理员下大概率 attach 失败."""
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def find_pid_by_name(process_name: str) -> Optional[int]:
    """轻量探测: 不开进程, 仅枚举 PID; 用于 attach 前预检.

    Uses the Unicode Win32 process enumeration path so a non-UTF-8 executable
    name from any unrelated process cannot abort game process discovery.
    """
    return _find_pid_by_name_wide(process_name)


StarProcess = GameProcess


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
    except StarProcessError as e:
        print(f"[err] {e}")
