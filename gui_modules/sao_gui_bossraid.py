from __future__ import annotations

import tkinter as tk
from typing import Any, Callable, Dict, Optional, Tuple

from sao_web_panel_common import (
    PANEL_BG,
    PANEL_BG_ALT,
    PANEL_CARD,
    PANEL_CARD_ALT,
    PANEL_EDGE,
    PANEL_HEADER,
    TEXT_DIM,
    TEXT_MAIN,
    TEXT_MUTED,
    GOLD,
    CYAN,
    DANGER,
    READY,
    LINE,
    apply_surface_chrome,
    apply_badge,
    attach_tab_underline,
    bind_drag,
    calc_panel_geometry,
    clear_frame,
    create_scrollable_area,
    make_action_button,
    make_section_title,
    make_tab_label,
    panel_font,
    place_corner_accents,
    set_tab_active,
)


def _fmt_num(value: Any) -> str:
    try:
        num = float(value or 0)
    except Exception:
        return '0'
    if num >= 1_000_000_000:
        return f'{num / 1_000_000_000:.1f}B'
    if num >= 1_000_000:
        return f'{num / 1_000_000:.1f}M'
    if num >= 1_000:
        return f'{num / 1_000:.1f}K'
    return str(int(round(num)))


def _fmt_time(seconds: Any) -> str:
    try:
        total = max(0, int(float(seconds or 0)))
    except Exception:
        total = 0
    return f'{total // 60}:{total % 60:02d}'


def _trigger_text(trigger: Optional[Dict[str, Any]]) -> str:
    if not isinstance(trigger, dict):
        return 'Manual'
    trigger_type = str(trigger.get('type') or 'manual')
    value = trigger.get('value') or 0
    if trigger_type == 'manual':
        return 'Manual (F8)'
    if trigger_type == 'time':
        return f'{int(float(value or 0))}s elapsed'
    if trigger_type == 'hp_pct':
        return f'HP ≤ {int(float(value or 0))}%'
    if trigger_type == 'dps_total':
        return f'DMG ≥ {_fmt_num(value)}'
    if trigger_type == 'breaking':
        return 'Break event'
    if trigger_type == 'shield_broken':
        return 'Shield broken'
    if trigger_type == 'overdrive':
        return 'Overdrive'
    if trigger_type == 'extinction_pct':
        return f'Break bar ≥ {int(float(value or 0))}%'
    if trigger_type == 'breaking_stage':
        return f'Break stage ≥ {int(float(value or 0))}'
    if trigger_type == 'boss_mechanic':
        return f'Mechanic: {value}'
    if trigger_type == 'boss_mechanic_family':
        return f'Mechanic family: {value}'
    return f'{trigger_type}: {value}'


