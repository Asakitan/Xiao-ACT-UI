# -*- coding: utf-8 -*-
# Standalone Win32 handle-count monitor — no changes to the main app.
#
# Logs USER objects / GDI objects / total handle count for a target
# process every few seconds to a CSV, so a long real-usage session can be
# correlated against the exact moment a hang/CPU+GPU-spike happens
# without needing Task Manager's "USER objects"/"GDI objects" columns
# added manually (they're not shown by default).
#
# Usage:
# python tools/handle_count_monitor.py --pid 12345
# python tools/handle_count_monitor.py --title SAO
# python tools/handle_count_monitor.py            # auto-detect by title
#
# Ctrl+C to stop. Writes to tools/handle_count_log_<pid>.csv (append mode
# — safe to stop/restart without losing history from this run).
import argparse
import csv
import ctypes
import os
import sys
import time
from ctypes import wintypes as wt

_user32 = ctypes.windll.user32
_kernel32 = ctypes.windll.kernel32

_GR_GDIOBJECTS = 0
_GR_USEROBJECTS = 1
_PROCESS_QUERY_INFORMATION = 0x0400
_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000

_user32.GetGuiResources.restype = wt.DWORD
_user32.GetGuiResources.argtypes = [wt.HANDLE, wt.DWORD]

_kernel32.OpenProcess.restype = wt.HANDLE
_kernel32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
_kernel32.GetProcessHandleCount.restype = wt.BOOL
_kernel32.GetProcessHandleCount.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
_kernel32.CloseHandle.restype = wt.BOOL
_kernel32.CloseHandle.argtypes = [wt.HANDLE]

_EnumWindows = _user32.EnumWindows
_EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
_GetWindowTextW = _user32.GetWindowTextW
_GetWindowThreadProcessId = _user32.GetWindowThreadProcessId
_IsWindowVisible = _user32.IsWindowVisible


def _find_pid_by_title_substring(substr: str) -> int:
    found = {'pid': 0}
    substr_lower = substr.lower()

    def _cb(hwnd, _lparam):
        if not _IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        _GetWindowTextW(hwnd, buf, 256)
        title = buf.value
        if title and substr_lower in title.lower():
            pid = wt.DWORD()
            _GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            found['pid'] = pid.value
            return False  # stop enumeration
        return True

    _EnumWindows(_EnumWindowsProc(_cb), 0)
    return found['pid']


def _open_process(pid: int):
    h = _kernel32.OpenProcess(
        _PROCESS_QUERY_INFORMATION | _PROCESS_QUERY_LIMITED_INFORMATION,
        False, pid)
    return h if h else None


def _sample(hproc) -> tuple:
    user_objs = _user32.GetGuiResources(hproc, _GR_USEROBJECTS)
    gdi_objs = _user32.GetGuiResources(hproc, _GR_GDIOBJECTS)
    handle_count = wt.DWORD(0)
    _kernel32.GetProcessHandleCount(hproc, ctypes.byref(handle_count))
    return user_objs, gdi_objs, handle_count.value


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int, default=0,
                     help='Target process PID (find via Task Manager if '
                          'title auto-detect fails)')
    ap.add_argument('--title', type=str, default='SAO',
                     help='Substring to match a visible window title if '
                          '--pid is not given (default: "SAO")')
    ap.add_argument('--interval', type=float, default=5.0,
                     help='Seconds between samples (default: 5)')
    args = ap.parse_args()

    pid = args.pid
    if not pid:
        pid = _find_pid_by_title_substring(args.title)
        if not pid:
            print(f'No visible window with title containing {args.title!r} '
                  f'found. Pass --pid <PID> explicitly (check Task Manager '
                  f'-> Details tab -> find the SAO process -> PID column).',
                  flush=True)
            return 1
        print(f'Auto-detected PID {pid} via window title match.', flush=True)

    hproc = _open_process(pid)
    if hproc is None:
        print(f'OpenProcess failed for PID {pid} (process may not exist, '
              f'or insufficient access — try running this as the same '
              f'user that started the target process).', flush=True)
        return 1

    log_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        f'handle_count_log_{pid}.csv')
    is_new = not os.path.exists(log_path)
    print(f'Monitoring PID {pid} every {args.interval}s -> {log_path}',
          flush=True)
    print('Columns: timestamp, elapsed_s, user_objects, gdi_objects, '
          'handle_count', flush=True)

    t0 = time.perf_counter()
    with open(log_path, 'a', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        if is_new:
            writer.writerow(
                ['timestamp', 'elapsed_s', 'user_objects', 'gdi_objects',
                 'handle_count'])
        try:
            while True:
                user_objs, gdi_objs, handles = _sample(hproc)
                now = time.strftime('%Y-%m-%d %H:%M:%S')
                elapsed = round(time.perf_counter() - t0, 1)
                writer.writerow([now, elapsed, user_objs, gdi_objs, handles])
                f.flush()
                print(f'{now}  t+{elapsed:>7.1f}s  '
                      f'USER={user_objs:>5}  GDI={gdi_objs:>5}  '
                      f'handles={handles:>6}', flush=True)
                time.sleep(args.interval)
        except KeyboardInterrupt:
            print('\nStopped.', flush=True)
    _kernel32.CloseHandle(hproc)
    return 0


if __name__ == '__main__':
    sys.exit(main())
