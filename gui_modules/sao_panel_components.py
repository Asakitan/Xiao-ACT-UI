# -*- coding: utf-8 -*-
"""Reusable SAO-style Tk components for Entity panels.

Centralized so every ACT floating panel (aggregate cockpit, action log, death
recap, timeline VCR, graph timeseries) shares one visual language. All colors
come from the live ``sao_panel_ui`` palette (light/dark) via ``_pc`` so a theme
swap repaints every panel consistently — never hardcode hex here.
"""

from __future__ import annotations

import math
import time as _time
import tkinter as tk
import tkinter.font as tkfont
from typing import Any, Callable, Iterable, Mapping, Optional

from gui_modules import sao_panel_ui as ui
from utils.sao_sound import get_sao_font, get_cjk_font


# ── 统一间距刻度（替代散落的 2/3/5/7/9 魔法值）──
SP_XS, SP_SM, SP_MD, SP_LG, SP_XL = 4, 8, 12, 16, 24

PAD_X = SP_MD
PAD_Y = SP_SM

FONT_SMALL = get_cjk_font(8)
FONT_META = get_cjk_font(8)
FONT_BODY = get_cjk_font(9)
FONT_BODY_BOLD = get_cjk_font(9, True)
FONT_VALUE = get_sao_font(18, True)       # 指标卡数值（醒目，原 13 太弱）
FONT_VALUE_SM = get_sao_font(11, True)    # 行内数值
FONT_TITLE = get_cjk_font(10, True)
FONT_CARET = get_sao_font(9)


def _pc(key: str, fallback: str = '') -> str:
    """Read a token from the active SAO palette with a safe fallback."""
    try:
        return ui._theme_color(key, fallback) or fallback
    except Exception:
        return fallback


def _finite_int(value: Any, default: int = 0, *, lo: Optional[int] = None,
                hi: Optional[int] = None) -> int:
    try:
        number = float(default if value is None or value == '' else value)
    except Exception:
        number = float(default or 0)
    if not math.isfinite(number):
        number = float(default or 0)
    if lo is not None:
        number = max(float(lo), number)
    if hi is not None:
        number = min(float(hi), number)
    return int(number)


# ── 可读时间格式化（所有 ACT 面板共用，禁止再拿 epoch-ms 直接 print）──
# 三类时间分开处理：绝对 epoch 毫秒→墙钟 HH:MM:SS；带符号偏移→+2.6s；纯时长→2.6s/M:SS。
# JS 侧的等价实现见 web/act_panel_util.js（window.ActTime），两边输出必须逐字一致。
def fmt_clock(time_ms: Any, *, with_seconds: bool = True) -> str:
    """ABSOLUTE epoch-ms → local wall clock 'HH:MM:SS'. '--' when <=0/None."""
    try:
        ms = int(time_ms or 0)
    except Exception:
        ms = 0
    if ms <= 0:
        return '--'
    return _time.strftime('%H:%M:%S' if with_seconds else '%H:%M', _time.localtime(ms / 1000.0))


def fmt_dur(ms: Any) -> str:
    """Non-negative DURATION → '2.6s' (<60s) / 'M:SS' (<1h) / 'H:MM:SS'."""
    try:
        total = max(0.0, float(ms or 0)) / 1000.0
    except Exception:
        total = 0.0
    if total < 60:
        return f'{total:.1f}s'
    if total < 3600:
        return f'{int(total // 60)}:{int(total % 60):02d}'
    return f'{int(total // 3600)}:{int((total % 3600) // 60):02d}:{int(total % 60):02d}'


def fmt_rel(time_ms: Any, base_ms: Any) -> str:
    """SIGNED offset of absolute time_ms from base_ms → '+2.6s' / '-1:23'.

    Falls back to fmt_clock when base is missing (so a lone absolute time still
    reads as a clock, never as raw ms).
    """
    try:
        base = int(base_ms or 0)
    except Exception:
        base = 0
    if not base:
        return fmt_clock(time_ms)
    try:
        delta = int(time_ms or 0) - base
    except Exception:
        delta = 0
    return f"{'+' if delta >= 0 else '-'}{fmt_dur(abs(delta))}"


