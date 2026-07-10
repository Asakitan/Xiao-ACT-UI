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
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Dict, Iterator, List, Optional


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
_DRIVER_EPOCH = None
_DRIVER_ACTIVE_PID = 0
_DRIVER_RETRY_AT = 0.0
_DRIVER_FAILURES = 0
_DRIVER_PID_REFS: Dict[int, int] = {}
_DRIVER_LOCK = threading.RLock()
_DRIVER_CONDITION = threading.Condition(_DRIVER_LOCK)


class TargetLeaseError(RuntimeError):
    # The process-global driver target could not be activated safely.
    pass


def _driver_session_epoch(*, allow_status: bool = False):
    # Return a cheap identity for the current proxy/helper session.
    #
    #     New proxy builds expose ``session_epoch()``.  The fallbacks keep source
    #     compatibility with older PYDs without making a STATUS IPC on every read.
    #     STATUS is only consulted on lifecycle/ensure paths.
    #
    drv = _drv
    if drv is None:
        return ("none",)
    try:
        getter = getattr(drv, "session_epoch", None)
        if callable(getter):
            return (
                "epoch", int(getter()),
                bool(getattr(drv, "_connected", True)),
            )
    except Exception:
        pass
    try:
        value = getattr(drv, "_session_epoch", None)
        if value is not None:
            return (
                "epoch", int(value),
                bool(getattr(drv, "_connected", True)),
            )
    except Exception:
        pass
    session = getattr(drv, "_session", None)
    if session is not None:
        try:
            value = getattr(session, "epoch", None)
            if value is not None:
                state = str(getattr(session, "state", "") or "")
                return ("epoch", int(value), state)
        except Exception:
            pass
    if allow_status:
        try:
            status = drv.status()
            if isinstance(status, dict) and status.get("session_epoch") is not None:
                return (
                    "epoch", int(status["session_epoch"]),
                    bool(status.get("connected", status.get("backend_ready", False))),
                )
        except Exception:
            pass
    proc = getattr(drv, "_helper_proc", None)
    try:
        proc_pid = int(getattr(proc, "pid", 0) or 0)
    except Exception:
        proc_pid = 0
    return (
        "legacy",
        id(drv),
        bool(getattr(drv, "_connected", False)),
        id(proc) if proc is not None else 0,
        proc_pid,
    )


def _refresh_driver_epoch_locked(*, allow_status: bool = False) -> None:
    global _DRIVER_EPOCH, _DRIVER_OK, _DRIVER_TRIED
    global _DRIVER_ACTIVE_PID, _DRIVER_RETRY_AT, _DRIVER_FAILURES
    epoch = _driver_session_epoch(allow_status=allow_status)
    if epoch == _DRIVER_EPOCH:
        return
    _DRIVER_EPOCH = epoch
    _DRIVER_OK = False
    _DRIVER_TRIED = False
    _DRIVER_ACTIVE_PID = 0
    _DRIVER_RETRY_AT = 0.0
    _DRIVER_FAILURES = 0


def _ensure_driver_locked() -> bool:
    # Ensure the driver with epoch-scoped exponential retry backoff.
    global _DRIVER_OK, _DRIVER_TRIED, _DRIVER_EPOCH
    global _DRIVER_ACTIVE_PID, _DRIVER_RETRY_AT, _DRIVER_FAILURES
    if _drv is None:
        return False
    _refresh_driver_epoch_locked()
    if _DRIVER_OK:
        return True
    now = time.monotonic()
    if now < _DRIVER_RETRY_AT:
        return False
    _DRIVER_TRIED = True
    try:
        ok = bool(_drv.ensure_loaded())
    except Exception:
        ok = False
    # ensure_loaded may create a new helper, so capture its final epoch before
    # publishing success.  Do not let the epoch refresh erase this result.
    final_epoch = _driver_session_epoch(allow_status=ok)
    if final_epoch != _DRIVER_EPOCH:
        _DRIVER_EPOCH = final_epoch
        _DRIVER_ACTIVE_PID = 0
    _DRIVER_OK = ok
    if ok:
        _DRIVER_FAILURES = 0
        _DRIVER_RETRY_AT = 0.0
        return True
    _DRIVER_FAILURES += 1
    _DRIVER_RETRY_AT = now + min(30.0, 2.0 * (2 ** min(_DRIVER_FAILURES - 1, 4)))
    return False


