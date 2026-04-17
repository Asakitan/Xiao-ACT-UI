"""
SAO Entity Mode — BossRaid 配置面板 (纯 tkinter Toplevel).

提供:
  • Profile CRUD (新建 / 复制 / 删除 / 激活 / 导入 / 导出)
  • 全局 enabled 开关 + 运行控制 (Start / Stop / NextPhase / Reset)
  • Phase 列表编辑 (不同 trigger 类型)
  • Timeline (Alert) 列表编辑
  • 运行时状态显示

使用方法:
    panel = BossRaidPanel(root, load_fn, save_fn, engine_ref, on_toggle, on_start, on_next, on_reset)
    panel.show()
"""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import uuid, copy, json
from typing import Callable, Optional, Any
from window_effects import apply_native_chrome

# ── 颜色 ──
_C = type('C', (), {
    'BG': '#1C1C1E', 'CARD': '#2C2C2E', 'INPUT': '#1C1C1E',
    'ACCENT': '#0A84FF', 'GREEN': '#30D158', 'RED': '#FF453A',
    'ORANGE': '#FF9F0A', 'GOLD': '#FFD700', 'PURPLE': '#BF5AF2',
    'TEXT': '#F5F5F7', 'TEXT2': '#98989D', 'DIM': '#636366',
    'BORDER': '#38383A', 'BTN': '#0A84FF', 'BTN2': '#48484A',
    'HOVER': '#3A3A3C',
})()

_TRIGGER_TYPES = [
    'manual', 'time', 'dps_total', 'hp_pct', 'breaking',
    'buff_event', 'shield_broken', 'overdrive', 'extinction_pct', 'breaking_stage',
]
_ALERT_TYPES = ['sound', 'visual', 'both']
_COND_TYPES = ['always', 'hp_pct', 'shield_active', 'breaking']
_COMPARATORS = ['>=', '<=', '>', '<', '==']


def _make_id(prefix: str = 'br') -> str:
    return f'{prefix}_{uuid.uuid4().hex[:12]}'


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