def fmt_signed(delta_ms: Any) -> str:
    """Already-computed delta (relative_ms / cursor) → 'T0' at zero, else '+2.6s'/'-1:23'."""
    try:
        d = int(delta_ms or 0)
    except Exception:
        d = 0
    if d == 0:
        return 'T0'
    return f"{'+' if d > 0 else '-'}{fmt_dur(abs(d))}"


# ── 技术术语 → 人话（所有 ACT 面板共用，JS 侧见 web/act_panel_util.js window.ActText）──
# 目的：把 topic/kind/source 这些内部字段名，渲染成用户看得懂的中文，而不是
# "scene / tcp / actor_skill / src" 这种黑话。两边映射必须一致。
_TOPIC_CN = {
    "damage": "伤害", "heal": "治疗", "skill": "技能", "actor_skill": "技能",
    "monster": "怪物", "monster_skill": "怪物技能", "boss": "Boss", "boss_state": "Boss状态",
    "boss_mechanic": "Boss机制", "boss_mechanic_skill": "Boss机制", "dungeon": "地牢",
    "scene": "场景", "death": "死亡", "buff": "增益", "player_buff": "玩家增益",
    "factor_buff": "因子增益", "trigger": "触发", "timer": "计时", "target": "目标",
    "log": "日志", "event": "事件", "shield": "护盾", "mitigation": "减伤",
    "incoming_damage": "承受伤害", "healing": "治疗", "ultimate_skill": "终极技",
    "environment_skill": "环境技能", "field_marker": "场地标记",
    "system": "系统", "plugin_ui_invalidate": "插件重绘", "encounter_finalized": "战斗结束",
    "encounter_started": "战斗开始", "encounter_reset": "战斗重置",
}
_SOURCE_CN = {
    "tcp": "封包", "packet": "封包", "entity": "实体", "mem": "内存", "memory": "内存",
    "live": "实时", "hybrid": "混合", "history": "历史",
    "replay": "回放", "ui": "界面", "offline_import": "离线导入", "plugin": "插件",
    "unknown": "未知",
}


def topic_cn(value: Any, *, default: str = "事件") -> str:
    """topic/kind 字段 → 中文标签。未知值原样返回。"""
    text = str(value or "").strip()
    if not text:
        return default
    return _TOPIC_CN.get(text.lower(), text)


def source_cn(value: Any, *, default: str = "未知") -> str:
    """数据来源标识 → 中文。tcp→封包 / entity→实体 / mem→内存 …"""
    text = str(value or "").strip()
    if not text:
        return default
    return _SOURCE_CN.get(text.lower(), text)


def readable_event_line(row: Mapping[str, Any], *, value_fmt: Optional[Callable[[Any], Any]] = None) -> str:
    """一条代表事件 → 人话行：时钟 · 类型 · 来源→目标 · 标签 · 值。

    topic 译成中文；把当 actor 用的来源标识(tcp/entity)译成中文；丢掉无意义的
    0 值和与类型重复的标签。所有 ACT 面板共用，避免各自拼一套黑话。
    """
    if not isinstance(row, Mapping):
        return ""
    fmt_value = value_fmt or (lambda v: "" if v in (None, "") else str(v))
    clock = fmt_clock(row.get("time_ms"))
    topic = topic_cn(row.get("topic"))
    actor = str(row.get("actor") or "").strip()
    target = str(row.get("target") or "").strip()
    label = str(row.get("label") or "").strip()
    value = str(fmt_value(row.get("value")) or "")
    parts = [clock, topic]
    actor_disp = source_cn(actor) if actor else ""
    if actor_disp and target:
        parts.append(f"{actor_disp} → {target}")
    elif target or actor_disp:
        parts.append(target or actor_disp)
    raw_topic = str(row.get("topic") or "").strip().lower()
    if label and label.lower() != raw_topic and label not in (target, topic):
        parts.append(label)
    if value and value not in ("0", "0.00"):
        parts.append(value)
    return " · ".join(part for part in parts if part)


def _accent(kind: str = "gold") -> str:
    kind = str(kind or "gold").lower()
    if kind in {"cyan", "accent", "info"}:
        return _pc('accent', ui._SAO_PANEL_ACCENT)
    if kind in {"ok", "good", "heal"}:
        return _pc('ok', '#5cc46a')
    if kind in {"danger", "bad", "error"}:
        return _pc('danger', '#ef684e')
    return _pc('gold', ui._SAO_PANEL_GOLD)


