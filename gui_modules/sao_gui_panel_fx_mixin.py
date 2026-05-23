# -*- coding: utf-8 -*-
"""
SAOPlayerGUIPanelFxMixin — seventeenth mixin extracted from
SAOPlayerGUI (round 64 of the sao_gui split refactor). 2 methods +
2 class attributes + 1 module-level helper, ~150 lines.

The SAO HUD panel-fx shared scheduler — the bit that draws the
faint left/right ornamental sliders on every floating panel and
ticks them together on one shared after(90 ms) loop so multiple
floating panels share a single main-thread callback.

Methods + class attrs:
  * Class attr ``_sao_fx_panels: list[tuple]`` — active panel
    list, items are ``(panel, body_canvas, pw, ph)``.
  * Class attr ``_sao_fx_after_id: int | None`` — shared after()
    ID (None when no panels are registered).
  * ``_attach_sao_panel_fx(self, panel, header, inner, accent)`` —
    register a Tk floating panel for the shared HUD scheduler.
    Sets up:
      - one HUD canvas under the header + one under the body
        (via ``_make_sao_panel_hud``)
      - signature cache (sao_fx_last_sig) so a tick with no
        coordinate change skips Canvas operations entirely
      - destroy binding to auto-remove the panel from the
        registry when its Toplevel goes away
      - starts the shared tick if this is the first registration
  * ``_sao_fx_shared_tick(self_ref)`` (staticmethod, ``@_probe``-
    decorated) — the 90 ms shared loop. Iterates the active panels
    list, computes per-panel coordinates via
    ``_CY_UI.sao_fx_coords``, and only moves Canvas items whose
    sig changed. Re-schedules itself via ``self_ref.root.after``.

Required SAOPlayerGUI attrs:
  * self.root, self._destroyed

These class attributes previously lived on
``SAOPlayerGUIFloatHpMixin`` (incidentally — they were declared
there because they had been on SAOPlayerGUI proper and survived
the round-56 extraction). Round 64 relocates them to their
proper home with the methods that use them. Existing
``SAOPlayerGUI._sao_fx_*`` references inside the methods are
rewritten to ``type(self)._sao_fx_*`` / ``type(self_ref)._sao_fx_*``
so the mixin doesn't need to import the not-yet-defined
SAOPlayerGUI class (semantics identical via MRO + class-attr
lookup).
"""

from __future__ import annotations

import time
import tkinter as tk
from typing import Any, List, Optional, Tuple

from perf_probe import probe as _probe, gauge as _perf_gauge

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


def _make_sao_panel_hud(parent, width: int, height: int, alpha: float = 0.18):
    """生成一个轻量 SAO HUD 画布，提供左右错层飘移装饰。"""
    cv = tk.Canvas(parent, width=width, height=height, bg=parent.cget('bg'),
                   highlightthickness=0, bd=0)
    cv.place(x=0, y=0, relwidth=1, relheight=1)
    cv.tk.call('lower', cv._w)
    return cv


