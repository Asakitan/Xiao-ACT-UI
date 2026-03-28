"""
SAO Entity Mode — AutoKey 配置面板 (纯 tkinter Toplevel).

提供:
  • Profile CRUD (新建 / 复制 / 删除 / 激活 / 导入 / 导出)
  • 全局 enabled 开关
  • Actions 列表编辑 (添加 / 删除 / 上下移)
  • 每个 Action 的全部字段
  • 可选 Conditions 编辑

使用方法:
    panel = AutoKeyPanel(root, load_fn, save_fn, engine_ref, on_toggle)
    panel.show()   # toggle visibility
"""
from __future__ import annotations

import tkinter as tk
from tkinter import ttk, messagebox, filedialog
import uuid, copy, json, os
from typing import Callable, Optional, Any

# ── 颜色令牌 (与 sao_gui ModernColors 一致) ──
_C = type('C', (), {
    'BG': '#1C1C1E', 'CARD': '#2C2C2E', 'INPUT': '#1C1C1E',
    'ACCENT': '#0A84FF', 'GREEN': '#30D158', 'RED': '#FF453A',
    'ORANGE': '#FF9F0A', 'GOLD': '#FFD700',
    'TEXT': '#F5F5F7', 'TEXT2': '#98989D', 'DIM': '#636366',
    'BORDER': '#38383A', 'BTN': '#0A84FF', 'BTN2': '#48484A',
    'HOVER': '#3A3A3C',
})()

# ── 支持的按键列表 ──
_VK_KEYS = (
    ['SPACE', 'TAB', 'ENTER', 'ESC', 'SHIFT', 'CTRL', 'ALT']
    + [str(i) for i in range(10)]
    + [chr(c) for c in range(65, 91)]
    + [f'F{i}' for i in range(1, 13)]
)


def _make_id(prefix: str = 'ak') -> str:
    return f'{prefix}_{uuid.uuid4().hex[:12]}'


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


