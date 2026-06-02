# -*- coding: utf-8 -*-
"""Smoke test for the offline ACT replay / TCP parser contract.

Run from ``sao_auto``:

    python -m act_replay.selftest

This is intentionally not under ``tests/`` because the repository currently
ignores that directory.  It is a lightweight validation command for agents and
developers working on the TCP-first ACT stack.
"""

from __future__ import annotations

import json
from types import SimpleNamespace

from engines.act_trigger_engine import ActTriggerEngine
from engines.dps_tracker import DpsTracker
from engines.encounter_manager import EncounterManager
from engines.game_state import GameStateManager
from gui_modules.sao_gui_dps_theme_mixin import SAOPlayerGUIDpsThemeMixin
from gui_modules.sao_gui_packet_callbacks_mixin import SAOPlayerGUIPacketCallbacksMixin
from packet_parser.enums import NotifyMethod
from packet_parser.parser import PacketParser
from sao_webview import SAOWebViewGUI
import packet_parser.parser as parser_mod

from .events import boss_event, boss_state_event, damage_event, dungeon_event, monster_update_event, skill_event
from .harness import ActReplayHarness


class _FakePb:
    """Tiny protobuf stand-in for parser handler contract checks."""

    def SyncClientUseSkill(self):
        return SimpleNamespace(
            SkillTargetUuid=987654321064,
            SkillLevelId=110101,
            ParseFromString=lambda _data: None,
        )

    def SyncServerSkillEnd(self):
        return SimpleNamespace(
            SkillUuid=777,
            ParseFromString=lambda _data: None,
        )

    def SyncServerSkillStageEnd(self):
        return SimpleNamespace(
            SkillStageEndInfo=SimpleNamespace(
                SkillUuid=777,
                StageId=1,
                NewStageId=2,
                ConditionId=0,
            ),
            ParseFromString=lambda _data: None,
        )

    def SyncDungeonData(self):
        target_data = {
            1: SimpleNamespace(TargetId=1302101, Nums=1, Complete=0),
        }
        vdata = SimpleNamespace(
            SceneUuid=42001,
            DungeonSceneInfo=SimpleNamespace(Difficulty=3),
            Target=SimpleNamespace(TargetData=target_data),
            HasField=lambda name: name in {"DungeonSceneInfo", "Target"},
        )
        return SimpleNamespace(
            VData=vdata,
            ParseFromString=lambda _data: None,
        )

    def SyncDungeonDirtyData(self):
        return SimpleNamespace(
            VData=SimpleNamespace(Buffer=b"dirty-buffer"),
            ParseFromString=lambda _data: None,
        )

    def EnterScene(self):
        player_ent = SimpleNamespace(
            Uuid=(36668136 << 16) | 640,
            HasField=lambda name: False,
        )
        info = SimpleNamespace(
            SceneAttrs=object(),
            SubsceneAttrs=object(),
            PlayerEnt=player_ent,
            SceneGuid="demo-scene",
            ConnectGuid="demo-connect",
            HasField=lambda name: name == "PlayerEnt",
        )
        return SimpleNamespace(
            EnterSceneInfo=info,
            ParseFromString=lambda _data: None,
        )


class _FakeDpsActGui(SAOPlayerGUIDpsThemeMixin):
    """No-Tk stand-in for Entity/Tk ACT snapshot source-priority checks."""

    def __init__(self) -> None:
        self._dps_tracker = None
        self._dps_history_store = None
        self._state_mgr = GameStateManager()
        self._encounter_mgr = None
        self._packet_engine = {"data_source": "packet", "running": True}
        self._mem_bridge = {"data_source": "memory", "running": True}