def _cy_driver_attach_locked(pid: int) -> bool:
    try:
        from mem_probe import cy_memscan as _cy
        ok = bool(_cy.driver_attach(int(pid)))
        if not ok:
            _cy.driver_detach()
        return ok
    except Exception:
        try:
            from mem_probe import cy_memscan as _cy
            _cy.driver_detach()
        except Exception:
            pass
        return False


def _cy_driver_detach_locked() -> None:
    try:
        from mem_probe import cy_memscan as _cy
        _cy.driver_detach()
    except Exception:
        pass


def _activate_target_locked(pid: int) -> bool:
    global _DRIVER_ACTIVE_PID
    _refresh_driver_epoch_locked()
    if not _ensure_driver_locked():
        return False
    target = int(pid)
    if target <= 0:
        return False
    if _DRIVER_ACTIVE_PID == target:
        return True
    try:
        ensure_target = getattr(_drv, "_ensure_target", None)
        if callable(ensure_target):
            attached = bool(ensure_target(target))
        else:
            attached = bool(_drv.attach(target))
        if not attached:
            return False
    except Exception:
        return False
    _cy_driver_attach_locked(target)
    _DRIVER_ACTIVE_PID = target
    return True


class TargetLease:
    # Reference-counted ownership of the process-global rt_io target.
    #
    #     rt_io and the Cython fast path both keep one attached PID per process.
    #     Every operation therefore holds the same re-entrant lock while activating
    #     its PID and performing the read/write.  Multiple same-PID consumers share
    #     a reference; different-PID consumers are explicitly switched, never read
    #     through whichever consumer happened to attach last.
    #

    def __init__(self, pid: int) -> None:
        self.pid = int(pid)
        if self.pid <= 0:
            raise ValueError("pid must be positive")
        self._closed = False
        with _DRIVER_CONDITION:
            _DRIVER_PID_REFS[self.pid] = _DRIVER_PID_REFS.get(self.pid, 0) + 1
            _DRIVER_CONDITION.notify_all()

    def ensure_active(self) -> bool:
        with _DRIVER_LOCK:
            if self._closed:
                return False
            return _activate_target_locked(self.pid)

    @contextmanager
    def operation(self):
        with _DRIVER_LOCK:
            if self._closed:
                raise TargetLeaseError(f"target lease for pid {self.pid} is closed")
            if not _activate_target_locked(self.pid):
                raise TargetLeaseError(f"failed to activate driver target pid {self.pid}")
            target_operation = getattr(_drv, "target_operation", None)
            if callable(target_operation):
                with target_operation(self.pid):
                    yield _drv
            else:
                yield _drv

    def close(self) -> None:
        global _DRIVER_ACTIVE_PID
        with _DRIVER_CONDITION:
            if self._closed:
                return
            self._closed = True
            remaining = _DRIVER_PID_REFS.get(self.pid, 0) - 1
            if remaining > 0:
                _DRIVER_PID_REFS[self.pid] = remaining
                _DRIVER_CONDITION.notify_all()
                return
            _DRIVER_PID_REFS.pop(self.pid, None)
            if _DRIVER_ACTIVE_PID != self.pid and _DRIVER_PID_REFS:
                _DRIVER_CONDITION.notify_all()
                return
            # Never leave the Cython PID pointing at a lease that no longer
            # exists.  Surviving different-PID leases reactivate lazily.
            _cy_driver_detach_locked()
            try:
                if _drv is not None and _DRIVER_OK:
                    release_target = getattr(_drv, "_release_target", None)
                    if callable(release_target):
                        release_target(self.pid)
                    else:
                        _drv.detach()
            except Exception:
                pass
            _DRIVER_ACTIVE_PID = 0
            _DRIVER_CONDITION.notify_all()

    def __enter__(self) -> "TargetLease":
        if not self.ensure_active():
            self.close()
            raise TargetLeaseError(f"failed to activate driver target pid {self.pid}")
        return self

    def __exit__(self, *_exc) -> None:
        self.close()


def _reset_target_leases_for_test() -> None:
    # Reset only Python ownership state; never touches a live backend.
    global _DRIVER_OK, _DRIVER_TRIED, _DRIVER_EPOCH, _DRIVER_ACTIVE_PID
    global _DRIVER_RETRY_AT, _DRIVER_FAILURES
    with _DRIVER_CONDITION:
        _DRIVER_PID_REFS.clear()
        _DRIVER_OK = False
        _DRIVER_TRIED = False
        _DRIVER_EPOCH = None
        _DRIVER_ACTIVE_PID = 0
        _DRIVER_RETRY_AT = 0.0
        _DRIVER_FAILURES = 0
        _DRIVER_CONDITION.notify_all()


