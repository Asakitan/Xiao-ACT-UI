# -*- coding: utf-8 -*-
# Click-passthrough probe: creates a normal (non-topmost) Tk window,
# synthesizes a real mouse click on it, and reports whether the click
# arrived. With the fullscreen compositor host above it, the click only
# gets through if the host's window region has a hole there.
import sys
import ctypes
import tkinter as tk

PX, PY, PW, PH = 500, 350, 240, 160

root = tk.Tk()
root.title('click-probe')
root.geometry(f'{PW}x{PH}+{PX}+{PY}')
root.configure(bg='#227722')
got = {'click': False}


def _on_click(_e):
    got['click'] = True
    print('PROBE_CLICKED', flush=True)
    root.after(200, root.destroy)


root.bind('<Button-1>', _on_click)


def _send_click():
    # Bring the probe above other normal windows (it may have appeared
    # under the focused editor) but NOT topmost — it must stay below the
    # compositor host so the click genuinely traverses the host's region.
    root.attributes('-topmost', True)
    root.update()
    root.attributes('-topmost', False)
    user32 = ctypes.windll.user32
    user32.SetCursorPos(PX + PW // 2, PY + PH // 2)
    root.after(120, _press)


def _press():
    ctypes.windll.user32.mouse_event(0x0002, 0, 0, 0, 0)
    root.after(60, _release)


def _release():
    ctypes.windll.user32.mouse_event(0x0004, 0, 0, 0, 0)
    root.after(2500, _verdict)


def _verdict():
    if not got['click']:
        print('PROBE_BLOCKED', flush=True)
    root.destroy()


root.after(1200, _send_click)
root.mainloop()
sys.exit(0 if got['click'] else 1)