class _FakeRuntimeActGui(SAOPlayerGUIPacketCallbacksMixin, SAOPlayerGUIDpsThemeMixin):
    """No-Tk stand-in for Entity/Tk packet callback → ACT snapshot checks."""

    def __init__(self) -> None:
        self._dps_tracker = DpsTracker()
        self._dps_history_store = None
        self._state_mgr = GameStateManager()
        self._encounter_mgr = EncounterManager()
        self._packet_engine = {"data_source": "packet", "running": True}
        self._mem_bridge = None
        self._boss_raid_engine = None
        self._bb_recent_targets = {}
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._bb_last_hp_motion_ts = 0.0
        self._scene_hide_token = 0
        self._last_boss_hp_push_sig = None
        self._scene_combat_reset_pending = False
        self._scene_combat_reset_anchor = {}
        self._pending_combat_reset_deadline = 0.0
        self._dps_overlay = None
        self.push_count = 0
        self.last_snapshot = {}

    def _cancel_dps_idle_reset_after(self):
        pass

    def _maybe_apply_pending_combat_reset(self, _event, _is_self_combat_target):
        pass

    def _normalize_damage_event_for_self(self, event):
        return event

    def _normalize_damage_event_target_for_entity(self, event):
        return event

    def _is_known_friendly_uid(self, _uid):
        return False

    def _push_dps_act_snapshot(self):
        self.push_count += 1
        self.last_snapshot = self._get_dps_act_snapshot()


class _FakeWebViewRuntimeActGui:
    """No-window stand-in for WebView packet callback → ACT snapshot checks."""

    def __init__(self) -> None:
        self._dps_tracker = DpsTracker()
        self._dps_history_store = None
        self._state_mgr = GameStateManager()
        self._encounter_mgr = EncounterManager()
        self._packet_engine = {"data_source": "packet", "running": True}
        self._mem_bridge = None
        self._boss_raid_engine = None
        self._bb_recent_targets = {}
        self._bb_last_target_uuid = 0
        self._bb_last_damage_ts = 0.0
        self._pending_combat_reset_after = 0.0
        self._pending_combat_reset_reason = ''
        self.push_count = 0
        self.last_snapshot = {}

    def _current_player_uid_int(self):
        return 36668136

    def _normalize_damage_event_for_self(self, event):
        return event

    def _normalize_damage_event_target_for_webview(self, event):
        return event

    def _maybe_apply_pending_combat_reset(self, _event, _is_self_combat_target):
        pass

    def _push_dps_act_snapshot(self):
        self.push_count += 1
        self.last_snapshot = self._build_dps_act_snapshot()

    def _build_dps_act_snapshot(self, history_limit: int = 20):
        return SAOWebViewGUI._build_dps_act_snapshot(self, history_limit)


class _MemoryHistoryStore:
    """Tiny in-memory report store for encounter finalize contract checks."""

    def __init__(self) -> None:
        self.reports = []

    def add_report(self, report):
        self.reports.append(dict(report or {}))

    def latest_report(self):
        return dict(self.reports[-1]) if self.reports else None

    def list_reports(self, _limit=20):
        return [dict(report) for report in self.reports]


def build_demo_events():
    self_uid = 36668136
    target_uuid = 987654321064
    return self_uid, [
        dungeon_event(
            "enter_scene",
            scene_id=155001,
            scene_guid="demo-scene",
            connect_guid="demo-connect",
            player_uid=self_uid,
        ),
        dungeon_event(
            "start_dungeon",
            dungeon_id=42001,
            difficulty=3,
        ),
        skill_event(
            "client_use",
            method_id=0x3002,
            skill_level_id=110101,
            target_uuid=target_uuid,
            caster_uid=self_uid,
        ),
        skill_event(
            "server_stage_end",
            method_id=0x3004,
            skill_uuid=777,
            stage_id=1,
            new_stage_id=2,
            condition_id=0,
        ),
        boss_state_event(
            boss_current_hp=900000,
            boss_total_hp=1000000,
            boss_raid_active=True,
            boss_breaking_stage=0,
            boss_extinction_pct=0.25,
        ),
        boss_event(
            event_type=101,
            host_uuid=target_uuid,
            buff_uuid=888001,
            stage="shield_break",
            label="Demo boss buff",
        ),
        damage_event(
            attacker_uid=self_uid,
            attacker_uuid=(self_uid << 16) | 640,
            attacker_is_self=True,
            target_uuid=target_uuid,
            target_is_player=False,
            target_is_monster=True,
            target_is_combat_target=True,
            skill_id=1101,
            skill_key=1101,
            damage=123456,
            is_crit=True,
        ),
        skill_event(
            "server_end",
            method_id=0x3005,
            skill_uuid=777,
        ),
    ]


