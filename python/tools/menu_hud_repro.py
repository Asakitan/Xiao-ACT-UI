# -*- coding: utf-8 -*-
"""Offline repro harness for the SAO popup-menu glass HUD alignment bug.

Boots the real SAOPopUpMenu (GPU HUD path) against a fake floating anchor,
then dumps the ground-truth numbers from every stage of the geometry
pipeline (Tk content frame -> MenuHudOverlay -> GpuOverlayWindow ->
compositor layer) plus a full-screen screenshot for visual measurement.
"""
import os
import sys
import json
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import tkinter as tk

SHOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'menu_hud_repro.png')

def main() -> None:
    root = tk.Tk()
    root.geometry('80x40+0+0')
    root.title('hud-repro-root')

    # Opaque backdrop so the translucent plate is measurable in a screenshot.
    backdrop = tk.Toplevel(root)
    backdrop.overrideredirect(True)
    sw, sh = root.winfo_screenwidth(), root.winfo_screenheight()
    backdrop.geometry(f'{sw}x{sh}+0+0')
    backdrop.configure(bg='#101010')
    backdrop.lower()

    # Fake floating anchor ball, mid-right like the real app.
    ball = tk.Toplevel(root)
    ball.overrideredirect(True)
    ball.attributes('-topmost', True)
    if len(sys.argv) >= 3:
        bx, by = int(sys.argv[1]), int(sys.argv[2])
    else:
        bx, by = int(sw * 0.62), int(sh * 0.72)
    ball.geometry(f'48x48+{bx}+{by}')
    ball.configure(bg='#ff00ff')

    root.update_idletasks()

    # Match production: menu HUD renders as a unified-compositor layer.
    from render.gpu_overlay_window import (
        set_unified_overlay_mode, prestart_unified_overlay)
    set_unified_overlay_mode(True)
    prestart_unified_overlay(root)
    from render.gpu_overlay_window import _get_unified_overlay
    uo = _get_unified_overlay(root)
    if not uo.wait_ready(timeout=15.0):
        print('[repro] compositor never became ready')
        os._exit(3)
    print('[repro] compositor ready, opening menu')

    from sao_theme.popup_menu import SAOPopUpMenu

    icons = [
        {'icon': '⚙', 'name': 'settings'},
        {'icon': '#', 'name': 'grid'},
        {'icon': '⬡', 'name': 'hex'},
        {'icon': '◉', 'name': 'dot'},
        {'icon': 'i', 'name': 'info', 'can_active': False},
        {'icon': '♞', 'name': 'pet'},
    ]
    child_menus = {
        'settings': [{'label': 'Item A'}, {'label': 'Item B'}],
        'grid': [{'label': 'Item C'}],
    }

    menu = SAOPopUpMenu(
        root, icons, child_menus,
        username='Repro', description='alignment probe',
        key_code='a', slide_down=False,
        anchor_widget=ball,
        external_close=False, alt_toggle_close=False,
        cascade_mode=True,
    )
    menu.open()

    samples = []

    def sample(tag: str) -> None:
        try:
            content = menu._content
            hud = menu._hud_overlay
            gw = getattr(hud, '_gpu_window', None) if hud else None
            layer = getattr(gw, '_delegate', None) if gw else None
            rec = {
                'tag': tag,
                't': round(time.time() % 1000, 3),
                'ball_xy': (ball.winfo_rootx(), ball.winfo_rooty(),
                            ball.winfo_width(), ball.winfo_height()),
                'content_root_xy': (content.winfo_rootx(), content.winfo_rooty()),
                'content_wh': (content.winfo_width(), content.winfo_height()),
                'content_req_wh': (content.winfo_reqwidth(), content.winfo_reqheight()),
                'content_place_xy': (menu._content_x, menu._content_y),
                'overlay_root_xy': (menu._overlay.winfo_rootx(), menu._overlay.winfo_rooty()),
                'overlay_wh': (menu._overlay.winfo_width(), menu._overlay.winfo_height()),
                'content_winfo_xy_in_overlay': (content.winfo_x(), content.winfo_y()),
            }
            if hud is not None:
                rec['hud_anchor'] = (hud._anchor_x, hud._anchor_y)
                rec['hud_content_wh'] = (hud._content_w, hud._content_h)
                rec['hud_sprite_off'] = tuple(hud._sprite_off)
                rec['hud_gpu_pad'] = hud._renderer.gpu_pad
                rec['hud_screen_wh'] = (hud._screen_w, hud._screen_h)
            if gw is not None:
                rec['gpu_win_xywh'] = (gw._x, gw._y, gw._w, gw._h)
                rec['gpu_win_unified'] = bool(getattr(gw, '_unified', False))
            if layer is not None:
                lyr = getattr(layer, '_layer', layer)
                if hasattr(lyr, 'x'):
                    rec['layer_xywh'] = (lyr.x, lyr.y, lyr.width, lyr.height)
                    rec['layer_frame_wh'] = (getattr(lyr, '_frame_w', None),
                                             getattr(lyr, '_frame_h', None))
                else:
                    rec['delegate_type'] = type(layer).__name__
                    rec['delegate_attrs'] = [a for a in vars(layer)][:30]
            # Expected: HUD window top-left == content top-left - gpu_pad
            if hud is not None and gw is not None:
                exp_x = rec['content_root_xy'][0] - rec['hud_gpu_pad']
                exp_y = rec['content_root_xy'][1] - rec['hud_gpu_pad']
                rec['EXPECT_win_xy'] = (exp_x, exp_y)
                rec['DELTA_win_xy'] = (gw._x - exp_x, gw._y - exp_y)
                exp_w = rec['content_wh'][0] + 2 * rec['hud_gpu_pad']
                exp_h = rec['content_wh'][1] + 2 * rec['hud_gpu_pad']
                rec['EXPECT_win_wh'] = (exp_w, exp_h)
                rec['DELTA_win_wh'] = (gw._w - exp_w, gw._h - exp_h)
            samples.append(rec)
            print(json.dumps(rec, ensure_ascii=False))
            sys.stdout.flush()
        except Exception as e:  # noqa: BLE001
            print(f'[sample {tag}] error: {e!r}')
            sys.stdout.flush()

    def shoot() -> None:
        try:
            from PIL import ImageGrab
            img = ImageGrab.grab()
            img.save(SHOT)
            print(f'[shot] saved {SHOT} size={img.size}')
        except Exception as e:  # noqa: BLE001
            print(f'[shot] error: {e!r}')
        sys.stdout.flush()

    def finish() -> None:
        try:
            menu.close()
        except Exception:
            pass
        root.after(600, lambda: os._exit(0))

    root.after(1200, lambda: sample('t+1.2s'))
    root.after(2500, lambda: sample('t+2.5s'))
    root.after(3600, lambda: sample('t+3.6s'))
    root.after(3800, shoot)
    root.after(4600, finish)
    root.after(15000, lambda: os._exit(2))
    root.mainloop()


if __name__ == '__main__':
    main()
