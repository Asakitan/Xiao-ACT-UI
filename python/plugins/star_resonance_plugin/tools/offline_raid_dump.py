# -*- coding: utf-8 -*-
# offline_raid_dump - 纯离线: 从已有name_table + raid示例交叉分析全部bossraid机制。
#
# 不需要游戏运行。数据源:
# assets/name_tables/boss_mechanic_skill.json  (452个机制技能)
# assets/name_tables/boss_skill.json           (355个boss技能)
# assets/name_tables/buff.json                 (9533个buff含raid范围)
# assets/name_tables/monster.json              (1038个怪物)
# assets/boss_raids/*机制示例.json              (9个raid示例)
# assets/boss_raids/raid_scene_names.json      (raid元数据)
#
# Usage:
# python -m tools.offline_raid_dump
from __future__ import annotations

import json
import os
import sys
from typing import Dict, List, Set, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_NT = os.path.join(_ROOT, "assets", "name_tables")
_BR = os.path.join(_ROOT, "assets", "boss_raids")
_OUT = os.path.join(_ROOT, "exports", "boss_raids")


def _utf8():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def _load(name: str) -> dict:
    p = os.path.join(_NT, name + ".json")
    if os.path.exists(p):
        with open(p, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


# ── skill/buff prefix matching ───────────────────────────────────────────────

RAID_BOSS_SKILL_PREFIXES = {
    13003: {
        "冰龙": ["10140", "10121"],
        "虚蚀龙": ["10240"],
        "光龙": ["10270"],
    },
    13013: {
        "双子": ["10280", "10281"],
        "石头人": ["10290"],
        "蚀花": ["10300"],
    },
    13023: {
        "始": ["10310"],
        "继": ["10320"],
        "终": ["10330"],
    },
}

RAID_BUFF_RANGES = {
    13003: [(882100, 882699)],
    13013: [(827100, 827399), (828100, 828199)],
    13023: [(829100, 829399)],
}

NON_MECHANIC_KW = [
    "普攻", "瞬移", "待机", "闲置", "死亡", "演绎", "出生",
    "转90", "转180", "转向", "播放", "结束",
]

NOISE_BUFF_KW = [
    "弱点研究", "天生标记", "初始buff", "初始标记", "开启云", "暂停云",
    "关闭云", "检测血量", "监控", "转阶段标记", "转阶段成功",
    "转阶段结束", "已经释放过", "通用秒杀",
]

MECHANIC_KW = [
    "分摊", "点名", "致死", "连线", "狂暴", "全场", "秒杀", "机制",
    "环形", "吐息", "横扫", "砸地", "十字", "激光", "光束", "冲锋",
    "分散", "扩散", "衰减", "陨石", "领地", "角斗", "死亡计时",
    "自爆", "弹球", "折跃", "刻度", "归途", "律动", "交响",
    "放逐", "宣告", "试炼", "魔方", "进度条",
]


def _is_mechanic_name(name: str) -> bool:
    if any(kw in name for kw in NON_MECHANIC_KW):
        return False
    return any(kw in name for kw in MECHANIC_KW)


def _matches_prefix(sid: str, prefixes: List[str]) -> bool:
    return any(sid.startswith(p) for p in prefixes)


# ── main dump ────────────────────────────────────────────────────────────────

def build_dump() -> dict:
    mech_skills = _load("boss_mechanic_skill")
    boss_skills = _load("boss_skill")
    buffs = _load("buff")
    monsters = _load("monster")
    bosses = _load("boss")
    dungeons = _load("dungeon")

    with open(os.path.join(_BR, "raid_scene_names.json"), "r", encoding="utf-8") as f:
        raid_scenes = json.load(f)

    examples = {}
    for fn in sorted(os.listdir(_BR)):
        if fn.endswith("机制示例.json"):
            with open(os.path.join(_BR, fn), "r", encoding="utf-8") as f:
                examples[fn] = json.load(f)

    all_names = {}
    all_names.update(boss_skills)
    all_names.update(mech_skills)

    result = {
        "_meta": {
            "source": "offline name_tables + raid examples",
            "mechanic_skill_count": len(mech_skills),
            "boss_skill_count": len(boss_skills),
            "buff_count": len(buffs),
            "example_count": len(examples),
        },
        "raids": {},
    }

    for did_s, scene in raid_scenes.items():
        if did_s.startswith("_"):
            continue
        did = int(did_s)
        raid_name = scene.get("raid", "")
        buff_ranges = RAID_BUFF_RANGES.get(did, [])
        skill_prefix_map = RAID_BOSS_SKILL_PREFIXES.get(did, {})

        raid = {
            "dungeon_id": did,
            "raid_name": raid_name,
            "dungeon_name": dungeons.get(did_s, ""),
            "encounters": [],
        }

        for enc in scene.get("encounters", []):
            map_name = enc.get("map", "")
            boss_name = enc.get("boss", "")
            boss_ids = enc.get("boss_ids", [])
            alias = enc.get("alias", "")

            ex_file = None
            for ef in examples:
                if f"{did}_{map_name}" in ef:
                    ex_file = ef
                    break

            # find skill prefixes for this encounter
            prefixes = []
            for label, pfx_list in skill_prefix_map.items():
                if label in map_name or label in alias or label in boss_name:
                    prefixes.extend(pfx_list)
            if not prefixes:
                for _, pfx_list in skill_prefix_map.items():
                    prefixes.extend(pfx_list)

            # collect all skills matching this boss
            enc_all_skills = {}
            enc_mech_table = {}
            for sid, sname in boss_skills.items():
                if _matches_prefix(sid, prefixes):
                    enc_all_skills[sid] = sname
            for sid, sname in mech_skills.items():
                if _matches_prefix(sid, prefixes):
                    enc_mech_table[sid] = sname
                    enc_all_skills[sid] = sname

            # collect raid buffs
            enc_buffs = {}
            for bid_s, bname in buffs.items():
                bid = int(bid_s)
                for lo, hi in buff_ranges:
                    if lo <= bid <= hi:
                        enc_buffs[bid_s] = bname

            # analyze example coverage
            ex_covered_skills: Set[str] = set()
            ex_covered_buffs: Set[str] = set()
            ex_mechs = []
            if ex_file and ex_file in examples:
                for m in examples[ex_file].get("profile", {}).get("mechanics", []):
                    det = m.get("detect", {})
                    sids = det.get("skill_ids", [])
                    bids = det.get("buff_ids", [])
                    for s in sids:
                        ex_covered_skills.add(str(s))
                    for b in bids:
                        ex_covered_buffs.add(str(b))
                    ex_mechs.append({
                        "name": m.get("name", ""),
                        "skill_ids": sids,
                        "buff_ids": bids,
                        "source": det.get("source", "any"),
                        "event": det.get("event", ""),
                        "has_dodge": m.get("dodge", {}).get("enabled", False),
                        "has_geometry": bool(
                            m.get("dodge", {}).get("inline", {}).get("geometry", {}).get("source", "")),
                    })

            # classify skills
            skills_analysis = []
            for sid in sorted(enc_all_skills.keys(), key=lambda x: int(x)):
                sname = enc_all_skills[sid]
                is_in_mech_table = sid in enc_mech_table
                is_covered = sid in ex_covered_skills
                is_mech = _is_mechanic_name(sname) or is_in_mech_table
                skills_analysis.append({
                    "id": sid,
                    "name": sname,
                    "in_mechanic_table": is_in_mech_table,
                    "covered_in_example": is_covered,
                    "is_mechanic": is_mech,
                    "status": ("covered" if is_covered
                               else "skip" if not is_mech
                               else "UNCOVERED_MECHANIC"),
                })

            # classify buffs
            uncovered_mech_buffs = []
            for bid_s in sorted(enc_buffs.keys(), key=lambda x: int(x)):
                bname = enc_buffs[bid_s]
                if bid_s in ex_covered_buffs:
                    continue
                if any(kw in bname for kw in NOISE_BUFF_KW):
                    continue
                is_mech_buff = (bid_s in mech_skills or _is_mechanic_name(bname))
                if is_mech_buff:
                    uncovered_mech_buffs.append({"id": bid_s, "name": bname,
                                                 "in_mechanic_table": bid_s in mech_skills})

            # encounter summary
            uncov_skills = [s for s in skills_analysis if s["status"] == "UNCOVERED_MECHANIC"]
            no_geom = [m for m in ex_mechs if not m["has_geometry"]]

            encounter = {
                "map": map_name,
                "boss": boss_name,
                "alias": alias,
                "boss_ids": boss_ids,
                "boss_names": {str(b): monsters.get(str(b), bosses.get(str(b), ""))
                               for b in boss_ids},
                "has_example": ex_file is not None,
                "example_file": ex_file or "",
                "stats": {
                    "total_skills": len(enc_all_skills),
                    "mechanic_table_skills": len(enc_mech_table),
                    "example_mechanics": len(ex_mechs),
                    "covered_skills": len(ex_covered_skills),
                    "covered_buffs": len(ex_covered_buffs),
                    "uncovered_mechanic_skills": len(uncov_skills),
                    "uncovered_mechanic_buffs": len(uncovered_mech_buffs),
                    "no_geometry_mechanics": len(no_geom),
                    "total_raid_buffs": len(enc_buffs),
                },
                "skills": skills_analysis,
                "example_mechanics": ex_mechs,
                "uncovered_mechanic_buffs": uncovered_mech_buffs,
            }
            raid["encounters"].append(encounter)

        result["raids"][did_s] = raid

    return result


def _print_report(dump: dict) -> None:
    print("=" * 70)
    print("OFFLINE BOSSRAID MECHANICS DUMP")
    print("=" * 70)
    meta = dump["_meta"]
    print(f"Sources: {meta['mechanic_skill_count']} mechanic skills, "
          f"{meta['boss_skill_count']} boss skills, "
          f"{meta['buff_count']} buffs, {meta['example_count']} examples\n")

    for did_s, raid in sorted(dump["raids"].items()):
        print(f"\n{'='*60}")
        print(f"Raid {did_s}: {raid['raid_name']} ({raid['dungeon_name']})")
        print("=" * 60)

        for enc in raid["encounters"]:
            st = enc["stats"]
            print(f"\n  [{enc['map']}] {enc['boss']} ({enc.get('alias','')})")
            print(f"  Boss IDs: {enc['boss_ids']}")
            print(f"  Example: {'YES' if enc['has_example'] else 'NO'} "
                  f"({enc['example_file']})")
            print(f"  Skills: {st['total_skills']} total, "
                  f"{st['mechanic_table_skills']} in mechanic table, "
                  f"{st['covered_skills']} covered")
            print(f"  Mechanics: {st['example_mechanics']} defined, "
                  f"{st['no_geometry_mechanics']} without geometry")
            print(f"  Buffs: {st['total_raid_buffs']} raid-range, "
                  f"{st['covered_buffs']} covered")

            uncov = [s for s in enc["skills"] if s["status"] == "UNCOVERED_MECHANIC"]
            if uncov:
                print(f"\n  >>> UNCOVERED MECHANIC SKILLS ({len(uncov)}):")
                for s in uncov:
                    tag = " [MECH_TABLE]" if s["in_mechanic_table"] else ""
                    print(f"      {s['id']:>12} {s['name']}{tag}")

            if enc["uncovered_mechanic_buffs"]:
                print(f"\n  >>> UNCOVERED MECHANIC BUFFS ({len(enc['uncovered_mechanic_buffs'])}):")
                for b in enc["uncovered_mechanic_buffs"][:15]:
                    tag = " [MECH_TABLE]" if b["in_mechanic_table"] else ""
                    print(f"      {b['id']:>8} {b['name']}{tag}")
                remain = len(enc["uncovered_mechanic_buffs"]) - 15
                if remain > 0:
                    print(f"      ... and {remain} more")

    # Overall summary
    total_mechs = 0
    total_uncov_skills = 0
    total_uncov_buffs = 0
    total_no_geom = 0
    for _, raid in dump["raids"].items():
        for enc in raid["encounters"]:
            st = enc["stats"]
            total_mechs += st["example_mechanics"]
            total_uncov_skills += st["uncovered_mechanic_skills"]
            total_uncov_buffs += st["uncovered_mechanic_buffs"]
            total_no_geom += st["no_geometry_mechanics"]

    print(f"\n{'='*60}")
    print(f"OVERALL: {total_mechs} mechanics defined, "
          f"{total_uncov_skills} uncovered mechanic skills, "
          f"{total_uncov_buffs} uncovered mechanic buffs, "
          f"{total_no_geom} without geometry")
    print("=" * 60)


def main() -> int:
    _utf8()
    dump = build_dump()
    _print_report(dump)

    out_path = os.path.join(_OUT, "offline_raid_mechanics_dump.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(dump, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"\nExported -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
