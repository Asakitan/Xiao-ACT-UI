# -*- coding: utf-8 -*-
# Best-effort GDI object TYPE breakdown for a target process.
#
# GetGuiResources(GR_GDIOBJECTS) (used by handle_count_monitor.py) only
# gives a TOTAL GDI object count — no way to tell whether it's bitmaps,
# device contexts, regions, brushes, pens, or fonts that are climbing.
# There is no documented public API for a type breakdown; tools like
# Process Explorer / GDIView get it by walking the session-wide
# "GDI shared handle table", a structure whose layout is NOT part of the
# public Windows SDK — it's been reverse-engineered and published by the
# tool-author community and has stayed stable across Windows 7 through
# 11 (64-bit), but is NOT guaranteed by Microsoft to stay that way.
#
# Because this is unofficial, this script cross-checks its own total
# count against the OFFICIAL GetGuiResources() number for the same PID
# before showing anything — if they don't roughly match, the layout
# assumption is wrong on this machine and the type breakdown below it is
# not trustworthy (this script says so explicitly rather than guessing).
#
# Usage:
# python tools/gdi_type_breakdown.py --pid 12345
# python tools/gdi_type_breakdown.py            # auto-detect by title "SAO"
#
# Read-only: only calls ReadProcessMemory, never WriteProcessMemory —
# cannot corrupt or destabilize the target process even if the layout
# assumptions below are wrong for this Windows build (a wrong read just
# returns garbage bytes we then sanity-check, not a crash).
import argparse
import ctypes
import sys
from ctypes import wintypes as wt

_ntdll = ctypes.windll.ntdll
_kernel32 = ctypes.windll.kernel32
_user32 = ctypes.windll.user32

_PROCESS_QUERY_INFORMATION = 0x0400
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_PROCESS_VM_READ = 0x0010

_GR_GDIOBJECTS = 0

# Reverse-engineered PEB field offset (64-bit), stable Win7 through
# Win11 per multiple independent published tools (Process Hacker,
# GDIView-style utilities) — NOT part of the public SDK.
_PEB_GDI_SHARED_HANDLE_TABLE_OFFSET = 0xF8

# GDI shared handle table cell layout (64-bit), 0x18 (24) bytes/entry:
#   +0x00 KernelAddress (8 bytes, opaque)
#   +0x08 ProcessId     (2 bytes, LOWORD of owning PID only)
#   +0x0A Count         (2 bytes)
#   +0x0C Upper         (2 bytes)
#   +0x0E Type          (2 bytes, the actual GDI object type tag)
#   +0x10 UserAddress   (8 bytes, opaque)
_GDI_CELL_SIZE = 0x18
_PID_OFFSET = 0x08
_TYPE_OFFSET = 0x0E
# Session-wide table capacity — sized generously; unused tail entries
# just read as all-zero (ProcessId 0, never matches a real target PID).
_MAX_ENTRIES = 0x10000

# GDI_OBJECT_TYPE_* tags as stored in the shared table's Type field
# (distinct from GetObjectType's OBJ_* return values) — also community-
# documented, not SDK-public.
_TYPE_NAMES = {
    0x0100: 'DC (unclassified)',
    0x0201: 'DC (memory)',
    0x0301: 'DC (metafile)',
    0x0400: 'REGION',
    0x0500: 'BITMAP',
    0x0800: 'PALETTE',
    0x0900: 'FONT (unclassified)',
    0x0A00: 'FONT',
    0x1000: 'BRUSH',
    0x2100: 'EMF_DC',
    0x2600: 'METAFILE_DC',
    0x3000: 'PEN',
    0x5000: 'EXTPEN',
    0x0000: '(empty slot)',
}


class _PROCESS_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('ExitStatus', ctypes.c_long),
        ('PebBaseAddress', ctypes.c_void_p),
        ('AffinityMask', ctypes.c_void_p),
        ('BasePriority', ctypes.c_long),
        ('UniqueProcessId', ctypes.c_void_p),
        ('InheritedFromUniqueProcessId', ctypes.c_void_p),
    ]


def _find_pid_by_title_substring(substr: str) -> int:
    found = {'pid': 0}
    substr_lower = substr.lower()
    _EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def _cb(hwnd, _lparam):
        if not _user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        _user32.GetWindowTextW(hwnd, buf, 256)
        if buf.value and substr_lower in buf.value.lower():
            pid = wt.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            found['pid'] = pid.value
            return False
        return True

    _user32.EnumWindows(_EnumWindowsProc(_cb), 0)
    return found['pid']


def _read_process_memory(hproc, address: int, size: int):
    buf = ctypes.create_string_buffer(size)
    bytes_read = ctypes.c_size_t(0)
    ok = _kernel32.ReadProcessMemory(
        hproc, ctypes.c_void_p(address), buf, size, ctypes.byref(bytes_read))
    if not ok or bytes_read.value == 0:
        return None
    return buf.raw[:bytes_read.value]


