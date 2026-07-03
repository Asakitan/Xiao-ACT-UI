# -*- coding: utf-8 -*-
"""Standalone target process for cross-process click-passthrough tests.

Runs as its OWN separate process (not a thread, not another Tk toplevel
in the same interpreter) — a plain, ordinary top-level window at a
known screen rect. Writes 'CLICKED' to the given file the first time it
receives a real left-button click, then exits shortly after.

This exists because every earlier test in this investigation used
same-process Tk windows to represent "the game", which cannot catch
bugs specific to cross-process/cross-thread click delivery (the exact
class of bug this session's SetWindowRgn regression falls into).
"""
import sys
import tkinter as tk


def main() -> None:
    x, y, w, h, marker_path = sys.argv[1:6]
    x, y, w, h = int(x), int(y), int(w), int(h)

    root = tk.Tk()
    root.title('cross_process_target')
    root.overrideredirect(True)
    root.geometry(f'{w}x{h}+{x}+{y}')
    root.configure(bg='#2a6e2a')
    root.attributes('-topmost', False)

    def _on_click(_event=None):
        try:
            with open(marker_path, 'w', encoding='utf-8') as f:
                f.write('CLICKED')
        except Exception:
            pass
        root.after(200, root.destroy)

    root.bind('<ButtonPress-1>', _on_click)
    root.after(15000, root.destroy)  # safety timeout
    root.mainloop()


if __name__ == '__main__':
    main()
