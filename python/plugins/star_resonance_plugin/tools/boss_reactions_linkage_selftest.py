# -*- coding: utf-8 -*-
"""Selftest for the extended BossAutoKeyLinkage (mem boss-action driven).

Covers mapping back-compat, on_boss_action matching by skill_id/base_id, the
offensive rising-edge windows, cooldown, and delay/sequence dispatch. No game.

    python tools/boss_reactions_linkage_selftest.py
"""
from __future__ import annotations

import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.boss_autokey_linkage import (  # noqa: E402
    BossAutoKeyLinkage, normalize_mapping, default_linkage_config,
)

_passed = 0
_failed = 0


def check(name, cond, detail=""):
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  [ok] {name}")
    else:
        _failed += 1
        print(f"  [FAIL] {name} :: {detail}")


class _Settings:
    def __init__(self, cfg):
        self._cfg = cfg

    def get(self, k, default=None):
        return self._cfg if k == "boss_autokey_linkage" else default

    def set(self, k, v):
        self._cfg = v

    def save(self):
        pass


def _cfg(mappings, *, global_cd=0.0):
    c = default_linkage_config()
    c["enabled"] = True
    c["global_cooldown_s"] = global_cd
    c["mappings"] = [normalize_mapping(m) for m in mappings]
    return c


def _linkage(cfg):
    sent = []
    lk = BossAutoKeyLinkage(_Settings(cfg),
                            send_key=lambda k, pm, hm, pc: sent.append((k, pm, hm, pc)))
    return lk, sent


def _fire(lk, action):
    lk.on_boss_action(action)
    time.sleep(0.06)   # let the daemon dispatch thread run


def test_backcompat():
    print("[mapping back-compat]")
    old = {"id": "m1", "trigger_type": "phase_enter", "trigger_match": "P2", "action_key": "q"}
    m = normalize_mapping(old)
    check("legacy trigger kept", m["trigger_type"] == "phase_enter")
    check("new fields defaulted", m["skill_id"] == 0 and m["boss_base_id"] == 0 and m["sequence"] == [])
    check("delay/lead default 0", m["delay_ms"] == 0 and m["lead_ms"] == 0)


def test_boss_cast_match():
    print("[boss_cast skill_id match]")
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_cast", "skill_id": 100, "action_key": "SPACE", "cooldown_s": 0},
    ]))
    _fire(lk, {"cast_edge": "start", "skill_id": 100, "boss_base_id": 114})
    check("matching skill fires", sent == [("SPACE", "tap", 80, 1)], repr(sent))
    sent.clear()
    _fire(lk, {"cast_edge": "start", "skill_id": 200, "boss_base_id": 114})
    check("non-matching skill no fire", sent == [], repr(sent))
    sent.clear()
    _fire(lk, {"cast_edge": "end", "skill_id": 100, "boss_base_id": 114})
    check("cast_end no fire", sent == [], repr(sent))


def test_instant_action():
    print("[instant action skill_id=0]")
    # an instant counterattack (no cast id) must still fire an "any skill" mapping
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_cast", "skill_id": 0, "boss_base_id": 122,
         "action_key": "SPACE", "cooldown_s": 0},
    ]))
    _fire(lk, {"cast_edge": "start", "skill_id": 0, "boss_base_id": 122})
    check("instant action fires any-skill mapping", sent == [("SPACE", "tap", 80, 1)], repr(sent))
    # but a per-skill mapping must NOT fire on a 0-id action
    lk2, sent2 = _linkage(_cfg([
        {"trigger_type": "boss_cast", "skill_id": 555, "boss_base_id": 122,
         "action_key": "Q", "cooldown_s": 0},
    ]))
    _fire(lk2, {"cast_edge": "start", "skill_id": 0, "boss_base_id": 122})
    check("per-skill mapping ignores 0-id action", sent2 == [])