def _accent_text(kind: str = "gold") -> str:
    """Accent color tuned for legible TEXT (cyan needs a darker shade on tints)."""
    kind = str(kind or "gold").lower()
    if kind in {"cyan", "accent", "info"}:
        return _pc('accent_strong', _accent('cyan'))
    return _accent(kind)


def _accent_soft(kind: str = "gold") -> str:
    kind = str(kind or "gold").lower()
    if kind in {"cyan", "accent", "info"}:
        return _pc('accent_soft', _pc('header_bg', ui._SAO_PANEL_HEADER_BG))
    if kind in {"ok", "good", "heal"}:
        return _pc('ok_soft', _pc('header_bg', ui._SAO_PANEL_HEADER_BG))
    if kind in {"danger", "bad", "error"}:
        return _pc('danger_soft', _pc('header_bg', ui._SAO_PANEL_HEADER_BG))
    return _pc('gold_soft', _pc('header_bg', ui._SAO_PANEL_HEADER_BG))


def _bind_click(widget: tk.Misc, command: Callable[[], Any]) -> None:
    def walk(w: tk.Misc) -> None:
        try:
            w.bind('<Button-1>', lambda _e: command(), add='+')
            w.configure(cursor='hand2')
        except Exception:
            pass
        try:
            children = w.winfo_children()
        except Exception:
            children = []
        for child in children:
            walk(child)
    walk(widget)


def _bind_hover(row: tk.Misc, base_bg: str, hover_bg: str) -> None:
    """Lighten the row (and same-bg children) on mouse-over for live feedback."""
    if not hover_bg or hover_bg == base_bg:
        return
    targets: list[tk.Misc] = []

    def collect(w: tk.Misc) -> None:
        try:
            if str(w.cget('bg')) == base_bg:
                targets.append(w)
        except Exception:
            pass
        try:
            children = w.winfo_children()
        except Exception:
            children = []
        for child in children:
            collect(child)

    collect(row)

    def _enter(_e: Any) -> None:
        for w in targets:
            try:
                w.configure(bg=hover_bg)
            except Exception:
                pass

    def _leave(_e: Any) -> None:
        for w in targets:
            try:
                w.configure(bg=base_bg)
            except Exception:
                pass

    row.bind('<Enter>', _enter, add='+')
    row.bind('<Leave>', _leave, add='+')


