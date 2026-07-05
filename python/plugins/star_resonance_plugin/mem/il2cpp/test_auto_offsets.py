# -*- coding: utf-8 -*-
# Offline parity test for the fixed->auto offset conversion.
#
# Loads the curated on-disk bundle's DumpCsIndex and asserts that every offset a
# reader now resolves BY NAME equals the verified literal it used to hardcode. This
# validates the field names (incl. C# <Name>k__BackingField forms) and the fallback
# logic WITHOUT a running game. Run:  python -m mem_probe.il2cpp.test_auto_offsets
from __future__ import annotations

import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.dump_cs_parser import DumpCsIndex
from plugins.star_resonance_plugin.mem.il2cpp import auto_offsets as _ao

_BUNDLE = os.path.join(_HERE, "_cache", "bundle.json")

# (class, field_name, expected_literal) — the offsets readers now resolve by name.
CASES = [
    # ZEntityMgr (mem_entity_mgr)
    ("Panda.ZGame.ZEntityMgr", "playerUuid_", 0x10),
    ("Panda.ZGame.ZEntityMgr", "entityDict_", 0x28),
    ("Panda.ZGame.ZEntityMgr", "bossDict_", 0x60),
    ("Panda.ZGame.ZEntityMgr", "monsterDict_", 0x68),
    ("Panda.ZGame.ZEntityMgr", "npcDict_", 0x70),
    # ZEntity (mem_entity_mgr / mem_entity_provider / mem_entity_combat) — auto-properties
    ("Panda.ZGame.ZEntity", "attrs_", 0x48),
    ("Panda.ZGame.ZEntity", "entityState_", 0x28),
    ("Panda.ZGame.ZEntity", "Uuid", 0xC0),
    ("Panda.ZGame.ZEntity", "ConfigUuid", 0xC8),
    ("Panda.ZGame.ZEntity", "EntId", 0xD0),
    ("Panda.ZGame.ZEntity", "CharId", 0xD8),
    ("Panda.ZGame.ZEntity", "BaseId", 0xE0),
    # ZAttrCollection (mem_entity_combat cacheSlim_, mem_attr_reader mixItemDict_)
    ("Panda.ZGame.ZAttrCollection", "cacheSlim_", 0x18),
    ("Panda.ZGame.ZAttrCollection", "mixItemDict_", 0x28),
    # mem_state_anchor self-state layouts
    ("Zproto.CharSerialize", "CharId", 0x10),
    ("Zproto.CharSerialize", "Attr", 0x88),
    ("Zproto.CharSerialize", "CharBase", 0x18),
    ("Zproto.CharSerialize", "EnergyItem", 0x70),
    ("Zproto.CharSerialize", "RoleLevel", 0xB8),
    ("Zproto.CharSerialize", "ProfessionList", 0x1F8),
    ("Zproto.CharSerialize", "SceneData", 0x20),
    ("Zproto.UserFightAttr", "CurHp", 0x10),
    ("Zproto.UserFightAttr", "MaxHp", 0x18),
    ("Zproto.UserFightAttr", "OriginEnergy", 0x20),
    ("Zproto.UserFightAttr", "IsDead", 0x38),
    ("Zproto.UserFightAttr", "CdInfo", 0x50),
    ("Zproto.CharBaseInfo", "Name", 0x30),
    ("Zproto.CharBaseInfo", "FightPoint", 0xE8),
    ("Zproto.CharBaseInfo", "InitProfessionId", 0xC8),
    ("Zproto.RoleLevel", "Level", 0x10),
    ("Zproto.RoleLevel", "CurLevelExp", 0x18),
    ("Zproto.EnergyItem", "EnergyLimit", 0x10),
    ("Zproto.EnergyItem", "ExtraEnergyLimit", 0x14),
    ("Zproto.ProfessionList", "CurProfessionId", 0x10),
    ("Zproto.SkillCDInfo", "SkillLevelId", 0x10),
    ("Zproto.SkillCDInfo", "Duration", 0x20),
    ("Zproto.SceneData", "MapId", 0x10),
    ("Zproto.SceneData", "LevelMapId", 0x40),
    ("Zproto.SceneData", "LineId", 0x80),
    # mem_boss_action_reader (BuffItem not in curated bundle -> stays literal)
    ("Panda.ZGame.ZEntity", "stateMachine_", 0x70),
    ("Panda.ZGame.ZEntity", "buffComp_", 0x98),
    ("Panda.ZGame.ZStateMachine", "currentState_", 0x20),
    ("Panda.ZGame.BuffComp", "buffList_", 0x30),
    ("Panda.ZGame.BuffItem", "BuffUuid", 0x10),
    ("Panda.ZGame.BuffItem", "BuffBaseId", 0x14),
    ("Panda.ZGame.BuffItem", "CreateTime", 0x30),
    ("Panda.ZGame.BuffItem", "Duration", 0x38),
]


def _build_dci(path):
    with open(path, "r", encoding="utf-8") as f:
        bundle = json.load(f)
    classes = {}
    for full, c in bundle.get("classes", {}).items():
        classes[full] = {
            "namespace": c.get("namespace", ""),
            "name": full.rsplit(".", 1)[-1],
            "kind": "class",
            "type_def_index": c.get("type_def_index", -1),
            "bases": "",
            "fields": c.get("fields", []),
        }
    return DumpCsIndex(classes)


def _full_dump_dci():
    # The complete on-disk dump_cs_index (newest), for classes not yet curated into
    # the shipping bundle (e.g. DamageDataMgr/BuffItem until the next rebuild).
    import glob
    cands = sorted(glob.glob(os.path.join(os.path.dirname(_HERE), "il2cpp", "out",
                                          "*", "dump_cs_index.json")),
                   key=os.path.getmtime, reverse=True)
    for p in cands:
        try:
            return DumpCsIndex.load_from_json(p)
        except Exception:
            continue
    return None


def main() -> int:
    if not os.path.isfile(_BUNDLE):
        print(f"[SKIP] no bundle at {_BUNDLE}")
        return 0
    dci = _build_dci(_BUNDLE)
    full = _full_dump_dci()   # fallback for classes not in the curated bundle yet
    ok = fail = 0
    for cls, field, literal in CASES:
        got = _ao.field_offset(dci, cls, field)
        src = "bundle"
        if got is None and full is not None:   # not curated -> validate name vs full dump
            got = _ao.field_offset(full, cls, field)
            src = "dump"
        status = "OK" if got == literal else "FAIL"
        if got == literal:
            ok += 1
        else:
            fail += 1
        print(f"  [{status}] {cls}.{field}: {src}=0x{(got or -1):X} literal=0x{literal:X}")

    # fallback behaviour: an unknown class/field keeps the literal, and a real dump
    # value is preferred when it differs from the (stale) literal.
    res = _ao.resolve(dci, "Panda.ZGame.ZEntity", {
        "real": ("BaseId", 0x999),          # stale literal -> dump 0xE0 wins
        "missing": ("NoSuchField_", 0x77),  # absent -> literal kept
    })
    fb_ok = (res["real"] == 0xE0 and res["missing"] == 0x77)
    print(f"  [{'OK' if fb_ok else 'FAIL'}] fallback/prefer-dump: real=0x{res['real']:X} missing=0x{res['missing']:X}")
    if not fb_ok:
        fail += 1
    else:
        ok += 1

    print(f"\n[auto_offsets] {ok} passed, {fail} failed")
    return 1 if fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
