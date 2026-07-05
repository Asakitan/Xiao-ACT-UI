# -*- coding: utf-8 -*-
# Faithful repro: real WorkshopPanel publish-tab Entry click + alpha.
#
# Constructs the actual WorkshopPanel (real SaoToplevel mirror, light
# theme, canvas-scrolled publish form), switches to the publish tab, then:
# - measures the captured frame's alpha over a real Entry field
# - PtInRegion-tests the host click region at the Entry center
# - synthesizes a real click and checks the Entry receives focus
# Runs the fix (rect_hit on) and, with SAO_NO_RECT_HIT=1, the old
# per-pixel-alpha path for a direct before/after.
import os
import sys
import ctypes
from ctypes import wintypes
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

got = {'press': False, 'focus': False}


def _pt_in_region(hwnd: int, px: int, py: int):
    user32 = ctypes.windll.user32
    gdi32 = ctypes.windll.gdi32
    hrgn = gdi32.CreateRectRgn(0, 0, 0, 0)
    kind = user32.GetWindowRgn(hwnd, hrgn)
    r = wintypes.RECT()
    gdi32.GetRgnBox(hrgn, ctypes.byref(r))
    inside = bool(gdi32.PtInRegion(hrgn, px, py))
    gdi32.DeleteObject(hrgn)
    names = {0: 'NO_REGION', 1: 'NULL', 2: 'SIMPLE', 3: 'COMPLEX'}
    return names.get(kind, kind), (r.left, r.top, r.right, r.bottom), inside


def _find_entry(root_widget):
    stack = [root_widget]
    while stack:
        w = stack.pop()
        try:
            if isinstance(w, tk.Entry) and w.winfo_viewable() and w.winfo_width() > 20:
                return w
            stack.extend(w.winfo_children())
        except Exception:
            pass
    return None


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

    from gui_modules.sao_gui_workshop import WorkshopPanel

    class _StubOwner:
        def _maybe_stop_fisheye(self):
            pass

    panel = WorkshopPanel(root, _StubOwner())
    panel.show()
    panel._show_tab('publish')

    def _pump():
        try:
            uo.ensure_tk_poller()
        except Exception:
            pass
        root.after(80, _pump)
    _pump()

    def _measure():
        win = panel._win
        mirror = getattr(win, '_mirror', None)
        if mirror is None:
            print('NO_MIRROR (compositor path off)', flush=True)
            os._exit(4)

        if os.environ.get('SAO_NO_RECT_HIT'):
            try:
                mirror._layer.rect_hit = False
                print('COUNTERTEST rect_hit forced OFF', flush=True)
            except Exception as e:
                print(f'countertest err {e!r}', flush=True)

        root.update_idletasks()
        entry = _find_entry(win)
        if entry is None:
            print('NO_ENTRY_FOUND', flush=True)
            os._exit(5)
        entry.bind('<ButtonPress-1>', lambda e: got.__setitem__('press', True))
        entry.bind('<FocusIn>', lambda e: got.__setitem__('focus', True))

        ex = entry.winfo_rootx() + entry.winfo_width() // 2
        ey = entry.winfo_rooty() + entry.winfo_height() // 2

        # Probe the input-forwarding walker directly.
        try:
            w = mirror._widget_at(ex, ey)
            print(f'TARGET_ENTRY str={str(entry)} class={entry.winfo_class()}', flush=True)
            if w is None:
                print('WIDGET_AT=None', flush=True)
            else:
                print(f'WIDGET_AT str={str(w)} class={w.winfo_class()} '
                      f'is_entry={w is entry} viewable={w.winfo_viewable()}', flush=True)
        except Exception as e:
            print(f'widget_at err {e!r}', flush=True)
        # Does the entry's own binding fire when dispatched directly?
        try:
            entry.event_generate('<ButtonPress-1>', x=5, y=5,
                                  rootx=ex, rooty=ey)
            root.update()
            print(f'DIRECT_DISPATCH press={got["press"]}', flush=True)
            got['press'] = False
        except Exception as e:
            print(f'direct dispatch err {e!r}', flush=True)

        # Measure captured alpha over the entry's rect.
        layer = mirror._layer
        fb = layer._frame_bytes
        fw, fh = layer._frame_w, layer._frame_h
        wx = win.winfo_rootx()
        wy = win.winfo_rooty()
        lx0 = entry.winfo_rootx() - wx
        ly0 = entry.winfo_rooty() - wy
        lw = entry.winfo_width()
        lh = entry.winfo_height()
        if fb and fw > 0 and fh > 0:
            import numpy as np
            arr = np.frombuffer(fb, dtype=np.uint8)
            if arr.size == fw * fh * 4:
                a = arr.reshape(fh, fw, 4)[..., 3]
                sub = a[max(0, ly0):ly0 + lh, max(0, lx0):lx0 + lw]
                if sub.size:
                    print(f'ENTRY_ALPHA min={int(sub.min())} max={int(sub.max())} '
                          f'mean={float(sub.mean()):.1f} zero%={float((sub==0).mean()*100):.0f}',
                          flush=True)
        kind, box, inside = _pt_in_region(uo.hwnd, ex, ey)
        print(f'HOST_REGION kind={kind} box={box}', flush=True)
        print(f'ENTRY_CENTER=({ex},{ey}) PtInRegion={inside}', flush=True)

        u = ctypes.windll.user32
        u.SetCursorPos(ex, ey)
        root.after(150, lambda: _click(ex, ey))

    def _click(ex, ey):
        u = ctypes.windll.user32
        u.mouse_event(0x0002, 0, 0, 0, 0)
        root.after(60, lambda: (u.mouse_event(0x0004, 0, 0, 0, 0),
                                root.after(500, _verdict)))

    def _verdict():
        print(f'ENTRY_PRESS={got["press"]} ENTRY_FOCUS={got["focus"]}', flush=True)
        print('DONE', flush=True)
        os._exit(0)

    root.after(2500, _measure)
    root.after(25000, lambda: os._exit(2))
    root.mainloop()


if __name__ == '__main__':
    main()
