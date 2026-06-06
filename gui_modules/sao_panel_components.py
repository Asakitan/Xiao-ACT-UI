# -*- coding: utf-8 -*-
"""Reusable SAO-style Tk components for Entity panels.

Centralized so every ACT floating panel (aggregate cockpit, action log, death
recap, timeline VCR, graph timeseries) shares one visual language. All colors
come from the live ``sao_panel_ui`` palette (light/dark) via ``_pc`` so a theme
swap repaints every panel consistently — never hardcode hex here.
"""

from __future__ import annotations

import time as _time
import tkinter as tk
from typing import Any, Callable, Iterable, Mapping, Optional

from gui_modules import sao_panel_ui as ui


# ── 统一间距刻度（替代散落的 2/3/5/7/9 魔法值）──
SP_XS, SP_SM, SP_MD, SP_LG, SP_XL = 4, 8, 12, 16, 24

PAD_X = SP_MD
PAD_Y = SP_SM

FONT_SMALL = ('Segoe UI', 8)
FONT_META = ('Segoe UI', 8)
FONT_BODY = ('Segoe UI', 9)
FONT_BODY_BOLD = ('Segoe UI', 9, 'bold')
FONT_VALUE = ('Segoe UI', 18, 'bold')       # 指标卡数值（醒目，原 13 太弱）
FONT_VALUE_SM = ('Segoe UI', 11, 'bold')    # 行内数值
FONT_TITLE = ('Segoe UI', 10, 'bold')
FONT_CARET = ('Segoe UI', 9)


def _pc(key: str, fallback: str = '') -> str:
    """Read a token from the active SAO palette with a safe fallback."""
    try:
        return ui._theme_color(key, fallback) or fallback
    except Exception:
        return fallback


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
}
_SOURCE_CN = {
    "tcp": "封包", "entity": "实体", "mem": "内存", "memory": "内存", "history": "历史",
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


def status_badge(parent: tk.Misc, text: str, *, kind: str = "gold") -> tk.Label:
    fg = _accent_text(kind)
    bg = _accent_soft(kind)
    return tk.Label(
        parent, text=str(text or "-"), bg=bg, fg=fg, font=FONT_SMALL,
        padx=SP_SM, pady=2, highlightthickness=1, highlightbackground=_accent(kind),
    )


def action_button(parent: tk.Misc, text: str, command: Optional[Callable[[], Any]] = None, *, kind: str = "normal") -> tk.Button:
    bg = _pc('header_bg', ui._SAO_PANEL_HEADER_BG)
    fg = _pc('header_fg', ui._SAO_PANEL_HEADER_FG)
    active = _accent(kind)
    return tk.Button(
        parent,
        text=text,
        command=command,
        bg=bg,
        fg=fg,
        activebackground=active,
        activeforeground=_pc('active_fg', 'white'),
        relief='flat',
        bd=0,
        padx=SP_MD,
        pady=SP_XS + 1,
        font=FONT_BODY_BOLD,
        cursor='hand2' if callable(command) else '',
    )


def metric_tile(parent: tk.Misc, label: str, value: Any, *, sub: str = "", accent: str = "gold") -> tk.Frame:
    color = _accent(accent)
    card_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    border = _pc('border', ui._SAO_PANEL_BORDER)
    card = tk.Frame(parent, bg=card_bg, highlightthickness=1, highlightbackground=border)
    tk.Frame(card, bg=color, height=4).pack(fill='x')           # 4px 顶部强调条（原 3px 像渲染瑕疵）
    inner = tk.Frame(card, bg=card_bg)
    inner.pack(fill='both', expand=True, padx=SP_MD, pady=(SP_SM, SP_SM))
    tk.Label(inner, text=str(label or "-").upper(), bg=card_bg, fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG), font=FONT_SMALL).pack(anchor='w')
    tk.Label(inner, text=str(value if value not in (None, '') else "0"), bg=card_bg, fg=_pc('value_fg', ui._SAO_PANEL_VALUE_FG), font=FONT_VALUE).pack(anchor='w', pady=(2, 0))
    if sub:
        tk.Label(inner, text=str(sub), bg=card_bg, fg=color, font=FONT_SMALL).pack(anchor='w', pady=(1, 0))
    return card