def status_badge(parent: tk.Misc, text: str, *, kind: str = "gold") -> tk.Canvas:
    """Rounded pill badge (canvas) — matches the web .act-badge / .act-pill."""
    fontspec = get_cjk_font(8)
    f = tkfont.Font(font=fontspec)
    txt = str(text or "-")
    w = f.measure(txt) + 2 * 9
    h = f.metrics('linespace') + 2 * 3
    c = tk.Canvas(parent, width=w, height=h, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG),
                  highlightthickness=0, bd=0)
    c.create_polygon(_round_pts(1, 1, w - 1, h - 1, min(9, h / 2)), smooth=True, splinesteps=16,
                     fill=_accent_soft(kind), outline=_accent(kind), width=1)
    c.create_text(w // 2, h // 2 + 1, text=txt, fill=_accent_text(kind), font=fontspec)
    return c


class _RoundedButton(tk.Canvas):
    """Rounded flat button (canvas) — Tk has no rounded Button. Matches web .act-btn.

    Supports ``configure(command=…)`` / ``configure(text=…)`` so existing callers
    (e.g. dropdown_button) keep working.
    """

    def __init__(self, parent, text='', command=None, *, kind='normal', radius=7, padx=12, pady=5):
        self._radius = radius
        self._text = str(text)
        self._command = command
        self._font = tkfont.Font(font=get_cjk_font(9, True))
        self._fill = _pc('control_bg', _pc('card_bg', ui._SAO_PANEL_BODY_BG))
        self._fill_hover = _pc('card_bg_alt', _pc('header_bg', ui._SAO_PANEL_HEADER_BG))
        self._border = _pc('border', ui._SAO_PANEL_BORDER) if kind == 'normal' else _accent(kind)
        self._fg = _pc('value_fg', ui._SAO_PANEL_VALUE_FG) if kind == 'normal' else _accent_text(kind)
        w = self._font.measure(self._text) + 2 * padx
        h = self._font.metrics('linespace') + 2 * pady
        super().__init__(parent, width=w, height=h, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG),
                         highlightthickness=0, bd=0, cursor='hand2' if command else '')
        self.bind('<Configure>', lambda _e: self._draw())
        self.bind('<Button-1>', self._on_click)
        self.bind('<Enter>', lambda _e: self._draw(hover=True))
        self.bind('<Leave>', lambda _e: self._draw(hover=False))
        self._draw()

    def _draw(self, hover=False):
        self.delete('all')
        w = self.winfo_width() or int(self['width'])
        h = self.winfo_height() or int(self['height'])
        self.create_polygon(_round_pts(1, 1, w - 1, h - 1, self._radius), smooth=True, splinesteps=16,
                            fill=self._fill_hover if hover else self._fill,
                            outline=_accent('cyan') if hover else self._border, width=1)
        self.create_text(w // 2, h // 2, text=self._text, fill=self._fg, font=self._font)

    def _on_click(self, _e):
        if callable(self._command):
            self._command()

    def configure(self, cnf=None, **kw):
        if 'command' in kw:
            self._command = kw.pop('command')
            tk.Canvas.configure(self, cursor='hand2' if self._command else '')
        if 'text' in kw:
            self._text = str(kw.pop('text'))
            self._draw()
        if cnf is not None or kw:
            return tk.Canvas.configure(self, cnf, **kw)

    config = configure


def action_button(parent: tk.Misc, text: str, command: Optional[Callable[[], Any]] = None, *, kind: str = "normal"):
    return _RoundedButton(parent, text, command, kind=kind)


def dropdown_button(parent: tk.Misc, text: str, items: Iterable[Any], *, kind: str = "normal") -> tk.Button:
    """聚合按钮：点击弹出条目菜单，把同排过密的相似动作收进一个父按钮。

    ``items`` 为 ``(label, command)`` 序列；条目为 ``'-'``（或 label 为 '-'）时
    插入分隔线。菜单配色走面板调色板，主题切换无需重建。
    """
    btn = action_button(parent, f'{text} ▾', None, kind=kind)
    menu = tk.Menu(
        btn, tearoff=0,
        bg=_pc('control_bg', '#fafbfb'), fg=_pc('value_fg', ui._SAO_PANEL_VALUE_FG),
        activebackground=_accent(kind), activeforeground=_pc('active_fg', '#ffffff'),
        relief='flat', bd=0, font=FONT_BODY,
    )
    for entry in items or []:
        if entry == '-' or (isinstance(entry, (tuple, list)) and entry and entry[0] == '-'):
            menu.add_separator()
            continue
        try:
            label, command = entry
        except Exception:
            continue
        menu.add_command(label=str(label), command=command)

    def _pop() -> None:
        try:
            x = btn.winfo_rootx()
            y = btn.winfo_rooty() + btn.winfo_height()
            menu.tk_popup(x, y)
        finally:
            try:
                menu.grab_release()
            except Exception:
                pass

    btn.configure(command=_pop, cursor='hand2')
    btn._sao_dropdown_menu = menu  # type: ignore[attr-defined]  # keep a live ref
    return btn


def attach_tooltip(widget: tk.Misc, text: str, *, delay_ms: int = 450) -> None:
    """悬停延迟弹出的轻量提示气泡——解释 Cursor ms / Window s 这类不直观字段。

    Web 端等价做法是控件上的 ``title`` 属性，文案保持两边一致。
    """
    tip_text = str(text or '').strip()
    if not tip_text:
        return
    state: dict[str, Any] = {'after': None, 'tip': None}

    def _show() -> None:
        state['after'] = None
        if state['tip'] is not None:
            return
        try:
            tip = tk.Toplevel(widget)
            tip.overrideredirect(True)
            tip.attributes('-topmost', True)
            bg = _pc('header_bg', ui._SAO_PANEL_HEADER_BG)
            tk.Label(
                tip, text=tip_text, bg=bg, fg=_pc('value_fg', ui._SAO_PANEL_VALUE_FG),
                font=FONT_SMALL, justify='left', wraplength=320, padx=SP_SM, pady=SP_XS,
                highlightthickness=1, highlightbackground=_pc('border', ui._SAO_PANEL_BORDER),
            ).pack()
            tip.geometry(f"+{widget.winfo_rootx()}+{widget.winfo_rooty() + widget.winfo_height() + 4}")
            state['tip'] = tip
        except Exception:
            state['tip'] = None

    def _cancel() -> None:
        pending = state['after']
        state['after'] = None
        if pending is not None:
            try:
                widget.after_cancel(pending)
            except Exception:
                pass

    def _hide(_e: Any = None) -> None:
        _cancel()
        tip = state['tip']
        state['tip'] = None
        if tip is not None:
            try:
                tip.destroy()
            except Exception:
                pass

    def _enter(_e: Any) -> None:
        _cancel()
        try:
            state['after'] = widget.after(delay_ms, _show)
        except Exception:
            state['after'] = None

    widget.bind('<Enter>', _enter, add='+')
    widget.bind('<Leave>', _hide, add='+')
    widget.bind('<Button-1>', _hide, add='+')


def more_indicator(parent: tk.Misc, hidden_count: int, *, noun: str = "条") -> tk.Label:
    """列表截断提示：'… 还有 N 条'。让用户知道没看到的不是全部。"""
    count = _finite_int(hidden_count, 0, lo=0)
    return tk.Label(
        parent, text=f'… 还有 {count} {noun}',
        bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG), fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG),
        font=FONT_SMALL, anchor='w', padx=SP_SM, pady=2,
    )