def _assert_game_state_contract() -> None:
    state_mgr = GameStateManager()
    dungeon = dungeon_event(
        "sync_dungeon_data",
        dungeon_id=42001,
        scene_uuid=42001,
        dungeon_difficulty=3,
    )
    skill = skill_event(
        "server_end",
        method_id=NotifyMethod.SYNC_SERVER_SKILL_END,
        skill_uuid=777,
    )
    state_mgr.update(
        dungeon_id=42001,
        dungeon_scene_id=42001,
        dungeon_difficulty=3,
        last_dungeon_event=dungeon,
        last_skill_event=skill,
    )
    dungeon["kind"] = "mutated_after_update"
    skill["kind"] = "mutated_after_update"
    context = state_mgr.snapshot().to_dict()
    assert context["dungeon_id"] == 42001, context
    assert context["dungeon_scene_id"] == 42001, context
    assert context["dungeon_difficulty"] == 3, context
    assert context["last_dungeon_event"].get("kind") == "sync_dungeon_data", context
    assert context["last_skill_event"].get("kind") == "server_end", context


def _assert_parser_handler_contract() -> None:
    skill_events = []
    dungeon_events = []
    self_uid = 36668136
    fake_pb = _FakePb()
    old_ensure_pb = parser_mod._ensure_pb
    old_decode_fields = parser_mod._decode_fields
    old_parse_dungeon_dirty_buffer = parser_mod._parse_dungeon_dirty_buffer
    old_extract_scene_basic_id = parser_mod._extract_scene_basic_id_from_attrs
    old_is_player = parser_mod._is_player
    old_uuid_to_uid = parser_mod._uuid_to_uid
    parser_mod._ensure_pb = lambda: fake_pb
    parser_mod._decode_fields = lambda _data: {1: [42001]}
    parser_mod._parse_dungeon_dirty_buffer = lambda _data: (
        2,
        [{"target_id": 1302101, "nums": 2, "complete": 1}],
    )
    parser_mod._extract_scene_basic_id_from_attrs = lambda _attrs: 155001
    parser_mod._is_player = lambda _uuid: True
    parser_mod._uuid_to_uid = lambda _uuid: self_uid
    try:
        parser = PacketParser(
            on_self_update=lambda _player: None,
            on_skill_event=skill_events.append,
            on_dungeon_event=dungeon_events.append,
            preferred_uid=self_uid,
        )
        parser._current_uuid = (self_uid << 16) | 640
        parser._on_enter_scene(b"")
        parser._on_notify_start_playing_dungeon(b"")
        parser._on_sync_client_use_skill(b"")
        parser._on_sync_server_skill_stage_end(b"")
        parser._on_sync_server_skill_end(b"")
        parser._on_sync_dungeon_data(b"")
        parser._on_sync_dungeon_dirty_data(b"")
    finally:
        parser_mod._ensure_pb = old_ensure_pb
        parser_mod._decode_fields = old_decode_fields
        parser_mod._parse_dungeon_dirty_buffer = old_parse_dungeon_dirty_buffer
        parser_mod._extract_scene_basic_id_from_attrs = old_extract_scene_basic_id
        parser_mod._is_player = old_is_player
        parser_mod._uuid_to_uid = old_uuid_to_uid

    assert [event.get("kind") for event in skill_events] == [
        "client_use",
        "server_stage_end",
        "server_end",
    ], skill_events
    client_use, stage_end, server_end = skill_events
    assert client_use["source"] == "tcp", client_use
    assert client_use["method_id"] == NotifyMethod.SYNC_CLIENT_USE_SKILL, client_use
    assert client_use["skill_level_id"] == 110101, client_use
    assert client_use["target_uuid"] == 987654321064, client_use
    assert client_use["caster_uid"] == self_uid, client_use
    assert stage_end["method_id"] == NotifyMethod.SYNC_SERVER_SKILL_STAGE_END, stage_end
    assert stage_end["skill_uuid"] == 777, stage_end
    assert stage_end["stage_id"] == 1, stage_end
    assert stage_end["new_stage_id"] == 2, stage_end
    assert server_end["method_id"] == NotifyMethod.SYNC_SERVER_SKILL_END, server_end
    assert server_end["skill_uuid"] == 777, server_end

    assert [event.get("kind") for event in dungeon_events] == [
        "enter_scene",
        "start_playing_dungeon",
        "sync_dungeon_data",
        "sync_dungeon_dirty_data",
    ], dungeon_events
    enter_scene, start_dungeon, dungeon, dirty = dungeon_events
    assert enter_scene["source"] == "tcp", enter_scene
    assert enter_scene["scene_id"] == 155001, enter_scene
    assert enter_scene["scene_guid"] == "demo-scene", enter_scene
    assert enter_scene["connect_guid"] == "demo-connect", enter_scene
    assert enter_scene["player_uid"] == self_uid, enter_scene
    assert start_dungeon["source"] == "tcp", start_dungeon
    assert start_dungeon["dungeon_id"] == 42001, start_dungeon
    assert dungeon["source"] == "tcp", dungeon
    assert dungeon["dungeon_id"] == 42001, dungeon
    assert dungeon["scene_uuid"] == 42001, dungeon
    assert dungeon["dungeon_difficulty"] == 3, dungeon
    assert dungeon["targets"] == [{"target_id": 1302101, "nums": 1, "complete": 0}], dungeon
    assert dirty["source"] == "tcp", dirty
    assert dirty["dungeon_id"] == 42001, dirty
    assert dirty["flow_state"] == 2, dirty
    assert dirty["targets"] == [{"target_id": 1302101, "nums": 2, "complete": 1}], dirty

    harness = ActReplayHarness()
    snap = harness.replay([*dungeon_events, *skill_events])
    context = snap.get("context") or {}
    assert context["dungeon_id"] == 42001, context
    assert context["dungeon_scene_id"] == 42001, context
    assert context["dungeon_difficulty"] == 3, context
    assert context["last_dungeon_event"].get("kind") == "sync_dungeon_dirty_data", context
    assert context["last_skill_event"].get("kind") == "server_end", context