def _wait_for_target_leases_closed(timeout: float = 10.0) -> bool:
    # Wait until every memory consumer has released its target lease.
    #
    #     This is an internal shutdown barrier.  A plugin that timed out while
    #     joining its scan thread keeps its lease alive, so helper teardown cannot
    #     race that thread's next read or detach.
    #
    deadline = time.monotonic() + max(0.0, float(timeout))
    with _DRIVER_CONDITION:
        while _DRIVER_PID_REFS:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            _DRIVER_CONDITION.wait(remaining)
        return True


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

try:
    _GetExitCodeProcess = ctypes.windll.kernel32.GetExitCodeProcess
    _GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    _GetExitCodeProcess.restype = wintypes.BOOL
except Exception:
    _GetExitCodeProcess = None


def _mem_read(handle, addr, buf, size, p_got):
    # Read via driver backend only. No user-mode API fallback.
    with _DRIVER_LOCK:
        if _drv is None or not _ensure_driver_locked():
            return False
        return _drv._rpm(handle, addr, buf, size, p_got)


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


@dataclass
class ProcessEntry:
    name: str
    pid: int
    ppid: int = 0
    thread_count: int = 0
    working_set: int = 0
    kernel_time: int = 0
    user_time: int = 0


def _query_system_process_info(_ntdll=None) -> Optional[tuple]:
    if _ntdll is None:
        _ntdll = ctypes.windll.ntdll
    _SPI = 5
    buf_size = 0x100000
    for _ in range(3):
        buf = ctypes.create_string_buffer(buf_size)
        ret_len = ctypes.c_ulong(0)
        status = _ntdll.NtQuerySystemInformation(
            _SPI, buf, buf_size, ctypes.byref(ret_len))
        # NTSTATUS is a signed LONG at the native boundary, while constants
        # are conventionally written as unsigned hexadecimal values.
        status_u32 = int(status) & 0xFFFFFFFF
        if status_u32 == 0:
            return buf, buf.raw, int(ret_len.value)
        if status_u32 == 0xC0000004:
            buf_size = int(ret_len.value) + 0x10000
            continue
        return None
    return None


def _iter_process_entries_wide() -> Iterator[tuple[str, int]]:
    # Enumerate processes via NtQuerySystemInformation(SystemProcessInformation).
    #
    # Avoids CreateToolhelp32Snapshot which is commonly monitored by anti-cheat.
    result = _query_system_process_info()
    if result is None:
        return
    buf, raw, total = result
    buf_addr = ctypes.addressof(buf)
    offset = 0
    while offset < total:
        next_entry = int.from_bytes(raw[offset:offset + 4], "little")
        pid = int.from_bytes(raw[offset + 0x50:offset + 0x58], "little")
        name_us_len = int.from_bytes(raw[offset + 0x38:offset + 0x3A], "little")
        name_us_buf = int.from_bytes(raw[offset + 0x40:offset + 0x48], "little")
        name = ""
        if name_us_len > 0 and name_us_buf:
            buf_offset = name_us_buf - buf_addr
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


