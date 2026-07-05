# -*- coding: utf-8 -*-
# SAOPlayerGUIEngineTogglesMixin — seventh mixin extracted from
# SAOPlayerGUI (round 44 of the sao_gui split refactor).
#
# Burst cluster (burst-skill trigger):
# * _pick_burst_trigger_slot — cython state-machine delegate
# * _normalize_watched_skill_slots — cython delegate
# * _reset_burst_tracking_state — clears last_burst_* caches
# * _toggle_burst_enabled — toggles `burst_enabled` setting
# * _toggle_burst_slot — adds/removes a slot from `watched_skill_slots`
#
# NOTE: the auto hide-and-seek cluster used to live here; it has been
# extracted into the self-contained ``plugins/hide_seek_plugin`` example.
#
# Required SAOPlayerGUI attrs:
# * self._last_burst_slot, self._last_burst_ready,
# self._last_burst_slot_shown
# * self.root, self._destroyed
#
# Required SAOPlayerGUI methods (via MRO):
# * _show_entity_alert (SAOPlayerGUI)
# * _refresh_menu_if_open (Menu mixin)
# * _get_setting, _set_setting (SAOPlayerGUI)

from __future__ import annotations

from typing import Any, Dict, List, Optional

try:
    import _sao_cy_sr_uihelpers as _CY_UI  # type: ignore[import-not-found]
except ImportError:
    _CY_UI = None  # type: ignore[assignment]


class SAOPlayerGUIEngineTogglesMixin:
    # Mixin bundling Burst engine toggles + state (hide-and-seek is a plugin).

    def _pick_burst_trigger_slot(self, gs):
        watched = self._get_setting(
            'watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        slots = getattr(gs, 'skill_slots', []) or []
        prev_slot = int(getattr(self, '_last_burst_slot', 0) or 0)
        if _CY_UI is not None:
            chosen = int(_CY_UI.pick_burst_trigger_slot(slots, watched, prev_slot))
        else:
            chosen = int(watched[0]) if watched else 1
        self._last_burst_slot = chosen
        return chosen

    def _normalize_watched_skill_slots(self, slots):
        if _CY_UI is not None:
            return _CY_UI.normalize_watched_skill_slots(slots)
        return sorted(set(int(s) for s in (slots or []) if 1 <= int(s) <= 9))

    def _reset_burst_tracking_state(self):
        self._last_burst_ready = False
        self._last_burst_slot = 0
        self._last_burst_slot_shown = 0

    def _toggle_burst_enabled(self):
        cur = bool(self._get_setting('burst_enabled', True))
        self._set_setting('burst_enabled', not cur)
        self._reset_burst_tracking_state()
        self._refresh_menu_if_open()

    def _toggle_burst_slot(self, slot: int):
        watched = self._normalize_watched_skill_slots(
            self._get_setting('watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9])
        )
        if not watched:
            watched = [1]
        watched_set = set(watched)
        if slot in watched_set:
            if len(watched_set) == 1:
                self._show_entity_alert('BURST SKILLS', '至少保留一个技能槽', display_time=3.0)
                return
            watched_set.remove(slot)
        else:
            watched_set.add(slot)
        self._set_setting('watched_skill_slots', sorted(watched_set))
        self._reset_burst_tracking_state()
        self._refresh_menu_if_open()

    # ── Custom Skill CD Monitor ──

    def _get_custom_monitors(self) -> List[dict]:
        raw = self._get_setting('custom_skill_monitors', [])
        return validate_monitors(raw)

    def _set_custom_monitors(self, monitors: List[dict]):
        self._set_setting('custom_skill_monitors', validate_monitors(monitors))
        self._refresh_menu_if_open()

    def _add_custom_monitor(self, skill_level_id: int, *,
                            name: str = '', parent_slot: int = 0,
                            tts_enabled: bool = True,
                            visual_enabled: bool = True) -> bool:
        monitors = self._get_custom_monitors()
        if len(monitors) >= MAX_CUSTOM_SLOTS:
            self._show_entity_alert('CUSTOM CD', f'最多{MAX_CUSTOM_SLOTS}个自定义槽', display_time=3.0)
            return False
        for m in monitors:
            if m['skill_level_id'] == skill_level_id:
                self._show_entity_alert('CUSTOM CD', '该技能已在监控列表中', display_time=3.0)
                return False
        slot = next_available_slot(monitors)
        if slot == 0:
            return False
        sid = skill_level_id // 100
        if not name:
            name = _resolve_skill_name(sid, skill_level_id)
        monitors.append({
            'slot': slot,
            'skill_level_id': skill_level_id,
            'skill_id': sid,
            'name': name,
            'parent_slot': parent_slot,
            'tts_enabled': tts_enabled,
            'tts_text': '',
            'visual_enabled': visual_enabled,
        })
        self._set_custom_monitors(monitors)
        return True

    def _remove_custom_monitor(self, slot: int):
        monitors = self._get_custom_monitors()
        monitors = [m for m in monitors if m['slot'] != slot]
        self._set_custom_monitors(monitors)

    def _update_custom_monitor(self, slot: int, **kwargs):
        monitors = self._get_custom_monitors()
        for m in monitors:
            if m['slot'] == slot:
                for k, v in kwargs.items():
                    if k in m:
                        m[k] = v
                break
        self._set_custom_monitors(monitors)

    def _get_available_skills_for_picker(self) -> List[dict]:
        bridge = getattr(self, '_packet_bridge', None)
        player = getattr(bridge, '_player', None) if bridge else None
        cd_map: dict = {}
        seen: list = []
        equipped: set = set()
        if player:
            cd_map = getattr(player, 'skill_cd_map', {}) or {}
            seen = getattr(player, 'skill_seen_ids', []) or []
            slot_map = getattr(player, 'skill_slot_map', {}) or {}
            equipped = set(slot_map.values())
        return enumerate_available_skills(cd_map, seen, equipped)

    def _compute_custom_slots(self, gs=None) -> List[dict]:
        monitors = self._get_custom_monitors()
        if not monitors:
            return []
        bridge = getattr(self, '_packet_bridge', None)
        player = getattr(bridge, '_player', None) if bridge else None
        cd_map: dict = {}
        player_attrs: dict = {}
        server_offset: Optional[float] = None
        if player:
            cd_map = getattr(player, 'skill_cd_map', {}) or {}
            server_offset = getattr(player, 'server_time_offset_ms', None)
            player_attrs = {
                'attr_skill_cd': getattr(player, 'attr_skill_cd', 0),
                'attr_skill_cd_pct': getattr(player, 'attr_skill_cd_pct', 0),
                'attr_cd_accelerate_pct': getattr(player, 'attr_cd_accelerate_pct', 0),
                'temp_attr_cd_pct': getattr(player, 'temp_attr_cd_pct', 0),
                'temp_attr_cd_fixed': getattr(player, 'temp_attr_cd_fixed', 0),
                'temp_attr_cd_accel': getattr(player, 'temp_attr_cd_accel', 0),
            }
        if gs is None and hasattr(self, '_state_mgr') and self._state_mgr:
            gs = self._state_mgr.state
        prev = getattr(gs, 'custom_skill_slots', []) if gs else []
        return compute_custom_skill_slots(
            monitors, cd_map, server_offset, player_attrs, prev)

    def _open_skill_picker(self):
        # 技能选择器 — 游戏插件覆盖此方法。
        pass