class SAOPlayerGUIPanelFxMixin:
    """Mixin bundling the SAO HUD panel-fx shared scheduler + state."""

    _sao_fx_panels = []       # [(panel, body_cv, pw, ph)] — 活跃面板列表
    _sao_fx_after_id = None   # 共享 after ID

    def _attach_sao_panel_fx(self, panel, header, inner, accent='#86dfff'):
        """给 Tk 浮动面板附加 SAO 风格 HUD 装饰 (共享调度 + 签名缓存).

        v2.3.15 优化:
        - 所有面板共用一个 after(66) 循环 (合并去重)
        - Canvas 内容按坐标签名缓存, 整数坐标不变时跳过 delete+rebuild
        - 从 33ms/panel → 66ms/global, 多面板时主线程开销从 O(N) 降到 O(1)
        """
        try:
            panel.update_idletasks()
            pw = max(80, panel.winfo_width())
            ph = max(60, panel.winfo_height())
        except Exception:
            return

        if getattr(panel, '_sao_fx_inited', False):
            return
        panel._sao_fx_inited = True
        header_cv = _make_sao_panel_hud(header, pw, 24)
        body_cv = _make_sao_panel_hud(inner, pw, ph)
        panel._sao_header_hud = header_cv
        panel._sao_body_hud = body_cv

        # 签名缓存: 上次绘制的坐标元组, 不变时跳过
        panel._sao_fx_last_sig = None
        cyan = '#86dfff'
        gold = '#f3af12'
        panel._sao_fx_items = {
            'lines': [
                body_cv.create_line(0, 0, 0, 0, fill=cyan, width=1),
                body_cv.create_line(0, 0, 0, 0, fill=gold, width=1),
                body_cv.create_line(0, 0, 0, 0, fill=cyan, width=1),
                body_cv.create_line(0, 0, 0, 0, fill=gold, width=1),
            ],
            'ticks': [
                (
                    body_cv.create_line(0, 0, 0, 0, fill=cyan, width=1),
                    body_cv.create_line(0, 0, 0, 0, fill=gold, width=1),
                )
                for _ in range(5)
            ],
            'rects': [
                body_cv.create_rectangle(0, 0, 0, 0, outline=cyan, width=1),
                body_cv.create_rectangle(0, 0, 0, 0, outline=gold, width=1),
            ],
        }

        # 注册到共享列表
        type(self)._sao_fx_panels.append((panel, body_cv, pw, ph))

        # 面板销毁时自动移除
        def _on_destroy(event=None):
            type(self)._sao_fx_panels[:] = [
                (p, cv, w, h) for p, cv, w, h in type(self)._sao_fx_panels
                if p is not panel
            ]
        panel.bind('<Destroy>', _on_destroy, add='+')

        # 启动共享调度 (仅当首次注册时)
        if type(self)._sao_fx_after_id is None:
            type(self)._sao_fx_shared_tick(self)

    @staticmethod
    @_probe.decorate('ui.sao_fx_shared_tick')
    def _sao_fx_shared_tick(self_ref):
        """共享 HUD 装饰 tick — 66ms 一次驱动所有面板.

        比旧方案 (每面板独立 after(33)) 省 N-1 个 after 回调.
        签名缓存确保仅坐标变化时才执行 Canvas 操作.
        """
        if self_ref._destroyed or not type(self_ref)._sao_fx_panels:
            type(self_ref)._sao_fx_after_id = None
            return

        tt = time.time()
        cyan = '#86dfff'
        gold = '#f3af12'
        active_count = 0

        for panel, body_cv, pw, ph in type(self_ref)._sao_fx_panels:
            try:
                if (not panel.winfo_exists()
                        or not panel.winfo_viewable()
                        or not body_cv.winfo_exists()):
                    continue
            except Exception:
                continue
            active_count += 1

            # 每面板加一个相位偏移避免完全同步；坐标计算在 Cython。
            left_far, left_near, right_far, right_near = _CY_UI.sao_fx_coords(
                tt, id(panel), pw)

            # 签名: 所有可能变化的整数坐标
            sig = (left_far, left_near, right_far, right_near)
            if sig == panel._sao_fx_last_sig:
                continue  # 没有变化, 跳过 Canvas 操作
            panel._sao_fx_last_sig = sig

            # 有变化才移动现有 Canvas items，避免 delete + rebuild。
            items = getattr(panel, '_sao_fx_items', None)
            if not items:
                continue
            lines = items.get('lines') or []
            ticks = items.get('ticks') or []
            rects = items.get('rects') or []
            try:
                body_cv.coords(lines[0], left_far, 30, left_far + 78, 30)
                body_cv.coords(lines[1], left_near, ph - 44,
                               left_near + 102, ph - 44)
                body_cv.coords(lines[2], right_far - 88, 42, right_far, 42)
                body_cv.coords(lines[3], right_near - 110, ph - 58,
                               right_near, ph - 58)
                for i, pair in enumerate(ticks):
                    lx = left_far + i * 12
                    rx2 = right_far - i * 13
                    body_cv.coords(pair[0], lx, 48, lx, 54 + (i % 2) * 3)
                    body_cv.coords(pair[1], rx2, ph - 74,
                                   rx2, ph - 68 - (i % 2) * 3)
                body_cv.coords(rects[0], left_near + 8, ph - 36,
                               left_near + 66, ph - 24)
                body_cv.coords(rects[1], right_near - 74, 22,
                               right_near - 12, 34)
            except Exception:
                pass

        _perf_gauge('ui.sao_fx.active_panels', active_count)
        try:
            delay_ms = 90 if active_count > 0 else 250
            type(self_ref)._sao_fx_after_id = self_ref.root.after(
                delay_ms, lambda: type(self_ref)._sao_fx_shared_tick(self_ref))
        except Exception:
            type(self_ref)._sao_fx_after_id = None

    # ══════════════════════════════════════════════
    #  悬浮触发按钮 — 纯 SAO-UI HP 组件 (对标 HP/src/index.vue)
    # ══════════════════════════════════════════════