def _iter_process_entries_ext() -> Iterator[ProcessEntry]:
    # Extended process enumeration — yields ProcessEntry with ppid, threads, memory, CPU times.
    #
    #     SYSTEM_PROCESS_INFORMATION x64 offsets:
    #       +0x00 NextEntryOffset (4)    +0x04 NumberOfThreads (4)
    #       +0x18 KernelTime (8)         +0x20 UserTime (8)
    #       +0x38 ImageName.Length (2)    +0x40 ImageName.Buffer (8)
    #       +0x50 UniqueProcessId (8)    +0x58 InheritedFromUniqueProcessId (8)
    #       VM_COUNTERS_EX2 starts ~+0x70; WorkingSetSize at +0x98 (8)
    #
    result = _query_system_process_info()
    if result is None:
        return
    buf, raw, total = result
    buf_addr = ctypes.addressof(buf)
    _i4 = lambda o: int.from_bytes(raw[o:o + 4], "little")
    _i8 = lambda o: int.from_bytes(raw[o:o + 8], "little")
    offset = 0
    while offset < total:
        next_entry = _i4(offset)
        pid = _i8(offset + 0x50)
        if pid > 0:
            name_us_len = int.from_bytes(raw[offset + 0x38:offset + 0x3A], "little")
            name_us_buf = _i8(offset + 0x40)
            name = ""
            if name_us_len > 0 and name_us_buf:
                buf_off = name_us_buf - buf_addr
                if 0 <= buf_off <= total - name_us_len:
                    try:
                        name = raw[buf_off:buf_off + name_us_len].decode("utf-16-le")
                    except Exception:
                        pass
            ppid = _i8(offset + 0x58)
            thread_count = _i4(offset + 0x04)
            kernel_time = _i8(offset + 0x18)
            user_time = _i8(offset + 0x20)
            working_set = 0
            if offset + 0xA0 <= total:
                working_set = _i8(offset + 0x98)
            yield ProcessEntry(
                name=name, pid=pid, ppid=ppid,
                thread_count=thread_count,
                working_set=working_set,
                kernel_time=kernel_time,
                user_time=user_time,
            )
        if next_entry == 0:
            break
        offset += next_entry


def _iter_thread_entries_for_pid(target_pid: int) -> Iterator[dict]:
    # Parse SYSTEM_THREAD_INFORMATION entries for a specific PID from SystemProcessInformation.
    #
    #     Each thread entry (56 bytes on x64) follows its parent SYSTEM_PROCESS_INFORMATION.
    #
    result = _query_system_process_info()
    if result is None:
        return
    _, raw, total = result
    _i4 = lambda o: int.from_bytes(raw[o:o + 4], "little")
    _i8 = lambda o: int.from_bytes(raw[o:o + 8], "little")
    offset = 0
    _THREAD_SIZE = 56
    _PROC_HEADER_SIZE = 0x100
    while offset < total:
        next_entry = _i4(offset)
        pid = _i8(offset + 0x50)
        n_threads = _i4(offset + 0x04)
        if pid == target_pid and n_threads > 0:
            thread_base = offset + _PROC_HEADER_SIZE
            for i in range(n_threads):
                t_off = thread_base + i * _THREAD_SIZE
                if t_off + _THREAD_SIZE > total:
                    break
                tid = _i8(t_off + 0x28)
                start_addr = _i8(t_off + 0x10)
                base_prio = _i4(t_off + 0x30)
                prio = _i4(t_off + 0x34)
                state = _i4(t_off + 0x04)
                wait_reason = _i4(t_off + 0x38)
                yield {
                    "tid": tid, "start_address": start_addr,
                    "base_priority": base_prio, "priority": prio,
                    "state": state, "wait_reason": wait_reason,
                }
            return
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


