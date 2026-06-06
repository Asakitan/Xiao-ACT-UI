# -*- coding: utf-8 -*-
"""
SAOPlayerGUIEngineTogglesMixin — seventh mixin extracted from
SAOPlayerGUI (round 44 of the sao_gui split refactor).

Burst cluster (burst-skill trigger):
  * _pick_burst_trigger_slot — cython state-machine delegate
  * _normalize_watched_skill_slots — cython delegate
  * _reset_burst_tracking_state — clears last_burst_* caches
  * _toggle_burst_enabled — toggles `burst_enabled` setting
  * _toggle_burst_slot — adds/removes a slot from `watched_skill_slots`

NOTE: the auto hide-and-seek cluster used to live here; it has been
extracted into the self-contained ``plugins/hide_seek_plugin`` example.

Required SAOPlayerGUI attrs:
  * self._last_burst_slot, self._last_burst_ready,
    self._last_burst_slot_shown
  * self.root, self._destroyed

Required SAOPlayerGUI methods (via MRO):
  * _show_entity_alert (SAOPlayerGUI)
  * _refresh_menu_if_open (Menu mixin)
  * _get_setting, _set_setting (SAOPlayerGUI)
"""

from __future__ import annotations

from typing import Any, Optional

import _sao_cy_uihelpers as _CY_UI  # type: ignore[import-not-found]


class SAOPlayerGUIEngineTogglesMixin:
    """Mixin bundling Burst engine toggles + state (hide-and-seek is a plugin)."""

    def _pick_burst_trigger_slot(self, gs):
        # v2.4.31: state machine moved to _sao_cy_uihelpers.
        watched = self._get_setting(
            'watched_skill_slots', [1, 2, 3, 4, 5, 6, 7, 8, 9]) or []
        slots = getattr(gs, 'skill_slots', []) or []
        prev_slot = int(getattr(self, '_last_burst_slot', 0) or 0)
        chosen = int(_CY_UI.pick_burst_trigger_slot(slots, watched, prev_slot))
        self._last_burst_slot = chosen
        return chosen

    # ── AutoKey config helpers ──
    def _normalize_watched_skill_slots(self, slots):
        # v2.4.31: cython helper.
        return _CY_UI.normalize_watched_skill_slots(slots)

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

