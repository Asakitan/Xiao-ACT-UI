# -*- coding: utf-8 -*-
"""
SAOPlayerGUIMenuMixin — fourth mixin extracted from SAOPlayerGUI
(round 40 of the sao_gui split refactor). 17 methods, ~526 lines.

Holds the SAO PopUpMenu lifecycle + refresh + signature-caching logic:
  * ``_setup_sao_menu`` — constructs the SAOPopUpMenu with 7 categories.
  * ``_toggle_sao_menu`` — open/close handler (with motion blur + sounds).
  * ``_close_sao_menu_from_background`` / ``_clear_sao_menu_close_pending``
    — outside-click close path with fisheye z-order release.
  * ``_on_sao_menu_open`` / ``_on_sao_menu_close`` — open/close hooks
    that drive fisheye overlay + breath animation + entity-menu state
    persistence.
  * ``_refresh_menu_if_open`` / ``_refresh_menu_immediate`` —
    debounced + immediate refresh paths.
  * ``_apply_menu_refresh_if_open`` — main-thread continuation invoked
    by the debounce after().
  * ``_compute_menu_refresh_signature`` — 200 ms-cached signature for
    detecting whether the menu actually needs to rebuild.
  * ``_get_menu_children_cached`` — sig-cached menu children dict.
  * ``_cancel_pending_menu_refresh`` — cancel a pending debounce.
  * ``_build_menu_children`` — the 7-category dict builder (the
    biggest method in the cluster at ~139 lines).
  * ``_persist_entity_menu_state`` / ``_restore_entity_menu_state`` —
    remember which child menu was last active across menu close/open.
  * ``_dismiss_sao_menu_for_panel`` — close the SAO menu before
    showing a floating panel (avoid topmost overlay covering it).
  * ``_build_update_menu_label`` — formats the updater status string
    for the menu's "关于" → "检查更新" entry.

All 17 methods are SAOPlayerGUI instance methods today; this mixin
holds the definitions. SAOPlayerGUI's __init__ still owns the
relevant state (``self._sao_menu``, ``self._menu_icons``,
``self._menu_refresh_after_id``, signature caches, etc.) — the mixin
only contains method bodies that access ``self.X``.
"""

from __future__ import annotations

from collections.abc import Mapping
import math
import time
from typing import Any, Callable, Dict, List, Optional

from act_platform.runtime import (
    act_plugin_action,
    act_plugin_disable,
    act_plugin_enable,
    act_plugin_menu,
    act_plugin_menu_surfaces,
    act_plugin_pin,
    act_plugin_reload,
    act_plugin_script_menus,
    act_plugin_status,
    ensure_act_event_bus,
    ensure_act_plugin_manager,
)
from config import DEFAULT_HOTKEYS
from utils.sao_sound import play_sound
from sao_theme import SAOPopUpMenu


_CLOSE_BEFORE_COMMAND_DELAY_MS = 420


def _finite_float(value: Any, default: float = 0.0, *, lo: float | None = None, hi: float | None = None) -> float:
    try:
        number = float(value)
    except Exception:
        number = float(default)
    if not math.isfinite(number):
        number = float(default)
    if lo is not None:
        number = max(lo, number)
    if hi is not None:
        number = min(hi, number)
    return number


def _finite_int(value: Any, default: int = 0, *, lo: int | None = None, hi: int | None = None) -> int:
    return int(_finite_float(value, float(default), lo=lo, hi=hi))


def _mapping(value: Any) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _list_count(value: Any) -> int:
    return len(value) if isinstance(value, (list, tuple)) else 0


def _mapping_items(value: Any) -> list[Mapping[str, Any]]:
    if not isinstance(value, (list, tuple)):
        return []
    return [item for item in value if isinstance(item, Mapping)]


