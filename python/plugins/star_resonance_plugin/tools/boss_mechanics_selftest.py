# -*- coding: utf-8 -*-
# Selftest: boss raid profile mechanics — schema v2 normalize, cast/buff/event/
# hp/time matching, cooldown dedup, binding inbox, enrage milestones, and the
# linkage mechanic trigger + inline dodge dispatch.
from __future__ import annotations

import os
import json
import sys
import time
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.boss_raid_engine import (                        # noqa: E402
    BossRaidEngine, normalize_profile, clone_profile,
    bind_mechanic_skill, unbind_mechanic_skill,
)
from plugins.star_resonance_plugin.engines.boss_autokey_linkage import (                    # noqa: E402
    BossAutoKeyLinkage, normalize_mapping,
)
from plugins.star_resonance_plugin.engines.game_state import GameStateManager               # noqa: E402


class _Settings:
    def __init__(self, data=None):
        self._d = dict(data or {})

    def get(self, k, default=None):
        return self._d.get(k, default)

    def set(self, k, v):
        self._d[k] = v

    def save(self):
        pass


def _mech(mid="m1", skill_ids=(555,), **kw):
    base = {
        "id": mid, "name": "测试机制", "enabled": True,
        "detect": {"skill_ids": list(skill_ids)},
        "alert": {"tts_text": "测试播报", "banner_text": "测试横幅",
                  "countdown_s": 8, "pre_warn_s": 3, "cooldown_s": 5},
        "dodge": {"enabled": True,
                  "inline": {"action_key": "SPACE", "lead_ms": 1000,
                             "cooldown_s": 0.5}},
    }
    for k, v in kw.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            base[k].update(v)
        else:
            base[k] = v
    return base


def _profile(mechanics, **kw):
    p = {"id": "boss_t", "profile_name": "T", "enrage_time_s": 600,
         "phases": [{"id": "ph1", "name": "P1",
                     "trigger": {"type": "manual", "value": 0}}],
         "mechanics": mechanics}
    p.update(kw)
    return p


def _load_example_profile_by_id(profile_id):
    root = os.path.join(_ROOT, "assets", "boss_raids")
    for name in sorted(os.listdir(root)):
        if not (name.startswith("13023_") and name.endswith("_机制示例.json")):
            continue
        path = os.path.join(root, name)
        with open(path, "r", encoding="utf-8") as f:
            payload = json.load(f)
        profile = payload.get("profile") or {}
        if profile.get("id") == profile_id:
            return profile
    raise AssertionError("missing boss raid example profile: %s" % profile_id)


def _cast(skill_id, base_id=90001, dur=3000, edge="start"):
    return {"boss_base_id": base_id, "boss_uuid": 1000, "skill_id": skill_id,
            "skill_name": "", "cast_edge": edge, "cast_duration_ms": dur}


def _monster(uuid, buffs, **kw):
    md = {
        "uuid": uuid, "hp": 1000, "max_hp": 1000, "template_id": 90001,
        "name": "测试Boss", "breaking_stage": -1, "in_overdrive": False,
        "stunned": 0, "extinction_pct": 0.0,
        "buff_list": [{"buff_id": b, "duration": d} for b, d in buffs],
    }
    md.update(kw)
    return md