def query_process_identity(pid: int) -> Optional[tuple[int, str, int]]:
    # Return ``(pid, image_name, create_time)`` without opening a handle.
    target_pid = int(pid)
    if target_pid <= 0:
        return None
    result = _query_system_process_info()
    if result is None:
        return None
    buf, raw, total = result
    buf_addr = ctypes.addressof(buf)
    offset = 0
    while offset < total:
        next_entry = int.from_bytes(raw[offset:offset + 4], "little")
        current_pid = int.from_bytes(raw[offset + 0x50:offset + 0x58], "little")
        if current_pid == target_pid:
            name_len = int.from_bytes(raw[offset + 0x38:offset + 0x3A], "little")
            name_buf = int.from_bytes(raw[offset + 0x40:offset + 0x48], "little")
            name = ""
            if name_len > 0 and name_buf:
                buf_offset = name_buf - buf_addr
                if 0 <= buf_offset <= total - name_len:
                    try:
                        name = raw[buf_offset:buf_offset + name_len].decode("utf-16-le")
                    except Exception:
                        name = ""
            create_time = int.from_bytes(raw[offset + 0x20:offset + 0x28], "little")
            return current_pid, os.path.basename(name).casefold(), create_time
        if next_entry == 0:
            break
        offset += next_entry
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
        self._closed = False
        self._target_lease: Optional[TargetLease] = None
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
        drv_attached = False
        target_lease: Optional[TargetLease] = None
        if _drv is not None:
            target_lease = TargetLease(found_pid)
            drv_attached = target_lease.ensure_active()

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
                if target_lease is not None:
                    target_lease.close()
                raise GameProcessError(f"E_ACCESS ({e})") from e
        except Exception:
            if target_lease is not None:
                target_lease.close()
            raise
        self._attached_name = attached_name
        self._pid = int(found_pid)
        self._handle = int(self._pm.process_handle)
        self._target_lease = target_lease
        self._region_cache: Optional[List[MemoryRegion]] = None
        self._region_cache_time: float = 0.0

    @contextmanager
    def _driver_operation(self):
        if self._closed:
            raise TargetLeaseError(f"process reader for pid {self._pid} is closed")
        lease = self._target_lease
        if lease is None:
            raise TargetLeaseError("driver backend is unavailable")
        with lease.operation() as drv:
            yield drv

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

    def is_alive(self) -> bool:
        if self._closed:
            return False
        if self._handle and _GetExitCodeProcess is not None:
            try:
                exit_code = wintypes.DWORD(0)
                ok = _GetExitCodeProcess(
                    wintypes.HANDLE(self._handle), ctypes.byref(exit_code)
                )
                return bool(ok) and int(exit_code.value) == 259
            except Exception:
                return False
        try:
            return any(int(pid) == self._pid for _name, pid in _iter_process_entries_wide())
        except Exception:
            return False

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
        if _drv is not None:
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
            with self._driver_operation() as drv:
                pbi = (ctypes.c_byte * 48)()
                ret_len = ctypes.c_ulong(0)
                _ntqip = ctypes.windll.ntdll.NtQueryInformationProcess
                st = _ntqip(ctypes.c_void_p(self._handle), 0, pbi, 48, ctypes.byref(ret_len))
                if st < 0:
                    return None
                peb_addr = int.from_bytes(bytes(pbi[8:16]), "little")
                if not peb_addr:
                    return None

                peb_data = drv.read(peb_addr, 0x20)
                if not peb_data or len(peb_data) < 0x20:
                    return None
                ldr_addr = int.from_bytes(peb_data[0x18:0x20], "little")
                if not ldr_addr:
                    return None

                ldr_data = drv.read(ldr_addr, 0x30)
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
                    entry = drv.read(flink, 0x78)
                    if not entry or len(entry) < 0x78:
                        break
                    dll_base = int.from_bytes(entry[0x20:0x28], "little")
                    size_of_image = int.from_bytes(entry[0x40:0x44], "little")
                    name_len = int.from_bytes(entry[0x58:0x5A], "little")
                    name_buf = int.from_bytes(entry[0x60:0x68], "little")
                    name = ""
                    if name_buf and name_len:
                        raw = drv.read(name_buf, min(name_len, 520))
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
            with self._driver_operation():
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
            with self._driver_operation():
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
        if _drv is not None and hasattr(_drv, 'read_batch'):
            try:
                with self._driver_operation() as drv:
                    return drv.read_batch([(int(a), int(s)) for a, s in requests])
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
            with self._driver_operation():
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
        try:
            with self._driver_operation():
                for a in addrs:
                    try:
                        got.value = 0
                        ok = _mem_read(h, int(a), pbuf, word_size, pgot)
                        if ok and got.value == word_size:
                            out.append(int(buf.value))
                        else:
                            out.append(None)
                    except Exception:
                        out.append(None)
        except Exception:
            while len(out) < len(addrs):
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
        if not data or _drv is None:
            return False
        try:
            with self._driver_operation() as drv:
                return bool(drv.write(int(addr), data))
        except Exception:
            return False

    def can_write(self) -> bool:
        if _drv is None:
            return False
        try:
            with self._driver_operation() as drv:
                caps = drv.ENGINE_CAPS.get(drv._engine, 0)
                return bool(caps & drv.CAP_WRITE)
        except Exception:
            return False

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
        bases = list(base_addrs)
        field_offsets = list(offsets)
        try:
            with self._driver_operation():
                from mem_probe import cy_memscan as _cy
                res = _cy.read_slab_many(self._handle, bases, field_offsets, word_size)
                if res is not None:
                    return res
        except Exception:
            pass
        out: list = []
        read_fn = self.read_u64 if word_size == 8 else self.read_u32
        for base in bases:
            for off in field_offsets:
                out.append(read_fn(int(base) + int(off)))
        return out

    # ───── 关闭 ─────
    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        lease = self._target_lease
        self._target_lease = None
        if lease is not None:
            lease.close()
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