def _round_pts(x1, y1, x2, y2, r):
    """Point list for a smooth (bezier) rounded rectangle on a tk.Canvas."""
    return [
        x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r,
        x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
        x1, y2, x1, y2 - r, x1, y1 + r, x1, y1,
    ]


def rounded_panel(parent, *, bg, border, radius=8, rail=None, rail_w=3, pad=10, height=None):
    """Canvas-backed rounded card (Tk has no rounded Frame). Returns (canvas, inner).

    Draws a smooth rounded rect (fill ``bg``, 1px ``border``) and, when ``rail``
    is set, a flat colored left rail (rounded to follow the corner) like the web
    ``border-left`` accent. Content goes in the returned ``inner`` frame, inset by
    ``pad`` so the rounded edge stays visible.
    """
    body_bg = _pc('body_bg', ui._SAO_PANEL_BODY_BG)
    canvas = tk.Canvas(parent, bg=body_bg, highlightthickness=0, bd=0)
    if height:
        canvas.configure(height=height)
    inner = tk.Frame(canvas, bg=bg)
    canvas.create_window(pad + (rail_w if rail else 0), pad, window=inner, anchor='nw', tags='inner')

    def _redraw(_e=None):
        w = canvas.winfo_width()
        h = canvas.winfo_height()
        if w <= 2 or h <= 2:
            return
        canvas.delete('bg')
        if rail:
            canvas.create_polygon(_round_pts(1, 1, w - 1, h - 1, radius), smooth=True, splinesteps=20,
                                  fill=rail, outline=rail, tags='bg')
            canvas.create_polygon(_round_pts(1 + rail_w, 1, w - 1, h - 1, radius), smooth=True, splinesteps=20,
                                  fill=bg, outline=border, width=1, tags='bg')
        else:
            canvas.create_polygon(_round_pts(1, 1, w - 1, h - 1, radius), smooth=True, splinesteps=20,
                                  fill=bg, outline=border, width=1, tags='bg')
        canvas.tag_lower('bg')
        canvas.coords('inner', pad + (rail_w if rail else 0), pad)
        if height is not None:
            canvas.itemconfigure('inner', width=w - 2 * pad - (rail_w if rail else 0), height=h - 2 * pad)
        else:
            canvas.itemconfigure('inner', width=w - 2 * pad - (rail_w if rail else 0))

    canvas.bind('<Configure>', _redraw)
    if height is None:
        # auto-grow the canvas to the inner content height (variable-height cards/sections)
        def _fit(_e=None):
            try:
                req = inner.winfo_reqheight() + 2 * pad
                if abs((canvas.winfo_height() or 0) - req) > 1:
                    canvas.configure(height=req)
                _redraw()
            except Exception:
                pass
        inner.bind('<Configure>', _fit)
    return canvas, inner