def section_card(parent: tk.Misc, title: str, *, subtitle: str = "", badge: str = "", accent: str = "cyan") -> tk.Frame:
    body_bg = _pc('body_bg', ui._SAO_PANEL_BODY_BG)
    header_bg = _pc('header_bg', ui._SAO_PANEL_HEADER_BG)
    box = tk.Frame(parent, bg=body_bg, highlightthickness=1, highlightbackground=_pc('border', ui._SAO_PANEL_BORDER))
    head = tk.Frame(box, bg=header_bg)
    head.pack(fill='x')
    tk.Frame(head, bg=_accent(accent), width=4).pack(side='left', fill='y')   # 左侧强调轨（按 section 配色）
    text_box = tk.Frame(head, bg=header_bg)
    text_box.pack(side='left', fill='x', expand=True, padx=(SP_SM, SP_XS), pady=SP_SM)
    tk.Label(text_box, text=str(title or "SECTION"), bg=header_bg, fg=_pc('gold', ui._SAO_PANEL_GOLD), font=FONT_TITLE, anchor='w').pack(fill='x')
    if subtitle:
        tk.Label(text_box, text=str(subtitle), bg=header_bg, fg=_pc('label_fg', ui._SAO_PANEL_LABEL_FG), font=FONT_SMALL, anchor='w').pack(fill='x')
    if badge:
        status_badge(head, badge, kind=accent).pack(side='right', padx=SP_SM, pady=SP_SM)
    return box


def aggregate_row(parent: tk.Misc, *, title: str, meta: str = "", value: str = "", ratio: float = 0.0,
                  accent: str = "gold", zebra: bool = False, command: Optional[Callable[[], Any]] = None,
                  expanded: Optional[bool] = None) -> tk.Frame:
    color = _accent(accent)
    base_bg = _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG) if zebra else _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    hover_bg = _pc('card_bg', ui._SAO_PANEL_BODY_BG) if zebra else _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG)
    row = tk.Frame(parent, bg=base_bg, highlightthickness=1, highlightbackground=_pc('sep', ui._SAO_PANEL_SEP),
                   cursor='hand2' if callable(command) else '')

    # 全宽进度槽 + 比例填充（替代原本最多 280px 的像素条，随窗口缩放）
    track = tk.Frame(row, bg=_pc('track_bg', base_bg), height=5)
    track.pack(fill='x', side='top')
    track.pack_propagate(False)
    fill = tk.Frame(track, bg=color)
    fill.place(x=0, y=0, relheight=1.0, relwidth=max(0.0, min(1.0, float(ratio or 0.0))))

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

    # Only advertise interactivity (hand cursor + hover-lighten) when the row
    # actually has a handler — a hovering-but-dead row reads as "click me" and
    # then does nothing (the user's "点开进不去" complaint).
    if callable(command):
        _bind_click(row, command)
        _bind_hover(row, base_bg, hover_bg)
    return row


def detail_row(parent: tk.Misc, text: str, *, accent: str = "cyan", zebra: bool = False, strong: bool = False) -> tk.Frame:
    """One drilldown line: thin accent rule + monospace-ish aligned text, zebra striped."""
    base_bg = _pc('card_bg_alt', ui._SAO_PANEL_HEADER_BG) if zebra else _pc('card_bg', ui._SAO_PANEL_BODY_BG)
    fg = _pc('value_fg', ui._SAO_PANEL_VALUE_FG) if strong else _pc('label_fg', ui._SAO_PANEL_LABEL_FG)
    row = tk.Frame(parent, bg=base_bg)
    tk.Frame(row, bg=_accent(accent), width=2).pack(side='left', fill='y')
    tk.Label(row, text=str(text), bg=base_bg, fg=fg, font=FONT_META, anchor='w', justify='left').pack(
        side='left', fill='x', expand=True, padx=(SP_SM, SP_SM), pady=1)
    return row


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
        text = f"{source_cn(item.get('source'), default='-')} · {int(item.get('count') or 0)}"
        status_badge(frame, text, kind='cyan').pack(side='left', padx=(0, SP_SM), pady=2)
    return frame