def test_base_scope():
    print("[boss_base_id scope]")
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_cast", "skill_id": 0, "boss_base_id": 114,
         "action_key": "1", "cooldown_s": 0},
    ]))
    _fire(lk, {"cast_edge": "start", "skill_id": 5, "boss_base_id": 114})
    check("matching boss fires", len(sent) == 1)
    sent.clear()
    _fire(lk, {"cast_edge": "start", "skill_id": 5, "boss_base_id": 999})
    check("other boss no fire", sent == [])


def test_offensive_edges():
    print("[offensive rising edge]")
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_breaking", "action_key": "R", "cooldown_s": 0},
    ]))
    _fire(lk, {"cast_edge": "none", "breaking_edge": True})
    check("rising edge fires", sent == [("R", "tap", 80, 1)], repr(sent))
    sent.clear()
    _fire(lk, {"cast_edge": "none", "breaking_edge": False})
    check("no edge no fire", sent == [])


def test_cooldown():
    print("[cooldown]")
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_overdrive", "action_key": "F", "cooldown_s": 5.0},
    ]))
    _fire(lk, {"cast_edge": "none", "overdrive_edge": True})
    _fire(lk, {"cast_edge": "none", "overdrive_edge": True})
    check("second within cooldown skipped", len(sent) == 1, repr(sent))


def test_sequence():
    print("[sequence dispatch]")
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_stun", "sequence": [{"key": "1"}, {"key": "2"}, {"key": "3"}],
         "cooldown_s": 0},
    ]))
    _fire(lk, {"cast_edge": "none", "stun_edge": True})
    check("sequence pressed in order", [s[0] for s in sent] == ["1", "2", "3"], repr(sent))


def test_disabled():
    print("[master disabled]")
    cfg = _cfg([{"trigger_type": "boss_cast", "skill_id": 100, "action_key": "SPACE"}])
    cfg["enabled"] = False
    lk, sent = _linkage(cfg)
    _fire(lk, {"cast_edge": "start", "skill_id": 100, "boss_base_id": 1})
    check("disabled no fire", sent == [])


def test_wasd_skip_while_dodging():
    print("[WASD skip while directional dodge active]")
    sent = []
    active = {"on": True}
    lk = BossAutoKeyLinkage(
        _Settings(_cfg([
            {"trigger_type": "boss_stun",
             "sequence": [{"key": "W"}, {"key": "1"}, {"key": "D"}], "cooldown_s": 0},
        ])),
        send_key=lambda k, pm, hm, pc: sent.append(k),
        dodge_active_gate=lambda: active["on"])
    _fire(lk, {"cast_edge": "none", "stun_edge": True})
    check("WASD skipped, non-move key kept", sent == ["1"], repr(sent))
    # 躲避结束后移动键照发
    sent.clear()
    active["on"] = False
    _fire(lk, {"cast_edge": "none", "stun_edge": True})
    check("all keys fire when dodge inactive", sent == ["W", "1", "D"], repr(sent))


def test_panic_stop():
    print("[panic_stop kills in-flight]")
    # 带 delay 的派发: panic_stop 在等待窗口内调用 → 应被作废, 不发键
    lk, sent = _linkage(_cfg([
        {"trigger_type": "boss_cast", "skill_id": 100, "action_key": "SPACE",
         "delay_ms": 120, "cooldown_s": 0},
    ]))
    lk.on_boss_action({"cast_edge": "start", "skill_id": 100, "boss_base_id": 1})
    time.sleep(0.03)        # 仍在 120ms 等待窗口内
    lk.panic_stop()
    time.sleep(0.15)        # 越过原 delay
    check("in-flight key cancelled by panic_stop", sent == [], repr(sent))
    # panic 后新触发照常工作 (epoch 快照只杀那一次)
    lk.on_boss_action({"cast_edge": "start", "skill_id": 100, "boss_base_id": 1})
    time.sleep(0.2)
    check("post-panic trigger still fires", sent == [("SPACE", "tap", 80, 1)], repr(sent))


def main():
    test_backcompat()
    test_boss_cast_match()
    test_instant_action()
    test_base_scope()
    test_offensive_edges()
    test_cooldown()
    test_sequence()
    test_disabled()
    test_wasd_skip_while_dodging()
    test_panic_stop()
    print(f"\n{_passed} passed, {_failed} failed")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