class _SaoScroll(tk.Canvas):
    """Custom slim, dark, rounded scrollbar (native tk.Scrollbar ignores colors on
    Windows — it stays white/split). Drop-in: pass as ``yscrollcommand=sb.set`` and
    ``command=canvas.yview``. Thumb auto-hides when everything fits."""

    def __init__(self, parent, command, *, width=9):
        super().__init__(parent, width=width, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG),
                         highlightthickness=0, bd=0, takefocus=0)
        self._command = command
        self._f0, self._f1 = 0.0, 1.0
        self.bind('<Configure>', lambda _e: self._draw())
        self.bind('<Button-1>', self._on_drag)
        self.bind('<B1-Motion>', self._on_drag)

    def set(self, first, last):
        self._f0 = float(first)
        self._f1 = float(last)
        self._draw()

    def _draw(self):
        self.delete('all')
        h = self.winfo_height()
        w = self.winfo_width()
        if h <= 2 or w <= 2:
            return
        if self._f0 <= 0.0 and self._f1 >= 1.0:
            return  # everything fits — no thumb
        y1 = max(1.0, self._f0 * h + 1)
        y2 = min(h - 1.0, self._f1 * h - 1)
        if y2 - y1 < 12:
            y2 = min(h - 1.0, y1 + 12)
        r = max(2.0, (w - 3) / 2.0)
        self.create_polygon(_round_pts(2, y1, w - 1, y2, r), smooth=True, splinesteps=14,
                            fill=_pc('border', ui._SAO_PANEL_BORDER), outline='')

    def _on_drag(self, e):
        h = self.winfo_height() or 1
        span = max(0.0, self._f1 - self._f0)
        frac = (e.y / h) - span / 2.0
        if callable(self._command):
            self._command('moveto', max(0.0, min(1.0, frac)))


def sao_scrollbar(parent, command, *, width=9):
    return _SaoScroll(parent, command, width=width)


def sao_entry(parent, textvariable=None, *, width=14):
    """Flat dark text input (cyan focus border), matching the web .act-input."""
    card_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    e = tk.Entry(
        parent, textvariable=textvariable, width=width,
        bg=card_bg, fg=_pc('value_fg', ui._SAO_PANEL_VALUE_FG),
        disabledbackground=card_bg, insertbackground=_pc('value_fg', ui._SAO_PANEL_VALUE_FG),
        relief='flat', bd=0, highlightthickness=1,
        highlightbackground=_pc('border', ui._SAO_PANEL_BORDER),
        highlightcolor=_accent('cyan'), font=get_cjk_font(9),
    )
    return e


def sao_option_menu(parent, var, *values, command=None):
    """Flat dark dropdown (replaces the cramped/raised native tk.OptionMenu)."""
    card_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    text = _pc('value_fg', ui._SAO_PANEL_VALUE_FG)
    accent = _accent('cyan')
    om = tk.OptionMenu(parent, var, *values, command=command)
    om.configure(
        bg=card_bg, fg=text, activebackground=accent, activeforeground=card_bg,
        relief='flat', bd=0, highlightthickness=1,
        highlightbackground=_pc('border', ui._SAO_PANEL_BORDER),
        font=get_cjk_font(9), anchor='w', padx=8, pady=2, cursor='hand2',
        indicatoron=True, takefocus=0,
    )
    try:
        om['menu'].configure(
            bg=card_bg, fg=text, activebackground=accent, activeforeground=card_bg,
            relief='flat', bd=0, font=get_cjk_font(9),
        )
    except Exception:
        pass
    return om


