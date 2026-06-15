# -*- coding: utf-8 -*-
"""
SAOPlayerGUISessionMixin — first mixin extracted from SAOPlayerGUI
(round 31 of the sao_gui split refactor).

Holds the in-session player roster helpers — the cluster that backs
the SAO menu's "本次登录玩家" left-stack panel. These methods are
tightly coupled (each one calls the others through ``self``) so they
live together; the mixin only contains method definitions, while the
actual instance state (``self._session_players``, ``self._username``,
etc.) is still initialised by ``SAOPlayerGUI.__init__``.

Pattern: SAOPlayerGUI inherits from this mixin, so ``self.X`` accesses
in these methods resolve to the SAOPlayerGUI instance's attributes.
``self._refresh_menu_if_open`` lives on SAOPlayerGUI itself — that's
fine, attribute lookup goes through the MRO.
"""

from __future__ import annotations

import time
from typing import Any, Dict, List

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


class SAOPlayerGUISessionMixin:
    """Mixin providing the SAO menu's in-session player roster helpers.

    SAOPlayerGUI must initialise the following attributes in its
    ``__init__`` (this is the existing contract — the mixin does not
    seed them):

      * ``self._session_players: dict[int, dict]``
      * ``self._session_players_version: int``
      * ``self._session_players_self_uid: int``
      * ``self._session_players_last_sync_ts: float``
      * ``self._session_players_rows_cache_sig`` (tuple or None)
      * ``self._session_players_rows_cache: list``
      * ``self._session_players_panel`` (or None until the menu builds it)
      * ``self._menu_left_stack`` (or None until the menu builds it)
      * ``self._last_session_players_panel_sig`` (tuple or None)
      * ``self._last_session_players_panel_push_ts: float``
      * ``self._packet_engine`` (bridge instance or None)
      * ``self._game_state`` / ``self._state_mgr`` (game state plumbing)
      * ``self._username`` (string)
    """

    # ══════════════════════════════════════════════
    #  SAO 菜单 = 主界面
    # ══════════════════════════════════════════════
    @staticmethod
    def _session_int(value, default: int = 0) -> int:
        # v2.4.31: cython helper.
        return int(_CY_UI.session_int(value, default))

    def _session_self_uid(self) -> int:
        for source in (
                getattr(self, '_game_state', None),
                getattr(getattr(self, '_state_mgr', None), 'state', None)):
            uid = self._session_int(getattr(source, 'player_id', 0), 0)
            if uid > 0:
                return uid
        return 0

    def _merge_session_player(self, uid, name='', fight_point=0, is_self=False):
        uid = self._session_int(uid, 0)
        if uid <= 0:
            return
        now = time.time()
        changed = False
        entry = self._session_players.get(uid)
        if not entry:
            entry = {
                'uid': uid,
                'name': '',
                'fight_point': 0,
                'first_seen': now,
                'updated_at': now,
                'is_self': False,
            }
            self._session_players[uid] = entry
            changed = True
        name = str(name or '').strip()
        fight_point = self._session_int(fight_point, 0)
        if name and entry.get('name') != name:
            entry['name'] = name
            changed = True
        if fight_point > 0 and entry.get('fight_point') != fight_point:
            entry['fight_point'] = fight_point
            changed = True
        next_is_self = bool(entry.get('is_self') or is_self)
        if bool(entry.get('is_self')) != next_is_self:
            entry['is_self'] = next_is_self
            changed = True
        entry['updated_at'] = now
        if changed:
            self._session_players_version += 1

    def _sync_session_players_cache(self, gs=None, min_interval: float = 0.0):
        """Merge SELF + bridge players into the session cache.

        `min_interval`: skip the work if the previous sync ran within this
        many seconds. Hot-path callers (_apply_fast_state_update and
        _push_packet_overlays fire ≥ 10× / s) pass ~0.05 because session-
        player rows refresh at human-visible cadence; menu/panel callers
        leave the default 0 so they always see the freshest snapshot.
        """
        if min_interval > 0.0:
            now_chk = time.time()
            if (now_chk - self._session_players_last_sync_ts) < min_interval:
                return
            self._session_players_last_sync_ts = now_chk

        self_uid = self._session_self_uid()
        if self_uid > 0 and self_uid != self._session_players_self_uid:
            if self._session_players_self_uid:
                self._session_players.clear()
                self._session_players_version += 1
                self._session_players_rows_cache_sig = None
            self._session_players_self_uid = self_uid

        if gs is None:
            gs = getattr(self, '_game_state', None)
        if gs is not None:
            gs_uid = self._session_int(getattr(gs, 'player_id', 0), 0)
            self._merge_session_player(
                gs_uid,
                getattr(gs, 'player_name', '') or self._username or '',
                getattr(gs, 'fight_point', 0) or 0,
                is_self=bool(gs_uid and gs_uid == self_uid),
            )

        bridge = getattr(self, '_packet_engine', None)
        if not bridge:
            if min_interval <= 0.0:
                self._session_players_last_sync_ts = time.time()
            return
        try:
            players = bridge.get_players() or {}
        except Exception:
            players = {}
        for raw_uid, pdata in players.items():
            uid = self._session_int(raw_uid, 0) or self._session_int(getattr(pdata, 'uid', 0), 0)
            self._merge_session_player(
                uid,
                getattr(pdata, 'name', '') or '',
                getattr(pdata, 'fight_point', 0) or 0,
                is_self=bool(uid and uid == self_uid),
            )
        if min_interval <= 0.0:
            self._session_players_last_sync_ts = time.time()

    @staticmethod
    def _format_session_power(value) -> str:
        # v2.4.31: cython helper.
        return _CY_UI.format_session_power(value)

    def _get_session_player_rows(self, sync: bool = True) -> List[Dict[str, Any]]:
        if sync:
            self._sync_session_players_cache(getattr(self, '_game_state', None))
        cache_sig = (
            len(self._session_players),
            self._session_players_version,
            self._session_self_uid(),
        )
        if cache_sig == getattr(self, '_session_players_rows_cache_sig', None):
            return list(getattr(self, '_session_players_rows_cache', []) or [])
        rows = []
        self_uid = self._session_self_uid()
        for uid, entry in self._session_players.items():
            fp = self._session_int(entry.get('fight_point'), 0)
            is_self = bool(uid and uid == self_uid) or bool(entry.get('is_self'))
            rows.append({
                'uid': str(uid) if uid else '--',
                'name': str(entry.get('name') or ''),
                'fight_power': self._format_session_power(fp),
                'fight_power_value': fp,
                'is_self': is_self,
                'first_seen': float(entry.get('first_seen') or 0.0),
            })
        rows.sort(key=lambda r: (
            0 if r.get('is_self') else 1,
            -int(r.get('fight_power_value') or 0),
            str(r.get('name') or ''),
            str(r.get('uid') or ''),
        ))
        self._session_players_rows_cache_sig = cache_sig
        self._session_players_rows_cache = list(rows)
        return rows

    def _refresh_session_players_panel(self, force: bool = False):
        panel = getattr(self, '_session_players_panel', None)
        stack = getattr(self, '_menu_left_stack', None)
        stack_active = bool(getattr(stack, '_active', False)) if stack is not None else False
        # `already_synced` lets us skip the trailing _sync call when an
        # earlier show-panel branch (which already forces sync=True via
        # _get_session_player_rows) ran on this same invocation.
        already_synced = False
        if not panel and force and stack is not None and stack_active:
            try:
                panel = stack.show_session_players(
                    rows=self._get_session_player_rows(sync=True),
                    force=True,
                )
                self._session_players_panel = panel
                already_synced = True
            except Exception:
                panel = None
        if not panel:
            return
        if stack is not None and hasattr(stack, 'is_session_players_visible'):
            try:
                if not stack.is_session_players_visible():
                    if not force or not stack_active:
                        return
                    panel = stack.show_session_players(
                        rows=self._get_session_player_rows(sync=True),
                        force=True,
                    )
                    self._session_players_panel = panel
                    already_synced = True
            except Exception:
                pass
        if not already_synced:
            # v3.1.9 round 21: throttle the inner sync too — when called from
            # `_push_packet_overlays` (5 Hz when menu visible), the outer call
            # site already ran a throttled sync at 0.25 s cadence. Passing
            # `min_interval=0.25` here keeps the same effective rate without
            # duplicating the bridge.get_players() iteration on every call.
            self._sync_session_players_cache(
                getattr(self, '_game_state', None), min_interval=0.25)
        sig = (len(self._session_players), self._session_players_version, self._session_self_uid())
        now = time.time()
        if not force and sig == self._last_session_players_panel_sig:
            return
        if not force and now - self._last_session_players_panel_push_ts < 0.5:
            return
        rows = self._get_session_player_rows(sync=False)
        self._last_session_players_panel_sig = sig
        self._last_session_players_panel_push_ts = now
        try:
            panel.update_rows(rows, force=force)
        except Exception:
            pass

    def _toggle_session_players_panel(self):
        """Show/refresh the in-session player list from the menu item."""
        stack = getattr(self, '_menu_left_stack', None)
        if stack is None:
            return
        try:
            rows = self._get_session_player_rows(sync=True)
            panel = stack.show_session_players(rows=rows, force=True)
            self._session_players_panel = panel
            self._last_session_players_panel_sig = (
                len(self._session_players),
                self._session_players_version,
                self._session_self_uid(),
            )
            self._last_session_players_panel_push_ts = time.time()
        except Exception:
            pass
        self._refresh_menu_if_open(force=True)
