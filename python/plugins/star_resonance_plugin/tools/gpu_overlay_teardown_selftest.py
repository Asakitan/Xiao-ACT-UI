# -*- coding: utf-8 -*-
"""Regression coverage for GPU overlay teardown ordering."""

from __future__ import annotations

import os
import sys
import unittest

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui_modules.sao_child_bar_gpu import ChildBarGpuPainter
from gui_modules.sao_child_bar_gpu import BarColors, _ChildBarSnapshot, _RowSnapshot
from plugins.star_resonance_plugin.panels.sao_gui_bosshp import BossHpOverlay
from plugins.star_resonance_plugin.panels.sao_gui_hp import HpOverlay
from gui_modules.sao_gui_menu_hud import MenuHudOverlay
from plugins.star_resonance_plugin.panels.sao_gui_state_mixin import SAOPlayerGUIStateMixin
from gui_modules.sao_left_info_gpu import LeftInfoGpuPainter, _LeftInfoSnapshot
from plugins.star_resonance_plugin.panels.sao_left_info_gpu import (
    PlayerPanelGpuPainter, SessionPlayersGpuPainter,
    _PlayerPanelSnapshot, _SessionPlayersSnapshot,
)
from gui_modules.sao_menu_bar_gpu import _ButtonSnapshot
from gui_modules.sao_menu_bar_gpu import MenuBarGpuPainter


class _Worker:
    def __init__(self, order):
        self.order = order

    def stop(self) -> None:
        self.order.append("worker.stop")


class _Presenter:
    def __init__(self, order):
        self.order = order
        self.release_count = 0

    def release(self) -> None:
        self.release_count += 1
        self.order.append("presenter.release")

    def render(self):
        return None


class _GpuWindow:
    def __init__(self, order):
        self.order = order
        self.destroy_count = 0

    def destroy(self) -> None:
        self.destroy_count += 1
        self.order.append("gpu_window.destroy")


class _Renderer:
    def __init__(self) -> None:
        self.reset_count = 0

    def reset(self) -> None:
        self.reset_count += 1


class _BossHpMonster:
    def __init__(self, uuid: int, hp: int, max_hp: int, payload: dict) -> None:
        self.uuid = uuid
        self.hp = hp
        self.max_hp = max_hp
        self._payload = dict(payload)

    def to_dict(self) -> dict:
        return dict(self._payload)


class _BossHpBridge:
    def __init__(self, monsters: list[_BossHpMonster]) -> None:
        self._monsters = {m.uuid: m for m in monsters}

    def get_monster(self, uuid: int):
        return self._monsters.get(uuid)


class _BossHpGameState:
    boss_raid_active = False
    boss_hp_source = "packet"


class _BossHpStateOwner(SAOPlayerGUIStateMixin):
    def __init__(self) -> None:
        self._boss_hp_overlay = object()
        self._bb_last_target_uuid = 1001
        self._bb_last_damage_ts = 10.0
        self._bb_recent_targets = {1001: 10.0, 1002: 9.9}
        self._scene_damage_grace_until = 0.0
        self._last_boss_hp_push_sig = None
        self._bb_last_hp_motion_sig = None
        self._bb_last_hp_motion_ts = 0.0
        self._dps_tracker = None
        self._packet_engine = _BossHpBridge([
            _BossHpMonster(
                1001, 900, 1000,
                {
                    "name": "Bad Payload Boss",
                    "shield_active": True,
                    "shield_pct": "bad",
                    "breaking_stage": "bad",
                    "has_break_data": True,
                    "extinction_pct": float("nan"),
                    "extinction": "bad",
                    "max_extinction": float("inf"),
                },
            ),
            _BossHpMonster(
                1002, 500, 1000,
                {
                    "name": "Add",
                    "hp_pct": float("nan"),
                    "extinction_pct": "bad",
                    "has_break_data": True,
                    "breaking_stage": "bad",
                    "shield_active": True,
                    "shield_pct": float("inf"),
                },
            ),
        ])

    def _get_setting(self, key: str, default=None):
        if key == "boss_bar_mode":
            return "boss_raid"
        if key == "boss_hp_stable_hide_s":
            return 2.5
        return default

    def _boss_hp_hold_timeout_s(self) -> float:
        return 5.0

    def _boss_monster_usable(self, monster) -> bool:
        return monster is not None


