# -*- coding: utf-8 -*-
"""Regression coverage for entity packet callback payload tolerance."""

from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from plugins.star_resonance_plugin.panels.sao_gui_damage_events_mixin import SAOPlayerGUIDamageEventsMixin
from plugins.star_resonance_plugin.panels.sao_gui_packet_callbacks_mixin import SAOPlayerGUIPacketCallbacksMixin


class _StateMgr:
    def __init__(self) -> None:
        self.updates = []

    def update(self, **kwargs) -> None:
        self.updates.append(dict(kwargs))


class _EncounterMgr:
    def __init__(self) -> None:
        self.dungeon_events = []
        self.pending = []

    def on_dungeon_event(self, event) -> None:
        self.dungeon_events.append(dict(event))

    def arm_pending_reset(self, reason, delay_s=0.0) -> None:
        self.pending.append((reason, delay_s))


class _PacketOwner(SAOPlayerGUIPacketCallbacksMixin):
    def __init__(self) -> None:
        self._state_mgr = _StateMgr()
        self._encounter_mgr = _EncounterMgr()
        self.banners = []

    def _schedule_map_banner(self, name: str) -> None:
        self.banners.append(name)


class _FloatOwner(SAOPlayerGUIDamageEventsMixin):
    def __init__(self) -> None:
        self._encounter_mgr = _EncounterMgr()
        self._scene_damage_grace_until = "bad"
        self._last_boss_hp_push_sig = "stale"
        self._dps_tracker = None


class EntityPacketCallbackTests(unittest.TestCase):
    def test_dungeon_event_keeps_valid_scene_when_numeric_fields_are_bad(self) -> None:
        owner = _PacketOwner()
        published = []

        with mock.patch(
            "plugins.star_resonance_plugin.panels.sao_gui_packet_callbacks_mixin.publish_owner_event",
            side_effect=lambda *args, **kwargs: published.append((args, kwargs)),
        ):
            owner._on_dungeon_event({
                "kind": "sync_dungeon_data",
                "dungeon_id": "bad",
                "scene_id": 42001,
                "dungeon_difficulty": float("nan"),
                "dungeon_name": "Demo Scene",
            })

        self.assertTrue(owner._state_mgr.updates)
        self.assertEqual(owner._state_mgr.updates[-1]["dungeon_scene_id"], 42001)
        self.assertNotIn("dungeon_id", owner._state_mgr.updates[-1])
        self.assertNotIn("dungeon_difficulty", owner._state_mgr.updates[-1])
        self.assertEqual(owner.banners, ["Demo Scene"])
        self.assertTrue(owner._encounter_mgr.dungeon_events)
        self.assertTrue(any(args[1] == "scene" for args, _kwargs in published))

    def test_arm_pending_combat_reset_tolerates_bad_existing_grace(self) -> None:
        owner = _FloatOwner()

        owner._arm_pending_combat_reset({
            "kind": "same_instance_restart",
            "reset_delay_s": 1.5,
        })

        self.assertGreater(owner._pending_combat_reset_after, 0.0)
        self.assertEqual(owner._pending_combat_reset_reason, "same_instance_restart")
        self.assertGreater(owner._scene_damage_grace_until, owner._pending_combat_reset_after)
        self.assertIsNone(owner._last_boss_hp_push_sig)
        self.assertEqual(owner._encounter_mgr.pending[-1], ("same_instance_restart", 1.5))


if __name__ == "__main__":
    unittest.main()