def _get_peb_address(hproc):
    pbi = _PROCESS_BASIC_INFORMATION()
    ret_len = ctypes.c_ulong(0)
    status = _ntdll.NtQueryInformationProcess(
        hproc, 0, ctypes.byref(pbi), ctypes.sizeof(pbi),
        ctypes.byref(ret_len))
    if status != 0 or not pbi.PebBaseAddress:
        return None
    return pbi.PebBaseAddress


def _get_gdi_table_address(hproc, peb_addr: int):
    data = _read_process_memory(
        hproc, peb_addr + _PEB_GDI_SHARED_HANDLE_TABLE_OFFSET, 8)
    if data is None or len(data) < 8:
        return None
    addr = int.from_bytes(data, 'little')
    return addr if addr else None


def _walk_gdi_table(hproc, table_addr: int, target_pid: int):
    type_counts = {}
    total = 0
    target_pid_lo = target_pid & 0xFFFF
    chunk_entries = 4096
    chunk_bytes = chunk_entries * _GDI_CELL_SIZE
    for base_entry in range(0, _MAX_ENTRIES, chunk_entries):
        data = _read_process_memory(
            hproc, table_addr + base_entry * _GDI_CELL_SIZE, chunk_bytes)
        if data is None:
            break
        n = len(data) // _GDI_CELL_SIZE
        for i in range(n):
            off = i * _GDI_CELL_SIZE
            pid = int.from_bytes(data[off + _PID_OFFSET:off + _PID_OFFSET + 2], 'little')
            if pid != target_pid_lo or pid == 0:
                continue
            obj_type = int.from_bytes(data[off + _TYPE_OFFSET:off + _TYPE_OFFSET + 2], 'little')
            total += 1
            type_counts[obj_type] = type_counts.get(obj_type, 0) + 1
    return total, type_counts


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, default=0)
    ap.add_argument('--title', type=str, default='SAO')
    args = ap.parse_args()

    pid = args.pid or _find_pid_by_title_substring(args.title)
    if not pid:
        print(f'No visible window matching title {args.title!r}. Pass '
              f'--pid <PID> explicitly.', flush=True)
        return 1

    hproc = _kernel32.OpenProcess(
        _PROCESS_QUERY_INFORMATION | _PROCESS_QUERY_LIMITED_INFORMATION
        | _PROCESS_VM_READ, False, pid)
    if not hproc:
        print(f'OpenProcess failed for PID {pid} (need PROCESS_VM_READ — '
              f'try running from an elevated prompt if this fails).',
              flush=True)
        return 1

    official_count = _user32.GetGuiResources(hproc, _GR_GDIOBJECTS)
    print(f'PID {pid}: official GetGuiResources GDI count = {official_count}',
          flush=True)

    peb = _get_peb_address(hproc)
    if peb is None:
        print('Could not read PEB address (NtQueryInformationProcess '
              'failed) — cannot attempt type breakdown.', flush=True)
        _kernel32.CloseHandle(hproc)
        return 1

    table_addr = _get_gdi_table_address(hproc, peb)
    if table_addr is None:
        print('Could not read GdiSharedHandleTable pointer from PEB+0xF8 — '
              'this offset may not apply to this Windows build. Type '
              'breakdown unavailable.', flush=True)
        _kernel32.CloseHandle(hproc)
        return 1

    total, type_counts = _walk_gdi_table(hproc, table_addr, pid)
    _kernel32.CloseHandle(hproc)

    print(f'Table walk found {total} entries for this PID '
          f'(table @ 0x{table_addr:016X}).', flush=True)

    if official_count == 0:
        print('Official count is 0 — nothing to validate against.', flush=True)
        return 0

    ratio = total / official_count
    if not (0.5 <= ratio <= 2.0):
        print(f'\n*** UNRELIABLE: table-walk total ({total}) does not '
              f'reasonably match the official count ({official_count}), '
              f'ratio={ratio:.2f}. The reverse-engineered offset/layout '
              f'this script assumes does not hold on this Windows build — '
              f'do NOT trust the type breakdown below (shown anyway for '
              f'reference, but treat it as noise, not signal). ***\n',
              flush=True)
    else:
        print(f'Table-walk total is close enough to the official count '
              f'(ratio={ratio:.2f}) to trust the breakdown below.\n',
              flush=True)

    print('Type breakdown (raw tag -> count, sorted by count desc):', flush=True)
    for tag, count in sorted(type_counts.items(), key=lambda kv: -kv[1]):
        name = _TYPE_NAMES.get(tag, f'unknown(0x{tag:04X})')
        print(f'  0x{tag:04X}  {name:24s}  count={count}', flush=True)

    return 0


if __name__ == '__main__':
    sys.exit(main())
