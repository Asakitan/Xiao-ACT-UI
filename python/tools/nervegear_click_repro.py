# -*- coding: utf-8 -*-
# Repro harness for the NerveGear full-screen click-block bug.
#
# Boots the unified compositor + a real GpuNerveGearButton (interactive
# layer, click_through=False) exactly like NervGear-ON mode. While it
# runs, a separate process (tools/click_passthrough_probe.py) verifies
# that clicks OUTSIDE the button still reach other windows, and a
# synthetic click on the button itself must fire its on_click.
import os
import sys
import tkinter as tk

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

BX, BY = 1700, 900


def main() -> None:
    root = tk.Tk()
    root.geometry('80x40+0+0')
    root.withdraw()

    from render.gpu_overlay_window import (
        set_unified_overlay_mode, prestart_unified_overlay,
        _get_unified_overlay)
    set_unified_overlay_mode(True)
    prestart_unified_overlay(root)
    uo = _get_unified_overlay(root)
    if not uo.wait_ready(timeout=15.0):
        print('NG_COMPOSITOR_TIMEOUT', flush=True)
        os._exit(3)

    from gui_modules.sao_gui_nervegear_button import GpuNerveGearButton

    btn = GpuNerveGearButton(
        root, BX, BY,
        on_click=lambda: print('NG_CLICKED', flush=True),
    )
    btn.deiconify()

    def _pump_tk_poller() -> None:
        # The real app's main loop calls this so compositor-routed input
        # callbacks (post_to_tk) get drained on the Tk thread.
        try:
            uo.ensure_tk_poller()
        except Exception:
            pass
        root.after(100, _pump_tk_poller)

    _pump_tk_poller()
    root.after(1500, lambda: print('NG_READY', flush=True))
    root.after(45000, lambda: os._exit(0))
    root.mainloop()


if __name__ == '__main__':
    main()