class SAOPlayerGUIMenuMixin:
    """Mixin providing the SAO PopUpMenu lifecycle + refresh logic.

    SAOPlayerGUI must initialise the following attributes in its
    ``__init__`` (this is the existing contract — the mixin does not
    seed them):

      * ``self._sao_menu`` (SAOPopUpMenu or None until first toggle)
      * ``self._menu_icons`` (list[dict] — set by _setup_sao_menu)
      * ``self._menu_refresh_after_id`` (int or None)
      * ``self._menu_refresh_force`` (bool)
      * ``self._menu_children_cache`` (dict or None)
      * ``self._menu_children_cache_sig`` (tuple or None)
      * ``self._last_menu_refresh_sig`` (tuple or None)
      * ``self._last_menu_refresh_sig_time`` (float)
      * ``self._sao_menu_close_pending`` (bool)
      * ``self._fisheye_close_suppress_until`` (float)

    Plus all the other ``self.X`` fields the methods read (which are
    initialised by SAOPlayerGUI proper).
    """

    def _persist_entity_menu_state(self, save_now: bool = False):
        settings = getattr(self, '_cfg_settings_ref', None)
        if not settings:
            return
        active_name = ''
        try:
            menu_bar = getattr(getattr(self, '_sao_menu', None), '_menu_bar', None)
            active_item = getattr(menu_bar, '_active_item', None)
            if isinstance(active_item, dict):
                active_name = str(active_item.get('name') or '').strip()
        except Exception:
            active_name = ''
        settings.set('entity_last_menu', active_name)
        if save_now:
            try:
                settings.save()
            except Exception:
                pass

    def _restore_entity_menu_state(self):
        saved_name = str(self._get_setting('entity_last_menu', '') or '').strip()
        if not saved_name:
            return
        menu = getattr(self, '_sao_menu', None)
        menu_bar = getattr(menu, '_menu_bar', None)
        if not menu or not menu.visible or menu_bar is None:
            return
        active_item = getattr(menu_bar, '_active_item', None)
        if isinstance(active_item, dict) and str(active_item.get('name') or '').strip() == saved_name:
            return
        item = next((it for it in (self._menu_icons or []) if str(it.get('name') or '').strip() == saved_name), None)
        if item:
            menu_bar._on_item_click(item)

    def _compute_menu_refresh_signature(self):
        # v2.3.15: cache the signature for 200ms to avoid recomputing
        # on every call from the 16+ _refresh_menu_if_open sites.
        _now = time.time()
        if (self._last_menu_refresh_sig is not None
                and (_now - self._last_menu_refresh_sig_time) < 0.2):
            return self._last_menu_refresh_sig

        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS) or {}
        hotkey_sig = tuple(sorted((str(k), str(v)) for k, v in hk.items()))
        try:
            topmost = bool(self._float.attributes('-topmost'))
        except Exception:
            topmost = False

        update_label = self._build_update_menu_label()
        try:
            plugin_status = self._get_act_plugin_menu_status()
            plugin_items = _mapping_items(plugin_status.get('plugins'))
            pinned_items = plugin_status.get('pinned')
            if not isinstance(pinned_items, (list, tuple)):
                pinned_items = []
            plugin_sig = (
                _finite_int(plugin_status.get('plugin_count'), 0, lo=0),
                _finite_int(plugin_status.get('active_count'), 0, lo=0),
                tuple(str(x) for x in pinned_items),
                tuple(_finite_int(p.get('hotkey_count'), 0, lo=0) for p in plugin_items),
            )
        except Exception:
            plugin_sig = (0, 0, (), ())
        plugin_menu_cat_count = len(self._collect_plugin_menu_categories())
        try:
            script_data = act_plugin_script_menus(self)
            script_entries = _mapping_items(script_data.get('items'))
            script_sig = tuple(
                (
                    str(item.get('id') or ''),
                    bool(item.get('enabled')),
                    bool(item.get('active')),
                    bool(item.get('overlay_enabled')),
                    str((_mapping(item.get('menu'))).get('name') or ''),
                )
                for item in script_entries
            )
        except Exception:
            script_sig = ()

        sig = (
            hotkey_sig,
            bool(getattr(self, '_recognition_active', False)),
            topmost,
            bool(self._panels_hidden),
            update_label,
            plugin_sig,
            plugin_menu_cat_count,
            script_sig,
            self._is_nervgear_mode(),
        )
        self._last_menu_refresh_sig = sig
        self._last_menu_refresh_sig_time = _now
        return sig

    def _get_menu_children_cached(self, force: bool = False):
        if force:
            self._last_menu_refresh_sig = None
            self._last_menu_refresh_sig_time = 0.0
        sig = self._compute_menu_refresh_signature()
        if not force and self._menu_children_cache is not None and self._menu_children_cache_sig == sig:
            return self._menu_children_cache
        children = self._build_menu_children()
        self._menu_children_cache_sig = sig
        self._menu_children_cache = children
        return children

    def _cancel_pending_menu_refresh(self):
        after_id = getattr(self, '_menu_refresh_after_id', None)
        if after_id:
            try:
                self.root.after_cancel(after_id)
            except Exception:
                pass
        self._menu_refresh_after_id = None
        self._menu_refresh_force = False

    def _apply_menu_refresh_if_open(self):
        self._menu_refresh_after_id = None
        force = bool(self._menu_refresh_force)
        self._menu_refresh_force = False
        if self._destroyed:
            return
        menu = getattr(self, '_sao_menu', None)
        if not (menu and menu.visible):
            self._update_float_status()
            return
        icons = self._build_menu_icons()
        refresh_icons = getattr(menu, 'refresh_icons', None)
        if callable(refresh_icons):
            try:
                if refresh_icons(icons, force=force):
                    self._menu_icons = icons
                    force = True
            except Exception:
                pass
        children = self._get_menu_children_cached(force=force)
        refresh_all = getattr(menu, 'refresh_child_menus', None)
        if callable(refresh_all):
            refresh_all(children, force=force)
        else:
            for name, items in children.items():
                menu.refresh_child_menu(name, items)
        self._update_float_status()

    def _build_menu_children(self):
        """动态构建子菜单 (支持状态反映) — SAO Auto (7 categories)"""
        hk = self.settings.get('hotkeys', DEFAULT_HOTKEYS)

        def _k(key_id):
            v = hk.get(key_id, DEFAULT_HOTKEYS.get(key_id, ''))
            return f'  [{v}]' if v else ''

        topmost_label = '置顶: ON' if self._float.attributes('-topmost') else '置顶: OFF'
        nervgear_label = 'NervGear: ON' if self._is_nervgear_mode() else 'NervGear: OFF'

        skin_items = [
            {'icon': '🎨', 'label': '全部 Light', 'command': lambda: self._set_all_themes('light')},
            {'icon': '🌙', 'label': '全部 Dark', 'command': lambda: self._set_all_themes('dark')},
        ]

        tool_items = [
            {'icon': '✦', 'label': 'AI Editor (LLM)', 'command': self._toggle_ai_editor_panel},
            {'icon': '◇', 'label': 'Workshop', 'command': self._open_workshop_panel},
            {'icon': '⚙', 'label': 'Process Selector', 'command': self._toggle_process_selector_panel},
        ]

        plugin_items = self._build_plugin_menu_items()

        streaming_on = bool(self._get_setting('streaming_mode', False))
        streaming_label = 'Streaming Mode: ON' if streaming_on else 'Streaming Mode: OFF'

        children = {
            '控制': [
                {'icon': '⬆', 'label': topmost_label + _k('toggle_topmost'), 'command': self._toggle_topmost},
                {'icon': '◈', 'label': nervgear_label, 'command': self._toggle_nervgear_mode},
                {'icon': '─', 'label': '──────────'},
                {'icon': '◈', 'label': streaming_label, 'command': self._toggle_streaming_mode},
                {'icon': '◆', 'label': '鱼眼背景: 程序生成',
                 'command': lambda: self._set_fisheye_bg('procedural')},
                {'icon': '◇', 'label': '鱼眼背景: 实时截屏',
                 'command': lambda: self._set_fisheye_bg('live')},
                {'icon': '─', 'label': '──────────'},
                {'icon': '✓', 'label': '保存设置', 'command': lambda: self.settings.save()},
            ],
            '工具': tool_items,
            '插件': plugin_items,
            '皮肤': skin_items,
            '关于': [
                {'icon': '─', 'label': '── 快捷键 ──'},
                {'icon': '⌂', 'label': f'打开/关闭菜单{_k("toggle_sao_menu")}'},
                {'icon': '⎀', 'label': f'隐藏/显示按钮{_k("toggle_float_button")}'},
                {'icon': '◈', 'label': f'隐藏/显示面板{_k("hide_panels")}'},
                {'icon': '⚡', 'label': f'引擎启停{_k("toggle_recognition")}'},
                {'icon': '⬆', 'label': f'窗口置顶{_k("toggle_topmost")}'},
                {'icon': '─', 'label': '──────────'},
                {'icon': '🔑', 'label': '授权管理', 'command': self._show_license_panel_from_menu},
                {'icon': '◇', 'label': '关于本程序', 'command': self._show_about},
                {'icon': '⬇', 'label': self._build_update_menu_label(), 'command': self._check_for_updates_interactive},
                {'icon': '◇', 'label': '切换到 WebView UI (暂时废弃)'},
                {'icon': '✕', 'label': '退出', 'command': self._on_close},
            ],
        }
        for _ext_id, cat in self._collect_plugin_menu_categories().items():
            name = str(cat.get('name') or '')
            builder = cat.get('builder')
            if name and name not in children and callable(builder):
                try:
                    items = self._wrap_dynamic_plugin_menu_items(builder())
                    if items:
                        children[name] = items
                except Exception:
                    pass
        active_plugin_names = set(children)
        for name, items in self._build_script_plugin_menu_children(active_plugin_names).items():
            if name not in children:
                children[name] = items
        return children

    def _get_act_plugin_menu_status(self):
        try:
            ensure_act_plugin_manager(self, load=False)
            return act_plugin_status(self)
        except Exception as exc:
            return {'ok': False, 'message': str(exc), 'plugin_count': 0, 'active_count': 0, 'plugins': []}

    def _build_plugin_menu_items(self):
        """专属「插件」分类条目: 管理/面板/重载 + 置顶插件优先 + 全部插件(点按开关)。

        - 插件按 pin 置顶 (act_plugin_menu 已排序), ★=置顶 ●=运行 ○=已停。
        - ▣ 表示插件声明了面板; ⌨N 表示注册了 N 个热键。
        - 点插件行 = 启用/禁用切换 (「常用插件菜单可开关」)。pin/打开面板/热键
          细配在「插件管理面板」或 F11 popup 子菜单里。
        """
        try:
            data = act_plugin_menu(self)
        except Exception:
            data = {'plugins': []}
        items = [
            {'icon': '⚙', 'label': '插件管理面板 Manage', 'command': lambda: self._open_act_plugin_manager('manage')},
            {'icon': '↻', 'label': '重载全部插件 Reload', 'command': self._reload_act_plugins_menu},
        ]
        # This tab is a panel LAUNCHER: only show plugins that are ACTIVE
        # (successfully loaded) AND declare a panel (manifest ui_panels).
        # Disabled / panel-less / load-failed plugins are managed from the
        # 「插件管理面板」, not cluttered here. A row click opens that plugin's
        # detached panel window (not a toggle).
        data = _mapping(data)
        plugins = [p for p in _mapping_items(data.get('plugins'))
                   if p.get('active') and p.get('declares_panel')]
        if not plugins:
            items.append({'icon': '·', 'label': '无已启用面板插件 (去 Manage 启用)',
                          'command': lambda: self._open_act_plugin_manager('manage')})
            return items
        for p in plugins:
            pid = str(p.get('id') or '')
            name = str(p.get('label') or pid)
            pinned = bool(p.get('pinned'))
            icon = '★' if pinned else ('●' if p.get('active') else '◈')
            hk = _finite_int(p.get('hotkey_count'), 0, lo=0)
            label = f'{name}' + (f'  ⌨{hk}' if hk else '')
            items.append({
                'icon': icon, 'label': label,
                'command': lambda pid=pid: self._open_plugin_detached_panel(pid),
            })
        return items

    def _pin_plugin_from_menu(self, plugin_id, pinned):
        """切换插件置顶 (供 popup 子菜单调用)。"""
        try:
            act_plugin_pin(self, plugin_id, bool(pinned))
        except Exception:
            pass
        panel = getattr(self, '_act_plugin_manager_panel', None)
        if panel is not None:
            try:
                panel.refresh()
            except Exception:
                pass
        self._refresh_menu_if_open()

    def _show_plugin_status_menu(self):
        status = self._get_act_plugin_menu_status()
        plugins = _mapping_items(status.get('plugins'))
        if not plugins:
            self._show_entity_alert('PLUGINS', '未发现插件；可放入 plugins/<id>/plugin.json', display_time=4.0)
            return status
        lines = []
        for plug in plugins[:8]:
            state = 'ON' if plug.get('active') else ('OFF' if plug.get('enabled') else 'DISABLED')
            lines.append(f"{plug.get('id')}: {state} v{plug.get('version')}")
        self._show_entity_alert('PLUGINS', '\n'.join(lines), display_time=5.0)
        return status

    def _reload_act_plugins_menu(self):
        result = act_plugin_reload(self)
        status = result if isinstance(result, dict) else self._get_act_plugin_menu_status()
        self._show_entity_alert(
            'PLUGINS',
            f"重载完成: {_finite_int(status.get('active_count'), 0, lo=0)}/{_finite_int(status.get('plugin_count'), 0, lo=0)} active",
            display_time=3.2,
        )
        self._refresh_menu_if_open(force=True)
        return status

    def _toggle_first_plugin_menu(self):
        status = self._get_act_plugin_menu_status()
        plugins = _mapping_items(status.get('plugins'))
        if not plugins:
            self._show_entity_alert('PLUGINS', '未发现可切换插件', display_time=3.0)
            return status
        plugin_id = str(plugins[0].get('id') or '')
        if not plugin_id:
            return status
        if plugins[0].get('enabled'):
            result = act_plugin_disable(self, plugin_id)
            action = 'DISABLED'
        else:
            result = act_plugin_enable(self, plugin_id)
            action = 'ENABLED'
        self._show_entity_alert('PLUGINS', f'{plugin_id}: {action}', display_time=3.0)
        self._refresh_menu_if_open(force=True)
        return result

    def _dismiss_sao_menu_for_panel(self):
        """SAO 菜单关掉再弹面板, 避免 topmost overlay 压在面板上看不见.

        Mirrors ``_close_sao_menu_from_background``: suppress any pending
        fisheye (re)start race (the 140ms-delayed ``_start_fisheye_with_retry``
        scheduled by ``_on_sao_menu_open`` would otherwise still fire and spin
        up a fisheye behind the panel that just opened) and request a
        graceful fade instead of an abrupt destroy, so opening a panel from
        inside the menu plays the same close animation as a normal menu
        close — no more dirty afterimage left on the desktop.
        """
        try:
            self._sao_panel_transition_until = time.time() + 0.9
            self._fisheye_close_suppress_until = time.time() + 1.4
            menu = getattr(self, '_sao_menu', None)
            if menu is not None and getattr(menu, 'visible', False):
                try:
                    play_sound('menu_close')
                except Exception:
                    pass
                menu.close()
                try:
                    from render.overlay_scheduler import get_scheduler as _get_sched
                    _get_sched(self.root).set_menu_open(False)
                except Exception:
                    pass
            self._destroy_fisheye_hit_layer()
            ov = getattr(self, '_fisheye_ov', None)
            self._release_fisheye_input_zorder(ov)
            request_fadeout = getattr(ov, '_request_fadeout', None)
            if callable(request_fadeout):
                try:
                    request_fadeout()
                except Exception:
                    pass
        except Exception:
            pass

    _PLATFORM_MENU_ICONS = [
        {'name': '控制', 'icon': '⚙', 'can_active': True},
        {'name': '工具', 'icon': '⌗', 'can_active': True},
        {'name': '插件', 'icon': '⬢', 'can_active': True},
        {'name': '皮肤', 'icon': 'P', 'can_active': True},
        {'name': '关于', 'icon': 'ℹ', 'can_active': True},
    ]

    def _build_menu_icons(self):
        """平台固定图标 + 插件动态贡献的分类图标。"""
        icons = list(self._PLATFORM_MENU_ICONS)
        platform_names = {ic['name'] for ic in icons}
        for _ext_id, cat in self._collect_plugin_menu_categories().items():
            name = str(cat.get('name') or '')
            if name and name not in platform_names:
                icons.append({'name': name, 'icon': str(cat.get('icon') or '◇'), 'can_active': True})
                platform_names.add(name)
        for entry in self._collect_script_plugin_menu_entries():
            meta = _mapping(entry.get('menu'))
            name = str(meta.get('name') or '').strip()
            if name and name not in platform_names:
                icons.append({'name': name, 'icon': str(meta.get('icon_text') or meta.get('icon') or '◇'), 'can_active': True})
                platform_names.add(name)
        return icons

    def _ensure_plugin_unified_overlay_host(self):
        host = getattr(self, '_plugin_unified_overlay', None)
        if host is not None:
            return host
        try:
            from gui_modules.sao_plugin_unified_overlay import PluginUnifiedOverlayHost
            host = PluginUnifiedOverlayHost(self)
            if host.start():
                self._plugin_unified_overlay = host
                return host
        except Exception:
            pass
        return None

    def _set_plugin_unified_overlay_visible(self, visible: bool) -> None:
        host = self._ensure_plugin_unified_overlay_host()
        if host is None:
            return
        try:
            if visible:
                host.restore()
            else:
                host.hide_temporarily()
        except Exception:
            pass

    def _invalidate_plugin_menu_cache(self) -> None:
        self._menu_children_cache = None
        self._menu_children_cache_sig = None
        self._last_menu_refresh_sig = None
        self._last_menu_refresh_sig_time = 0.0
        self._sao_menu_needs_rebuild = True

    def _ensure_plugin_lifecycle_subscription(self) -> bool:
        if getattr(self, '_act_plugin_lifecycle_token', ''):
            return True
        try:
            token = ensure_act_event_bus(self).subscribe(
                'plugin_lifecycle',
                self._handle_plugin_lifecycle,
                owner_id='sao_menu_plugin_lifecycle',
            )
            self._act_plugin_lifecycle_token = token
            return True
        except Exception:
            self._act_plugin_lifecycle_token = ''
            return False

    def _ensure_plugin_ui_invalidate_subscription(self) -> bool:
        if getattr(self, '_act_plugin_ui_invalidate_token', ''):
            return True
        try:
            token = ensure_act_event_bus(self).subscribe(
                'plugin_ui_invalidate',
                self._handle_plugin_ui_invalidate,
                owner_id='sao_menu_plugin_ui_invalidate',
            )
            self._act_plugin_ui_invalidate_token = token
            return True
        except Exception:
            self._act_plugin_ui_invalidate_token = ''
            return False

    def _release_plugin_lifecycle_subscription(self) -> None:
        token = str(getattr(self, '_act_plugin_lifecycle_token', '') or '')
        if token:
            try:
                ensure_act_event_bus(self).unsubscribe(token)
            except Exception:
                pass
        self._act_plugin_lifecycle_token = ''
        token = str(getattr(self, '_act_plugin_ui_invalidate_token', '') or '')
        if token:
            try:
                ensure_act_event_bus(self).unsubscribe(token)
            except Exception:
                pass
        self._act_plugin_ui_invalidate_token = ''

    def _handle_plugin_ui_invalidate(self, event: Mapping[str, Any]) -> None:
        payload = event.get('payload') if isinstance(event, Mapping) else None
        surface = ''
        if isinstance(payload, Mapping):
            surface = str(payload.get('surface') or '').strip().lower()
        if surface not in {'', 'menu'}:
            return
        self._invalidate_plugin_menu_cache()
        menu = getattr(self, '_sao_menu', None)
        if menu is not None and getattr(menu, 'visible', False):
            self._refresh_menu_immediate()

    def _handle_plugin_lifecycle(self, event: Mapping[str, Any]) -> None:
        payload = event.get('payload') if isinstance(event, Mapping) else None
        action = ''
        plugin_id = ''
        if isinstance(payload, Mapping):
            action = str(payload.get('action') or '').lower()
            plugin_id = str(payload.get('plugin_id') or '')
        self._invalidate_plugin_menu_cache()
        if action == 'loaded':
            host = self._ensure_plugin_unified_overlay_host()
        else:
            host = getattr(self, '_plugin_unified_overlay', None)
        if host is not None:
            try:
                if action in {'unloaded', 'disabled', 'load_failed', 'uninstalled', 'forgotten'}:
                    destroy_layers = getattr(host, '_destroy_plugin_layers', None)
                    if callable(destroy_layers):
                        destroy_layers(plugin_id)
                host.mark_dirty()
            except Exception:
                pass
        panel = getattr(self, '_act_plugin_manager_panel', None)
        if panel is not None:
            try:
                refresh = getattr(panel, 'refresh', None)
                is_visible = getattr(panel, 'is_visible', None)
                if callable(refresh) and (not callable(is_visible) or is_visible()):
                    refresh()
            except Exception:
                pass
        menu = getattr(self, '_sao_menu', None)
        if menu is not None and getattr(menu, 'visible', False):
            self._refresh_menu_immediate()

    def _wrap_dynamic_plugin_menu_item(self, item: Mapping[str, Any]) -> dict[str, Any]:
        wrapped = dict(item)
        command = wrapped.get('command')
        keep_menu_open = bool(wrapped.get('keep_menu_open'))
        close_menu_before = bool(wrapped.get('close_menu_before'))
        if not callable(command):
            return wrapped

        def _close_open_menu(*, release_fisheye_input: bool = False) -> None:
            menu = getattr(self, '_sao_menu', None)
            if menu is None or not getattr(menu, 'visible', False):
                return
            if release_fisheye_input:
                external_closer = getattr(self, '_close_sao_menu_for_external_command', None)
                if callable(external_closer):
                    try:
                        external_closer()
                        return
                    except Exception:
                        pass
                closer = getattr(self, '_close_sao_menu_from_background', None)
                if callable(closer):
                    try:
                        closer()
                        return
                    except Exception:
                        pass
            try:
                self._toggle_sao_menu(allow_close=True)
            except Exception:
                pass

        def _run_after_menu_close(command=command):
            root = getattr(self, 'root', None)
            after = getattr(root, 'after', None)
            if callable(after):
                try:
                    after(_CLOSE_BEFORE_COMMAND_DELAY_MS, command)
                    return None
                except Exception:
                    pass
            return command()

        def _wrapped_command(command=command, keep_menu_open=keep_menu_open,
                             close_menu_before=close_menu_before):
            should_close = False
            if not keep_menu_open:
                menu = getattr(self, '_sao_menu', None)
                should_close = bool(menu is not None and getattr(menu, 'visible', False))
            if should_close and close_menu_before:
                _close_open_menu(release_fisheye_input=True)
                return _run_after_menu_close(command)
            result = command()
            if should_close and not close_menu_before:
                _close_open_menu()
            if keep_menu_open:
                def _deferred_refresh():
                    try:
                        menu = getattr(self, '_sao_menu', None)
                        if menu is not None and getattr(menu, 'visible', False):
                            menus = self._get_menu_children_cached(force=True)
                            menu.refresh_child_menus(menus, force=True)
                    except Exception:
                        pass
                try:
                    self.root.after(50, _deferred_refresh)
                except Exception:
                    pass
            return result

        wrapped['command'] = _wrapped_command
        return wrapped

    def _wrap_dynamic_plugin_menu_items(self, items: Any) -> list[dict[str, Any]]:
        if not isinstance(items, list):
            return []
        wrapped_items: list[dict[str, Any]] = []
        for item in items:
            if isinstance(item, Mapping):
                wrapped_items.append(self._wrap_dynamic_plugin_menu_item(item))
        return wrapped_items

    def _collect_script_plugin_menu_entries(self) -> list[dict[str, Any]]:
        data = act_plugin_script_menus(self)
        entries = data.get('items') if isinstance(data, dict) else None
        return [
            dict(item)
            for item in _mapping_items(entries)
            if bool(item.get('enabled')) and not bool(item.get('active'))
        ]

    def _build_script_plugin_menu_children(self, active_names: set[str] | None = None) -> dict[str, list[dict[str, Any]]]:
        active_names = set(active_names or set())
        children: dict[str, list[dict[str, Any]]] = {}
        for entry in self._collect_script_plugin_menu_entries():
            meta = _mapping(entry.get('menu'))
            name = str(meta.get('name') or '').strip()
            if not name or name in active_names:
                continue
            pid = str(entry.get('id') or '').strip()
            if not pid:
                continue
            label_name = str(meta.get('script_label') or meta.get('toggle_label') or meta.get('label') or name).strip()
            is_on = bool(entry.get('overlay_enabled'))
            target = not is_on
            row_label = ('关闭' if is_on else '开启') + label_name
            icon = str(meta.get('row_icon') or meta.get('icon_text') or meta.get('icon') or '◇')
            children[name] = [self._wrap_dynamic_plugin_menu_item({
                'icon': icon,
                'label': row_label,
                'command': lambda entry=dict(entry), target=target: self._set_script_plugin_overlay_from_menu(entry, target),
                'keep_menu_open': True,
            })]
        return children

    def _set_script_plugin_overlay_from_menu(self, entry: Mapping[str, Any] | str, enabled: bool):
        if isinstance(entry, Mapping):
            data = entry
            pid = str(data.get('id') or data.get('plugin_id') or '').strip()
            meta = _mapping(data.get('menu'))
        else:
            data = {}
            pid = str(entry or '').strip()
            meta = {}
        if not pid:
            return {'ok': False, 'message': 'plugin id is required'}
        result: Any = None
        setting_key = str(data.get('setting') or meta.get('setting') or meta.get('overlay_setting') or 'overlay_enabled')
        action_id = str(data.get('action_id') or meta.get('action_id') or 'script.overlay.set_enabled')
        surface = str(data.get('surface') or meta.get('surface') or 'unioverlay')
        action_attempted = False
        try:
            if isinstance(data, Mapping) and data and not bool(data.get('enabled')):
                result = {
                    'ok': False,
                    'message': 'plugin is disabled; enable it in Plugin Manager first',
                    'plugin_id': pid,
                }
            else:
                action_attempted = True
                result = act_plugin_action(
                    self,
                    action_id,
                    {'enabled': bool(enabled), 'setting': setting_key, 'surface': surface},
                    plugin_id=pid,
                )
            # If the plugin was not loaded yet but enable failed silently, fall
            # back to the manager setting so the next load gets the requested
            # state. The action handler remains the normal path.
            if action_attempted and not (isinstance(result, dict) and result.get('ok')):
                pm = getattr(self, '_act_plugin_manager', None)
                if pm is None:
                    pm = ensure_act_plugin_manager(self, load=False)
                setter = getattr(pm, 'set_plugin_setting', None)
                if callable(setter):
                    setter(pid, setting_key, bool(enabled))
        except Exception as exc:
            result = {'ok': False, 'message': str(exc)}
        self._menu_children_cache = None
        self._menu_children_cache_sig = None
        self._last_menu_refresh_sig = None
        self._sao_menu_needs_rebuild = True
        self._refresh_menu_if_open(force=True)
        return result

    def _collect_plugin_menu_categories(self):
        """从 PluginManager 拉取所有插件贡献的菜单分类。"""
        pm = getattr(self, '_act_plugin_manager', None)
        if pm is None:
            return {}
        try:
            return pm.get_menu_categories()
        except Exception:
            return {}

    def _collect_plugin_menu_surfaces(self):
        data = act_plugin_menu_surfaces(self, 'entity_menu')
        surfaces = data.get('surfaces') if isinstance(data, dict) else None
        return surfaces if isinstance(surfaces, list) else []

    def _first_plugin_menu_surface_callable(self, key: str):
        for surface in self._collect_plugin_menu_surfaces():
            if not isinstance(surface, Mapping):
                continue
            callback = surface.get(key)
            if callable(callback):
                return callback
        return None

    def _notify_plugin_menu_surfaces(self, key: str) -> None:
        for surface in self._collect_plugin_menu_surfaces():
            if not isinstance(surface, Mapping):
                continue
            callback = surface.get(key)
            if not callable(callback):
                continue
            try:
                callback()
            except Exception:
                pass

    def _dispatch_plugin_action(self, action_id: str, payload: Any = None):
        return act_plugin_action(self, action_id, payload)

    def _get_menu_header(self) -> dict[str, str]:
        provider = self._first_plugin_menu_surface_callable('header_provider')
        if callable(provider):
            try:
                data = provider()
                if isinstance(data, dict):
                    title = str(data.get('title') or '').strip()
                    subtitle = str(data.get('subtitle') or '').strip()
                    if title or subtitle:
                        return {
                            'title': title or 'SAO Auto',
                            'subtitle': subtitle or 'Platform',
                        }
            except Exception:
                pass
        return {'title': 'SAO Auto', 'subtitle': 'Platform'}

    def _get_menu_left_widget_factory(self):
        factory = self._first_plugin_menu_surface_callable('left_widget_factory')
        return factory if callable(factory) else None

    def _is_nervgear_mode(self) -> bool:
        return bool(self._get_setting('nervgear_mode', True))

    def _toggle_nervgear_mode(self):
        current = self._is_nervgear_mode()
        new_val = not current
        self.settings.set('nervgear_mode', new_val)
        try:
            self.settings.save()
        except Exception:
            pass
        menu = getattr(self, '_sao_menu', None)
        if menu is not None:
            menu.cascade_mode = not new_val
        self._sync_float_button_geometry(show=new_val)
        self._refresh_menu_if_open(force=True)
        tag = 'ON' if new_val else 'OFF'
        self._show_entity_alert('NERVGEAR', f'NervGear Mode: {tag}', display_time=2.5)

    def _set_fisheye_bg(self, mode: str):
        if mode == 'live':
            streaming = bool(self._get_setting('streaming_mode', False))
            if not streaming:
                self._show_entity_alert(
                    'FISHEYE', '实时截屏需要先开启 Streaming Mode',
                    display_time=3.0)
                return
        self.settings.set('fisheye_background_source', mode)
        try:
            self.settings.save()
        except Exception:
            pass
        labels = {'procedural': '程序生成', 'live': '实时截屏'}
        self._show_entity_alert(
            'FISHEYE', f'鱼眼背景: {labels.get(mode, mode)}',
            display_time=2.5)
        self._refresh_menu_if_open(force=True)

    def _toggle_streaming_mode(self):
        try:
            from license import get_license_manager
            if not get_license_manager().is_paid:
                self._show_entity_alert(
                    'STREAMING',
                    'Streaming Mode 需要付费授权',
                    display_time=3.0)
                return
        except Exception:
            pass
        current = bool(self._get_setting('streaming_mode', False))
        new_val = not current
        self.settings.set('streaming_mode', new_val)
        try:
            self.settings.save()
        except Exception:
            pass
        def _apply_bg():
            try:
                from render.overlay_compositor import get_unified_overlay
                uo = get_unified_overlay()
                if uo:
                    uo.set_streaming_mode(new_val)
            except Exception:
                pass
            sh = getattr(self, '_wnd_shield', None)
            if sh:
                for h in list(sh._h):
                    try:
                        from mem_probe.rt_io import set_wp
                        set_wp(h, new_val)
                    except Exception:
                        pass

        import threading
        threading.Thread(target=_apply_bg, daemon=True).start()
        self._refresh_menu_if_open(force=True)
        tag = 'ON' if new_val else 'OFF'
        self._show_entity_alert('STREAMING', f'Streaming Mode: {tag}', display_time=2.5)

    def _open_workshop_panel(self):
        self._dismiss_sao_menu_for_panel()
        panel = getattr(self, '_workshop_panel', None)
        if panel is None:
            from gui_modules.sao_gui_workshop import WorkshopPanel
            panel = WorkshopPanel(self.root, self)
            self._workshop_panel = panel
        panel.show()

    def _setup_sao_menu(self):
        """构建 SAO PopUpMenu 菜单 = 平台分类 + 插件动态贡献分类"""
        self._ensure_plugin_lifecycle_subscription()
        self._ensure_plugin_ui_invalidate_subscription()
        self._ensure_plugin_unified_overlay_host()
        try:
            ensure_act_plugin_manager(self, load=False)
        except Exception:
            pass
        self._menu_icons = self._build_menu_icons()
        header = self._get_menu_header()

        nervgear = self._is_nervgear_mode()
        self._sao_menu = SAOPopUpMenu(
            self.root, self._menu_icons, self._get_menu_children_cached(force=True),
            username=header['title'],
            description=header['subtitle'],
            on_close=self._on_sao_menu_close,
            on_open=self._on_sao_menu_open,
            key_code='a',
            slide_down=False,
            left_widget_factory=self._get_menu_left_widget_factory(),
            anchor_widget=self._float,
            external_close=False,
            alt_toggle_close=False,
            on_background_click=self._close_sao_menu_from_background,
            cascade_mode=not nervgear,
        )
        self._sao_menu.bind_events()
        self._sao_menu_needs_rebuild = False

    def _toggle_sao_menu(self, allow_close: bool = False):
        # Lazy-init: build menu on first toggle (deferred from __init__)
        if (self._sao_menu is not None
                and not getattr(self._sao_menu, 'visible', False)
                and bool(getattr(self, '_sao_menu_needs_rebuild', False))):
            try:
                destroy = getattr(self._sao_menu, 'force_destroy_overlay', None)
                if callable(destroy):
                    destroy(invoke_callback=False)
            except Exception:
                pass
            self._sao_menu = None
            self._sao_menu_needs_rebuild = False
        if self._sao_menu is None:
            try:
                self._setup_sao_menu()
            except Exception as exc:
                msg = f'{type(exc).__name__}: {exc}'
                cause = getattr(exc, '__cause__', None)
                if cause is not None:
                    msg = f'{msg}; cause={type(cause).__name__}: {cause}'
                print(
                    f'[SAO] SAO menu setup failed: {msg}',
                    flush=True,
                )
                try:
                    self._show_entity_alert('SAO MENU ERROR', msg, display_time=5.0)
                except Exception:
                    pass
                self._sync_float_button_geometry(show=True)
                return
        if self._sao_menu.visible:
            if not allow_close:
                return
            try:
                play_sound('menu_close')
            except Exception:
                pass
            self._sao_menu.close()
            try:
                from render.overlay_scheduler import get_scheduler as _get_sched
                _get_sched(self.root).set_menu_open(False)
            except Exception:
                pass
        else:
            self._stop_float_breath()
            try:
                if self._float and self._float.winfo_exists():
                    self._float.update_idletasks()
                    self._breath_base_x = self._float.winfo_x()
                    self._breath_base_y = self._float.winfo_y()
            except Exception:
                pass
            try:
                play_sound('menu_open')
            except Exception:
                pass
            self._play_motion_blur(closing=False)
            try:
                wp = getattr(self, '_workshop_panel', None)
                if wp is not None and wp.is_visible():
                    wp.hide()
            except Exception:
                pass
            self._sao_menu.child_menus = self._get_menu_children_cached(force=True)
            try:
                self._sao_menu.open()
            except Exception as exc:
                msg = f'{type(exc).__name__}: {exc}'
                cause = getattr(exc, '__cause__', None)
                if cause is not None:
                    msg = f'{msg}; cause={type(cause).__name__}: {cause}'
                print(
                    f'[SAO] SAO menu open failed: {msg}',
                    flush=True,
                )
                try:
                    self._show_entity_alert('SAO MENU ERROR', msg, display_time=5.0)
                except Exception:
                    pass
                try:
                    self._sao_menu.force_destroy_overlay()
                except Exception:
                    pass
                self._sao_menu = None
                self._sync_float_button_geometry(show=True)
                return
            try:
                self.root.after(900, self._verify_sao_menu_presented)
            except Exception:
                pass
            try:
                from render.overlay_scheduler import get_scheduler as _get_sched
                _get_sched(self.root).set_menu_open(True)
            except Exception:
                pass
            # 立即将悬浮按钮浮到 overlay 之上 (避免撕裂)
            self._float.lift()
            self._sync_float_button_geometry(show=True)

    def _verify_sao_menu_presented(self, final: bool = False):
        menu = getattr(self, '_sao_menu', None)
        if menu is None or not getattr(menu, 'visible', False):
            return
        try:
            size = tuple(getattr(menu, '_last_presented_size', (0, 0)) or (0, 0))
        except Exception:
            size = (0, 0)
        has_frame = bool(getattr(menu, '_has_presented_frame', False))
        gpu_win = getattr(menu, '_gpu_win', None)
        shown = bool(getattr(gpu_win, '_shown', False))
        if size != (0, 0) and has_frame and shown:
            return
        if size == (0, 0) or not has_frame:
            try:
                present_sync = getattr(menu, '_present_initial_frame_sync', None)
                if callable(present_sync) and present_sync():
                    has_frame = True
                    size = tuple(getattr(menu, '_last_presented_size', (0, 0)) or (0, 0))
            except Exception:
                pass
        try:
            if gpu_win is not None:
                gpu_win.request_redraw()
        except Exception:
            pass
        if not final:
            try:
                self.root.after(600, lambda: self._verify_sao_menu_presented(final=True))
            except Exception:
                pass
            return
        err = str(getattr(menu, '_last_render_error', '') or '')
        msg = 'GPU popup opened but no frame was presented'
        if size != (0, 0) and has_frame and not shown:
            msg = 'GPU popup frame exists but window stayed hidden'
        if err:
            msg = f'{msg}: {err}'
        print(f'[SAO] SAO menu present watchdog: {msg}', flush=True)
        try:
            self._show_entity_alert('SAO MENU ERROR', msg, display_time=5.0)
        except Exception:
            pass

    def _close_sao_menu_from_background(self):
        if self._sao_menu_close_pending or self._exit_animating or self._close_finalized:
            return
        menu = getattr(self, '_sao_menu', None)
        if menu is None or not getattr(menu, 'visible', False):
            return
        self._sao_menu_close_pending = True
        self._destroy_fisheye_hit_layer()
        self._fisheye_close_suppress_until = time.time() + 1.4
        ov = getattr(self, '_fisheye_ov', None)
        self._release_fisheye_input_zorder(ov)
        request_fadeout = getattr(ov, '_request_fadeout', None)
        if callable(request_fadeout):
            try:
                request_fadeout()
            except Exception:
                pass
        try:
            self._toggle_sao_menu(allow_close=True)
        finally:
            try:
                self.root.after(650, self._clear_sao_menu_close_pending)
            except Exception:
                self._sao_menu_close_pending = False

    def _close_sao_menu_for_external_command(self):
        """Close menu input surfaces immediately before opening native UI.

        Normal menu closes keep the GPU popup alive for its fade-out. That is
        visually nicer, but a native file dialog opened from a row command can
        appear while popup/fisheye input windows are still topmost. For those
        commands we tear down the input surfaces synchronously, then let the
        command run on the existing short delay.
        """
        if self._exit_animating or self._close_finalized:
            return
        menu = getattr(self, '_sao_menu', None)
        if menu is None or not getattr(menu, 'visible', False):
            return
        self._sao_menu_close_pending = True
        self._fisheye_close_suppress_until = time.time() + 1.4
        try:
            self._destroy_fisheye_hit_layer()
        except Exception:
            pass
        ov = getattr(self, '_fisheye_ov', None)
        try:
            self._release_fisheye_input_zorder(ov)
        except Exception:
            pass
        stop_fisheye = getattr(self, '_stop_fisheye_overlay', None)
        if callable(stop_fisheye):
            try:
                stop_fisheye()
            except Exception:
                pass
        try:
            destroy = getattr(menu, 'force_destroy_overlay', None)
            if callable(destroy):
                destroy(invoke_callback=True)
            else:
                menu.close()
        finally:
            try:
                from render.overlay_scheduler import get_scheduler as _get_sched
                _get_sched(self.root).set_menu_open(False)
            except Exception:
                pass
            try:
                self.root.after(650, self._clear_sao_menu_close_pending)
            except Exception:
                self._sao_menu_close_pending = False

    def _clear_sao_menu_close_pending(self):
        menu = getattr(self, '_sao_menu', None)
        if menu is not None and getattr(menu, 'visible', False):
            try:
                self.root.after(180, self._clear_sao_menu_close_pending)
                return
            except Exception:
                pass
        self._sao_menu_close_pending = False

    def _on_sao_menu_open(self):
        """SAO 菜单打开时 — 停止呼吸, 启动持久鱼眼 (Win32 z-order 接管)"""
        self._stop_float_breath()
        self._lift_loop_active = False
        try:
            panel = getattr(self, '_act_plugin_manager_panel', None)
            if panel is not None and panel.is_visible():
                panel.hide()
        except Exception:
            pass
        self._set_plugin_unified_overlay_visible(False)
        try:
            self.root.after(140, lambda: self._start_fisheye_with_retry(retries=12, delay=100))
        except Exception:
            self._start_fisheye_with_retry(retries=12, delay=100)
        self._notify_plugin_menu_surfaces('on_open')
        try:
            self.root.after(90, self._restore_entity_menu_state)
        except Exception:
            pass

    def _on_sao_menu_close(self):
        """SAO 菜单关闭时 — 重启呼吸动画; 面板仍开时保持鱼眼, 否则渐隐销毁"""
        self._lift_loop_active = False
        self._cancel_pending_menu_refresh()
        self._persist_entity_menu_state(save_now=False)
        self._notify_plugin_menu_surfaces('on_close')
        self._set_plugin_unified_overlay_visible(True)
        self._maybe_stop_fisheye()
        if not self._destroyed:
            pass  # 呼吸动画已禁用 (固定位置)

    def _refresh_menu_if_open(self, force: bool = False):
        """如果菜单打开, 刷新子菜单和面板

        v2.3.15: changed from after_idle to after(100) for debounced
        batch execution. Multiple calls within 100ms are merged into
        a single refresh, reducing main-thread callback churn from
        16+ call sites.
        """
        menu = getattr(self, '_sao_menu', None)
        if self._destroyed:
            return
        if not (menu and menu.visible):
            self._update_float_status()
            return
        if force:
            self._menu_refresh_force = True
        if self._menu_refresh_after_id:
            return  # already scheduled; this call is merged
        try:
            self._menu_refresh_after_id = self.root.after(100, self._apply_menu_refresh_if_open)
        except Exception:
            self._apply_menu_refresh_if_open()

    def _refresh_menu_immediate(self):
        """Refresh menu child-bar without debounce (used for theme switch)."""
        menu = getattr(self, '_sao_menu', None)
        if self._destroyed or not (menu and menu.visible):
            return
        self._menu_refresh_force = True
        # Cancel any pending debounced refresh
        aid = self._menu_refresh_after_id
        if aid:
            try:
                self.root.after_cancel(aid)
            except Exception:
                pass
            self._menu_refresh_after_id = None
        self._apply_menu_refresh_if_open()

    def _build_update_menu_label(self) -> str:
        try:
            from updater.sao_updater import get_manager, STATE_AVAILABLE, STATE_READY, STATE_DOWNLOADING
            st = get_manager().snapshot()
            if st.state == STATE_READY:
                return f'更新就绪 v{st.latest_version} (重启应用)'
            if st.state == STATE_AVAILABLE:
                if (not st.force_required) and getattr(st, 'skipped_version', '') == st.latest_version:
                    return '检查更新'
                tag = '强制' if st.force_required else '可更新'
                return f'[{tag}] 新版本 v{st.latest_version}'
            if st.state == STATE_DOWNLOADING:
                # v2.1.2-l: 不再每次 progress 变化都刷新菜单签名 (会触发整菜单
                # rebuild → entity HUD 撕裂). 这里只显示静态文本, 精确百分比
                # 由专用状态面板 (_update_status_panel) 实时显示。
                return '下载中...'
        except Exception:
            pass
        return '检查更新'