def _assert_entity_act_source_priority() -> None:
    gui = _FakeDpsActGui()
    snap = gui._get_dps_act_snapshot()
    assert snap.get("sources", {}).get("packet", {}).get("data_source") == "packet", snap
    assert snap.get("sources", {}).get("memory", {}).get("data_source") == "memory", snap
    assert snap.get("sources", {}).get("summary", {}).get("data_source") == "packet+memory", snap
    assert snap.get("sources", {}).get("summary", {}).get("hybrid") is True, snap
    gui._packet_engine = None
    snap = gui._get_dps_act_snapshot()
    assert "packet" not in snap.get("sources", {}), snap
    assert snap.get("sources", {}).get("memory", {}).get("data_source") == "memory", snap
    assert snap.get("sources", {}).get("summary", {}).get("data_source") == "memory", snap


def _assert_entity_damage_callback_act_contract() -> None:
    gui = _FakeRuntimeActGui()
    event = damage_event(
        attacker_uid=36668136,
        attacker_uuid=(36668136 << 16) | 640,
        attacker_is_self=True,
        target_uuid=987654321064,
        target_is_player=False,
        target_is_monster=True,
        target_is_combat_target=True,
        skill_id=1101,
        skill_key=1101,
        damage=32100,
    )
    gui._on_packet_damage(event)
    assert gui.push_count == 1, gui.push_count
    snap = gui.last_snapshot
    assert snap.get("live", {}).get("total_damage") == 32100, snap
    assert snap.get("encounter", {}).get("status") == "active", snap
    assert snap.get("encounter", {}).get("last_damage_event", {}).get("damage") == 32100, snap
    assert snap.get("render_spec", {}).get("mode") == "live", snap
    assert snap.get("render_spec", {}).get("totals", {}).get("damage") == 32100, snap
    assert snap.get("render_spec", {}).get("sources", {}).get("summary", {}).get("data_source") == "packet", snap