class MechanicsTest(unittest.TestCase):
    def setUp(self):
        import tempfile
        fd, self._store_path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        os.remove(self._store_path)
        os.environ["SAO_BOSS_SKILL_STORE"] = self._store_path
        self.actions = []
        self.events = []

    def tearDown(self):
        os.environ.pop("SAO_BOSS_SKILL_STORE", None)
        if os.path.exists(self._store_path):
            os.remove(self._store_path)

    def _engine(self, profile):
        eng = BossRaidEngine(GameStateManager(), settings=_Settings(),
                             on_boss_action=lambda a: self.actions.append(a),
                             on_mechanic=lambda e: self.events.append(e))
        eng._running = True   # keep the loop thread off; ticks run manually
        eng.start(profile)
        return eng

    def _wait(self, pred, timeout=2.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if pred():
                return True
            time.sleep(0.01)
        return pred()

    def _tick(self, eng):
        with eng._lock:
            eng._tick_locked()
        eng._dispatch_mech_forwards()

    # ── schema ──

    def test_v1_profile_normalizes_with_defaults(self):
        p = normalize_profile({"id": "x", "profile_name": "old",
                               "enrage_time_s": 480,
                               "phases": [{"id": "a", "name": "P1"}]})
        self.assertEqual(p["mechanics"], [])
        self.assertEqual(p["enrage"]["time_s"], 480)
        self.assertEqual(p["enrage"]["anchor"], "fight")
        self.assertEqual(p["enrage"]["tts_milestones"], [60, 30, 10])
        self.assertEqual(p["schema_version"], 2)

    def test_mechanic_normalize_coercions(self):
        p = normalize_profile(_profile([_mech(skill_ids=(7, "3", 7, 0, -2))]))
        m = p["mechanics"][0]
        self.assertEqual(m["detect"]["skill_ids"], [3, 7])
        self.assertEqual(m["alert"]["alert_type"], "both")
        self.assertEqual(m["dodge"]["inline"]["action_key"], "SPACE")
        self.assertTrue(m["dodge"]["enabled"])

    def test_clone_remaps_phase_refs(self):
        cfg = {"profiles": [normalize_profile(_profile(
            [_mech(phase_ids=["ph1"])],
            enrage={"time_s": 300, "anchor": "phase", "phase_id": "ph1"}))],
            "active_profile_id": "boss_t"}
        cloned = clone_profile(cfg, "boss_t")
        got = [p for p in cfg["profiles"] if p["id"] == cloned["id"]][0]
        new_phase_id = got["phases"][0]["id"]
        self.assertNotEqual(new_phase_id, "ph1")
        self.assertEqual(got["mechanics"][0]["phase_ids"], [new_phase_id])
        self.assertEqual(got["enrage"]["phase_id"], new_phase_id)
        self.assertNotEqual(got["mechanics"][0]["id"], "m1")

    def test_bind_unbind_helpers(self):
        p = normalize_profile(_profile([_mech(skill_ids=())]))
        self.assertTrue(bind_mechanic_skill(p, "m1", 999))
        self.assertFalse(bind_mechanic_skill(p, "m1", 999))
        self.assertEqual(p["mechanics"][0]["detect"]["skill_ids"], [999])
        self.assertTrue(unbind_mechanic_skill(p, "m1", 999))
        self.assertEqual(p["mechanics"][0]["detect"]["skill_ids"], [])

    # ── cast matching (mem path) ──

    def test_cast_match_fires_event_and_dodge_forward(self):
        eng = self._engine(_profile([_mech()]))
        eng.on_mem_boss_action(_cast(555))
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))
        evt = self.events[0]
        self.assertEqual(evt["mechanic_id"], "m1")
        self.assertEqual(evt["tts_text"], "测试播报")
        self.assertEqual(evt["countdown_s"], 3.0)
        self.assertEqual(evt["configured_countdown_s"], 8)
        fwd = [a for a in self.actions if a.get("mechanic_id")]
        self.assertEqual(len(fwd), 1)
        self.assertEqual(fwd[0]["mechanic_dodge"]["action_key"], "SPACE")
        self.assertAlmostEqual(fwd[0]["dodge_wait_s"], 2.0, places=2)

    def test_cooldown_dedup(self):
        eng = self._engine(_profile([_mech()]))
        eng.on_mem_boss_action(_cast(555))
        eng.on_mem_boss_action(_cast(555, edge="end"))
        eng.on_mem_boss_action(_cast(555))
        self._wait(lambda: len(self.events) >= 1)
        time.sleep(0.1)
        self.assertEqual(len(self.events), 1, "second fire within cooldown")

    def test_boss_base_id_scoping(self):
        eng = self._engine(_profile(
            [_mech(detect={"skill_ids": [555], "boss_base_id": 70000})]))
        eng.on_mem_boss_action(_cast(555, base_id=90001))
        time.sleep(0.1)
        self.assertEqual(self.events, [], "wrong boss must not fire")

    def test_phase_scoping(self):
        prof = _profile([_mech(phase_ids=["ph2"])])
        prof["phases"].append({"id": "ph2", "name": "P2",
                               "trigger": {"type": "manual", "value": 0}})
        eng = self._engine(prof)
        eng.on_mem_boss_action(_cast(555))
        time.sleep(0.1)
        self.assertEqual(self.events, [], "phase-scoped mechanic out of phase")
        eng.next_phase()
        eng.on_mem_boss_action(_cast(555))
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))

    def test_self_buff_feed_fires_self_source(self):
        prof = _profile([_mech(skill_ids=(), detect={"buff_ids": [829304],
                                                     "source": "self"})])
        eng = self._engine(prof)
        eng.on_self_buff_change([100, 200])           # baseline
        eng.on_self_buff_change([100, 200, 829304])   # you got point-named
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))
        self.assertEqual(self.events[0]["mechanic_id"], "m1")
        self.assertEqual(self.events[0]["source"], "self")

    def test_source_isolation_boss_feed_skips_self_mech(self):
        prof = _profile([_mech(skill_ids=(), detect={"buff_ids": [829304],
                                                     "source": "self"})])
        eng = self._engine(prof)
        # the same buff id arriving on the BOSS feed must NOT fire a self-source mech
        eng.on_mem_boss_action(_cast(829304))
        time.sleep(0.1)
        self.assertEqual(self.events, [])

    def test_self_buff_no_refire_on_persistent(self):
        prof = _profile([_mech(skill_ids=(), detect={"buff_ids": [829305],
                                                     "source": "self"})])
        eng = self._engine(prof)
        eng.on_self_buff_change([829305])
        self._wait(lambda: len(self.events) >= 1)
        self.events.clear()
        eng.on_self_buff_change([829305])   # still present, not a new edge
        time.sleep(0.1)
        self.assertEqual(self.events, [], "persistent self-buff must not refire")

    def test_unbound_inbox(self):
        eng = self._engine(_profile([_mech()]))
        eng.on_mem_boss_action(_cast(777, dur=2500))
        rows = eng.get_unbound_skills()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["skill_id"], 777)
        self.assertEqual(rows[0]["last_cast_duration_ms"], 2500)
        eng.clear_unbound_skill(777)
        self.assertEqual(eng.get_unbound_skills(), [])

    # ── TCP buff path ──

    def test_tcp_secondary_buff_matches_mechanic(self):
        # mechanic bound to the SHORTER (non-primary) new buff id
        eng = self._engine(_profile([_mech(detect={"skill_ids": [700]})]))
        eng.on_monster_update(_monster(1000, [(1, -1)]))
        eng.on_monster_update(_monster(1000, [(1, -1), (700, 1000), (701, 5000)]))
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))
        self.assertEqual(self.events[0]["mechanic_id"], "m1")
        self.assertEqual(self.events[0]["skill_id"], 700)

    # ── hp / time anchors ──

    def test_hp_crossing_fires_once(self):
        eng = self._engine(_profile(
            [_mech(detect={"skill_ids": [], "hp_pct": 65.0})]))
        eng.on_monster_update(_monster(1000, [], hp=600, max_hp=1000))
        self._tick(eng)
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))
        self.assertEqual(self.events[0]["source"], "hp")
        self._tick(eng)
        time.sleep(0.1)
        self.assertEqual(len(self.events), 1, "hp anchor is one-shot")

    def test_time_anchor_fires_countdown_early(self):
        eng = self._engine(_profile(
            [_mech(detect={"skill_ids": [], "time_into_fight_s": 10.0},
                   alert={"countdown_s": 8, "cooldown_s": 0})]))
        eng._start_time = time.time() - 1.0   # elapsed 1s < eff 2s
        self._tick(eng)
        time.sleep(0.05)
        self.assertEqual(self.events, [])
        eng._start_time = time.time() - 3.0   # elapsed 3s >= eff 2s
        self._tick(eng)
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))
        self.assertEqual(self.events[0]["source"], "time")
        self._tick(eng)
        time.sleep(0.1)
        self.assertEqual(len(self.events), 1, "one-shot without repeat")

    # ── enrage ──

    def test_enrage_milestones_and_urgency(self):
        prof = _profile([], enrage={"time_s": 600, "anchor": "fight",
                                    "warn_threshold_s": 60,
                                    "urgent_threshold_s": 30,
                                    "tts_milestones": [60, 30, 10]})
        eng = self._engine(prof)
        eng._start_time = time.time() - (600 - 50)   # remaining 50s
        self._tick(eng)
        self.assertTrue(self._wait(
            lambda: any(e["source"] == "enrage" for e in self.events)))
        evt = [e for e in self.events if e["source"] == "enrage"][0]
        self.assertIn("60", evt["tts_text"])
        st = eng.get_status(include_entities=False)
        self.assertEqual(st["enrage_urgency"], "warn")
        eng._start_time = time.time() - (600 - 20)   # remaining 20s
        st = eng.get_status(include_entities=False)
        self.assertEqual(st["enrage_urgency"], "urgent")

    def test_phase_anchored_enrage_arms_on_phase_enter(self):
        prof = _profile([], enrage={"time_s": 300, "anchor": "phase",
                                    "phase_id": "ph2"})
        prof["phases"].append({"id": "ph2", "name": "P2",
                               "trigger": {"type": "manual", "value": 0}})
        eng = self._engine(prof)
        st = eng.get_status(include_entities=False)
        self.assertFalse(st["enrage_armed"])
        self.assertEqual(st["enrage_remaining_s"], 300.0)
        eng.next_phase()
        st = eng.get_status(include_entities=False)
        self.assertTrue(st["enrage_armed"])
        self.assertLessEqual(st["enrage_remaining_s"], 300.0)

    def test_runtime_enrage_timer_buff_overrides_profile_fallback(self):
        eng = self._engine(_profile([], enrage={"time_s": 600, "anchor": "fight"}))
        eng.on_monster_update(_monster(1000, [(501712, 90000)]))
        st = eng.get_status(include_entities=False)
        self.assertEqual(st["enrage_source"], "buff:501712")
        self.assertTrue(st["enrage_armed"])
        self.assertLessEqual(st["enrage_remaining_s"], 90.0)
        self.assertGreater(st["enrage_remaining_s"], 80.0)

    # ── GameState push ──

    def test_game_state_carries_mechanic_fields(self):
        eng = self._engine(_profile([_mech()]))
        eng.on_mem_boss_action(_cast(555))
        self._wait(lambda: len(self.events) >= 1)
        gs = eng._state_mgr.state
        self.assertEqual(gs.boss_mechanic_event.get("mechanic_id"), "m1")
        self.assertEqual(len(gs.boss_mechanic_countdowns), 1)
        self.assertEqual(gs.boss_mechanic_countdowns[0]["name"], "测试机制")

    # ── 拆箱即用示例档案 ──

    def test_example_profile_out_of_box(self):
        prof = normalize_profile(
            _load_example_profile_by_id("boss_example_13023_nm_p3")
        )
        mechs = prof.get("mechanics") or []
        self.assertEqual(len(mechs), 16)
        for m in mechs:
            det = m.get("detect") or {}
            bound = list(det.get("skill_ids") or []) + \
                list(det.get("buff_ids") or [])
            self.assertTrue(bound, "未绑定检测id: %s" % m.get("name"))
            self.assertTrue(m.get("enabled"), m.get("name"))
            notes = m.get("notes") or ""
            self.assertIn("机制：", notes, m.get("name"))
            self.assertIn("躲法：", notes, m.get("name"))
            tts = (m.get("alert") or {}).get("tts_text") or ""
            self.assertTrue(0 < len(tts) <= 10,
                            "TTS应为简易机制名: %s=%r" % (m.get("name"), tts))
            self.assertTrue((m.get("alert") or {}).get("banner_text"),
                            m.get("name"))
        eng = self._engine(prof)
        eng.on_self_buff_change([])        # baseline snapshot (zero buffs)
        eng.on_self_buff_change([829304])  # 你被点了红色分摊
        self.assertTrue(self._wait(lambda: len(self.events) >= 1))
        self.assertEqual(self.events[0].get("tts_text"), "红色分摊")

    def test_dash_dodge_sequence_roundtrip_and_summary(self):
        from plugins.star_resonance_plugin.engines.boss_raid_engine import normalize_mechanic
        from plugins.star_resonance_plugin.engines.boss_mechanics_state import mechanic_summary
        dash = [{"key": "SHIFT", "delay_ms": 0, "hold_ms": 0},
                {"key": "SHIFT", "delay_ms": 300, "hold_ms": 0},
                {"key": "SHIFT", "delay_ms": 300, "hold_ms": 0}]
        m = _mech(dodge={"enabled": True,
                         "inline": {"action_key": "", "sequence": dash,
                                    "lead_ms": 300}})
        n = normalize_mechanic(m)
        seq = (n.get("dodge") or {}).get("inline", {}).get("sequence") or []
        self.assertEqual(len(seq), 3)
        self.assertTrue(all(s["key"] == "SHIFT" for s in seq))
        self.assertEqual([s["delay_ms"] for s in seq], [0, 300, 300])
        desc = mechanic_summary(n)["dodge_desc"]
        self.assertIn("冲刺", desc)
        self.assertIn("SHIFT", desc)

    def test_dash_dodge_fires_three_shifts(self):
        seq = [{"key": "SHIFT", "delay_ms": 0, "hold_ms": 0},
               {"key": "SHIFT", "delay_ms": 10, "hold_ms": 0},
               {"key": "SHIFT", "delay_ms": 10, "hold_ms": 0}]
        lk = self._linkage()
        ok = lk.fire_mapping_test({"action_key": "", "sequence": seq})
        self.assertTrue(ok)
        self.assertTrue(self._wait(lambda: len(self.sent) >= 3))
        self.assertEqual([s[0] for s in self.sent[:3]], ["SHIFT", "SHIFT", "SHIFT"])

    def test_state_contract_resolves_buff_names(self):
        from plugins.star_resonance_plugin.engines.boss_mechanics_state import (
            mechanic_summary, _resolve_detect_name,
        )
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import _name_resolver
        m = _mech(detect={"skill_ids": [], "buff_ids": [829304]})
        self.assertEqual(mechanic_summary(m)["bound_count"], 1)
        nm = _name_resolver()
        if nm is not None:
            self.assertEqual(_resolve_detect_name(nm, 829304), "分摊·普")

    # ── linkage ──

    def _linkage(self, cfg=None):
        self.sent = []
        settings = _Settings({"boss_autokey_linkage": cfg or {}})
        lk = BossAutoKeyLinkage(
            settings,
            send_key=lambda k, m, h, c: self.sent.append((k, m, h, c)))
        return lk

    def test_linkage_inline_dodge_independent_of_enabled(self):
        lk = self._linkage({"enabled": False, "dodge_enabled": True})
        lk.on_boss_action({"mechanic_id": "m1", "mechanic_name": "x",
                           "mechanic_dodge": {"action_key": "SPACE",
                                              "cooldown_s": 0.5},
                           "dodge_wait_s": 0.0})
        self.assertTrue(self._wait(lambda: len(self.sent) >= 1))
        self.assertEqual(self.sent[0][0], "SPACE")

    def test_linkage_dodge_master_switch_blocks(self):
        lk = self._linkage({"enabled": False, "dodge_enabled": False})
        lk.on_boss_action({"mechanic_id": "m1",
                           "mechanic_dodge": {"action_key": "SPACE"},
                           "dodge_wait_s": 0.0})
        time.sleep(0.15)
        self.assertEqual(self.sent, [])

    def test_linkage_mechanic_mapping_matches_by_id(self):
        mapping = normalize_mapping({"trigger_type": "mechanic",
                                     "mechanic_id": "m1", "action_key": "Q",
                                     "cooldown_s": 0.0})
        lk = self._linkage({"enabled": True, "dodge_enabled": True,
                            "global_cooldown_s": 0.0, "mappings": [mapping]})
        lk.on_boss_action({"mechanic_id": "m2"})
        time.sleep(0.1)
        self.assertEqual(self.sent, [], "mechanic_id mismatch must not fire")
        lk.on_boss_action({"mechanic_id": "m1"})
        self.assertTrue(self._wait(lambda: len(self.sent) >= 1))
        self.assertEqual(self.sent[0][0], "Q")

    def test_linkage_sequence_per_step_hold(self):
        seq = [{"key": "A", "delay_ms": 0, "hold_ms": 600},
               {"key": "SPACE", "delay_ms": 10, "hold_ms": 0}]
        lk = self._linkage()
        ok = lk.fire_mapping_test({"action_key": "", "sequence": seq})
        self.assertTrue(ok)
        self.assertTrue(self._wait(lambda: len(self.sent) >= 2))
        self.assertEqual(self.sent[0], ("A", "hold", 600, 1))
        self.assertEqual(self.sent[1], ("SPACE", "tap", 0, 1))


if __name__ == "__main__":
    suite = unittest.TestLoader().loadTestsFromTestCase(MechanicsTest)
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    print(f"\n{res.testsRun - len(res.failures) - len(res.errors)} passed, "
          f"{len(res.failures) + len(res.errors)} failed")
    sys.exit(1 if (res.failures or res.errors) else 0)
