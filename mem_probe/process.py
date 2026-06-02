"""Star.exe 进程附加与基础读取封装.

只暴露 ``StarProcess`` 一个类, 内部用 pymem 完成 OpenProcess + ReadProcessMemory.
所有读操作均包裹 try/except, 失败返回 None 而不是抛, 因为内存扫描场景下
触碰未映射页是常态。

依赖: pymem>=1.13 (PoC 可选依赖, 未在打包 spec 中)
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wintypes
import os
import sys
from dataclasses import dataclass
from typing import Iterator, List, Optional

# 保持与主项目 config.GAME_PROCESS_NAMES 同源, 避免硬编码漂移。
try:
    # tools/mem_probe/process.py -> sao_auto/ 在 sys.path 顶层时直接 import
    from config import GAME_PROCESS_NAMES  # type: ignore
except Exception:
    GAME_PROCESS_NAMES = ["star.exe"]


# ───────────────────────── Win32 常量 / 结构体 ─────────────────────────
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
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
class StarProcessError(RuntimeError):
    pass


class StarProcess:
    """对 Star.exe 的只读包装."""

    def __init__(self, process_name: Optional[str] = None) -> None:
        try:
            import pymem  # noqa: F401  延迟 import, 主程序不强依赖
        except ImportError as e:
            raise StarProcessError(
                "pymem 未安装。请在源码运行环境执行: pip install pymem"
            ) from e

        import pymem.process
        from pymem import Pymem
        from pymem.exception import ProcessNotFound, CouldNotOpenProcess

        candidates = [process_name] if process_name else list(GAME_PROCESS_NAMES)
        last_err: Optional[Exception] = None
        self._pm = None
        attached_name = None
        for name in candidates:
            if not name:
                continue
            try:
                proc = pymem.process.process_from_name(name)
                pid = int(proc.th32ProcessID)
                handle = pymem.process.open(
                    pid,
                    debug=False,
                    process_access=(
                        PROCESS_QUERY_INFORMATION
                        | PROCESS_QUERY_LIMITED_INFORMATION
                        | PROCESS_VM_READ
                    ),
                )
                if not handle:
                    raise CouldNotOpenProcess(pid)
                self._pm = Pymem()
                self._pm.process_id = pid
                self._pm.process_handle = handle
                attached_name = name
                break
            except ProcessNotFound as e:
                last_err = e
                continue
            except CouldNotOpenProcess as e:
                # 找到了但打不开 — 通常是反作弊或权限不足, 直接抛, 不再尝试其它候选
                raise StarProcessError(
                    f"找到进程 {name} 但无法 OpenProcess(PROCESS_QUERY|VM_READ); "
                    f"可能被反作弊保护或需要管理员权限。原始错误: {e}"
                ) from e
        if self._pm is None:
            raise StarProcessError(
                f"未找到游戏进程 (尝试候选: {candidates})。请确认 Star.exe 正在运行。"
                + (f" 最后错误: {last_err}" if last_err else "")
            )
        self._attached_name = attached_name
        self._pid = int(self._pm.process_id)
        self._handle = int(self._pm.process_handle)

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

    # ───── 模块 ─────
    def list_modules(self) -> List[ModuleInfo]:
        try:
            mods = list(self._pm.list_modules())
        except Exception as e:
            raise StarProcessError(f"list_modules 失败: {e}") from e
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
        raise StarProcessError(f"主模块 {target} 在模块列表中未找到")

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
        try:
            return self._pm.read_bytes(addr, n)
        except Exception:
            return None

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

    # ───── 关闭 ─────
    def close(self) -> None:
        try:
            self._pm.close_process()
        except Exception:
            pass

    def __enter__(self) -> "StarProcess":
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
    """轻量探测: 不开进程, 仅枚举 PID; 用于 attach 前预检."""
    try:
        import pymem.process
        return pymem.process.process_from_name(process_name).th32ProcessID  # type: ignore[attr-defined]
    except Exception:
        return None


if __name__ == "__main__":  # 简易自测
    print(f"is_admin={is_admin()} python={sys.executable}")
    try:
        with StarProcess() as sp:
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