def metric_tile(parent: tk.Misc, label: str, value: Any, *, sub: str = "", accent: str = "gold") -> tk.Frame:
    color = _accent(accent)
    card_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    border = _pc('border', ui._SAO_PANEL_BORDER)
    card, inner = rounded_panel(parent, bg=card_bg, border=border, radius=9,
                                rail=color, rail_w=3, pad=SP_MD, height=98)
    tk.Label(inner, text=str(label or "-").upper(), bg=card_bg, fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG), font=get_cjk_font(8)).pack(anchor='w')
    tk.Label(inner, text=str(value if value not in (None, '') else "0"), bg=card_bg, fg=_pc('value_fg', ui._SAO_PANEL_VALUE_FG), font=get_sao_font(20, True)).pack(anchor='w', pady=(2, 0))
    if sub:
        tk.Label(inner, text=str(sub), bg=card_bg, fg=color, font=get_cjk_font(8)).pack(anchor='w', pady=(1, 0))
    return card


def section_card(parent: tk.Misc, title: str, *, subtitle: str = "", badge: str = "", accent: str = "cyan") -> tk.Frame:
    card_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    header_bg = _pc('header_bg', ui._SAO_PANEL_HEADER_BG)
    border = _pc('border', ui._SAO_PANEL_BORDER)
    # Rounded section box (Tk canvas), auto-sized to content, raised card_bg fill to
    # match the web .act-section. The returned `inner` frame is where the caller adds
    # the section body; packing it actually packs the rounded canvas (proxy) so existing
    # callers (`box.pack(...)` + `tk.Frame(box)`) keep working.
    card, inner = rounded_panel(parent, bg=card_bg, border=border, radius=9, pad=SP_XS, height=None)
    head = tk.Frame(inner, bg=header_bg)
    head.pack(fill='x')
    tk.Frame(head, bg=_accent(accent), width=3).pack(side='left', fill='y')   # 3px 左侧强调轨（扁平单通道）
    text_box = tk.Frame(head, bg=header_bg)
    text_box.pack(side='left', fill='x', expand=True, padx=(SP_SM, SP_XS), pady=SP_SM)
    tk.Label(text_box, text=str(title or "SECTION"), bg=header_bg, fg=_pc('gold', ui._SAO_PANEL_GOLD), font=get_cjk_font(11, True), anchor='w').pack(fill='x')
    if subtitle:
        tk.Label(text_box, text=str(subtitle), bg=header_bg, fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG), font=get_cjk_font(8), anchor='w').pack(fill='x')
    if badge:
        status_badge(head, badge, kind=accent).pack(side='right', padx=SP_SM, pady=SP_SM)
    inner.pack = lambda **kw: card.pack(**kw)
    inner.grid = lambda **kw: card.grid(**kw)
    inner.place = lambda **kw: card.place(**kw)
    return inner


def aggregate_row(parent: tk.Misc, *, title: str, meta: str = "", value: str = "", ratio: float = 0.0,
                  accent: str = "gold", zebra: bool = False, command: Optional[Callable[[], Any]] = None,
                  expanded: Optional[bool] = None) -> tk.Frame:
    color = _accent(accent)
    base_bg = _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG) if zebra else _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    hover_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG) if zebra else _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG)
    row = tk.Frame(parent, bg=base_bg, cursor='hand2' if callable(command) else '')

    body = tk.Frame(row, bg=base_bg)
    body.pack(fill='x', padx=SP_SM, pady=SP_XS + 1)
    if expanded is not None:
        tk.Label(body, text=('▾' if expanded else '▸'), bg=base_bg, fg=color, font=FONT_CARET).pack(side='left', padx=(0, SP_XS))
    left = tk.Frame(body, bg=base_bg)
    left.pack(side='left', fill='x', expand=True)
    tk.Label(left, text=str(title or '-'), bg=base_bg, fg=_pc('value_fg', ui._SAO_PANEL_VALUE_FG), font=FONT_BODY_BOLD, anchor='w').pack(fill='x')
    if meta:
        tk.Label(left, text=str(meta), bg=base_bg, fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG), font=FONT_META, anchor='w').pack(fill='x')
    if value:
        tk.Label(body, text=str(value), bg=base_bg, fg=color, font=FONT_VALUE_SM, anchor='e').pack(side='right', padx=(SP_SM, 0))

    track = tk.Frame(row, bg=_pc('track_bg', base_bg), height=6)
    track.pack(fill='x', side='top', padx=(18, SP_SM), pady=(0, SP_XS + 2))
    track.pack_propagate(False)
    fill = tk.Frame(track, bg=color)
    fill.place(x=0, y=0, relheight=1.0, relwidth=max(0.0, min(1.0, float(ratio or 0.0))))

    # Only advertise interactivity (hand cursor + hover-lighten) when the row
    # actually has a handler — a hovering-but-dead row reads as "click me" and
    # then does nothing (the user's "点开进不去" complaint).
    if callable(command):
        _bind_click(row, command)
        _bind_hover(row, base_bg, hover_bg)
    return row


