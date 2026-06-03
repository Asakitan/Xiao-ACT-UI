# -*- coding: utf-8 -*-
"""Entity-mode ACT plugin manager panel.

This panel mirrors the WebView plugin manager surface while calling the same
``act_platform.runtime`` helpers as the web API.  It is intentionally small and
read-only except for enable/disable/reload actions.
"""

from __future__ import annotations

import tkinter as tk
from typing import Any, Dict, Iterable, List, Mapping, Optional

from act_platform.runtime import (
    act_plugin_disable,
    act_plugin_enable,
    act_plugin_reload,
    act_plugin_status,
)
from gui_modules.sao_panel_ui import (
    _SAO_PANEL_ACCENT,
    _SAO_PANEL_BG,
    _SAO_PANEL_BODY_BG,
    _SAO_PANEL_BORDER,
    _SAO_PANEL_GOLD,
    _SAO_PANEL_HEADER_BG,
    _SAO_PANEL_HEADER_FG,
    _SAO_PANEL_LABEL_FG,
    _SAO_PANEL_SEP,
    _SAO_PANEL_VALUE_FG,
    _apply_window_icon,
    _bind_panel_drag,
    _make_panel_close_button,
    _sao_panel_body,
    _sao_panel_header,
    _sao_pill,
)


class PluginManagerPanel:
    """SAO-styled Toplevel for Python ACT plugin management."""

    def __init__(self, root: tk.Misc, owner: Any):
        self.root = root
        self.owner = owner
        self._win: Optional[tk.Toplevel] = None
        self._list: Optional[tk.Frame] = None
        self._summary_var = tk.StringVar(value="0 / 0 ACTIVE")
        self._status_var = tk.StringVar(value="Ready")
        self._last_status: Dict[str, Any] = {}

    def show(self) -> None:
        if self._win is None or not self._exists():
            self._build()
        if self._win is None:
            return
        try:
            self._win.deiconify()
            self._win.lift()
            self._win.attributes('-topmost', True)
            self._win.after(220, lambda: self._win and self._win.attributes('-topmost', False))
        except Exception:
            pass
        self.refresh()

    def hide(self) -> None:
        if self._win is None:
            return
        try:
            self._win.withdraw()
        except Exception:
            pass

    def destroy(self) -> None:
        if self._win is not None:
            try:
                self._win.destroy()
            except Exception:
                pass
        self._win = None
        self._list = None

    def is_visible(self) -> bool:
        return bool(self._win is not None and self._exists() and self._win.state() != 'withdrawn')

    def refresh(self) -> Dict[str, Any]:
        try:
            status = act_plugin_status(self.owner)
        except Exception as exc:
            status = {"ok": False, "message": str(exc), "plugins": [], "plugin_count": 0, "active_count": 0}
        self._last_status = dict(status or {})
        self._render_status(self._last_status)
        return self._last_status

    def _exists(self) -> bool:
        try:
            return bool(self._win and self._win.winfo_exists())
        except Exception:
            return False

    def _build(self) -> None:
        win = tk.Toplevel(self.root)
        self._win = win
        win.title('SAO ACT Plugin Manager')
        win.geometry('760x540+160+120')
        win.minsize(620, 420)
        win.configure(bg=_SAO_PANEL_BG)
        try:
            win.overrideredirect(False)
            win.attributes('-alpha', 0.97)
        except Exception:
            pass
        try:
            _apply_window_icon(win)
        except Exception:
            pass
        header = _sao_panel_header(win, 'ACT PLUGIN MANAGER', on_close=self.hide)
        header.pack(fill='x')
        _bind_panel_drag(win, header)

        body = _sao_panel_body(win)
        body.pack(fill='both', expand=True, padx=1, pady=(0, 1))

        toolbar = tk.Frame(body, bg=_SAO_PANEL_BODY_BG)
        toolbar.pack(fill='x', padx=12, pady=(10, 8))
        _sao_pill(toolbar, 'PYTHON SDK').pack(side='left')
        tk.Label(
            toolbar,
            textvariable=self._summary_var,
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_GOLD,
            font=('Segoe UI', 10, 'bold'),
        ).pack(side='left', padx=(12, 0))
        for label, cmd in (
            ('刷新 Refresh', self.refresh),
            ('全部重载 Reload', self._reload_all),
            ('关闭 Close', self.hide),
        ):
            tk.Button(
                toolbar,
                text=label,
                command=cmd,
                bg=_SAO_PANEL_HEADER_BG,
                fg=_SAO_PANEL_HEADER_FG,
                activebackground=_SAO_PANEL_ACCENT,
                activeforeground='white',
                relief='flat',
                bd=0,
                padx=10,
                pady=4,
            ).pack(side='right', padx=(6, 0))

        status = tk.Label(
            body,
            textvariable=self._status_var,
            anchor='w',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            font=('Segoe UI', 9),
        )
        status.pack(fill='x', padx=12, pady=(0, 6))

        canvas = tk.Canvas(body, bg=_SAO_PANEL_BODY_BG, highlightthickness=0, bd=0)
        scroll = tk.Scrollbar(body, orient='vertical', command=canvas.yview)
        self._list = tk.Frame(canvas, bg=_SAO_PANEL_BODY_BG)
        self._list.bind('<Configure>', lambda _e: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=self._list, anchor='nw')
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True, padx=(12, 0), pady=(0, 12))
        scroll.pack(side='right', fill='y', padx=(0, 12), pady=(0, 12))
        win.protocol('WM_DELETE_WINDOW', self.hide)

    def _render_status(self, status: Mapping[str, Any]) -> None:
        total = int(status.get('plugin_count', 0) or len(status.get('plugins') or []))
        active = int(status.get('active_count', 0) or 0)
        self._summary_var.set(f'{active} / {total} ACTIVE')
        message = status.get('message') or ('OK' if status.get('ok', True) else 'Plugin manager unavailable')
        self._status_var.set(str(message))
        if self._list is None:
            return
        for child in list(self._list.winfo_children()):
            child.destroy()
        plugins = list(status.get('plugins') or [])
        if not plugins:
            self._render_empty()
            return
        for plugin in plugins:
            self._render_plugin(plugin)

    def _render_empty(self) -> None:
        if self._list is None:
            return
        box = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=_SAO_PANEL_BORDER)
        box.pack(fill='x', pady=8, padx=4)
        tk.Label(
            box,
            text='未发现 ACT 插件\n将 plugin.json 与 plugin.py 放入 plugins/<plugin_id>/ 后刷新。',
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            justify='center',
            font=('Segoe UI', 10),
            pady=28,
        ).pack(fill='x')

    def _render_plugin(self, plugin: Mapping[str, Any]) -> None:
        if self._list is None:
            return
        plugin_id = str(plugin.get('id') or '')
        enabled = bool(plugin.get('enabled'))
        active = bool(plugin.get('active'))
        border = _SAO_PANEL_GOLD if active else (_SAO_PANEL_BORDER if enabled else _SAO_PANEL_SEP)
        card = tk.Frame(self._list, bg=_SAO_PANEL_BODY_BG, highlightthickness=1, highlightbackground=border)
        card.pack(fill='x', pady=6, padx=4)

        top = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        top.pack(fill='x', padx=10, pady=(8, 2))
        tk.Label(
            top,
            text=str(plugin.get('name') or plugin_id),
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_VALUE_FG,
            anchor='w',
            font=('Segoe UI', 11, 'bold'),
        ).pack(side='left', fill='x', expand=True)
        state = 'ACTIVE' if active else ('ENABLED' if enabled else 'DISABLED')
        _sao_pill(top, state).pack(side='right')

        meta = tk.Label(
            card,
            text=self._format_meta(plugin),
            bg=_SAO_PANEL_BODY_BG,
            fg=_SAO_PANEL_LABEL_FG,
            anchor='w',
            justify='left',
            font=('Consolas', 9),
        )
        meta.pack(fill='x', padx=10, pady=(2, 4))

        error = str(plugin.get('last_error') or '').strip()
        if error:
            tk.Label(
                card,
                text=error,
                bg='#33161f',
                fg='#ffd8de',
                anchor='w',
                justify='left',
                wraplength=690,
                font=('Segoe UI', 9),
                padx=8,
                pady=5,
            ).pack(fill='x', padx=10, pady=(0, 6))

        logs = list(plugin.get('logs') or [])[-4:]
        if logs:
            tk.Label(
                card,
                text='\n'.join(str(x) for x in logs),
                bg='#07111c',
                fg='#bfe6ff',
                anchor='w',
                justify='left',
                wraplength=690,
                font=('Consolas', 8),
                padx=8,
                pady=5,
            ).pack(fill='x', padx=10, pady=(0, 6))

        actions = tk.Frame(card, bg=_SAO_PANEL_BODY_BG)
        actions.pack(fill='x', padx=10, pady=(0, 9))
        self._action_button(actions, '启用 Enable', lambda pid=plugin_id: self._enable(pid), enabled=not enabled)
        self._action_button(actions, '禁用 Disable', lambda pid=plugin_id: self._disable(pid), enabled=enabled)
        self._action_button(actions, '重载 Reload', lambda pid=plugin_id: self._reload(pid), enabled=enabled)

    def _format_meta(self, plugin: Mapping[str, Any]) -> str:
        games = ','.join(str(x) for x in (plugin.get('game_ids') or [])) or '-'
        perms = ','.join(str(x) for x in (plugin.get('permissions') or [])) or '-'
        caps = ','.join(str(x) for x in (plugin.get('capability_ids') or [])) or '-'
        return (
            f"id={plugin.get('id') or '-'}  v{plugin.get('version') or '-'}  "
            f"subs={plugin.get('subscription_count') or 0}  "
            f"fail={plugin.get('failures') or 0}/{plugin.get('event_failures') or 0}\n"
            f"games={games}  perms={perms}  caps={caps}\n"
            f"entry={plugin.get('entry') or '-'}"
        )

    def _action_button(self, parent: tk.Frame, text: str, command: Any, *, enabled: bool = True) -> None:
        btn = tk.Button(
            parent,
            text=text,
            command=command,
            state=('normal' if enabled else 'disabled'),
            bg=_SAO_PANEL_HEADER_BG,
            fg=_SAO_PANEL_HEADER_FG,
            disabledforeground='#6e8190',
            activebackground=_SAO_PANEL_ACCENT,
            activeforeground='white',
            relief='flat',
            bd=0,
            padx=9,
            pady=3,
        )
        btn.pack(side='left', padx=(0, 7))

    def _reload_all(self) -> None:
        result = act_plugin_reload(self.owner)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Reload failed'))
        self.refresh()

    def _reload(self, plugin_id: str) -> None:
        result = act_plugin_reload(self.owner, plugin_id)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Reload failed'))
        self.refresh()

    def _enable(self, plugin_id: str) -> None:
        result = act_plugin_enable(self.owner, plugin_id)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Enable failed'))
        self.refresh()

    def _disable(self, plugin_id: str) -> None:
        result = act_plugin_disable(self.owner, plugin_id)
        if isinstance(result, Mapping) and result.get('ok') is False:
            self._status_var.set(str(result.get('message') or 'Disable failed'))
        self.refresh()


__all__ = ["PluginManagerPanel"]