class AutoKeyPanel:
    """AutoKey 配置面板 — tkinter Toplevel."""

    def __init__(
        self,
        master: tk.Tk,
        load_fn: Callable[[], dict],
        save_fn: Callable[[dict], Any],
        engine_ref: Callable[[], Any],
        on_toggle: Callable[[bool], None],
        author_fn: Callable[[], dict] | None = None,
    ):
        self._master = master
        self._load = load_fn
        self._save = save_fn
        self._engine_ref = engine_ref
        self._on_toggle = on_toggle
        self._author_fn = author_fn or (lambda: {})
        self._win: Optional[tk.Toplevel] = None
        self._cfg: dict = {}
        self._dirty = False

    # ── public ──

    def show(self):
        if self._win and self._win.winfo_exists():
            self._win.lift()
            return
        self._cfg = self._load()
        self._build()

    def hide(self):
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
        w.title('AutoKey 配置')
        w.configure(bg=_C.BG)
        w.geometry('540x620')
        w.resizable(True, True)
        w.transient(self._master)
        try:
            w.attributes('-topmost', True)
        except Exception:
            pass
        w.protocol('WM_DELETE_WINDOW', self.hide)

        # ── toolbar ──
        tb = tk.Frame(w, bg=_C.CARD, height=36)
        tb.pack(fill='x', padx=4, pady=(4, 0))
        self._chk_enabled_var = tk.BooleanVar(value=self._cfg.get('enabled', False))
        chk = tk.Checkbutton(tb, text='启用 AutoKey', variable=self._chk_enabled_var,
                             bg=_C.CARD, fg=_C.TEXT, selectcolor=_C.BG,
                             activebackground=_C.CARD, activeforeground=_C.TEXT,
                             command=self._on_enabled_toggle)
        chk.pack(side='left', padx=6)

        for txt, cmd in [('新建', self._new_profile), ('复制', self._copy_profile),
                         ('删除', self._del_profile), ('导入', self._import_profile),
                         ('导出', self._export_profile)]:
            b = tk.Button(tb, text=txt, command=cmd,
                          bg=_C.BTN2, fg=_C.TEXT, relief='flat', padx=6, pady=1,
                          activebackground=_C.HOVER, activeforeground=_C.TEXT)
            b.pack(side='left', padx=2)

        # ── profile selector ──
        pf = tk.Frame(w, bg=_C.BG)
        pf.pack(fill='x', padx=4, pady=4)
        tk.Label(pf, text='配置:', bg=_C.BG, fg=_C.TEXT2).pack(side='left')
        self._prof_var = tk.StringVar()
        self._prof_cb = ttk.Combobox(pf, textvariable=self._prof_var, state='readonly', width=40)
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
        # mouse wheel
        def _on_mw(e):
            canvas.yview_scroll(int(-1 * (e.delta / 120)), 'units')
        canvas.bind_all('<MouseWheel>', _on_mw, add='+')
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
            from auto_key_engine import make_default_profile
            prof = make_default_profile()
        except Exception:
            prof = {'id': _make_id('profile'), 'profile_name': '新配置', 'actions': [],
                    'engine': {'tick_ms': 50, 'require_foreground': True, 'pause_on_death': True}}
        self._cfg.setdefault('profiles', []).append(prof)
        self._cfg['active_profile_id'] = prof['id']
        self._refresh_profile_list()
        self._populate_profile()

    def _copy_profile(self):
        src = self._active_profile()
        if not src:
            return
        cp = copy.deepcopy(src)
        cp['id'] = _make_id('profile')
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
                                          title='导入 AutoKey 配置')
        if not path:
            return
        try:
            with open(path, 'r', encoding='utf-8') as f:
                raw = json.load(f)
            if isinstance(raw, dict):
                raw.setdefault('id', _make_id('profile'))
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
                                            initialfile=f"autokey_{src.get('profile_name', 'profile')}.json",
                                            title='导出 AutoKey 配置')
        if not path:
            return
        try:
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(src, f, ensure_ascii=False, indent=2)
        except Exception as e:
            messagebox.showerror('导出失败', str(e), parent=self._win)

    # ── populate ──

    def _populate_profile(self):
        for w in self._body.winfo_children():
            w.destroy()
        prof = self._active_profile()
        if not prof:
            tk.Label(self._body, text='暂无配置，请新建', bg=_C.BG, fg=_C.TEXT2).pack(pady=20)
            return
        self._build_profile_header(prof)
        self._build_engine_settings(prof)
        self._build_actions_list(prof)

    def _build_profile_header(self, prof: dict):
        frm = tk.LabelFrame(self._body, text='配置信息', bg=_C.CARD, fg=_C.TEXT2,
                            labelanchor='nw', padx=8, pady=4)
        frm.pack(fill='x', padx=4, pady=4)
        # name
        r = tk.Frame(frm, bg=_C.CARD)
        r.pack(fill='x', pady=2)
        tk.Label(r, text='名称:', bg=_C.CARD, fg=_C.TEXT2, width=10, anchor='e').pack(side='left')
        self._prof_name_var = tk.StringVar(value=prof.get('profile_name', ''))
        tk.Entry(r, textvariable=self._prof_name_var, bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat').pack(side='left', fill='x', expand=True, padx=4)
        # description
        r2 = tk.Frame(frm, bg=_C.CARD)
        r2.pack(fill='x', pady=2)
        tk.Label(r2, text='描述:', bg=_C.CARD, fg=_C.TEXT2, width=10, anchor='e').pack(side='left')
        self._prof_desc_var = tk.StringVar(value=prof.get('description', ''))
        tk.Entry(r2, textvariable=self._prof_desc_var, bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat').pack(side='left', fill='x', expand=True, padx=4)

    def _build_engine_settings(self, prof: dict):
        eng = prof.get('engine', {})
        frm = tk.LabelFrame(self._body, text='引擎设置', bg=_C.CARD, fg=_C.TEXT2,
                            labelanchor='nw', padx=8, pady=4)
        frm.pack(fill='x', padx=4, pady=4)
        # tick_ms
        r = tk.Frame(frm, bg=_C.CARD)
        r.pack(fill='x', pady=2)
        tk.Label(r, text='Tick (ms):', bg=_C.CARD, fg=_C.TEXT2, width=14, anchor='e').pack(side='left')
        self._tick_var = tk.IntVar(value=eng.get('tick_ms', 50))
        tk.Spinbox(r, from_=10, to=1000, textvariable=self._tick_var, width=8,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=4)
        # require_foreground
        self._fg_var = tk.BooleanVar(value=eng.get('require_foreground', True))
        tk.Checkbutton(frm, text='仅前台时触发', variable=self._fg_var,
                       bg=_C.CARD, fg=_C.TEXT, selectcolor=_C.BG,
                       activebackground=_C.CARD, activeforeground=_C.TEXT).pack(anchor='w', pady=1)
        # pause_on_death
        self._death_var = tk.BooleanVar(value=eng.get('pause_on_death', True))
        tk.Checkbutton(frm, text='死亡时暂停', variable=self._death_var,
                       bg=_C.CARD, fg=_C.TEXT, selectcolor=_C.BG,
                       activebackground=_C.CARD, activeforeground=_C.TEXT).pack(anchor='w', pady=1)

    # ── actions list ──

    def _build_actions_list(self, prof: dict):
        actions = prof.get('actions', [])
        hdr = tk.Frame(self._body, bg=_C.BG)
        hdr.pack(fill='x', padx=4, pady=(8, 2))
        tk.Label(hdr, text=f'动作列表 ({len(actions)})', bg=_C.BG, fg=_C.TEXT).pack(side='left')
        tk.Button(hdr, text='+ 添加动作', command=lambda: self._add_action(prof),
                  bg=_C.GREEN, fg='#fff', relief='flat', padx=6,
                  activebackground='#28B04C', activeforeground='#fff').pack(side='right')

        self._action_widgets = []
        for i, act in enumerate(actions):
            self._build_action_card(prof, act, i)

    def _add_action(self, prof: dict):
        idx = len(prof.get('actions', [])) + 1
        try:
            from auto_key_engine import make_default_action
            act = make_default_action(idx)
        except Exception:
            act = {'id': _make_id('action'), 'label': f'Action {idx}', 'enabled': True,
                   'slot_index': min(idx, 9), 'key': str(min(idx, 9)),
                   'press_mode': 'tap', 'press_count': 1, 'press_interval_ms': 40,
                   'hold_ms': 80, 'ready_delay_ms': 0, 'min_rearm_ms': 800,
                   'post_delay_ms': 120, 'conditions': []}
        prof.setdefault('actions', []).append(act)
        self._populate_profile()

    def _remove_action(self, prof: dict, action_id: str):
        prof['actions'] = [a for a in prof.get('actions', []) if a.get('id') != action_id]
        self._populate_profile()

    def _build_action_card(self, prof: dict, act: dict, index: int):
        card = tk.LabelFrame(self._body, text=f"#{index+1}  {act.get('label', '')}",
                             bg=_C.CARD, fg=_C.ORANGE, labelanchor='nw', padx=6, pady=4)
        card.pack(fill='x', padx=4, pady=2)
        card._act_data = act  # keep ref for saving

        # row 1: label + enabled + delete
        r1 = tk.Frame(card, bg=_C.CARD)
        r1.pack(fill='x', pady=1)
        tk.Label(r1, text='标签:', bg=_C.CARD, fg=_C.TEXT2, width=8, anchor='e').pack(side='left')
        act['_label_var'] = tk.StringVar(value=act.get('label', ''))
        tk.Entry(r1, textvariable=act['_label_var'], bg=_C.INPUT, fg=_C.TEXT,
                 insertbackground=_C.TEXT, relief='flat', width=16).pack(side='left', padx=2)
        act['_enabled_var'] = tk.BooleanVar(value=act.get('enabled', True))
        tk.Checkbutton(r1, text='启用', variable=act['_enabled_var'],
                       bg=_C.CARD, fg=_C.TEXT, selectcolor=_C.BG,
                       activebackground=_C.CARD).pack(side='left', padx=6)
        tk.Button(r1, text='✕', command=lambda a=act: self._remove_action(prof, a.get('id', '')),
                  bg=_C.RED, fg='#fff', relief='flat', width=3,
                  activebackground='#CC3030').pack(side='right')

        # row 2: slot + key + mode
        r2 = tk.Frame(card, bg=_C.CARD)
        r2.pack(fill='x', pady=1)
        tk.Label(r2, text='技能槽:', bg=_C.CARD, fg=_C.TEXT2, width=8, anchor='e').pack(side='left')
        act['_slot_var'] = tk.IntVar(value=act.get('slot_index', 1))
        tk.Spinbox(r2, from_=1, to=9, textvariable=act['_slot_var'], width=4,
                   bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                   insertbackground=_C.TEXT).pack(side='left', padx=2)
        tk.Label(r2, text='按键:', bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(10, 0))
        act['_key_var'] = tk.StringVar(value=act.get('key', '1'))
        kcb = ttk.Combobox(r2, textvariable=act['_key_var'], values=_VK_KEYS,
                           state='readonly', width=8)
        kcb.pack(side='left', padx=2)
        tk.Label(r2, text='模式:', bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(10, 0))
        act['_mode_var'] = tk.StringVar(value=act.get('press_mode', 'tap'))
        ttk.Combobox(r2, textvariable=act['_mode_var'], values=['tap', 'hold'],
                     state='readonly', width=6).pack(side='left', padx=2)

        # row 3: timing params
        r3 = tk.Frame(card, bg=_C.CARD)
        r3.pack(fill='x', pady=1)
        timings = [
            ('次数:', 'press_count', 1, 1, 20, 4),
            ('间隔ms:', 'press_interval_ms', 40, 0, 10000, 6),
            ('按住ms:', 'hold_ms', 80, 0, 10000, 6),
        ]
        for label, key, default, lo, hi, w in timings:
            tk.Label(r3, text=label, bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(4, 0))
            var = tk.IntVar(value=act.get(key, default))
            act[f'_{key}_var'] = var
            tk.Spinbox(r3, from_=lo, to=hi, textvariable=var, width=w,
                       bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                       insertbackground=_C.TEXT).pack(side='left', padx=1)

        # row 4: delay params
        r4 = tk.Frame(card, bg=_C.CARD)
        r4.pack(fill='x', pady=1)
        delays = [
            ('就绪延迟ms:', 'ready_delay_ms', 0, 0, 60000, 6),
            ('冷却ms:', 'min_rearm_ms', 800, 0, 120000, 6),
            ('后延ms:', 'post_delay_ms', 120, 0, 120000, 6),
        ]
        for label, key, default, lo, hi, w in delays:
            tk.Label(r4, text=label, bg=_C.CARD, fg=_C.TEXT2).pack(side='left', padx=(4, 0))
            var = tk.IntVar(value=act.get(key, default))
            act[f'_{key}_var'] = var
            tk.Spinbox(r4, from_=lo, to=hi, textvariable=var, width=w,
                       bg=_C.INPUT, fg=_C.TEXT, buttonbackground=_C.BTN2,
                       insertbackground=_C.TEXT).pack(side='left', padx=1)

        # conditions (collapsed by default)
        conds = act.get('conditions', [])
        if conds:
            cf = tk.LabelFrame(card, text=f'条件 ({len(conds)})', bg=_C.BG, fg=_C.DIM,
                               padx=4, pady=2)
            cf.pack(fill='x', pady=2)
            for ci, cond in enumerate(conds):
                _t = cond.get('type', '?')
                _v = cond.get('value', '')
                tk.Label(cf, text=f'  {ci+1}. {_t} = {_v}', bg=_C.BG, fg=_C.TEXT2,
                         anchor='w').pack(fill='x')

        self._action_widgets.append(card)

    # ── save / reload ──

    def _collect_profile(self) -> Optional[dict]:
        prof = self._active_profile()
        if not prof:
            return None
        prof['profile_name'] = self._prof_name_var.get()
        prof['description'] = self._prof_desc_var.get()
        eng = prof.setdefault('engine', {})
        eng['tick_ms'] = _clamp(self._tick_var.get(), 10, 1000)
        eng['require_foreground'] = self._fg_var.get()
        eng['pause_on_death'] = self._death_var.get()
        # actions
        for act in prof.get('actions', []):
            for attr, key in [('_label_var', 'label'), ('_enabled_var', 'enabled'),
                              ('_slot_var', 'slot_index'), ('_key_var', 'key'),
                              ('_mode_var', 'press_mode')]:
                var = act.get(attr)
                if var is not None:
                    act[key] = var.get()
            for key in ['press_count', 'press_interval_ms', 'hold_ms',
                        'ready_delay_ms', 'min_rearm_ms', 'post_delay_ms']:
                var = act.get(f'_{key}_var')
                if var is not None:
                    act[key] = var.get()
            # clean up tk vars
            for k in list(act.keys()):
                if k.startswith('_') and k.endswith('_var'):
                    del act[k]
        return prof

    def _save_current(self):
        prof = self._collect_profile()
        if not prof:
            return
        # upsert
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
        self._dirty = False
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
