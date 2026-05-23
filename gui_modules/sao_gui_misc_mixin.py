# -*- coding: utf-8 -*-
"""
SAOPlayerGUIMiscMixin — nineteenth mixin extracted from
SAOPlayerGUI (round 68 of the sao_gui split refactor). 14 methods,
~210 lines.

A grab-bag of small helpers that hadn't fit into any of the
previous 18 domain-focused mixins. Each one is small (≤51 lines)
and has no obvious sibling cluster.

Methods:
  * _set_icon (5) — set Tk app icon via _apply_window_icon.
  * _create_hp_alpha_strip_windows (5) — legacy HP alpha strip
    no-op (kept for compat).
  * _render_hp_strip_image (3) — legacy no-op.
  * _sync_hp_alpha_strip_windows (4) — legacy no-op.
  * _render_hp_shell (4) — legacy no-op.
  * _render_hp_dynamic (4) — legacy no-op.
  * _get_skillfx_layout (51) — pick SkillFX overlay layout based on
    burst-trigger gs flags (used by State mixin's _push_packet_overlays).
  * _get_game_window_rect (25) — game window bounding rect for
    overlay alignment.
  * _get_game_window_context (26) — game window context dict for
    multi-monitor setups.
  * _format_level_text (3) — formats level+extra string ("123(+45)"
    when extra>0, else just "123").
  * _fade_panel_in (30) — ease-out alpha fade for floating panels.
  * _fade_panel_out (39) — ease-in alpha fade-then-destroy for
    floating panels.
  * _switch_to_old_ui (4) — no-op (old UI removed).
  * _show_leaderboard (4) — no-op (leaderboard removed).

Required SAOPlayerGUI attrs:
  * self._float, self._fisheye_ov, self.root, self.settings
  * self._burst_overlay, self._packet_engine (state-dependent paths)

Required SAOPlayerGUI methods (via MRO):
  * _apply_window_icon (from sao_panel_ui via the re-import block)
  * _start_fisheye_overlay (Fisheye mixin)
  * _maybe_stop_fisheye (Fisheye mixin)
  * _pick_burst_trigger_slot (EngineToggles mixin)
"""

from __future__ import annotations

import time
from typing import Any, Dict, Optional

from gui_modules.sao_panel_ui import _apply_window_icon, _set_process_app_id


class SAOPlayerGUIMiscMixin:
    """Mixin bundling small helpers that didn't fit elsewhere."""

    def _set_icon(self):
        _set_process_app_id('sao.auto.game.ui')
        _apply_window_icon(self.root)
        # icon.ico 应用到 root (所有子窗口自动继承)

    def _create_hp_alpha_strip_windows(self):
        """(ULW 模式下 HP 填充已由 PIL alpha 梯度渲染, 不再需要条带窗口)"""
        self._hp_alpha_windows = []
        self._hp_alpha_photos = []

    def _render_hp_strip_image(self, *a, **kw):
        return None

    def _sync_hp_alpha_strip_windows(self):
        """(ULW 模式下不需要同步条带窗口)"""
        pass

    def _render_hp_shell(self, hover=False, scale=4):
        """(deprecated) HP 外壳已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _render_hp_dynamic(self):
        """(deprecated) HP 动态内容已由 sao_gui_hp.HpOverlay 独立渲染。"""
        return None

    def _get_skillfx_layout(self, gs=None):
        """Compute the screen-relative layout for the BurstReady overlay.

        Mirrors the one in sao_webview._get_skillfx_layout so the tk
        port's anchor + callout positioning matches the original webview.

        v2.4.33: hot-loop geometry math moved to ``_sao_cy_uihelpers``.
        Python only collects the gs/window_locator inputs.
        """
        if gs is None and getattr(self, '_state_mgr', None) is not None:
            gs = self._state_mgr.state
        client_rect = getattr(gs, 'window_rect', None) if gs else None
        if not client_rect:
            try:
                from window_locator import WindowLocator
                client_rect = WindowLocator().get_rect()
            except Exception:
                client_rect = None
        if not client_rect:
            return None
        client_left, client_top = int(client_rect[0]), int(client_rect[1])

        slot_rects: List[Dict[str, Any]] = []
        for slot in list(getattr(gs, 'skill_slots', []) or []) if gs else []:
            if not isinstance(slot, dict):
                continue
            rect = slot.get('rect') or {}
            try:
                sx = int(rect.get('x', 0)); sy = int(rect.get('y', 0))
                sw = int(rect.get('w', 0)); sh = int(rect.get('h', 0))
                idx = int(slot.get('index', 0) or 0)
            except Exception:
                continue
            if idx <= 0 or sw <= 0 or sh <= 0:
                continue
            slot_rects.append({
                'index': idx,
                'screen_rect': {'x': client_left + sx, 'y': client_top + sy,
                                'w': sw, 'h': sh},
            })
        fallback: List[Dict[str, Any]] = []
        if not slot_rects:
            for item in get_skill_slot_rects(client_rect):
                left, top, right, bottom = item['bbox']
                fallback.append({
                    'index': int(item['index']),
                    'screen_rect': {'x': left, 'y': top,
                                    'w': right - left, 'h': bottom - top},
                })
        return _CY_UI.compute_skillfx_layout(client_rect, slot_rects, fallback)

    def _get_game_window_rect(self):
        rect = None
        try:
            gs = self._state_mgr.state if getattr(self, '_state_mgr', None) is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from window_locator import WindowLocator
            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()
                self._locator = locator
            result = locator.find_game_window()
            if result:
                _hwnd, _title, found_rect = result
                return tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return rect

    def _get_game_window_context(self):
        rect = None
        hwnd = 0
        try:
            gs = self._state_mgr.state if getattr(self, '_state_mgr', None) is not None else None
            window_rect = getattr(gs, 'window_rect', None) if gs else None
            if isinstance(window_rect, (list, tuple)) and len(window_rect) == 4:
                rect = tuple(int(v) for v in window_rect)
        except Exception:
            rect = None

        try:
            from window_locator import WindowLocator
            locator = getattr(self, '_locator', None)
            if locator is None:
                locator = WindowLocator()
                self._locator = locator
            result = locator.find_game_window()
            if result:
                hwnd, _title, found_rect = result
                return int(hwnd or 0), tuple(int(v) for v in found_rect)
        except Exception:
            pass

        return int(hwnd or 0), rect

    def _format_level_text(self, level_base: int, level_extra: int) -> str:
        return _CY_UI.format_level_text(level_base, level_extra, self._level or 1)

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

    def _switch_to_old_ui(self):
        """Old UI 已移除 — no-op"""
        pass

    def _show_leaderboard(self):
        """排行榜已移除 — no-op"""
        pass