def _assert_webview_damage_callback_act_contract() -> None:
    gui = _FakeWebViewRuntimeActGui()
    event = damage_event(
        attacker_uid=36668136,
        attacker_uuid=(36668136 << 16) | 640,
        attacker_is_self=True,
        target_uuid=987654321064,
        target_is_player=False,
        target_is_monster=True,
        target_is_combat_target=True,
        skill_id=1101,
        skill_key=1101,
        damage=65400,
    )
    SAOWebViewGUI._on_packet_damage(gui, event)
    assert gui.push_count == 1, gui.push_count
    snap = gui.last_snapshot
    assert snap.get("live", {}).get("total_damage") == 65400, snap
    assert snap.get("encounter", {}).get("status") == "active", snap
    assert snap.get("encounter", {}).get("last_damage_event", {}).get("damage") == 65400, snap
    assert snap.get("render_spec", {}).get("mode") == "live", snap
    assert snap.get("render_spec", {}).get("totals", {}).get("damage") == 65400, snap
    assert snap.get("render_spec", {}).get("sources", {}).get("summary", {}).get("data_source") == "packet", snap


def _assert_encounter_finalize_contract() -> None:
    self_uid, events = build_demo_events()
    store = _MemoryHistoryStore()
    harness = ActReplayHarness(history_store=store)
    harness.encounter_mgr.register_finalized_hook(store.add_report)
    harness.set_self_uid(self_uid)
    harness.replay(events)
    assert harness.encounter_mgr.finalize_if_idle(now=events[-1]["timestamp"] + 60.0), store.reports
    snap = harness.snapshot()
    latest = snap.get("last_report") or {}
    history = snap.get("history") or []
    encounter = snap.get("encounter") or {}
    assert latest.get("status") == "finalized", latest
    assert latest.get("report_reason") == "idle_timeout", latest
    assert latest.get("dungeon_id") == 42001, latest
    assert latest.get("dungeon_scene_id") == 155001, latest
    assert latest.get("boss_uuid") == 987654321064, latest
    assert latest.get("damage_events") == 1, latest
    assert history and history[-1].get("report_id") == latest.get("report_id"), history
    assert encounter.get("status") == "idle", encounter
    assert encounter.get("last_final_report_id") == latest.get("report_id"), encounter


def _assert_monster_update_act_contract() -> None:
    self_uid = 36668136
    target_uuid = 987654321064
    harness = ActReplayHarness()
    harness.set_self_uid(self_uid)
    snap = harness.replay([
        monster_update_event(
            uuid=target_uuid,
            name="Demo Boss",
            hp=750000,
            max_hp=1000000,
            has_break_data=True,
            breaking_stage=1,
            extinction_pct=0.4,
            shield_active=True,
            shield_pct=0.2,
        ),
        damage_event(
            attacker_uid=self_uid,
            attacker_uuid=(self_uid << 16) | 640,
            attacker_is_self=True,
            target_uuid=target_uuid,
            target_is_player=False,
            target_is_monster=True,
            target_is_combat_target=True,
            skill_id=1101,
            skill_key=1101,
            damage=1000,
        ),
    ])
    context = snap.get("context") or {}
    render_boss = (snap.get("render_spec") or {}).get("boss") or {}
    assert context["boss_current_hp"] == 750000, context
    assert context["boss_total_hp"] == 1000000, context
    assert context["boss_hp_est_pct"] == 0.75, context
    assert context["boss_hp_source"] == "packet", context
    assert context["boss_raid_active"] is False, context
    assert context["boss_breaking_stage"] == 1, context
    assert context["boss_extinction_pct"] == 0.4, context
    assert context["boss_shield_active"] is True, context
    assert context["boss_shield_pct"] == 0.2, context
    assert render_boss["active"] is True, render_boss
    assert render_boss["current_hp"] == 750000, render_boss
    assert render_boss["total_hp"] == 1000000, render_boss
    assert render_boss["hp_pct"] == 0.75, render_boss
    assert render_boss["hp_source"] == "packet", render_boss
    assert render_boss["breaking_stage"] == 1, render_boss
    assert render_boss["extinction_pct"] == 0.4, render_boss
    assert render_boss["shield_active"] is True, render_boss
    assert render_boss["shield_pct"] == 0.2, render_boss