def detail_row(parent: tk.Misc, text: str, *, accent: str = "cyan", zebra: bool = False, strong: bool = False,
               command: Optional[Callable[[], Any]] = None, expanded: Optional[bool] = None) -> tk.Frame:
    """One drilldown line: thin accent rule + monospace-ish aligned text, zebra striped.

    When `command` is set the line is clickable (e.g. expand the raw payload), with
    an optional caret showing expand state.
    """
    base_bg = _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG) if zebra else _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    fg = _pc('value_fg', ui._SAO_PANEL_VALUE_FG) if strong else _pc('label_fg', ui._SAO_PANEL_LABEL_FG)
    row = tk.Frame(parent, bg=base_bg, cursor='hand2' if callable(command) else '')
    tk.Frame(row, bg=_accent(accent), width=3).pack(side='left', fill='y')
    if expanded is not None:
        tk.Label(row, text=('▾' if expanded else '▸'), bg=base_bg, fg=_accent(accent), font=FONT_META).pack(side='left', padx=(SP_XS, 0))
    tk.Label(row, text=str(text), bg=base_bg, fg=fg, font=FONT_META, anchor='w', justify='left').pack(
        side='left', fill='x', expand=True, padx=(SP_SM, SP_SM), pady=1)
    if callable(command):
        _bind_click(row, command)
        _bind_hover(row, base_bg, _pc('card_bg_alt') if not zebra else _pc('card_bg'))
    return row


def keep_canvas_scroll(canvas: Optional[tk.Canvas], inner: Optional[tk.Misc]) -> None:
    """全量重建前调用: 记录滚动分数, 本轮事件处理结束后还原。
    web setContentHtml(preserveScroll) 的 Tk 对偶 — 不调用的面板
    每次签名刷新滚动都会跳回顶部。after_idle 在重建完成后触发,
    单插入点覆盖渲染函数的全部 return 路径。"""
    if canvas is None or inner is None:
        return
    try:
        y0 = canvas.yview()[0]
    except Exception:
        return
    if y0 <= 0.0:
        return

    def _restore() -> None:
        try:
            inner.update_idletasks()
            canvas.configure(scrollregion=canvas.bbox('all'))
            canvas.yview_moveto(y0)
        except Exception:
            pass

    try:
        canvas.after_idle(_restore)
    except Exception:
        pass


def empty_state(parent: tk.Misc, title: str, detail: str = "") -> tk.Frame:
    body_bg = _pc('body_bg', ui._SAO_PANEL_BODY_BG)
    box = tk.Frame(parent, bg=body_bg, highlightthickness=1, highlightbackground=_pc('sep', ui._SAO_PANEL_SEP))
    tk.Label(box, text=str(title or "No data"), bg=body_bg, fg=_pc('gold', ui._SAO_PANEL_GOLD), font=FONT_TITLE, pady=SP_MD).pack(fill='x')
    if detail:
        tk.Label(box, text=str(detail), bg=body_bg, fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG), font=FONT_BODY, wraplength=720, justify='center').pack(fill='x', padx=SP_MD, pady=(0, SP_MD))
    return box


def source_badges(parent: tk.Misc, sources: Iterable[Mapping[str, Any]]) -> tk.Frame:
    frame = tk.Frame(parent, bg=_pc('body_bg', ui._SAO_PANEL_BODY_BG))
    for item in sources or []:
        if not isinstance(item, Mapping):
            continue
        text = f"{source_cn(item.get('source'), default='-')} · {_finite_int(item.get('count'), 0, lo=0)}"
        status_badge(frame, text, kind='cyan').pack(side='left', padx=(0, SP_SM), pady=2)
    return frame