def _owned_pair():
    order = []
    return order, _Presenter(order), _GpuWindow(order)


class GpuOverlayTeardownTests(unittest.TestCase):
    def _assert_window_owns_presenter(self, obj, presenter_attr: str) -> None:
        order, presenter, gpu_window = _owned_pair()
        if presenter_attr == "_gpu_presenter":
            obj._gpu_presenter = presenter
        else:
            obj._presenter = presenter
        obj._gpu_window = gpu_window
        obj._render_worker = _Worker(order)

        obj.destroy()

        self.assertIn("gpu_window.destroy", order)
        self.assertNotIn("presenter.release", order)
        self.assertEqual(gpu_window.destroy_count, 1)
        self.assertEqual(presenter.release_count, 0)
        self.assertIsNone(obj._gpu_window)
        self.assertIsNone(getattr(obj, presenter_attr))

    def test_hp_overlay_destroys_gpu_window_before_dropping_presenter(self) -> None:
        obj = object.__new__(HpOverlay)
        obj._registered = False
        obj._gpu_managed = True
        obj._hwnd = 123
        obj._win = None
        obj._gpu_drag_active = False
        obj._input_passthrough_enabled = None
        obj._input_region_mode = None
        obj._drag_input_ignored = False
        obj._visible = True
        obj._exiting = True
        obj._hide_after_exit = True

        self._assert_window_owns_presenter(obj, "_gpu_presenter")

    def test_bosshp_overlay_destroys_gpu_window_before_dropping_presenter(self) -> None:
        obj = object.__new__(BossHpOverlay)
        obj._registered = False
        obj._gpu_managed = True
        obj._hwnd = 123
        obj._win = None
        obj._gpu_drag_active = False
        obj._visible = True
        obj._exiting = True
        obj._hide_after_exit = True

        self._assert_window_owns_presenter(obj, "_gpu_presenter")

    def test_left_info_painters_leave_presenter_to_gpu_window_teardown(self) -> None:
        for cls in (LeftInfoGpuPainter, SessionPlayersGpuPainter, PlayerPanelGpuPainter):
            with self.subTest(cls=cls.__name__):
                obj = object.__new__(cls)
                obj._destroyed = False
                self._assert_window_owns_presenter(obj, "_presenter")

    def test_menu_painters_leave_presenter_to_gpu_window_teardown(self) -> None:
        child = object.__new__(ChildBarGpuPainter)
        child._destroyed = False
        self._assert_window_owns_presenter(child, "_presenter")

        bar = object.__new__(MenuBarGpuPainter)
        bar._destroyed = False
        bar._hover_idx = None
        bar._leave_cb = None
        bar._target_geom = (0, 0, 1, 1)
        self._assert_window_owns_presenter(bar, "_presenter")

        hud = object.__new__(MenuHudOverlay)
        hud._destroyed = False
        hud._hwnd = 0
        hud._win = None
        hud._visible = True
        hud._renderer = _Renderer()
        self._assert_window_owns_presenter(hud, "_gpu_presenter")

    def test_presenter_releases_when_gpu_window_was_never_created(self) -> None:
        order = []
        obj = object.__new__(ChildBarGpuPainter)
        obj._destroyed = False
        obj._render_worker = _Worker(order)
        obj._presenter = _Presenter(order)
        obj._gpu_window = None

        obj.destroy()

        self.assertIn("presenter.release", order)
        self.assertEqual(obj._presenter, None)

    def test_session_players_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _SessionPlayersSnapshot(
            rows=[("Alice", "1", "bad", True)],
            total="inf",
            self_uid=123,
            first_index="bad",
            w="nan",
            h=None,
            reveal="bad",
        )

        self.assertEqual(snap.total, 0)
        self.assertEqual(snap.first_index, 0)
        self.assertEqual(snap.w, 1)
        self.assertEqual(snap.h, 1)
        self.assertEqual(snap.reveal, 1.0)
        self.assertEqual(snap.rows[0], ("Alice", "1", "bad", True))

    def test_left_info_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _LeftInfoSnapshot(
            username="Kirito",
            description="Black Swordsman",
            top_w="bad",
            top_h=float("nan"),
            bottom_w=-5,
            bottom_h=None,
            sweep_phase=float("inf"),
            sweep_strength="bad",
        )

        self.assertEqual((snap.top_w, snap.top_h, snap.bottom_w, snap.bottom_h), (1, 1, 1, 1))
        self.assertEqual(snap.sweep_phase, 0.0)
        self.assertEqual(snap.sweep_strength, 0.0)

    def test_player_panel_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _PlayerPanelSnapshot(
            username="Kirito",
            level="nan",
            level_extra="bad",
            season_exp=float("inf"),
            hp=("bad", float("inf")),
            sta=(None, "bad"),
            shift_mode="burst",
            top_w="bad",
            top_h=float("nan"),
            bottom_w=-5,
            bottom_h=None,
            scan_phase=float("inf"),
        )

        self.assertEqual(snap.level, 0)
        self.assertEqual(snap.level_extra, 0)
        self.assertEqual(snap.season_exp, 0)
        self.assertEqual(snap.hp, (0, 0))
        self.assertEqual(snap.sta, (0, 0))
        self.assertEqual((snap.top_w, snap.top_h, snap.bottom_w, snap.bottom_h), (1, 1, 1, 1))
        self.assertEqual(snap.scan_phase, 0.0)

    def test_hp_overlay_update_hp_tolerates_bad_numeric_payload(self) -> None:
        panel = object.__new__(HpOverlay)
        panel._visible = True
        panel._hp_pct_target = 0.5
        panel._hp_flash_start = 0.0
        panel._idle_submit_q = 0
        panel._level = "1"
        panel._schedule_tick = lambda *_args, **_kwargs: None

        panel.update_hp("bad", float("inf"), level="bad-level")

        self.assertEqual(panel._hp_cur, 0.0)
        self.assertEqual(panel._hp_max, 0.0)
        self.assertEqual(panel._hp_pct_target, 1.0)
        self.assertEqual(panel._idle_submit_q, -1)
        self.assertEqual(panel._level, "bad-level")

    def test_hp_overlay_update_sta_tolerates_bad_numeric_payload(self) -> None:
        panel = object.__new__(HpOverlay)
        panel._sta_offline = False
        panel._sta_offline_pending = False
        panel._idle_submit_q = 0
        panel._schedule_tick = lambda *_args, **_kwargs: None

        panel.update_sta("bad", float("nan"))

        self.assertEqual(panel._sta_cur, 0.0)
        self.assertEqual(panel._sta_max, 0.0)
        self.assertEqual(panel._sta_pct_target, 1.0)
        self.assertEqual(panel._sta_text, "0/0")
        self.assertEqual(panel._idle_submit_q, -1)

    def test_child_bar_snapshots_tolerate_bad_numeric_fields(self) -> None:
        row = _RowSnapshot(icon=">", label="Submenu", hover_t="bad", row_w="nan")
        colors = BarColors(
            "#010101",
            "#202020",
            "#ffffff",
            "#ffd27a",
            "#86dfff",
            "#f3af12",
            lambda a, _b, _t: a,
        )
        snap = _ChildBarSnapshot(
            line_w="bad",
            line_h=float("inf"),
            arrow_w=-5,
            fade_t=float("nan"),
            rows=[row],
            bg_hex="#010101",
            colors=colors,
        )

        self.assertEqual(row.hover_t, 0.0)
        self.assertEqual(row.row_w, 1)
        self.assertEqual((snap.line_w, snap.line_h, snap.arrow_w), (1, 1, 1))
        self.assertEqual(snap.fade_t, 0.0)
        self.assertEqual(snap.rows, [row])

    def test_menu_bar_button_snapshot_tolerates_bad_numeric_fields(self) -> None:
        snap = _ButtonSnapshot(size="nan", hover_t=float("inf"), active=True, icon="")

        self.assertEqual(snap.size, 1.0)
        self.assertEqual(snap.hover_t, 0.0)
        self.assertTrue(snap.active)
        self.assertEqual(snap.icon, "●")

    def test_bosshp_update_tolerates_bad_numeric_payload(self) -> None:
        panel = object.__new__(BossHpOverlay)
        panel._gpu_managed = True
        panel._hwnd = 0
        panel._visible = True
        panel._exiting = False
        panel._hide_after_exit = False
        panel._fade_alpha = 1.0
        panel._fade_from = 1.0
        panel._fade_target = 1.0
        panel._fade_start = 0.0
        panel._fade_duration = 0.0
        panel._target_hp_pct = 0.5
        panel._target_trail_pct = 0.5
        panel._trail_pending_time = 0.0
        panel._last_shield_active = False
        panel._last_shield_pct = 0.0
        panel._shield_ghost_pct = 0.0
        panel._break_state = "tracking"
        panel._recover_interpolating = False
        panel._target_break_pct = 0.0
        panel._last_breaking_stage = -1
        panel._last_break_pct = 0.0
        panel._additional_units = []
        panel._idle_committed = True
        panel._trigger_shield_fx = lambda *_args, **_kwargs: None
        panel._begin_recovery = lambda: None
        panel._trigger_recover_fastfill = lambda: None
        panel._update_break_state = lambda *_args, **_kwargs: None
        panel._schedule_tick = lambda *_args, **_kwargs: None

        panel.update({
            "active": True,
            "boss_name": "Bad Payload Boss",
            "hp_pct": "bad",
            "current_hp": "bad",
            "total_hp": float("inf"),
            "shield_active": True,
            "shield_pct": "bad",
            "breaking_stage": "bad",
            "extinction_pct": float("nan"),
            "additional": [
                {"name": "Phase", "hp_pct": "bad", "breaking_stage": "bad"},
            ],
        })

        self.assertEqual(panel._target_hp_pct, 0.0)
        self.assertEqual(panel._current_hp, 0.0)
        self.assertEqual(panel._total_hp, 0.0)
        self.assertEqual(panel._target_shield_pct, 0.0)
        self.assertEqual(panel._breaking_stage, -1)
        self.assertEqual(panel._last_break_pct, 0.0)
        self.assertEqual(panel._additional_units[0]["breaking_stage"], -1)

    def test_bosshp_additional_units_normalize_nonfinite_and_draw_bad_state(self) -> None:
        panel = object.__new__(BossHpOverlay)
        bad_units = [{
            "name": "Phase",
            "hp_pct": float("nan"),
            "extinction_pct": float("inf"),
            "has_break_data": True,
            "breaking_stage": "bad",
            "shield_active": True,
            "shield_pct": "bad",
        }]

        normalized = panel._normalize_additional_units(bad_units)

        self.assertEqual(normalized[0]["hp_pct"], 0.0)
        self.assertEqual(normalized[0]["extinction_pct"], 0.0)
        self.assertEqual(normalized[0]["shield_pct"], 0.0)
        self.assertEqual(normalized[0]["breaking_stage"], -1)

        panel._additional_units = bad_units
        img = Image.new("RGBA", (BossHpOverlay.WIDTH, BossHpOverlay.HEIGHT), (0, 0, 0, 0))
        panel._draw_additional_units(img, 0)

    def test_bosshp_state_delta_tolerates_bad_direct_payload(self) -> None:
        owner = _BossHpStateOwner()

        payload = owner._compute_boss_hp_delta(_BossHpGameState(), 10.0)

        self.assertIsNotNone(payload)
        self.assertEqual(payload["hp_pct"], 0.9)
        self.assertEqual(payload["shield_pct"], 0.0)
        self.assertEqual(payload["breaking_stage"], 0)
        self.assertEqual(payload["extinction_pct"], 0.0)
        self.assertEqual(payload["extinction"], 0)
        self.assertEqual(payload["max_extinction"], 0)
        self.assertEqual(payload["additional"][0]["hp_pct"], 0.0)
        self.assertEqual(payload["additional"][0]["extinction_pct"], 0.0)
        self.assertEqual(payload["additional"][0]["breaking_stage"], -1)
        self.assertEqual(payload["additional"][0]["shield_pct"], 0.0)


if __name__ == "__main__":
    unittest.main()
