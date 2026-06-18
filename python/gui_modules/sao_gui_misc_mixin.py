# -*- coding: utf-8 -*-
"""
SAOPlayerGUIMiscMixin — nineteenth mixin extracted from
SAOPlayerGUI (round 68 of the sao_gui split refactor). 14 methods,
~210 lines.

A grab-bag of small helpers that hadn't fit into any of the
previous 18 domain-focused mixins. Each one is small (≤51 lines)
and has no obvious sibling cluster.

Methods here are platform-level helpers only. Game-specific layout,
window, and level helpers are injected by game plugins.
"""

from __future__ import annotations

import time

from utils.sao_sound import play_sound
from gui_modules.sao_panel_ui import _apply_window_icon, _set_process_app_id


class SAOPlayerGUIMiscMixin:
    """Mixin bundling small helpers that didn't fit elsewhere."""

    def _set_icon(self):
        _set_process_app_id('sao.auto.game.ui')
        _apply_window_icon(self.root)
        # icon.ico 应用到 root (所有子窗口自动继承)

    def _fade_panel_in(self, panel, target=0.92, duration_ms=350):
        """浮动面板淡入 — 平滑 ease-out 动画, 并确保鱼眼叠加层运行"""
        # 面板打开时, 如果鱼眼尚未启动则启动
        if self._fisheye_ov is None:
            try:
                self.root.after(100, self._start_fisheye_overlay)
            except Exception:
                pass

        t0 = time.time()
        dur = duration_ms / 1000.0

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / dur)
            et = 1 - (1 - t) ** 3  # ease_out
            panel.attributes('-alpha', target * et)
            if t < 1.0:
                try:
                    self.root.after(16, _step)
                except Exception:
                    pass

        _step()

    def _fade_panel_out(self, panel, attr_name, settings_key, duration_ms=220):
        """浮动面板淡出 — ease-in 动画, 完成后 destroy"""
        try: play_sound('alert_close')
        except: pass
        try:
            start_alpha = float(panel.attributes('-alpha'))
        except Exception:
            start_alpha = 0.92

        t0 = time.time()
        dur = duration_ms / 1000.0

        def _step():
            try:
                if not panel.winfo_exists():
                    return
            except Exception:
                return
            elapsed = time.time() - t0
            t = min(1.0, elapsed / dur)
            et = t * t  # ease_in
            panel.attributes('-alpha', start_alpha * (1.0 - et))
            if t < 1.0:
                try:
                    self.root.after(16, _step)
                except Exception:
                    pass
            else:
                try:
                    panel.destroy()
                except Exception:
                    pass
                setattr(self, attr_name, None)
                self.settings.set(settings_key, False)
                self.settings.save()
                self._maybe_stop_fisheye()

        _step()
