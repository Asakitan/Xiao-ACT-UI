# -*- coding: utf-8 -*-
"""Repro/verify: a mirrored Tk panel's text Entry must be clickable.

Boots the unified compositor + a SaoToplevel (real TkMirrorLayer path)
containing a Tk Entry and a Button. Reports the host window region box
and synthesizes a real click on the Entry, checking it receives focus
and the ButtonPress binding fires. Before the rect_hit fix the Entry's
zero-alpha capture pixels were punched out of the region and the click
never reached it.
"""
import os
import sys
import ctypes
from ctypes import wintypes
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

PX, PY, PW, PH = 700, 400, 360, 220
got = {'entry_press': False, 'entry_focus': False, 'btn_press': False}


def _host_region(hwnd: int):
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    hrgn = gdi32.CreateRectRgn(0, 0, 0, 0)
    kind = user32.GetWindowRgn(hwnd, hrgn)
    r = wintypes.RECT()
    gdi32.GetRgnBox(hrgn, ctypes.byref(r))
    gdi32.DeleteObject(hrgn)
    names = {0: 'NO_REGION', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}
    return names.get(kind, kind), (r.left, r.top, r.right, r.bottom)


def main() -> None:
    root = tk.Tk()
    root.geometry('80x40+0+0')
    root.withdraw()

    from config import SettingsManager
    SettingsManager().set('compositor_tk_panels', True)

    from render.gpu_overlay_window import (
        set_unified_overlay_mode, prestart_unified_overlay,
        _get_unified_overlay)
    set_unified_overlay_mode(True)
    prestart_unified_overlay(root)
    uo = _get_unified_overlay(root)
    if not uo.wait_ready(timeout=15.0):
        print('COMPOSITOR_TIMEOUT', flush=True)
        os._exit(3)

    from render.tk_mirror import SaoToplevel

    win = SaoToplevel(root, mirror_name='entry_click_test')
    win.overrideredirect(True)
    win.geometry(f'{PW}x{PH}+{PX}+{PY}')
    win.configure(bg='#1a2230')

    tk.Label(win, text='mirror entry test', bg='#1a2230', fg='#ffffff').pack(pady=8)
    entry = tk.Entry(win, width=30)
    entry.pack(pady=10, ipady=4)
    btn = tk.Button(win, text='a button')
    btn.pack(pady=8)

    entry.bind('<ButtonPress-1>', lambda e: got.__setitem__('entry_press', True))
    entry.bind('<FocusIn>', lambda e: got.__setitem__('entry_focus', True))
    btn.bind('<ButtonPress-1>', lambda e: got.__setitem__('btn_press', True))

    win.deiconify()

    # Counter-test: force the old per-pixel-alpha region path to prove the
    # Entry really was excluded (root-cause confirmation, not just fix).
    if os.environ.get('SAO_NO_RECT_HIT'):
        try:
            m = win._mirror
            if m is not None and m._layer is not None:
                m._layer.rect_hit = False
                print('COUNTERTEST rect_hit forced OFF', flush=True)
        except Exception as e:
            print(f'countertest err {e!r}', flush=True)

    def _pump():
        try:
            uo.ensure_tk_poller()
        except Exception:
            pass
        root.after(80, _pump)
    _pump()

    def _report_region():
        hwnd = uo.hwnd
        kind, box = _host_region(hwnd)
        # Entry center in screen coords.
        root.update_idletasks()
        ex = entry.winfo_rootx() + entry.winfo_width() // 2
        ey = entry.winfo_rooty() + entry.winfo_height() // 2
        covers = box[0] <= ex < box[2] and box[1] <= ey < box[3]
        print(f'HOST_REGION kind={kind} box={box}', flush=True)
        print(f'ENTRY_CENTER=({ex},{ey}) region_covers_entry={covers}', flush=True)
        # Click the entry.
        u = ctypes.windll.user32
        u.SetCursorPos(ex, ey)
        root.after(150, lambda: _click(ex, ey))

    def _click(ex, ey):
        u = ctypes.windll.user32
        u.mouse_event(0x0002, 0, 0, 0, 0)
        root.after(60, lambda: (u.mouse_event(0x0004, 0, 0, 0, 0),
                                root.after(500, _verdict)))

    def _verdict():
        print(f'ENTRY_PRESS={got["entry_press"]} ENTRY_FOCUS={got["entry_focus"]} '
              f'BTN_PRESS={got["btn_press"]}', flush=True)
        print('DONE', flush=True)
        os._exit(0)

    root.after(2000, _report_region)
    root.after(20000, lambda: os._exit(2))
    root.mainloop()


if __name__ == '__main__':
    main()