class BossRaidPanel:
    """BossRaid 配置面板 — tkinter Toplevel."""

    def __init__(
        self,
        master: tk.Tk,
        load_fn: Callable[[], dict],
        save_fn: Callable[[dict], Any],
        engine_ref: Callable[[], Any],
        on_toggle: Callable[[bool], None],
        on_start: Callable[[], None],
        on_next: Callable[[], None],
        on_reset: Callable[[], None] | None = None,
    ):
        self._master = master
        self._load = load_fn
        self._save = save_fn
        self._engine_ref = engine_ref
        self._on_toggle = on_toggle
        self._on_start = on_start
        self._on_next = on_next
        self._on_reset = on_reset or (lambda: None)
        self._win: Optional[tk.Toplevel] = None
        self._cfg: dict = {}
        self._status_lbl: Optional[tk.Label] = None
        self._status_after_id = None

    # ── public ──

    def show(self):
        if self._win and self._win.winfo_exists():
            self._win.lift()
            return
        self._cfg = self._load()
        self._build()
        self._tick_status()

    def hide(self):
        if self._status_after_id and self._win:
            try:
                self._win.after_cancel(self._status_after_id)
            except Exception:
                pass
            self._status_after_id = None
        if self._win and self._win.winfo_exists():
            self._win.destroy()
        self._win = None

    def toggle(self):
        if self._win and self._win.winfo_exists():
            self.hide()
        else:
            self.show()

    def destroy(self):
        self.hide()

    # ── build ──

    def _build(self):
        w = tk.Toplevel(self._master)
        self._win = w
        w.title('BossRaid 配置')
        w.configure(bg=_C.BG)
        w.geometry('560x650')
        w.resizable(True, True)
        apply_native_chrome(w)
        try:
            w.attributes('-topmost', True)
        except Exception:
            pass
        w.protocol('WM_DELETE_WINDOW', self.hide)

        # ── toolbar row 1: enabled + runtime buttons ──
        tb = tk.Frame(w, bg=_C.CARD, height=36)
        tb.pack(fill='x', padx=4, pady=(4, 0))
        self._chk_enabled_var = tk.BooleanVar(value=self._cfg.get('enabled', False))
        tk.Checkbutton(tb, text='启用 BossRaid', variable=self._chk_enabled_var,
                       bg=_C.CARD, fg=_C.TEXT, selectcolor=_C.BG,
                       activebackground=_C.CARD, activeforeground=_C.TEXT,
                       command=self._on_enabled_toggle).pack(side='left', padx=6)
        # runtime
        tk.Button(tb, text='▶ 开始', command=self._on_start, bg=_C.GREEN, fg='#fff',
                  relief='flat', padx=6, activebackground='#28B04C').pack(side='left', padx=2)
        tk.Button(tb, text='⏭ 下一阶段', command=self._on_next, bg=_C.ACCENT, fg='#fff',
                  relief='flat', padx=6, activebackground='#0070E0').pack(side='left', padx=2)
        tk.Button(tb, text='⏹ 重置', command=self._on_reset, bg=_C.RED, fg='#fff',
                  relief='flat', padx=6, activebackground='#CC3030').pack(side='left', padx=2)

        # status line
        self._status_lbl = tk.Label(w, text='状态: idle', bg=_C.BG, fg=_C.GOLD, anchor='w')
        self._status_lbl.pack(fill='x', padx=8, pady=2)

        # ── toolbar row 2: profile CRUD ──
        tb2 = tk.Frame(w, bg=_C.CARD, height=36)
        tb2.pack(fill='x', padx=4, pady=(2, 0))
        for txt, cmd in [('新建', self._new_profile), ('复制', self._copy_profile),
                         ('删除', self._del_profile), ('导入', self._import_profile),
                         ('导出', self._export_profile)]:
            tk.Button(tb2, text=txt, command=cmd, bg=_C.BTN2, fg=_C.TEXT, relief='flat',
                      padx=6, pady=1, activebackground=_C.HOVER,
                      activeforeground=_C.TEXT).pack(side='left', padx=2)

        # ── profile selector ──
        pf = tk.Frame(w, bg=_C.BG)
        pf.pack(fill='x', padx=4, pady=4)
        tk.Label(pf, text='配置:', bg=_C.BG, fg=_C.TEXT2).pack(side='left')
        self._prof_var = tk.StringVar()
        self._prof_cb = ttk.Combobox(pf, textvariable=self._prof_var, state='readonly', width=42)
        self._prof_cb.pack(side='left', padx=4, fill='x', expand=True)
        self._prof_cb.bind('<<ComboboxSelected>>', self._on_profile_selected)
        self._refresh_profile_list()

        # ── scrollable body ──
        outer = tk.Frame(w, bg=_C.BG)
        outer.pack(fill='both', expand=True, padx=4, pady=2)
        canvas = tk.Canvas(outer, bg=_C.BG, highlightthickness=0)
        vsb = tk.Scrollbar(outer, orient='vertical', command=canvas.yview)
        self._body = tk.Frame(canvas, bg=_C.BG)
        self._body.bind('<Configure>', lambda e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=self._body, anchor='nw')
        canvas.configure(yscrollcommand=vsb.set)
        canvas.pack(side='left', fill='both', expand=True)
        vsb.pack(side='right', fill='y')
        canvas.bind_all('<MouseWheel>', lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units'), add='+')
        self._canvas = canvas

        # ── bottom bar ──
        bb = tk.Frame(w, bg=_C.CARD, height=36)
        bb.pack(fill='x', padx=4, pady=(0, 4))
        tk.Button(bb, text='保存', command=self._save_current, bg=_C.ACCENT, fg='#fff',
                  relief='flat', padx=12, activebackground='#0070E0',
                  activeforeground='#fff').pack(side='right', padx=4, pady=4)
        tk.Button(bb, text='还原', command=self._reload_current, bg=_C.BTN2, fg=_C.TEXT,
                  relief='flat', padx=12, activebackground=_C.HOVER,
                  activeforeground=_C.TEXT).pack(side='right', padx=4, pady=4)

        self._populate_profile()

    # ── status tick ──

    def _tick_status(self):
        if not (self._win and self._win.winfo_exists()):
            return
        eng = self._engine_ref()
        if eng:
            try:
                st = eng.get_status()
                state = st.get('state', 'idle')
                phase = st.get('phase_name', '-')
                elapsed = st.get('elapsed_s', 0)
                enrage = st.get('enrage_remaining_s', 0)
                txt = f"状态: {state}  |  阶段: {phase}  |  时间: {elapsed:.0f}s"
                if enrage > 0:
                    txt += f"  |  狂暴: {enrage:.0f}s"
                if self._status_lbl:
                    self._status_lbl.config(text=txt)
            except Exception:
                pass
        self._status_after_id = self._win.after(500, self._tick_status)

    # ── profile list ──

    def _refresh_profile_list(self):
        profiles = self._cfg.get('profiles', [])
        names = [f"{p.get('profile_name', '?')}  [{p.get('id', '')[:8]}]" for p in profiles]
        self._prof_cb['values'] = names
        active_id = self._cfg.get('active_profile_id', '')
        for i, p in enumerate(profiles):
            if p.get('id') == active_id:
                self._prof_cb.current(i)
                return
        if names:
            self._prof_cb.current(0)

    def _active_profile(self) -> Optional[dict]:
        profiles = self._cfg.get('profiles', [])
        idx = self._prof_cb.current()
        if 0 <= idx < len(profiles):
            return profiles[idx]
        aid = self._cfg.get('active_profile_id', '')
        for p in profiles:
            if p.get('id') == aid:
                return p
        return profiles[0] if profiles else None

    def _on_profile_selected(self, _evt=None):
        profiles = self._cfg.get('profiles', [])
        idx = self._prof_cb.current()
        if 0 <= idx < len(profiles):
            self._cfg['active_profile_id'] = profiles[idx]['id']
        self._populate_profile()

    # ── profile CRUD ──

    def _new_profile(self):
        try:
            from boss_raid_engine import make_default_profile
            prof = make_default_profile()
        except Exception:
            prof = {'id': _make_id('boss'), 'profile_name': '新 Boss 配置',
                    'boss_total_hp': 0, 'enrage_time_s': 600, 'simple_mode': True,
                    'target_name_pattern': '', 'phases': [], 'source': 'local'}
        self._cfg.setdefault('profiles', []).append(prof)
        self._cfg['active_profile_id'] = prof['id']
        self._refresh_profile_list()
        self._populate_profile()

    def _copy_profile(self):
        src = self._active_profile()
        if not src:
            return
        cp = copy.deepcopy(src)
        cp['id'] = _make_id('boss')
        cp['profile_name'] = src.get('profile_name', '') + ' (复制)'
        cp['source'] = 'local'
        self._cfg.setdefault('profiles', []).append(cp)
        self._cfg['active_profile_id'] = cp['id']
        self._refresh_profile_list()
        self._populate_profile()

    def _del_profile(self):
        src = self._active_profile()
        if not src:
            return
        if not messagebox.askyesno('删除', f"确认删除 '{src.get('profile_name', '')}'?", parent=self._win):
            return
        self._cfg['profiles'] = [p for p in self._cfg.get('profiles', []) if p.get('id') != src.get('id')]
        if self._cfg.get('active_profile_id') == src.get('id'):
            self._cfg['active_profile_id'] = ''
        self._refresh_profile_list()
        self._populate_profile()

    def _import_profile(self):
        path = filedialog.askopenfilename(parent=self._win, filetypes=[('JSON', '*.json')],
                                          title='导入 BossRaid 配置')
        if not path:
            return
        try:
            with open(path, 'r', encoding='utf-8') as f:
                raw = json.load(f)
            if isinstance(raw, dict):
                raw.setdefault('id', _make_id('boss'))
                raw['source'] = 'local'
                self._cfg.setdefault('profiles', []).append(raw)
                self._cfg['active_profile_id'] = raw['id']
                self._refresh_profile_list()
                self._populate_profile()
        except Exception as e:
            messagebox.showerror('导入失败', str(e), parent=self._win)

    def _export_profile(self):
        src = self._active_profile()
        if not src:
            return
        path = filedialog.asksaveasfilename(parent=self._win, filetypes=[('JSON', '*.json')],
                                            defaultextension='.json',
                                            initialfile=f"bossraid_{src.get('profile_name', 'profile')}.json",
                                            title='导出 BossRaid 配置')
        if not path:
            return
        try:
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(src, f, ensure_ascii=False, indent=2)
        except Exception as e:
            messagebox.showerror('导出失败', str(e), parent=self._win)

    # ── populate ──

    def _populate_profile(self):
        for ww in self._body.winfo_children():
            ww.destroy()
        prof = self._active_profile()
        if not prof:
            tk.Label(self._body, text='暂无配置，请新建', bg=_C.BG, fg=_C.TEXT2).pack(pady=20)
            return
        self._build_profile_header(prof)
        self._build_phases(prof)

    # ── profile header ──

    def _build_profile_header(self, prof: dict):
        frm = tk.LabelFrame(self._body, text='配置信息', bg=_C.CARD, fg=_C.TEXT2,
                            labelanchor='nw', padx=8, pady=4)
        frm.pack(fill='x', padx=4, pady=4)
        # name
        r = tk.Frame(frm, bg=_C.CARD)
        r.pack(fill='x', pady=2)
        tk.Label(r, text='名称:', bg=_C.CARD, fg=_C.TEXT2, width=12, anchor='e').pack(side='left')
        self._pn_var = tk.StringVar(value=prof.get('profile_name', ''))
        tk.Entry(r, textvariable=self._pn_var, bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat').pack(side='left', fill='x', expand=True, padx=4)
        # description
        r2 = tk.Frame(frm, bg=_C.CARD)
        r2.pack(fill='x', pady=2)
        tk.Label(r2, text='描述:', bg=_C.CARD, fg=_C.TEXT2, width=12, anchor='e').pack(side='left')
        self._pd_var = tk.StringVar(value=prof.get('description', ''))
        tk.Entry(r2, textvariable=self._pd_var, bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat').pack(side='left', fill='x', expand=True, padx=4)
        # numeric fields
        r3 = tk.Frame(frm, bg=_C.CARD)
        r3.pack(fill='x', pady=2)
        tk.Label(r3, text='Boss总HP:', bg=_C.CARD, fg=_C.TEXT2, width=12, anchor='e').pack(side='left')
        self._hp_var = tk.IntVar(value=prof.get('boss_total_hp', 0))
        tk.Spinbox(r3, from_=0, to=999999999, textvariable=self._hp_var, width=12,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=4)
        tk.Label(r3, text='狂暴(s):', bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(10, 0))
        self._enrage_var = tk.IntVar(value=prof.get('enrage_time_s', 600))
        tk.Spinbox(r3, from_=0, to=3600, textvariable=self._enrage_var, width=6,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=4)
        # simple mode + target
        r4 = tk.Frame(frm, bg=_C.CARD)
        r4.pack(fill='x', pady=2)
        self._simple_var = tk.BooleanVar(value=prof.get('simple_mode', True))
        tk.Checkbutton(r4, text='简易模式', variable=self._simple_var,
                       bg=_C.CARD, fg=_C.TEXT, selectcolor=_C.BG,
                       activebackground=_C.CARD).pack(side='left', padx=8)
        tk.Label(r4, text='目标名:', bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(10, 0))
        self._target_var = tk.StringVar(value=prof.get('target_name_pattern', ''))
        tk.Entry(r4, textvariable=self._target_var, bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat', width=20).pack(side='left', padx=4)

    # ── phases ──

    def _build_phases(self, prof: dict):
        phases = prof.get('phases', [])
        hdr = tk.Frame(self._body, bg=_C.BG)
        hdr.pack(fill='x', padx=4, pady=(8, 2))
        tk.Label(hdr, text=f'阶段 ({len(phases)})', bg=_C.BG, fg=_C.TEXT).pack(side='left')
        tk.Button(hdr, text='+ 添加阶段', command=lambda: self._add_phase(prof),
                  bg=_C.GREEN, fg='#fff', relief='flat', padx=6,
                  activebackground='#28B04C').pack(side='right')

        self._phase_widgets = []
        for i, phase in enumerate(phases):
            self._build_phase_card(prof, phase, i)

    def _add_phase(self, prof: dict):
        idx = len(prof.get('phases', [])) + 1
        try:
            from boss_raid_engine import make_default_phase
            phase = make_default_phase(idx)
        except Exception:
            phase = {'id': _make_id('phase'), 'name': f'P{idx}',
                     'trigger': {'type': 'manual', 'value': 0}, 'timelines': []}
        prof.setdefault('phases', []).append(phase)
        self._populate_profile()

    def _remove_phase(self, prof: dict, phase_id: str):
        prof['phases'] = [p for p in prof.get('phases', []) if p.get('id') != phase_id]
        self._populate_profile()

    def _build_phase_card(self, prof: dict, phase: dict, index: int):
        card = tk.LabelFrame(self._body, text=f"阶段 {index+1}: {phase.get('name', 'P?')}",
                             bg=_C.CARD, fg=_C.PURPLE, labelanchor='nw', padx=6, pady=4)
        card.pack(fill='x', padx=4, pady=2)
        card._phase_data = phase

        # row 1: name + trigger + delete
        r1 = tk.Frame(card, bg=_C.CARD)
        r1.pack(fill='x', pady=1)
        tk.Label(r1, text='名称:', bg=_C.CARD, fg=_C.TEXT2, width=6, anchor='e').pack(side='left')
        phase['_name_var'] = tk.StringVar(value=phase.get('name', ''))
        tk.Entry(r1, textvariable=phase['_name_var'], bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat', width=10).pack(side='left', padx=2)
        tk.Label(r1, text='触发:', bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(8, 0))
        trig = phase.get('trigger', {})
        phase['_trig_type_var'] = tk.StringVar(value=trig.get('type', 'manual'))
        ttk.Combobox(r1, textvariable=phase['_trig_type_var'], values=_TRIGGER_TYPES,
                     state='readonly', width=14).pack(side='left', padx=2)
        tk.Label(r1, text='值:', bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(4, 0))
        phase['_trig_val_var'] = tk.DoubleVar(value=float(trig.get('value', 0)))
        tk.Spinbox(r1, from_=0, to=999999, textvariable=phase['_trig_val_var'], width=8,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=2)
        tk.Button(r1, text='✕', command=lambda p=phase: self._remove_phase(prof, p.get('id', '')),
                  bg=_C.RED, fg='#fff', relief='flat', width=3,
                  activebackground='#CC3030').pack(side='right')

        # timelines
        self._build_timelines(prof, phase, card)
        self._phase_widgets.append(card)

    # ── timelines ──

    def _build_timelines(self, prof: dict, phase: dict, parent: tk.Widget):
        timelines = phase.get('timelines', [])
        hdr = tk.Frame(parent, bg=_C.CARD)
        hdr.pack(fill='x', pady=(4, 1))
        tk.Label(hdr, text=f'提醒 ({len(timelines)})', bg=_C.CARD, fg=_C.DIM).pack(side='left')
        tk.Button(hdr, text='+', command=lambda: self._add_timeline(prof, phase),
                  bg=_C.ACCENT, fg='#fff', relief='flat', width=3,
                  activebackground='#0070E0').pack(side='right')

        for ti, tl in enumerate(timelines):
            self._build_timeline_row(phase, tl, ti)

    def _add_timeline(self, prof: dict, phase: dict):
        try:
            from boss_raid_engine import make_default_timeline
            tl = make_default_timeline()
        except Exception:
            tl = {'id': _make_id('tl'), 'time_s': 30.0, 'label': 'Alert',
                  'alert_type': 'both', 'repeat_interval_s': 0, 'pre_warn_s': 0,
                  'duration_s': 0, 'condition': None}
        phase.setdefault('timelines', []).append(tl)
        self._populate_profile()

    def _remove_timeline(self, phase: dict, tl_id: str):
        phase['timelines'] = [t for t in phase.get('timelines', []) if t.get('id') != tl_id]
        self._populate_profile()

    def _build_timeline_row(self, phase: dict, tl: dict, index: int):
        row = tk.Frame(phase.get('_card_ref', self._body), bg=_C.BG)
        # find parent — use the last phase widget
        parent = self._phase_widgets[-1] if self._phase_widgets else self._body
        row = tk.Frame(parent, bg=_C.BG)
        row.pack(fill='x', pady=1, padx=4)

        tk.Label(row, text=f'{index+1}.', bg=_C.BG, fg=_C.DIM, width=3).pack(side='left')
        tl['_time_var'] = tk.DoubleVar(value=tl.get('time_s', 30))
        tk.Spinbox(row, from_=0, to=9999, textvariable=tl['_time_var'], width=5,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=1)
        tk.Label(row, text='s', bg=_C.BG, fg=_C.DIM).pack(side='left')

        tl['_label_var'] = tk.StringVar(value=tl.get('label', ''))
        tk.Entry(row, textvariable=tl['_label_var'], bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat', width=12).pack(side='left', padx=2)

        tl['_atype_var'] = tk.StringVar(value=tl.get('alert_type', 'both'))
        ttk.Combobox(row, textvariable=tl['_atype_var'], values=_ALERT_TYPES,
                     state='readonly', width=7).pack(side='left', padx=2)

        tl['_repeat_var'] = tk.DoubleVar(value=tl.get('repeat_interval_s', 0))
        tk.Label(row, text='重复:', bg=_C.BG, fg=_C.DIM).pack(side='left', padx=(4, 0))
        tk.Spinbox(row, from_=0, to=9999, textvariable=tl['_repeat_var'], width=4,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=1)

        tk.Button(row, text='✕', command=lambda t=tl: self._remove_timeline(phase, t.get('id', '')),
                  bg=_C.RED, fg='#fff', relief='flat', width=2,
                  activebackground='#CC3030').pack(side='right')

    # ── save / reload ──

    def _collect_profile(self) -> Optional[dict]:
        prof = self._active_profile()
        if not prof:
            return None
        prof['profile_name'] = self._pn_var.get()
        prof['description'] = self._pd_var.get()
        prof['boss_total_hp'] = max(0, self._hp_var.get())
        prof['enrage_time_s'] = max(0, self._enrage_var.get())
        prof['simple_mode'] = self._simple_var.get()
        prof['target_name_pattern'] = self._target_var.get()
        # phases
        for phase in prof.get('phases', []):
            nv = phase.get('_name_var')
            if nv:
                phase['name'] = nv.get()
            ttv = phase.get('_trig_type_var')
            tvv = phase.get('_trig_val_var')
            trig = phase.setdefault('trigger', {})
            if ttv:
                trig['type'] = ttv.get()
            if tvv:
                trig['value'] = tvv.get()
            # timelines
            for tl in phase.get('timelines', []):
                for attr, key in [('_time_var', 'time_s'), ('_label_var', 'label'),
                                  ('_atype_var', 'alert_type'), ('_repeat_var', 'repeat_interval_s')]:
                    var = tl.get(attr)
                    if var is not None:
                        tl[key] = var.get()
                # clean tk vars
                for k in list(tl.keys()):
                    if k.startswith('_') and k.endswith('_var'):
                        del tl[k]
            # clean phase tk vars
            for k in list(phase.keys()):
                if k.startswith('_') and k.endswith('_var'):
                    del phase[k]
        return prof

    def _save_current(self):
        prof = self._collect_profile()
        if not prof:
            return
        profiles = self._cfg.get('profiles', [])
        found = False
        for i, p in enumerate(profiles):
            if p.get('id') == prof.get('id'):
                profiles[i] = prof
                found = True
                break
        if not found:
            profiles.append(prof)
        self._cfg['profiles'] = profiles
        self._cfg['enabled'] = self._chk_enabled_var.get()
        self._save(self._cfg)
        self._refresh_profile_list()
        self._populate_profile()

    def _reload_current(self):
        self._cfg = self._load()
        self._refresh_profile_list()
        self._populate_profile()
        self._chk_enabled_var.set(self._cfg.get('enabled', False))

    def _on_enabled_toggle(self):
        enabled = self._chk_enabled_var.get()
        self._cfg['enabled'] = enabled
        self._save(self._cfg)
        self._on_toggle(enabled)