def main() -> int:
    _assert_game_state_contract()
    _assert_parser_handler_contract()
    _assert_entity_act_source_priority()
    _assert_entity_damage_callback_act_contract()
    _assert_webview_damage_callback_act_contract()
    _assert_encounter_finalize_contract()
    _assert_monster_update_act_contract()
    self_uid, events = build_demo_events()
    target_uuid = int(next(
        (e.get("target_uuid") for e in events if e.get("kind") == "damage"), 0) or 0)
    trigger_engine = ActTriggerEngine([
        {
            "id": "demo_damage_alert",
            "type": "damage_total",
            "threshold": 100000,
            "label": "Demo damage threshold",
            "message": "Damage crossed 100k",
        },
        {
            "id": "demo_skill_end",
            "type": "skill_kind",
            "match": "server_end",
            "label": "Skill ended",
            "message": "Server skill lifecycle ended",
        },
        {
            "id": "demo_boss_hp_alert",
            "type": "boss_hp_pct_below",
            "threshold": 0.95,
            "label": "Boss HP below 95%",
            "message": "Boss HP crossed 95%",
        },
        {
            "id": "demo_boss_event_alert",
            "type": "boss_event_type",
            "match": 101,
            "label": "Boss event observed",
            "message": "Boss buff event propagated into ACT",
        },
    ])
    harness = ActReplayHarness(
        trigger_engine=trigger_engine,
        source_probe={"data_source": "replay", "running": True, "error_msg": ""},
    )
    harness.set_self_uid(self_uid)
    snap = harness.replay(events)
    snap = harness.assert_minimal_act_contract()
    context = snap["context"]
    live = snap["live"] or {}
    state = harness.state_mgr.snapshot().to_dict()
    assert context["dungeon_scene_id"] == 155001, context
    assert context["dungeon_id"] == 42001, context
    assert context["last_skill_event"].get("kind") == "server_end", context
    assert context["boss_current_hp"] == 900000, context
    assert context["boss_total_hp"] == 1000000, context
    assert context["boss_hp_est_pct"] == 0.9, context
    assert context["boss_hp_source"] == "tcp", context
    assert context["last_boss_event"].get("event_type") == 101, context
    assert context["last_boss_event"].get("host_uuid") == target_uuid, context
    assert state["boss_current_hp"] == 900000, state
    assert state["boss_total_hp"] == 1000000, state
    assert state["boss_hp_est_pct"] == 0.9, state
    assert state["boss_hp_source"] == "tcp", state
    assert state["boss_raid_active"] is True, state
    assert state["boss_breaking_stage"] == 0, state
    assert state["boss_extinction_pct"] == 0.25, state
    encounter = snap.get("encounter") or {}
    render_spec = snap.get("render_spec") or {}
    triggers = snap.get("triggers") or {}
    sources = snap.get("sources") or {}
    assert encounter.get("status") == "active", encounter
    assert encounter.get("damage_events") == 1, encounter
    assert encounter.get("boss_uuid") == target_uuid, encounter
    assert encounter.get("dungeon_id") == 42001, encounter
    assert encounter.get("dungeon_scene_id") == 155001, encounter
    assert encounter.get("dungeon_difficulty") == 3, encounter
    assert encounter.get("last_dungeon_event", {}).get("kind") == "start_dungeon", encounter
    assert render_spec.get("mode") == "live", render_spec
    assert render_spec.get("version") == 1, render_spec
    assert render_spec.get("parity_targets") == ["webview", "entity"], render_spec
    assert render_spec.get("context", {}).get("dungeon_id") == 42001, render_spec
    assert render_spec.get("context", {}).get("dungeon_scene_id") == 155001, render_spec
    assert render_spec.get("context", {}).get("last_skill_kind") == "server_end", render_spec
    assert render_spec.get("context", {}).get("last_boss_event_type") == 101, render_spec
    assert render_spec.get("context", {}).get("last_boss_host_uuid") == target_uuid, render_spec
    assert render_spec.get("totals", {}).get("damage") == 123456, render_spec
    rows = render_spec.get("rows") or []
    assert len(rows) == 1, render_spec
    assert rows[0].get("uid") == self_uid, rows
    assert rows[0].get("damage") == 123456, rows
    assert rows[0].get("is_self") is True, rows
    assert render_spec.get("boss", {}).get("current_hp") == 900000, render_spec
    assert render_spec.get("boss", {}).get("total_hp") == 1000000, render_spec
    assert render_spec.get("boss", {}).get("hp_pct") == 0.9, render_spec
    assert render_spec.get("boss", {}).get("hp_source") == "tcp", render_spec
    assert render_spec.get("boss", {}).get("active") is True, render_spec
    assert render_spec.get("boss", {}).get("breaking_stage") == 0, render_spec
    assert render_spec.get("boss", {}).get("extinction_pct") == 0.25, render_spec
    assert sources.get("packet", {}).get("data_source") == "replay", sources
    assert render_spec.get("sources", {}).get("packet", {}).get("running") is True, render_spec
    trigger_events = (triggers.get("emitted") or triggers.get("recent") or [])
    trigger_rule_ids = {str(event.get("rule_id") or "") for event in trigger_events}
    assert len(trigger_events) >= 4, triggers
    assert "demo_damage_alert" in trigger_rule_ids, triggers
    assert "demo_skill_end" in trigger_rule_ids, triggers
    assert "demo_boss_hp_alert" in trigger_rule_ids, triggers
    assert "demo_boss_event_alert" in trigger_rule_ids, triggers
    assert live.get("total_damage", 0) == 123456, live
    assert live.get("entities"), live
    print(json.dumps({
        "ok": True,
        "game_state_contract": True,
        "parser_handler_contract": True,
        "entity_act_source_priority": True,
        "entity_damage_callback_act_contract": True,
        "webview_damage_callback_act_contract": True,
        "encounter_finalize_contract": True,
        "monster_update_act_contract": True,
        "boss_event_act_contract": True,
        "dual_ui_act_meta_bridge": True,
        "dungeon_id": context.get("dungeon_id"),
        "dungeon_scene_id": context.get("dungeon_scene_id"),
        "last_skill_kind": context.get("last_skill_event", {}).get("kind"),
        "last_boss_event_type": context.get("last_boss_event", {}).get("event_type"),
        "boss_hp_source": state.get("boss_hp_source"),
        "boss_hp_est_pct": state.get("boss_hp_est_pct"),
        "render_boss_hp_source": render_spec.get("boss", {}).get("hp_source"),
        "total_damage": live.get("total_damage"),
        "entities": len(live.get("entities") or []),
        "encounter_status": encounter.get("status"),
        "encounter_damage_events": encounter.get("damage_events"),
        "render_mode": render_spec.get("mode"),
        "trigger_events": len(trigger_events),
        "trigger_rule_ids": sorted(trigger_rule_ids),
        "data_source": sources.get("packet", {}).get("data_source"),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())