class _BossReactionsEditorMixin:
    """Shared scene→boss→observed-skill reaction editor, used by both the quick
    BossRaid panel and the detailed editor. Container-agnostic: each host sets
    self._rx_container (the frame to render into) + self._rx_rerender (a re-render
    callback) via _render_reactions(); reaction state lives on the host
    (_react_state / _react_boss / _react_scene / _load_reactions / _save_reaction)."""

    _TAG_BADGE = {
        'cast': ('施法', '#2b6f8a'), 'enrage': ('狂暴', '#c0392b'),
        'invincible': ('无敌', '#8e44ad'), 'shield': ('护盾', '#2e6fb0'),
        'super_armor': ('霸体', '#c97a1a'), 'breaking': ('破防', '#b8860b'),
        'fracture': ('碎裂', '#a85432'), 'stun': ('眩晕', '#b59b00'),
        'hp_line': ('血线', '#cf3a2f'), 'time': ('定时', '#3f8f4f'),
        'death': ('死亡', '#555a63'), 'body_part': ('部位', '#2f8f8f'),
        'mechanic': ('机制', '#555a63'),
    }

    def _init_reactions_state(self, load_reactions_fn, save_reaction_fn):
        self._load_reactions = load_reactions_fn
        self._save_reaction = save_reaction_fn
        self._react_state = {}
        self._react_boss = 0
        self._react_scene = None

    def _rx_empty(self, title: str, subtitle: str) -> None:
        wrap = tk.Frame(self._rx_container, bg=PANEL_BG)
        wrap.pack(fill=tk.X, pady=18)
        tk.Label(wrap, text=title, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(10),
                 justify='center').pack()
        if subtitle:
            tk.Label(wrap, text=subtitle, bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(8),
                     justify='center', wraplength=320).pack(pady=(4, 0))

    def _render_badges(self, parent, rec: Dict[str, Any]) -> None:
        import tkinter as _tk
        tags = list(rec.get('tags') or [])
        if not tags:
            return
        bar = _tk.Frame(parent, bg=PANEL_CARD)
        bar.pack(fill=tk.X, pady=(2, 0))
        for t in tags:
            label, color = self._TAG_BADGE.get(t, (t, '#555a63'))
            if t == 'hp_line' and rec.get('hp_line_pct') is not None:
                label = '血线~%d%%' % int(round(float(rec['hp_line_pct']) * 100))
            elif t == 'time' and rec.get('time_fixed_s') is not None:
                label = '定时~%ds' % int(round(float(rec['time_fixed_s'])))
            _tk.Label(bar, text=label, bg=color, fg='#ffffff', font=panel_font(7),
                      padx=4, pady=0).pack(side=tk.LEFT, padx=(0, 3))

    @staticmethod
    def _obs_label(rec: Dict[str, Any]) -> str:
        sid = int(rec.get('id') or 0)
        nm = (rec.get('name') or '').strip()
        cnt = int(rec.get('count') or 0)
        dur = ('%dms' % rec['last_cast_duration_ms']) if rec.get('last_cast_duration_ms') else '?'
        head = ('%s #%d' % (nm, sid)) if nm else ('#%d' % sid)
        return '%s ×%d · %s' % (head, cnt, dur)

    def _render_reactions(self, container, rerender) -> None:
        """Render the full reactions editor into `container`; `rerender` re-runs
        the host's render after scene/boss changes + saves. Observations come back
        name-resolved + grouped (casts / mechanics / timeline) per selected boss."""
        import tkinter as _tk
        self._rx_container = container
        self._rx_rerender = rerender
        if not self._load_reactions:
            self._rx_empty('Boss 反应不可用', '内存引擎未接入')
            return
        try:
            st = self._load_reactions(self._react_scene, self._react_boss or None) or {}
        except Exception:
            st = {}
        self._react_state = st
        if not st.get('mem_available'):
            self._rx_empty('Boss 反应未就绪',
                           '启动识别后即可自动记录并编辑 Boss 反应 (当前数据源: %s)'
                           % st.get('data_source', 'tcp'))
            return
        if self._react_scene is None:
            self._react_scene = str(st.get('selected_scene_key') or '0')
        # the contract resolves the default boss; mirror it so save/match align
        self._react_boss = int(st.get('selected_boss_base_id') or self._react_boss or 0)

        # ── scene selector (map / 场景) ──
        scenes = list(st.get('scenes') or [])
        scene_row = tk.Frame(container, bg=PANEL_BG)
        scene_row.pack(fill=tk.X, pady=(0, 4))
        tk.Label(scene_row, text='场景', bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(9)).pack(side=tk.LEFT)
        if scenes:
            sopts = {}
            for s in scenes:
                nm = s.get('name') or ('场景#%s' % s.get('scene_key'))
                sopts['%s (%d)' % (nm, int(s.get('boss_count') or 0))] = str(s.get('scene_key'))
            svar = _tk.StringVar()
            scur = next((l for l, k in sopts.items() if k == self._react_scene), list(sopts)[0])
            svar.set(scur)

            def _pick_scene(lbl, _o=sopts):
                self._react_scene = _o.get(lbl, self._react_scene)
                self._react_boss = 0   # reset boss when the scene changes
                self._rx_rerender()
            som = _tk.OptionMenu(scene_row, svar, *sopts.keys(), command=_pick_scene)
            som.config(font=panel_font(8), bg=PANEL_CARD, fg=TEXT_MAIN, highlightthickness=0)
            som.pack(side=tk.LEFT, padx=(6, 6))

        # ── boss selector (scene-scoped) ──
        bosses = list(st.get('bosses') or [])
        if not self._react_boss and bosses:
            self._react_boss = int(bosses[0].get('base_id') or 0)
        sel_row = tk.Frame(container, bg=PANEL_BG)
        sel_row.pack(fill=tk.X, pady=(0, 2))
        tk.Label(sel_row, text='Boss', bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(9)).pack(side=tk.LEFT)
        if bosses:
            opts = {}
            for b in bosses:
                lbl = '%s (%d)' % (b.get('name') or ('#%d' % int(b.get('base_id') or 0)),
                                   int(b.get('observed_count') or 0))
                opts[lbl] = int(b.get('base_id') or 0)
            var = _tk.StringVar()
            cur = next((l for l, bid in opts.items() if bid == self._react_boss), list(opts)[0])
            var.set(cur)

            def _pick(lbl, _opts=opts):
                self._react_boss = _opts.get(lbl, self._react_boss)
                self._rx_rerender()
            om = _tk.OptionMenu(sel_row, var, *opts.keys(), command=_pick)
            om.config(font=panel_font(8), bg=PANEL_CARD, fg=TEXT_MAIN, highlightthickness=0)
            om.pack(side=tk.LEFT, padx=(6, 6))
        make_action_button(sel_row, '从内存导入', lambda: self._rx_rerender()).pack(side=tk.RIGHT)

        detail = st.get('boss_detail') or {}
        self._render_boss_summary(detail.get('summary') or {})

        # ── casts / buffs (reaction-bindable) ──
        skills = list(detail.get('skills') or [])
        make_section_title(container, '技能 / Buff (施放)')
        if not skills:
            self._rx_empty('暂无技能', '打这个 Boss 时它施放的技能会自动出现')
        else:
            for s in skills:
                self._render_reaction_row('boss_cast', int(s.get('skill_id') or s.get('id') or 0),
                                          self._obs_label(s), rec=s)

        # ── mechanics / states (info + badges) ──
        mechanics = list(detail.get('mechanics') or [])
        if mechanics:
            make_section_title(container, '机制 / 状态 (Mechanics)')
            for s in mechanics:
                self._render_obs_info(self._obs_label(s), s)

        # ── timeline (time-ordered, read-only) ──
        timeline = list(detail.get('timeline') or [])
        if timeline:
            make_section_title(container, '时间线 (Timeline)')
            for t in timeline:
                self._render_timeline_row(t)

        make_section_title(container, '进攻窗口 / Offensive Windows')
        for trig, label in (('boss_breaking', '破防 Breaking'),
                            ('boss_overdrive', '过载 / 狂暴 Overdrive'),
                            ('boss_stun', '眩晕 Stun')):
            self._render_reaction_row(trig, 0, label)

    def _render_boss_summary(self, summary: Dict[str, Any]) -> None:
        if not summary:
            return
        parts = ['技能 %d' % int(summary.get('skill_count') or 0),
                 '机制 %d' % int(summary.get('mechanic_count') or 0)]
        if int(summary.get('hp_line_count') or 0):
            parts.append('血线 %d' % int(summary['hp_line_count']))
        if summary.get('approx_duration_ms'):
            parts.append('时长~%dms' % int(summary['approx_duration_ms']))
        tk.Label(self._rx_container, text=' · '.join(parts), bg=PANEL_BG, fg=TEXT_DIM,
                 font=panel_font(8), anchor='w').pack(fill=tk.X, pady=(0, 4))

    def _render_timeline_row(self, t: Dict[str, Any]) -> None:
        import tkinter as _tk
        card = tk.Frame(self._rx_container, bg=PANEL_CARD, highlightbackground=PANEL_EDGE,
                        highlightthickness=1, padx=8, pady=3)
        card.pack(fill=tk.X, pady=(0, 3))
        top = tk.Frame(card, bg=PANEL_CARD)
        top.pack(fill=tk.X)
        at = float(t.get('at_s') or 0.0)
        clock = ('%d:%02d' % (int(at) // 60, int(at) % 60)) if at >= 60 else ('%.0fs' % at)
        prefix = '⏱' if t.get('is_fixed') else '~'
        _tk.Label(top, text='%s%s' % (prefix, clock), bg=PANEL_CARD, fg=CYAN,
                  font=panel_font(8, bold=True), width=7, anchor='w').pack(side=tk.LEFT)
        nm = (t.get('name') or '').strip() or ('#%d' % int(t.get('id') or 0))
        _tk.Label(top, text=nm, bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(8),
                  anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(4, 0))
        self._render_badges(card, t)

    def _render_obs_info(self, label: str, rec: Dict[str, Any]) -> None:
        """Non-skill observation (mechanic / state) — marked with badges, info only.
        Reactions for breaking/overdrive/stun are bound in the offensive section."""
        card = tk.Frame(self._rx_container, bg=PANEL_CARD, highlightbackground=PANEL_EDGE,
                        highlightthickness=1, padx=8, pady=4)
        card.pack(fill=tk.X, pady=(0, 4))
        tk.Label(card, text=label, bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(9),
                 anchor='w').pack(fill=tk.X)
        self._render_badges(card, rec)

    def _find_react_mapping(self, trig: str, skill_id: int):
        for m in (self._react_state.get('mappings') or []):
            if m.get('trigger_type') != trig:
                continue
            if int(m.get('boss_base_id') or 0) != int(self._react_boss):
                continue
            if trig == 'boss_cast' and int(m.get('skill_id') or 0) != int(skill_id):
                continue
            return m
        return None

    def _render_reaction_row(self, trig: str, skill_id: int, label: str,
                             rec: Optional[Dict[str, Any]] = None) -> None:
        import tkinter as _tk
        m = self._find_react_mapping(trig, skill_id) or {}
        card = tk.Frame(self._rx_container, bg=PANEL_CARD, highlightbackground=PANEL_EDGE,
                        highlightthickness=1, padx=8, pady=5)
        card.pack(fill=tk.X, pady=(0, 4))
        apply_surface_chrome(card, accent=CYAN)
        tk.Label(card, text=label, bg=PANEL_CARD, fg=GOLD, font=panel_font(9, bold=True),
                 anchor='w').pack(fill=tk.X)
        if rec:
            self._render_badges(card, rec)
        row = tk.Frame(card, bg=PANEL_CARD)
        row.pack(fill=tk.X, pady=(3, 0))
        key_var = _tk.StringVar(value=str(m.get('action_key') or ''))
        delay_var = _tk.StringVar(value=str(int(m.get('delay_ms') or 0)))
        cd_var = _tk.StringVar(value=str(m.get('cooldown_s') if m.get('cooldown_s') is not None else 3))
        en_var = _tk.IntVar(value=1 if (m.get('enabled', False) and m.get('id')) else 0)

        def _field(text, var, w):
            tk.Label(row, text=text, bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT)
            _tk.Entry(row, textvariable=var, width=w, font=panel_font(8)).pack(side=tk.LEFT, padx=(2, 6))
        _field('键', key_var, 5)
        _field('延迟ms', delay_var, 6)
        _field('CDs', cd_var, 4)
        _tk.Checkbutton(row, text='启用', variable=en_var, bg=PANEL_CARD, fg=TEXT_MAIN,
                        font=panel_font(8), selectcolor=PANEL_CARD_ALT).pack(side=tk.LEFT)
        mid = str(m.get('id') or '')
        make_action_button(
            row, '保存',
            lambda: self._save_reaction_row(trig, skill_id, mid, key_var, delay_var, cd_var, en_var),
            kind='accent', width=4).pack(side=tk.RIGHT)

    def _save_reaction_row(self, trig, skill_id, mid, key_var, delay_var, cd_var, en_var) -> None:
        if not self._save_reaction:
            return

        def _i(s, d=0):
            try:
                return int(float(s))
            except Exception:
                return d

        def _f(s, d=0.0):
            try:
                return float(s)
            except Exception:
                return d
        mapping = {
            'id': mid, 'enabled': bool(en_var.get()), 'trigger_type': trig,
            'skill_id': int(skill_id), 'boss_base_id': int(self._react_boss),
            'action_key': (key_var.get() or '').strip().upper(),
            'delay_ms': _i(delay_var.get()), 'cooldown_s': _f(cd_var.get(), 3.0),
        }
        try:
            self._save_reaction(mapping)
        except Exception:
            pass
        self._rx_rerender()


class _MechanicsEditorMixin:
    """机制编辑器 (容器无关), 简单面板 / 详细编辑器 / Web 三处共用同一数据契约
    (engines.boss_mechanics_state.build_mechanics_state)。宿主提供
    self._mech_api: {'load','save_mech','delete_mech','test','set_master',
    'create_from_skill','bind','unbind','search_catalog'} 可调用集合。"""

    _MECH_COLORS = ('#ef684e', '#dea620', '#68e4ff', '#9ad334', '#8e44ad', '#2e6fb0')
    _DODGE_PRESETS = ('无', '轻点按键', '按住按键', '按键序列')

    def _init_mechanics(self, mech_api: Optional[Dict[str, Callable]]) -> None:
        self._mech_api = mech_api or {}
        self._mech_state: Dict[str, Any] = {}
        self._mech_editing: Optional[str] = None    # mechanic id being edited
        self._mech_draft: Dict[str, Any] = {}
        self._mech_vars: Dict[str, Any] = {}
        self._mech_catalog_results: list = []
        self._mech_rev = 0

    def _mech_bump(self) -> None:
        self._mech_rev += 1

    def _mech_call(self, name: str, *args, **kw):
        fn = self._mech_api.get(name)
        if not fn:
            return None
        try:
            return fn(*args, **kw)
        except Exception:
            return None

    def _mx_empty(self, title: str, subtitle: str = '') -> None:
        wrap = tk.Frame(self._mx_container, bg=PANEL_BG)
        wrap.pack(fill=tk.X, pady=18)
        tk.Label(wrap, text=title, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(10),
                 justify='center').pack()
        if subtitle:
            tk.Label(wrap, text=subtitle, bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(8),
                     justify='center', wraplength=320).pack(pady=(4, 0))

    def _render_mechanics(self, container, rerender) -> None:
        self._mx_container = container
        self._mx_rerender = rerender
        if not self._mech_api.get('load'):
            self._mx_empty('机制编辑器不可用', '引擎未接入')
            return
        st = self._mech_call('load') or {}
        self._mech_state = st
        self._render_mech_master(st.get('master') or {})
        if not st.get('ok'):
            self._mx_empty('没有可编辑的档案',
                           '先在 Phases 页创建档案, 或导入 assets/boss_raids 下的机制示例 JSON')
            return
        inbox = list(st.get('inbox') or [])
        if inbox:
            make_section_title(container, '未绑定技能收件箱')
            for rec in inbox[:6]:
                self._render_mech_inbox_row(rec)
        make_section_title(container, '机制列表 (%d)' % len(st.get('mechanics') or []))
        for mech in st.get('mechanics') or []:
            self._render_mech_card(mech)
        foot = tk.Frame(container, bg=PANEL_BG)
        foot.pack(fill=tk.X, pady=(6, 2))
        make_action_button(foot, '+ 新建机制', self._mech_new, kind='accent').pack(side=tk.LEFT)
        prof = st.get('profile') or {}
        enr = prof.get('enrage') or {}
        tk.Label(foot,
                 text='狂暴 %ds · 档案: %s' % (int(enr.get('time_s') or prof.get('enrage_time_s') or 0),
                                              prof.get('name') or ''),
                 bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(8)).pack(side=tk.RIGHT)

    # ── 总开关行 ──

    def _render_mech_master(self, master: Dict[str, Any]) -> None:
        import tkinter as _tk
        bar = tk.Frame(self._mx_container, bg=PANEL_CARD, highlightbackground=PANEL_EDGE,
                       highlightthickness=1, padx=8, pady=5)
        bar.pack(fill=tk.X, pady=(0, 5))
        apply_surface_chrome(bar, accent=GOLD)
        row = tk.Frame(bar, bg=PANEL_CARD)
        row.pack(fill=tk.X)

        def _switch(text, key, value):
            var = _tk.IntVar(value=1 if value else 0)

            def _flip():
                # preserve any in-progress edit-form input before the re-render
                if self._mech_editing:
                    self._mech_collect_draft()
                self._mech_call('set_master', {key: bool(var.get())})
                self._mech_bump()
            cb = _tk.Checkbutton(row, text=text, variable=var, command=_flip,
                                 bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(8),
                                 selectcolor=PANEL_CARD_ALT, activebackground=PANEL_CARD)
            cb.pack(side=tk.LEFT, padx=(0, 8))
            return var
        _switch('TTS播报', 'tts_enabled', master.get('tts_enabled', True))
        _switch('横幅提醒', 'banner_enabled', master.get('banner_enabled', True))
        _switch('自动躲避', 'dodge_enabled', master.get('dodge_enabled', True))
        tk.Label(row, text='紧急停用: %s' % (master.get('panic_hotkey') or 'F12'),
                 bg=PANEL_CARD, fg=TEXT_DIM, font=panel_font(8)).pack(side=tk.LEFT, padx=(4, 8))
        vol_var = _tk.StringVar(value=str(int(master.get('tts_volume') or 80)))
        tk.Label(row, text='音量', bg=PANEL_CARD, fg=TEXT_MUTED,
                 font=panel_font(8)).pack(side=tk.LEFT)
        _tk.Spinbox(row, from_=0, to=100, width=4, textvariable=vol_var,
                    font=panel_font(8),
                    command=lambda: self._mech_call('set_master',
                                                    {'tts_volume': vol_var.get()})
                    ).pack(side=tk.LEFT, padx=(2, 0))
        if master.get('zh_voice') is False:
            tk.Label(bar, text='⚠ 未检测到中文语音 (SAPI zh-CN), 播报可能不准确',
                     bg=PANEL_CARD, fg=DANGER, font=panel_font(8),
                     anchor='w').pack(fill=tk.X, pady=(3, 0))

    # ── 收件箱 ──

    def _render_mech_inbox_row(self, rec: Dict[str, Any]) -> None:
        import tkinter as _tk
        card = tk.Frame(self._mx_container, bg=PANEL_CARD, highlightbackground=PANEL_EDGE,
                        highlightthickness=1, padx=8, pady=4)
        card.pack(fill=tk.X, pady=(0, 3))
        sid = int(rec.get('skill_id') or 0)
        nm = (rec.get('name') or '').strip()
        dur = rec.get('last_cast_duration_ms')
        label = '%s #%d ×%d%s' % (nm or '技能', sid, int(rec.get('count') or 0),
                                  (' · %dms' % dur) if dur else '')
        tk.Label(card, text=label, bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(8),
                 anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True)
        mech_opts = {m.get('name') or m.get('id'): m.get('id')
                     for m in (self._mech_state.get('mechanics') or [])}
        if mech_opts:
            var = _tk.StringVar(value='绑定到▾')

            def _bind(lbl, _o=mech_opts, _sid=sid):
                mid = _o.get(lbl)
                if mid:
                    if self._mech_editing:
                        self._mech_collect_draft()
                    self._mech_call('bind', mid, _sid)
                    self._mech_bump()
                    self._mx_rerender()
            om = _tk.OptionMenu(card, var, *mech_opts.keys(), command=_bind)
            om.config(font=panel_font(8), bg=PANEL_CARD_ALT, fg=TEXT_MAIN,
                      highlightthickness=0)
            om.pack(side=tk.RIGHT, padx=(4, 0))

        def _create(_sid=sid, _nm=nm, _dur=dur):
            if self._mech_editing:
                self._mech_collect_draft()
            self._mech_call('create_from_skill', _sid, _nm, _dur)
            self._mech_bump()
            self._mx_rerender()
        make_action_button(card, '建机制', _create, kind='accent', width=6).pack(side=tk.RIGHT)

    # ── 机制卡片 ──

    def _render_mech_card(self, mech: Dict[str, Any]) -> None:
        import tkinter as _tk
        mid = str(mech.get('id') or '')
        card = tk.Frame(self._mx_container, bg=PANEL_CARD, highlightbackground=PANEL_EDGE,
                        highlightthickness=1, padx=8, pady=5)
        card.pack(fill=tk.X, pady=(0, 4))
        accent = mech.get('color') or CYAN
        try:
            apply_surface_chrome(card, accent=accent)
        except Exception:
            apply_surface_chrome(card, accent=CYAN)
        head = tk.Frame(card, bg=PANEL_CARD)
        head.pack(fill=tk.X)
        en_var = _tk.IntVar(value=1 if mech.get('enabled', True) else 0)

        def _flip_enable(_m=mech, _v=en_var):
            if self._mech_editing:
                self._mech_collect_draft()
                if self._mech_editing == _m.get('id'):
                    # keep the open form's draft in sync, or 保存 would
                    # silently flip enabled back to the stale value
                    self._mech_draft['enabled'] = bool(_v.get())
            m2 = dict(_m)
            m2.pop('summary', None)
            m2.pop('skill_names', None)
            m2['enabled'] = bool(_v.get())
            self._mech_call('save_mech', m2)
            self._mech_bump()
        _tk.Checkbutton(head, variable=en_var, command=_flip_enable, bg=PANEL_CARD,
                        selectcolor=PANEL_CARD_ALT, activebackground=PANEL_CARD
                        ).pack(side=tk.LEFT)
        try:
            tk.Frame(head, bg=mech.get('color') or '#68e4ff', width=8, height=8
                     ).pack(side=tk.LEFT, padx=(0, 5))
        except Exception:
            pass
        tk.Label(head, text=mech.get('name') or '机制', bg=PANEL_CARD, fg=GOLD,
                 font=panel_font(9, bold=True), anchor='w'
                 ).pack(side=tk.LEFT, fill=tk.X, expand=True)
        for txt, cb in (('删除', lambda _mid=mid: self._mech_delete(_mid)),
                        ('编辑', lambda _mid=mid: self._mech_edit(_mid)),
                        ('按键', lambda _m=mech: self._mech_test(_m, ('dodge',))),
                        ('横幅', lambda _m=mech: self._mech_test(_m, ('banner',))),
                        ('🔊', lambda _m=mech: self._mech_test(_m, ('tts',)))):
            make_action_button(head, txt, cb, width=4).pack(side=tk.RIGHT, padx=(3, 0))

        summary = mech.get('summary') or {}
        names = mech.get('skill_names') or {}
        det = mech.get('detect') or {}
        chips = []
        for sid in det.get('skill_ids') or []:
            nm = (names.get(str(sid)) or '').strip()
            chips.append('%s#%d' % (nm, sid) if nm else '#%d' % sid)
        for bid in det.get('buff_ids') or []:
            nm = (names.get(str(bid)) or '').strip()
            chips.append('%s#%d' % (nm, bid) if nm else '#%d' % bid)
        sub = ' · '.join(filter(None, [
            ('检测: ' + ', '.join(chips)) if chips else '未绑定检测ID',
            summary.get('alert_desc') or '',
            summary.get('dodge_desc') or '',
        ]))
        tk.Label(card, text=sub, bg=PANEL_CARD, fg=TEXT_DIM, font=panel_font(8),
                 anchor='w', wraplength=420, justify='left').pack(fill=tk.X, pady=(2, 0))
        notes = (mech.get('notes') or '').strip()
        if notes:
            tk.Label(card, text=notes, bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8),
                     anchor='w', wraplength=420, justify='left').pack(fill=tk.X, pady=(2, 0))
        if self._mech_editing == mid:
            self._render_mech_form(card, mech)

    def _mech_new(self) -> None:
        used = len(self._mech_state.get('mechanics') or [])
        draft = {
            'id': '', 'name': '新机制',
            'color': self._MECH_COLORS[used % len(self._MECH_COLORS)],
            'enabled': True, 'phase_ids': [],
            'detect': {'skill_ids': []},
            'alert': {'tts_text': '', 'banner_text': '', 'countdown_s': 8,
                      'pre_warn_s': 3, 'cooldown_s': 5, 'alert_type': 'both',
                      'enabled': True},
            'dodge': {'enabled': False, 'linkage_id': '', 'inline': {}},
        }
        cfg = self._mech_call('save_mech', draft)
        self._mech_bump()
        # 进入新机制的编辑态: 从返回配置里找到刚插入的 id
        try:
            profiles = list((cfg or {}).get('profiles') or [])
            active_id = (cfg or {}).get('active_profile_id')
            prof = next((p for p in profiles if p.get('id') == active_id), None)
            mechs = list((prof or {}).get('mechanics') or [])
            if mechs:
                self._mech_editing = str(mechs[-1].get('id'))
        except Exception:
            self._mech_editing = None
        self._mx_rerender()

    def _mech_edit(self, mid: str) -> None:
        self._mech_editing = None if self._mech_editing == mid else mid
        # collapse = cancel: always restart from the saved state so a reopened
        # form never refills from a stale draft
        self._mech_draft = {}
        self._mech_catalog_results = []
        self._mech_bump()
        self._mx_rerender()

    def _mech_delete(self, mid: str) -> None:
        if self._mech_editing and self._mech_editing != mid:
            self._mech_collect_draft()
        self._mech_call('delete_mech', mid)
        if self._mech_editing == mid:
            self._mech_editing = None
            self._mech_draft = {}
        self._mech_bump()
        self._mx_rerender()

    def _mech_test(self, mech: Dict[str, Any], kinds) -> None:
        self._mech_call('test', mech, tuple(kinds))

    # ── 编辑表单 (卡片内行展开) ──

    def _render_mech_form(self, card, mech: Dict[str, Any]) -> None:
        import tkinter as _tk
        if not isinstance(self._mech_draft, dict) or \
                self._mech_draft.get('id') != mech.get('id'):
            import copy as _copy
            self._mech_draft = _copy.deepcopy(mech)
            self._mech_draft.pop('summary', None)
            self._mech_draft.pop('skill_names', None)
        draft = self._mech_draft
        det = draft.setdefault('detect', {})
        alert = draft.setdefault('alert', {})
        dodge = draft.setdefault('dodge', {})
        inline = dodge.setdefault('inline', {})
        form = tk.Frame(card, bg=PANEL_CARD_ALT, padx=8, pady=6)
        form.pack(fill=tk.X, pady=(5, 0))
        v: Dict[str, Any] = {}
        self._mech_vars = v

        def _row(parent):
            r = tk.Frame(parent, bg=PANEL_CARD_ALT)
            r.pack(fill=tk.X, pady=(0, 3))
            return r

        def _field(parent, text, key, value, width=8):
            tk.Label(parent, text=text, bg=PANEL_CARD_ALT, fg=TEXT_MUTED,
                     font=panel_font(8)).pack(side=tk.LEFT)
            var = _tk.StringVar(value=str(value if value is not None else ''))
            v[key] = var
            _tk.Entry(parent, textvariable=var, width=width, font=panel_font(8)
                      ).pack(side=tk.LEFT, padx=(2, 8))
            return var

        r1 = _row(form)
        _field(r1, '名称', 'name', draft.get('name') or '', 14)
        tk.Label(r1, text='颜色', bg=PANEL_CARD_ALT, fg=TEXT_MUTED,
                 font=panel_font(8)).pack(side=tk.LEFT)
        for c in self._MECH_COLORS:
            sw = tk.Frame(r1, bg=c, width=14, height=14, cursor='hand2',
                          highlightthickness=2 if draft.get('color') == c else 0,
                          highlightbackground=TEXT_MAIN)
            sw.pack(side=tk.LEFT, padx=1)
            sw.bind('<Button-1>',
                    lambda _e, _c=c: (self._mech_collect_draft(),
                                      draft.__setitem__('color', _c),
                                      self._mx_rerender()))

        # 说明 / 躲法 (机制卡片直读这段文字, 写全机制描述和怎么躲)
        make_section_title(form, '说明 (机制 & 躲法)')
        notes_box = _tk.Text(form, height=4, width=52, font=panel_font(8),
                             bg=PANEL_CARD, fg=TEXT_MAIN, insertbackground=TEXT_MAIN,
                             wrap='char', relief='flat', padx=4, pady=3)
        notes_box.insert('1.0', (draft.get('notes') or '').strip())
        notes_box.pack(fill=tk.X, pady=(0, 4))
        v['notes_widget'] = notes_box

        # 检测: 绑定 chips (技能 + Buff, 每 3 个换行) + 添加来源
        make_section_title(form, '检测 (技能/Buff ID)')
        names = (mech.get('skill_names') or {})
        bound = list(det.get('skill_ids') or [])
        bound_buffs = list(det.get('buff_ids') or [])
        chip_items = [('skill_ids', sid) for sid in bound] + \
                     [('buff_ids', bid) for bid in bound_buffs]
        if not chip_items:
            tk.Label(_row(form), text='未绑定 — 从下方添加', bg=PANEL_CARD_ALT,
                     fg=TEXT_DIM, font=panel_font(8)).pack(side=tk.LEFT)
        chips_row = None
        for idx, (det_key, cid) in enumerate(chip_items):
            if idx % 3 == 0:
                chips_row = _row(form)
            nm = (names.get(str(cid)) or '').strip()
            prefix = 'B ' if det_key == 'buff_ids' else ''
            chip = tk.Label(chips_row, text='%s%s#%d ✕' % (prefix, nm + ' ' if nm else '', cid),
                            bg=PANEL_EDGE, fg=CYAN if det_key == 'buff_ids' else TEXT_MAIN,
                            font=panel_font(8), padx=5, pady=1, cursor='hand2')
            chip.pack(side=tk.LEFT, padx=(0, 4))
            chip.bind('<Button-1>',
                      lambda _e, _k=det_key, _cid=cid: (
                          self._mech_collect_draft(),
                          det.__setitem__(_k, [x for x in det.get(_k) or []
                                               if int(x) != int(_cid)]),
                          self._mx_rerender()))
        add_row = _row(form)
        observed = [o for o in (self._mech_state.get('observed') or [])
                    if int(o.get('id') or 0) not in set(int(x) for x in bound)]
        if observed:
            obs_opts = {}
            for o in observed[:25]:
                lbl = '%s #%d ×%d' % ((o.get('name') or '技能'), int(o.get('id') or 0),
                                      int(o.get('count') or 0))
                obs_opts[lbl] = int(o.get('id') or 0)
            ovar = _tk.StringVar(value='添加观测技能▾')

            def _add_obs(lbl, _o=obs_opts):
                sid = _o.get(lbl)
                if sid:
                    self._mech_collect_draft()
                    ids = list(det.get('skill_ids') or [])
                    if sid not in ids:
                        ids.append(sid)
                        det['skill_ids'] = sorted(ids)
                    self._mx_rerender()
            om = _tk.OptionMenu(add_row, ovar, *obs_opts.keys(), command=_add_obs)
            om.config(font=panel_font(8), bg=PANEL_CARD, fg=TEXT_MAIN,
                      highlightthickness=0)
            om.pack(side=tk.LEFT, padx=(0, 6))
        manual_var = _field(add_row, '技能ID', 'manual_sid', '', 9)

        def _add_manual_to(det_key, var):
            try:
                cid = int(float(var.get()))
            except Exception:
                return
            if cid > 0:
                self._mech_collect_draft()
                ids = list(det.get(det_key) or [])
                if cid not in ids:
                    ids.append(cid)
                    det[det_key] = sorted(ids)
                self._mx_rerender()
        make_action_button(add_row, '添加',
                           lambda: _add_manual_to('skill_ids', manual_var),
                           width=4).pack(side=tk.LEFT)
        buff_var = _field(add_row, 'BuffID', 'manual_bid', '', 9)
        make_action_button(add_row, '添加',
                           lambda: _add_manual_to('buff_ids', buff_var),
                           width=4).pack(side=tk.LEFT)
        search_row = _row(form)
        search_var = _field(search_row, '技能库', 'catalog_q', '', 12)

        def _do_search():
            self._mech_collect_draft()
            self._mech_catalog_results = list(
                self._mech_call('search_catalog', search_var.get()) or [])[:10]
            self._mx_rerender()
        make_action_button(search_row, '搜索', _do_search, width=4).pack(side=tk.LEFT)
        for hit in self._mech_catalog_results:
            hit_row = _row(form)
            tk.Label(hit_row, text='%s #%d (%s)' % (hit.get('name'), int(hit.get('id') or 0),
                                                    hit.get('kind') or ''),
                     bg=PANEL_CARD_ALT, fg=TEXT_MAIN, font=panel_font(8),
                     anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True)
            make_action_button(
                hit_row, '绑定',
                lambda _sid=int(hit.get('id') or 0): (
                    self._mech_collect_draft(),
                    det.__setitem__('skill_ids',
                                    sorted(set(list(det.get('skill_ids') or []) + [_sid]))),
                    self._mx_rerender()),
                kind='accent', width=4).pack(side=tk.RIGHT)

        # 提醒
        make_section_title(form, '提醒')
        a1 = _row(form)
        _field(a1, 'TTS文案', 'tts_text', alert.get('tts_text') or '', 22)
        make_action_button(a1, '试听',
                           lambda: self._mech_test(self._mech_collect_draft(), ('tts',)),
                           width=4).pack(side=tk.LEFT)
        a2 = _row(form)
        _field(a2, '横幅文案', 'banner_text', alert.get('banner_text') or '', 22)
        a3 = _row(form)
        _field(a3, '倒计时s', 'countdown_s', alert.get('countdown_s'), 5)
        _field(a3, '预警s', 'pre_warn_s', alert.get('pre_warn_s'), 5)
        _field(a3, '冷却s', 'cooldown_s', alert.get('cooldown_s'), 5)

        # 躲避
        make_section_title(form, '自动躲避')
        d1 = _row(form)
        den_var = _tk.IntVar(value=1 if dodge.get('enabled') else 0)
        v['dodge_enabled'] = den_var
        _tk.Checkbutton(d1, text='启用', variable=den_var, bg=PANEL_CARD_ALT,
                        fg=TEXT_MAIN, font=panel_font(8), selectcolor=PANEL_CARD,
                        activebackground=PANEL_CARD_ALT).pack(side=tk.LEFT, padx=(0, 8))
        seq = list(inline.get('sequence') or [])
        preset_idx = 3 if seq else (
            2 if str(inline.get('press_mode')) == 'hold' and inline.get('action_key') else (
                1 if inline.get('action_key') else 0))
        pvar = _tk.StringVar(value=self._DODGE_PRESETS[preset_idx])
        v['dodge_preset'] = pvar

        def _preset_pick(lbl):
            self._mech_collect_draft()
            if lbl == '按键序列' and not (inline.get('sequence') or []):
                inline['sequence'] = [{'key': '', 'delay_ms': 0, 'hold_ms': 0}]
            if lbl != '按键序列':
                inline['sequence'] = []
            inline['press_mode'] = 'hold' if lbl == '按住按键' else 'tap'
            self._mx_rerender()
        pm = _tk.OptionMenu(d1, pvar, *self._DODGE_PRESETS, command=_preset_pick)
        pm.config(font=panel_font(8), bg=PANEL_CARD, fg=TEXT_MAIN, highlightthickness=0)
        pm.pack(side=tk.LEFT, padx=(0, 8))
        _field(d1, '提前ms', 'lead_ms', inline.get('lead_ms', 300), 6)
        preset_lbl = pvar.get()
        if preset_lbl in ('轻点按键', '按住按键'):
            d2 = _row(form)
            _field(d2, '键', 'dodge_key', inline.get('action_key') or '', 6)
            if preset_lbl == '按住按键':
                _field(d2, '按住ms', 'dodge_hold', inline.get('hold_ms', 600), 6)
        elif preset_lbl == '按键序列':
            v['seq_vars'] = []
            for idx, step in enumerate(seq):
                sr = _row(form)
                kv = _tk.StringVar(value=str(step.get('key') or ''))
                dv = _tk.StringVar(value=str(int(step.get('delay_ms') or 0)))
                hv = _tk.StringVar(value=str(int(step.get('hold_ms') or 0)))
                v['seq_vars'].append((kv, dv, hv))
                tk.Label(sr, text='步%d 键' % (idx + 1), bg=PANEL_CARD_ALT,
                         fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT)
                _tk.Entry(sr, textvariable=kv, width=6, font=panel_font(8)).pack(side=tk.LEFT, padx=(2, 4))
                tk.Label(sr, text='延迟ms', bg=PANEL_CARD_ALT, fg=TEXT_MUTED,
                         font=panel_font(8)).pack(side=tk.LEFT)
                _tk.Entry(sr, textvariable=dv, width=6, font=panel_font(8)).pack(side=tk.LEFT, padx=(2, 4))
                tk.Label(sr, text='按住ms', bg=PANEL_CARD_ALT, fg=TEXT_MUTED,
                         font=panel_font(8)).pack(side=tk.LEFT)
                _tk.Entry(sr, textvariable=hv, width=6, font=panel_font(8)).pack(side=tk.LEFT, padx=(2, 4))
                make_action_button(
                    sr, '✕',
                    lambda _i=idx: (self._mech_collect_draft(),
                                    inline['sequence'].pop(_i),
                                    self._mx_rerender()),
                    width=2).pack(side=tk.RIGHT)
            seq_btn_row = _row(form)
            make_action_button(
                seq_btn_row, '+ 加一步',
                lambda: (self._mech_collect_draft(),
                         inline.setdefault('sequence', []).append(
                             {'key': '', 'delay_ms': 0, 'hold_ms': 0}),
                         self._mx_rerender()),
                width=8).pack(side=tk.LEFT)

        # 阶段范围
        ph_row = _row(form)
        phases = list(self._mech_state.get('phases') or [])
        ph_opts = {'全部阶段': ''}
        for p in phases:
            ph_opts[p.get('name') or p.get('id')] = p.get('id')
        cur_pids = list(draft.get('phase_ids') or [])
        cur_lbl = '全部阶段'
        if cur_pids:
            cur_lbl = next((lbl for lbl, pid in ph_opts.items() if pid and pid in cur_pids),
                           '全部阶段')
        phvar = _tk.StringVar(value=cur_lbl)
        v['phase_pick'] = (phvar, ph_opts)
        tk.Label(ph_row, text='阶段范围', bg=PANEL_CARD_ALT, fg=TEXT_MUTED,
                 font=panel_font(8)).pack(side=tk.LEFT)
        phm = _tk.OptionMenu(ph_row, phvar, *ph_opts.keys())
        phm.config(font=panel_font(8), bg=PANEL_CARD, fg=TEXT_MAIN, highlightthickness=0)
        phm.pack(side=tk.LEFT, padx=(4, 0))

        btns = _row(form)
        make_action_button(btns, '保存', self._mech_save_form, kind='accent',
                           width=6).pack(side=tk.RIGHT)
        make_action_button(btns, '取消',
                           lambda: (setattr(self, '_mech_editing', None),
                                    setattr(self, '_mech_draft', {}),
                                    self._mech_bump(), self._mx_rerender()),
                           width=6).pack(side=tk.RIGHT, padx=(0, 6))

    def _mech_collect_draft(self) -> Dict[str, Any]:
        """把当前表单变量回收进草稿 (供重渲染/试发/保存共用)。"""
        v = self._mech_vars or {}
        draft = self._mech_draft or {}
        det = draft.setdefault('detect', {})
        alert = draft.setdefault('alert', {})
        dodge = draft.setdefault('dodge', {})
        inline = dodge.setdefault('inline', {})

        def _sv(key, default=''):
            var = v.get(key)
            try:
                return var.get() if var is not None else default
            except Exception:
                return default

        def _num(key, default):
            try:
                return float(_sv(key, default))
            except Exception:
                return default
        if 'name' in v:
            draft['name'] = (_sv('name') or '机制').strip()
        nw = v.get('notes_widget')
        if nw is not None:
            try:
                draft['notes'] = nw.get('1.0', 'end').strip()
            except Exception:
                pass
        if 'tts_text' in v:
            alert['tts_text'] = _sv('tts_text').strip()
        if 'banner_text' in v:
            alert['banner_text'] = _sv('banner_text').strip()
        if 'countdown_s' in v:
            alert['countdown_s'] = _num('countdown_s', 8)
        if 'pre_warn_s' in v:
            alert['pre_warn_s'] = _num('pre_warn_s', 3)
        if 'cooldown_s' in v:
            alert['cooldown_s'] = _num('cooldown_s', 5)
        if 'dodge_enabled' in v:
            dodge['enabled'] = bool(v['dodge_enabled'].get())
        if 'lead_ms' in v:
            inline['lead_ms'] = int(_num('lead_ms', 300))
        preset = _sv('dodge_preset', '无')
        if preset == '轻点按键':
            inline['press_mode'] = 'tap'
            inline['action_key'] = _sv('dodge_key').strip().upper()
            inline['sequence'] = []
        elif preset == '按住按键':
            inline['press_mode'] = 'hold'
            inline['action_key'] = _sv('dodge_key').strip().upper()
            inline['hold_ms'] = int(_num('dodge_hold', 600))
            inline['sequence'] = []
        elif preset == '按键序列':
            # keep empty-key steps so indexes stay aligned with the rendered
            # rows (✕ deletes by index); they are filtered out at save time
            steps = []
            for kv, dv, hv in v.get('seq_vars') or []:
                key = (kv.get() or '').strip().upper()
                try:
                    delay = int(float(dv.get() or 0))
                except Exception:
                    delay = 0
                try:
                    hold = int(float(hv.get() or 0))
                except Exception:
                    hold = 0
                steps.append({'key': key, 'delay_ms': delay, 'hold_ms': hold})
            inline['sequence'] = steps
            inline['action_key'] = ''
        else:
            inline['action_key'] = ''
            inline['sequence'] = []
        pick = v.get('phase_pick')
        if pick:
            phvar, ph_opts = pick
            pid = ph_opts.get(phvar.get() or '全部阶段', '')
            draft['phase_ids'] = [pid] if pid else []
        return draft

    def _mech_save_form(self) -> None:
        draft = self._mech_collect_draft()
        inline = ((draft.get('dodge') or {}).get('inline') or {})
        if inline.get('sequence'):
            inline['sequence'] = [s for s in inline['sequence']
                                  if (s.get('key') or '').strip()]
        self._mech_call('save_mech', draft)
        self._mech_editing = None
        self._mech_draft = {}
        self._mech_bump()
        self._mx_rerender()


class BossRaidPanel(_MechanicsEditorMixin, _BossReactionsEditorMixin):
    def __init__(
        self,
        master: tk.Tk,
        load_fn: Callable[[], dict],
        save_fn: Callable[[dict], Any],
        engine_ref: Callable[[], Any],
        on_toggle: Callable[[bool], None],
        on_start: Callable[[], None],
        on_next: Callable[[], None],
        on_reset: Optional[Callable[[], None]] = None,
        load_reactions_fn: Optional[Callable[[], dict]] = None,
        save_reaction_fn: Optional[Callable[[dict], Any]] = None,
        mechanics_api: Optional[Dict[str, Callable]] = None,
    ):
        self._master = master
        self._load = load_fn
        self._save = save_fn
        self._engine_ref = engine_ref
        self._on_toggle = on_toggle
        self._on_start = on_start
        self._on_next = on_next
        self._on_reset = on_reset or (lambda: None)
        self._load_reactions = load_reactions_fn
        self._save_reaction = save_reaction_fn
        self._init_mechanics(mechanics_api)
        self._react_state: Dict[str, Any] = {}
        self._react_boss: int = 0
        self._react_scene: Optional[str] = None   # selected scene_key (None = live)
        self._react_widgets: Dict[str, Any] = {}
        self._win: Optional[tk.Toplevel] = None
        self._visible = False
        self._cfg: Dict[str, Any] = {}
        self._status: Dict[str, Any] = {}
        self._current_tab = 'entities'
        self._drag_ox = 0
        self._drag_oy = 0
        self._poll_after_id: Optional[str] = None
        self._tab_labels: Dict[str, tk.Label] = {}
        self._content_body: Optional[tk.Frame] = None
        self._canvas: Optional[tk.Canvas] = None
        self._badge: Optional[tk.Label] = None
        self._status_elapsed: Optional[tk.Label] = None
        self._status_dps: Optional[tk.Label] = None
        self._status_phase: Optional[tk.Label] = None
        self._last_render_signature: Optional[Tuple[Any, ...]] = None

    def is_visible(self) -> bool:
        return bool(self._visible and self._win and self._win.winfo_exists())

    def show(self) -> None:
        self._cfg = self._load()
        if self._win is None or not self._win.winfo_exists():
            self._build()
        self._visible = True
        try:
            self._win.deiconify()
            self._win.lift()
            self._win.focus_force()
        except Exception:
            pass
        self._refresh_live(force=True)

    def hide(self) -> None:
        self._cancel_poll()
        if self._win is not None:
            try:
                self._win.withdraw()
            except Exception:
                pass
        self._visible = False

    def toggle(self) -> None:
        if self.is_visible():
            self.hide()
        else:
            self.show()

    def destroy(self) -> None:
        self._cancel_poll()
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._visible = False

    def _build(self) -> None:
        width, height, pos_x, pos_y = calc_panel_geometry(
            self._master,
            min_w=360,
            min_h=460,
            width_ratio=0.22,
            height_ratio=0.52,
            x_ratio=0.012,
            y_ratio=0.18,
        )
        win = tk.Toplevel(self._master)
        win.overrideredirect(True)
        win.configure(bg=PANEL_EDGE)
        win.geometry(f'{width}x{height}+{pos_x}+{pos_y}')
        try:
            win.attributes('-topmost', True)
        except Exception:
            pass
        win.bind('<Escape>', lambda _event: self.hide())
        self._win = win

        shell = tk.Frame(win, bg=PANEL_BG, highlightthickness=1, highlightbackground=PANEL_EDGE)
        shell.pack(fill=tk.BOTH, expand=True)
        apply_surface_chrome(shell, accent=CYAN, accent_side='top')
        place_corner_accents(shell)

        header = tk.Frame(shell, bg=PANEL_HEADER, height=58)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        apply_surface_chrome(header)

        tk.Label(
            header,
            text='Live Raid Editor',
            bg=PANEL_HEADER,
            fg=TEXT_MUTED,
            font=panel_font(8),
        ).pack(anchor='w', padx=16, pady=(10, 0))

        title_row = tk.Frame(header, bg=PANEL_HEADER)
        title_row.pack(fill=tk.X, padx=16, pady=(2, 8))
        tk.Label(
            title_row,
            text='Boss Raid',
            bg=PANEL_HEADER,
            fg=TEXT_MAIN,
            font=panel_font(13, bold=True),
        ).pack(side=tk.LEFT)
        self._badge = tk.Label(
            title_row,
            text='IDLE',
            bg=TEXT_MUTED,
            fg='#ffffff',
            font=panel_font(8, bold=True),
            padx=8,
            pady=2,
        )
        self._badge.pack(side=tk.LEFT, padx=(10, 0))
        bind_drag(header, self._on_drag_start, self._on_drag_move)

        tk.Frame(shell, bg=LINE, height=1).pack(fill=tk.X)

        tab_bar = tk.Frame(shell, bg=PANEL_BG_ALT, height=34)
        tab_bar.pack(fill=tk.X)
        tab_bar.pack_propagate(False)
        for key, text in (
            ('entities', 'Entities'),
            ('phases', 'Phases'),
            ('timeline', 'Timeline'),
            ('reactions', 'Boss 反应'),
            ('mechanics', '机制'),
        ):
            slot = tk.Frame(tab_bar, bg=PANEL_BG_ALT)
            slot.pack(side=tk.LEFT, expand=True, fill=tk.BOTH)
            label = make_tab_label(slot, text, command=lambda tab=key: self._switch_tab(tab))
            label.pack(fill=tk.X, expand=True)
            attach_tab_underline(label, slot)
            self._tab_labels[key] = label
        self._refresh_tabs()

        content = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=8)
        content.pack(fill=tk.BOTH, expand=True)
        _, canvas, body = create_scrollable_area(content, PANEL_BG)
        self._canvas = canvas
        self._content_body = body

        action_row = tk.Frame(shell, bg=PANEL_BG, padx=12, pady=8)
        action_row.pack(fill=tk.X)
        make_action_button(action_row, 'NEXT PHASE', self._handle_next_phase, kind='accent').pack(side=tk.LEFT, fill=tk.X, expand=True)
        make_action_button(action_row, 'RESET', self._handle_reset).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(8, 0))

        tk.Frame(shell, bg=PANEL_EDGE, height=1).pack(fill=tk.X)

        status_bar = tk.Frame(shell, bg=PANEL_BG, padx=14, pady=5)
        status_bar.pack(fill=tk.X)
        self._status_elapsed = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_elapsed.pack(side=tk.LEFT)
        self._status_dps = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_dps.pack(side=tk.LEFT, expand=True)
        self._status_phase = tk.Label(status_bar, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(8, bold=True))
        self._status_phase.pack(side=tk.RIGHT)

    def _cancel_poll(self) -> None:
        if self._poll_after_id and self._win is not None:
            try:
                self._win.after_cancel(self._poll_after_id)
            except Exception:
                pass
        self._poll_after_id = None

    def _schedule_poll(self) -> None:
        self._cancel_poll()
        if self._win is None or not self._visible:
            return
        self._poll_after_id = self._win.after(250, self._refresh_live)

    def _refresh_live(self, force: bool = False) -> None:
        if self._win is None or not self._visible:
            return
        try:
            self._cfg = self._load()
        except Exception:
            pass
        engine = self._engine_ref() if self._engine_ref else None
        if engine and hasattr(engine, 'get_status'):
            # only the Entities tab consumes the (O(N)) entity list; skip building
            # it on the 250ms poll for every other tab so an open editor adds no
            # per-tick lock pressure during a crowded raid.
            want_entities = self._current_tab == 'entities'
            try:
                self._status = engine.get_status(include_entities=want_entities) or {}
            except TypeError:
                self._status = engine.get_status() or {}
            except Exception:
                self._status = {}
        else:
            self._status = {}
        self._update_header_status()
        self._render_if_needed(force=force)
        self._schedule_poll()

    def _update_header_status(self) -> None:
        state = str(self._status.get('state') or 'idle').lower()
        if self._badge is not None:
            if state == 'running':
                apply_badge(self._badge, 'RUNNING', 'running')
            elif state == 'completed':
                apply_badge(self._badge, 'DONE', 'active')
            else:
                apply_badge(self._badge, 'IDLE', 'off')
        if self._status_elapsed is not None:
            self._status_elapsed.configure(text=_fmt_time(self._status.get('elapsed_s') or 0))
        if self._status_dps is not None:
            self._status_dps.configure(text=f'{_fmt_num(self._status.get("dps") or 0)} DPS')
        if self._status_phase is not None:
            phase_name = self._status.get('phase_name') or f'P{int(self._status.get("phase_idx") or 0) + 1}'
            self._status_phase.configure(text=str(phase_name))

    def _render_if_needed(self, force: bool = False) -> None:
        if self._content_body is None:
            return
        signature = self._build_signature()
        if not force and signature == self._last_render_signature:
            return
        self._last_render_signature = signature
        clear_frame(self._content_body)
        if self._current_tab == 'entities':
            self._render_entities_tab()
        elif self._current_tab == 'phases':
            self._render_phases_tab()
        elif self._current_tab == 'reactions':
            self._render_reactions_tab()
        elif self._current_tab == 'mechanics':
            self._render_mechanics(self._content_body,
                                   lambda: self._render_if_needed(force=True))
        else:
            self._render_timeline_tab()
        try:
            self._canvas.configure(scrollregion=self._canvas.bbox('all'))
        except Exception:
            pass

    def _build_signature(self) -> Tuple[Any, ...]:
        if self._current_tab == 'reactions':
            # Stable while editing so the live 250ms poll never clobbers the Entry
            # widgets; re-render happens on tab-enter, scene/boss change, and save.
            return ('reactions', str(self._react_scene), int(self._react_boss))
        if self._current_tab == 'mechanics':
            # 同上: 编辑期间签名稳定, 重渲染由显式动作驱动 (_mech_bump)。
            return ('mechanics', int(self._mech_rev), str(self._mech_editing))
        profile = self._active_profile()
        phases = list((profile or {}).get('phases') or [])
        entities = list(self._status.get('entities') or [])
        entity_sig = tuple(
            (
                int(item.get('uuid') or 0),
                str(item.get('role') or ''),
                round(float(item.get('hp_pct') or 0.0), 3),
                int(item.get('damage_dealt') or 0),
                bool(item.get('shield_active')),
                int(item.get('breaking_stage') or 0),
                bool(item.get('in_overdrive')),
            )
            for item in entities
        )
        phase_sig = tuple(
            (
                str(phase.get('name') or ''),
                str((phase.get('trigger') or {}).get('type') or ''),
                (phase.get('trigger') or {}).get('value') or 0,
                tuple(
                    (
                        round(float(timeline.get('time_s') or 0.0), 1),
                        str(timeline.get('label') or ''),
                        str(timeline.get('alert_type') or ''),
                    )
                    for timeline in list(phase.get('timelines') or [])
                ),
            )
            for phase in phases
        )
        return (
            self._current_tab,
            str(self._status.get('state') or ''),
            int(self._status.get('phase_idx') or 0),
            round(float(self._status.get('elapsed_s') or 0.0), 1),
            int(self._status.get('dps') or 0),
            entity_sig,
            phase_sig,
        )

    def _active_profile(self) -> Optional[Dict[str, Any]]:
        profiles = list(self._cfg.get('profiles') or [])
        active_id = self._cfg.get('active_profile_id')
        for profile in profiles:
            if profile.get('id') == active_id:
                return profile
        return profiles[0] if profiles else None

    def _switch_tab(self, tab: str) -> None:
        if tab == self._current_tab:
            return
        self._current_tab = tab
        self._refresh_tabs()
        self._render_if_needed(force=True)

    def _refresh_tabs(self) -> None:
        for key, label in self._tab_labels.items():
            set_tab_active(label, key == self._current_tab)

    def _render_entities_tab(self) -> None:
        entities = list(self._status.get('entities') or [])
        if not entities:
            self._render_empty('Waiting for combat data...', 'Attack a monster to begin tracking')
            return
        container = tk.Frame(self._content_body, bg=PANEL_BG)
        container.pack(fill=tk.X)
        for idx, entity in enumerate(entities, start=1):
            role = str(entity.get('role') or 'enemy')
            is_boss = role == 'boss'
            card = tk.Frame(
                container,
                bg=PANEL_CARD,
                highlightbackground=PANEL_EDGE,
                highlightthickness=1,
                padx=10,
                pady=8,
            )
            card.pack(fill=tk.X, pady=(0, 6))
            apply_surface_chrome(card, accent=DANGER if is_boss else GOLD)
            tk.Frame(card, bg=DANGER if is_boss else GOLD, width=3, height=52).pack(side=tk.LEFT, fill=tk.Y, padx=(0, 10))
            rank = tk.Label(
                card,
                text=str(idx),
                bg=DANGER if is_boss else GOLD,
                fg='#ffffff',
                width=2,
                font=panel_font(9, bold=True),
            )
            rank.pack(side=tk.LEFT, padx=(0, 10))

            info = tk.Frame(card, bg=PANEL_CARD)
            info.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
            name = str(entity.get('name') or f'Entity {int(entity.get("uuid") or 0) & 0xFFFF}')
            tk.Label(info, text=name, bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(10, bold=True), anchor='w').pack(fill=tk.X)
            hp_pct = int(round(float(entity.get('hp_pct') or 0.0) * 100.0))
            parts = [f'DMG: {_fmt_num(entity.get("damage_dealt") or 0)}', f'HP: {hp_pct}%']
            if entity.get('shield_active'):
                parts.append('Shield')
            if entity.get('in_overdrive'):
                parts.append('OD')
            if int(entity.get('breaking_stage') or 0) > 0:
                parts.append('Break')
            tk.Label(info, text=' · '.join(parts), bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8), anchor='w').pack(fill=tk.X, pady=(1, 0))

            bar_bg = tk.Frame(info, bg=PANEL_CARD_ALT, height=4)
            bar_bg.pack(fill=tk.X, pady=(4, 0))
            fill_width = max(0, min(100, hp_pct))
            bar_fill = tk.Frame(bar_bg, bg=READY, height=4)
            bar_fill.place(relwidth=fill_width / 100.0, relheight=1.0)

            role_text = 'BOSS' if is_boss else 'ENEMY'
            role_bg = DANGER if is_boss else GOLD
            role_btn = tk.Label(
                card,
                text=role_text,
                bg=PANEL_CARD,
                fg=TEXT_MAIN,
                font=panel_font(8, bold=True),
                padx=10,
                pady=4,
                cursor='hand2',
            )
            role_btn.pack(side=tk.RIGHT)
            apply_badge(role_btn, role_text, 'running' if is_boss else 'active')
            role_btn.bind(
                '<Button-1>',
                lambda _event, uuid=int(entity.get('uuid') or 0), current=role: self._toggle_role(uuid, current),
            )

    def _render_reactions_tab(self) -> None:
        self._render_reactions(self._content_body,
                               lambda: self._render_if_needed(force=True))

    def _render_phases_tab(self) -> None:
        make_section_title(self._content_body, 'Raid Phases')
        profile = self._active_profile()
        phases = list((profile or {}).get('phases') or [])
        if not phases:
            self._render_empty('No phases configured', 'Create or download a raid profile first')
            return
        current_idx = int(self._status.get('phase_idx') or 0)
        for idx, phase in enumerate(phases):
            current = idx == current_idx
            bg = PANEL_CARD_ALT if current else PANEL_CARD
            border = GOLD if current else PANEL_EDGE
            card = tk.Frame(self._content_body, bg=bg, highlightbackground=border, highlightthickness=1, padx=12, pady=8)
            card.pack(fill=tk.X, pady=(0, 5))
            apply_surface_chrome(card, accent=GOLD if current else CYAN)
            tk.Label(card, text=str(phase.get('name') or f'P{idx + 1}'), bg=bg, fg=TEXT_MAIN, font=panel_font(10, bold=True)).pack(side=tk.LEFT, fill=tk.X, expand=True)
            tk.Label(card, text=_trigger_text(phase.get('trigger')), bg=bg, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.LEFT, padx=(8, 8))
            if current:
                make_action_button(card, '→', self._handle_next_phase, kind='accent', width=2).pack(side=tk.RIGHT)

    def _render_timeline_tab(self) -> None:
        profile = self._active_profile()
        phases = list((profile or {}).get('phases') or [])
        current_idx = int(self._status.get('phase_idx') or 0)
        current_phase = phases[current_idx] if 0 <= current_idx < len(phases) else None
        timelines = list((current_phase or {}).get('timelines') or [])
        if not timelines:
            self._render_empty('Phase timeline alerts will appear here during combat.', '')
            return
        title = str((current_phase or {}).get('name') or f'P{current_idx + 1}')
        make_section_title(self._content_body, f'{title} Timeline')
        for idx, timeline in enumerate(timelines, start=1):
            row = tk.Frame(self._content_body, bg=PANEL_CARD, highlightbackground=PANEL_EDGE, highlightthickness=1, padx=10, pady=6)
            row.pack(fill=tk.X, pady=(0, 4))
            apply_surface_chrome(row, accent=CYAN)
            tk.Label(row, text=f'{idx}.', bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8, bold=True), width=3).pack(side=tk.LEFT)
            tk.Label(row, text=f'{round(float(timeline.get("time_s") or 0.0), 1)}s', bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(9, bold=True), width=6).pack(side=tk.LEFT)
            tk.Label(row, text=str(timeline.get('label') or 'Alert'), bg=PANEL_CARD, fg=TEXT_MAIN, font=panel_font(9), anchor='w').pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 0))
            tk.Label(row, text=str(timeline.get('alert_type') or 'both').upper(), bg=PANEL_CARD, fg=TEXT_MUTED, font=panel_font(8)).pack(side=tk.RIGHT)

    def _render_empty(self, title: str, subtitle: str) -> None:
        wrap = tk.Frame(self._content_body, bg=PANEL_BG)
        wrap.pack(fill=tk.BOTH, expand=True, pady=26)
        tk.Label(wrap, text=title, bg=PANEL_BG, fg=TEXT_MUTED, font=panel_font(10), justify='center').pack()
        if subtitle:
            tk.Label(wrap, text=subtitle, bg=PANEL_BG, fg=TEXT_DIM, font=panel_font(8), justify='center').pack(pady=(4, 0))

    def _toggle_role(self, uuid: int, current_role: str) -> None:
        engine = self._engine_ref() if self._engine_ref else None
        if not engine or not hasattr(engine, 'set_entity_role'):
            return
        next_role = 'enemy' if current_role == 'boss' else 'boss'
        try:
            engine.set_entity_role(int(uuid), next_role)
        except Exception:
            pass
        self._refresh_live(force=True)

    def _handle_next_phase(self) -> None:
        try:
            self._on_next()
        except Exception:
            pass
        self._refresh_live(force=True)

    def _handle_reset(self) -> None:
        try:
            self._on_reset()
        except Exception:
            pass
        self._refresh_live(force=True)

    def _on_drag_start(self, event) -> None:
        if self._win is None:
            return
        self._drag_ox = event.x_root - self._win.winfo_x()
        self._drag_oy = event.y_root - self._win.winfo_y()

    def _on_drag_move(self, event) -> None:
        if self._win is None:
            return
        self._win.geometry(f'+{event.x_root - self._drag_ox}+{event.y_root - self._drag_oy}')