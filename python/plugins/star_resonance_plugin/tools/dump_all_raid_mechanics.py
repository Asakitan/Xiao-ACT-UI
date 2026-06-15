# -*- coding: utf-8 -*-
"""dump_all_raid_mechanics - 从内存配置表一次性提取全部bossraid机制数据。

读取链:
  RaidDungeonTable  → 所有raid副本 + 难度 + BossId[]
  MonsterTable      → 每个boss的SkillIds[], BornClientBuffs[], DeadClientBuffs[],
                       BreakingContinueTime, FractureDuration, WeeknessDuration,
                       BloodMark[], BloodTubeCount
  SkillTable        → 每个技能的 Name, IsDangerSkill, IsAoe, IsFractureSkill,
                       EffectIDs[], SkillType, TargetType, SkillRangeType,
                       NextSkillId, SkillDamType, CoolTimeType
  SkillEffectTable  → EffectRange[], InstallSkillAddBuffs[], SkillDamageDistance
  BuffTable         → buff名字, SkillId反查, BuffType, BuffAbilityType, Tags[]
  FieldTable        → Size[], SkillId (AOE地面场)
  DungeonStageTable → PhaseName

不需要进副本: ZLoader序列化数据在大厅就已加载。

Usage:
  python -m tools.dump_all_raid_mechanics --dry-run
  python -m tools.dump_all_raid_mechanics --apply
  python -m tools.dump_all_raid_mechanics --dungeon 13023 --dry-run
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from typing import Dict, List, Optional, Set, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp import table_columns
from mem_probe.il2cpp.mem_config_table_reader import MemConfigTableReader, TABLE_CLASS
from mem_probe.il2cpp.mem_string_pool import StringPoolBridge

RAID_CLS = TABLE_CLASS["raid_dungeon"]
MONSTER_CLS = TABLE_CLASS["monster"]
SKILL_CLS = TABLE_CLASS["skill"]
BUFF_CLS = TABLE_CLASS["buff"]
SKILL_EFFECT_CLS = TABLE_CLASS["skill_effect"]
FIELD_CLS = TABLE_CLASS["field"]
STAGE_CLS = TABLE_CLASS["dungeon_stage"]

_NAME_TABLES = os.path.join(_ROOT, "assets", "name_tables")
_EXPORTS = os.path.join(_ROOT, "exports", "boss_raids")


def _utf8_stdout() -> None:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def _strip_tags(s: str) -> str:
    return re.sub(r"<[^>]*>", "", s or "").strip()


# ── classifier: name → mechanic category ────────────────────────────────────

_NON_MECHANIC = [
    "普攻", "普通攻击", "瞬移", "传送用", "位移", "待机", "吼叫",
    "起手", "演绎", "结束", "转向", "出生", "转90", "转180",
    "闲置", "死亡", "倒地", "播放",
]

_CATEGORY_RULES = [
    ("stack",   ["领地分摊", "分摊", "集合", "叠加", "合体"]),
    ("spread",  ["分散", "扩散"]),
    ("charge",  ["角斗", "开冲", "冲锋", "突进"]),
    ("breath",  ["吐息", "龙息", "喷吐"]),
    ("sweep",   ["横扫", "扫尾", "顺劈"]),
    ("slam",    ["砸地", "拍地", "碎地", "重击"]),
    ("cross",   ["十字"]),
    ("line",    ["激光", "光线", "光束", "贯穿", "轨道"]),
    ("link",    ["连线", "连结"]),
    ("fall",    ["陨石", "陨星", "光陨"]),
    ("marker",  ["点名", "光柱"]),
    ("ring",    ["环形", "环型", "崩石环"]),
    ("wave",    ["裂石风暴", "地震波", "震荡"]),
    ("lethal",  ["死刑", "致死", "宣告", "终焉"]),
    ("raidwide", ["全场", "毁灭", "大范围"]),
    ("aoe",     ["aoe", "AOE", "范围", "地面", "充填", "填充"]),
    ("dodge",   ["闪避", "弹球", "折跃"]),
    ("phase",   ["狂暴", "转阶段", "切换", "变身"]),
]


def _classify(name: str) -> Tuple[str, bool]:
    """(category, is_mechanic). Non-mechanics get category='normal'."""
    if not name:
        return "unknown", False
    for kw in _NON_MECHANIC:
        if kw in name:
            return "normal", False
    for cat, keywords in _CATEGORY_RULES:
        for kw in keywords:
            if kw in name:
                return cat, True
    return "uncategorized", True


# ── main enumerator ─────────────────────────────────────────────────────────

class FullRaidEnumerator:
    """One-shot comprehensive enumerator of all raid mechanic data from config tables."""

    def __init__(self, src, log=print):
        self.src = src
        self.rd = MemConfigTableReader(src)
        self.pool = StringPoolBridge(src)
        self.log = log
        self.notes: List[str] = []
        self.columns: Dict[str, Dict[str, Tuple[int, str]]] = {}
        self._indexes: Dict[str, Dict[int, Tuple[int, int]]] = {}

    def prepare(self) -> bool:
        t0 = time.time()
        if not self.pool.build():
            self.log("[dump] string pool NOT located — names will be empty")
            self.notes.append("string_pool_unavailable")
        else:
            self.log(f"[dump] string pool ready ({time.time()-t0:.1f}s)")
        need = [RAID_CLS, MONSTER_CLS, SKILL_CLS, BUFF_CLS, SKILL_EFFECT_CLS]
        try:
            need.append(FIELD_CLS)
        except Exception:
            pass
        self.columns = table_columns.load_columns(need, log=self.log)
        return True

    def _col(self, cls: str, prop: str) -> Optional[int]:
        ent = self.columns.get(cls, {}).get(prop)
        return ent[0] if ent else None

    def _col_type(self, cls: str, prop: str) -> str:
        ent = self.columns.get(cls, {}).get(prop)
        return ent[1] if ent else ""

    # ── index builders ───────────────────────────────────────────────────────

    def _build_index(self, cls: str) -> Dict[int, Tuple[int, int]]:
        if cls in self._indexes:
            return self._indexes[cls]
        c_id = self._col(cls, "Id") or self._col(cls, "Key") or 0
        out: Dict[int, Tuple[int, int]] = {}
        t0 = time.time()
        for _key, zl, blob in self.rd.iter_rows_via_loader(cls):
            rid = self.rd.col_i32(blob, c_id)
            if rid is not None and rid > 0:
                out[rid] = (zl, blob)
        if not out:
            for _row, zl, blob in self.rd.iter_rows(cls):
                rid = self.rd.col_i32(blob, c_id)
                if rid is not None and rid > 0:
                    out.setdefault(rid, (zl, blob))
        self._indexes[cls] = out
        self.log(f"[dump] {cls}: {len(out)} rows ({time.time()-t0:.1f}s)")
        return out

    def _resolve_name(self, cls: str, blob: int, zl: int) -> Tuple[str, str]:
        """(name, source). Try MLString Name, then raw string NameDesign."""
        c_name = self._col(cls, "Name")
        nm = self.pool.resolve(self.rd.col_mlid(blob, c_name)) if c_name is not None else ""
        if nm:
            return _strip_tags(nm), "mlstring"
        c_nd = self._col(cls, "NameDesign")
        nd = self.rd.col_string(zl, blob, c_nd) if c_nd is not None else ""
        return (nd, "design") if nd else ("", "empty")

    # ── raid table ───────────────────────────────────────────────────────────

    def all_raid_rows(self) -> List[Dict]:
        seen: Dict[int, Dict] = {}
        for _key, zl, blob in self.rd.iter_rows_via_loader(RAID_CLS):
            r = self._decode_raid_row(zl, blob)
            if (r["dungeon_id"] or 0) > 0:
                seen.setdefault(blob, r)
        if not seen:
            for _row, zl, blob in self.rd.iter_rows(RAID_CLS):
                r = self._decode_raid_row(zl, blob)
                if (r["dungeon_id"] or 0) > 0:
                    seen.setdefault(blob, r)
        return sorted(seen.values(),
                      key=lambda r: (r.get("group_id") or 0, r.get("difficulty") or 0))

    def _decode_raid_row(self, zl: int, blob: int) -> Dict:
        desc_raw = self.pool.resolve(self.rd.col_mlid(blob, self._col(RAID_CLS, "Desc")))
        return {
            "dungeon_id": self.rd.col_i32(blob, self._col(RAID_CLS, "DungeonId")),
            "difficulty": self.rd.col_i32(blob, self._col(RAID_CLS, "Difficult")),
            "group_id": self.rd.col_i32(blob, self._col(RAID_CLS, "GroupId")),
            "name": self.pool.resolve(self.rd.col_mlid(blob, self._col(RAID_CLS, "Name"))),
            "desc": _strip_tags(desc_raw or ""),
            "boss_ids": self.rd.col_i32_array(zl, blob, self._col(RAID_CLS, "BossId")),
        }

    def raid_families(self) -> Dict[int, List[Dict]]:
        """group_id → list of difficulty rows."""
        by_group: Dict[int, List[Dict]] = {}
        for r in self.all_raid_rows():
            gid = r.get("group_id") or 0
            if gid > 0:
                by_group.setdefault(gid, []).append(r)
        return by_group

    # ── monster (boss) ───────────────────────────────────────────────────────

    def boss_full_info(self, boss_id: int) -> Optional[Dict]:
        idx = self._build_index(MONSTER_CLS)
        ent = idx.get(int(boss_id))
        if not ent:
            return None
        zl, blob = ent
        name, name_src = self._resolve_name(MONSTER_CLS, blob, zl)
        info: Dict = {
            "id": int(boss_id),
            "name": name,
            "name_src": name_src,
            "skill_ids": self.rd.col_i32_array(zl, blob, self._col(MONSTER_CLS, "SkillIds")),
            "born_client_buffs": self.rd.col_i32_array(zl, blob, self._col(MONSTER_CLS, "BornClientBuffs")),
            "dead_client_buffs": self.rd.col_i32_array(zl, blob, self._col(MONSTER_CLS, "DeadClientBuffs")),
            "blood_mark": self.rd.col_i32_array(zl, blob, self._col(MONSTER_CLS, "BloodMark")),
        }
        for f32_prop in ("BreakingContinueTime", "FractureDuration", "WeeknessDuration",
                         "AttackHeight", "TargetSelectWeight"):
            c = self._col(MONSTER_CLS, f32_prop)
            if c is not None:
                info[f32_prop] = self.rd.col_f32(blob, c)
        for i32_prop in ("BloodTubeCount", "MonsterType", "MonsterSizeType",
                         "MonsterFightArea", "BornSkillId", "ModelID",
                         "MonsterLogicLevel"):
            c = self._col(MONSTER_CLS, i32_prop)
            if c is not None:
                info[i32_prop] = self.rd.col_i32(blob, c)
        tags_c = self._col(MONSTER_CLS, "Tags")
        if tags_c is not None:
            info["tags"] = self.rd.col_i32_array(zl, blob, tags_c)
        c_rank = self._col(MONSTER_CLS, "MonsterRank")
        if c_rank is not None:
            info["monster_rank"] = self.rd.col_string(zl, blob, c_rank)
        return info

    # ── skill full metadata ──────────────────────────────────────────────────

    def skill_full_info(self, skill_id: int) -> Optional[Dict]:
        idx = self._build_index(SKILL_CLS)
        ent = idx.get(int(skill_id))
        if not ent:
            return None
        zl, blob = ent
        name, name_src = self._resolve_name(SKILL_CLS, blob, zl)
        category, is_mechanic = _classify(name)
        info: Dict = {
            "id": int(skill_id),
            "name": name,
            "name_src": name_src,
            "category": category,
            "is_mechanic": is_mechanic,
        }
        for bool_prop in ("IsAoe", "IsDangerSkill", "IsFractureSkill", "IsArmor",
                          "FaceTarget", "IsPreview", "LongPressOpen", "CantStiff",
                          "IsSearchEnemie"):
            c = self._col(SKILL_CLS, bool_prop)
            if c is not None:
                info[bool_prop] = self.rd.col_bool(blob, c)
        for i32_prop in ("SkillType", "TargetType", "SkillRangeType",
                         "SkillTargetRangeType", "SkillSelectPointType",
                         "SkillDamType", "SkillHatedType",
                         "NextSkillId", "SwitchSkillId", "SkySkillId",
                         "CoolTimeType", "BreakSkillPriority",
                         "UnbreakSkillPriority", "SlotType", "SkillLabel",
                         "SkillLookAtAngle", "DefocusLookAtangle",
                         "PCBgColour"):
            c = self._col(SKILL_CLS, i32_prop)
            if c is not None:
                v = self.rd.col_i32(blob, c)
                if v is not None:
                    info[i32_prop] = v
        for f32_prop in ("ComboTakeEffectTime", "LongPressTime",
                         "SkillRootShift", "PlayInSkyHeight",
                         "CancelLockDis", "ContinuesSkillDelayTime",
                         "SkillTalkTime"):
            c = self._col(SKILL_CLS, f32_prop)
            if c is not None:
                v = self.rd.col_f32(blob, c)
                if v is not None and v != 0.0:
                    info[f32_prop] = round(v, 4)
        for arr_prop in ("EffectIDs", "SkillPreloadGroup", "NecessaryParts",
                         "ExcludeParts", "SlotPositionId"):
            c = self._col(SKILL_CLS, arr_prop)
            if c is not None:
                v = self.rd.col_i32_array(zl, blob, c)
                if v:
                    info[arr_prop] = v
        c_desc = self._col(SKILL_CLS, "Desc")
        if c_desc is not None:
            desc = self.pool.resolve(self.rd.col_mlid(blob, c_desc))
            if desc:
                info["desc"] = _strip_tags(desc)
        c_talk = self._col(SKILL_CLS, "SkillTalk")
        if c_talk is not None:
            talk = self.pool.resolve(self.rd.col_mlid(blob, c_talk))
            if talk:
                info["skill_talk"] = _strip_tags(talk)
        c_icon = self._col(SKILL_CLS, "Icon")
        if c_icon is not None:
            icon = self.rd.col_string(zl, blob, c_icon)
            if icon:
                info["icon"] = icon
        return info

    # ── skill effect ─────────────────────────────────────────────────────────

    def skill_effects(self, effect_ids: List[int]) -> List[Dict]:
        if not effect_ids:
            return []
        idx = self._build_index(SKILL_EFFECT_CLS)
        out = []
        for eid in effect_ids:
            ent = idx.get(int(eid))
            if not ent:
                continue
            zl, blob = ent
            info: Dict = {"id": int(eid)}
            c_name = self._col(SKILL_EFFECT_CLS, "Name")
            if c_name is not None:
                info["name"] = self.rd.col_string(zl, blob, c_name)
            c_skill = self._col(SKILL_EFFECT_CLS, "SkillId")
            if c_skill is not None:
                info["skill_id"] = self.rd.col_i32(blob, c_skill)
            c_dist = self._col(SKILL_EFFECT_CLS, "SkillDamageDistance")
            if c_dist is not None:
                v = self.rd.col_f32(blob, c_dist)
                if v is not None and v > 0:
                    info["damage_distance"] = round(v, 2)
            c_buffs = self._col(SKILL_EFFECT_CLS, "InstallSkillAddBuffs")
            if c_buffs is not None:
                buffs = self.rd.col_i32_array(zl, blob, c_buffs)
                if buffs:
                    info["install_buffs"] = buffs
            c_level = self._col(SKILL_EFFECT_CLS, "Level")
            if c_level is not None:
                info["level"] = self.rd.col_i32(blob, c_level)
            for bool_prop in ("IsInBattleState", "NeedScaleCamera", "TakeWeaponInSkill"):
                c = self._col(SKILL_EFFECT_CLS, bool_prop)
                if c is not None:
                    v = self.rd.col_bool(blob, c)
                    if v:
                        info[bool_prop] = v
            c_maxh = self._col(SKILL_EFFECT_CLS, "MaxHorizontalMotionDis")
            if c_maxh is not None:
                v = self.rd.col_f32(blob, c_maxh)
                if v is not None and v > 0:
                    info["max_horizontal_motion"] = round(v, 2)
            c_tags = self._col(SKILL_EFFECT_CLS, "Tags")
            if c_tags is not None:
                tags = self.rd.col_i32_array(zl, blob, c_tags)
                if tags:
                    info["tags"] = tags
            out.append(info)
        return out

    # ── buff reverse mapping ─────────────────────────────────────────────────

    def _buff_reverse_map(self, skill_ids: Set[int]) -> Dict[int, List[Dict]]:
        """skill_id → [{buff_id, name, ...}]  via BuffTable.SkillId back-ref."""
        c_id = self._col(BUFF_CLS, "Id")
        c_skill = self._col(BUFF_CLS, "SkillId")
        if c_skill is None:
            return {}
        c_type = self._col(BUFF_CLS, "BuffType")
        c_atype = self._col(BUFF_CLS, "BuffAbilityType")
        c_astype = self._col(BUFF_CLS, "BuffAbilitySubType")
        c_tags = self._col(BUFF_CLS, "Tags")
        result: Dict[int, List[Dict]] = {}
        for _key, zl, blob in self.rd.iter_rows_via_loader(BUFF_CLS):
            bid = self.rd.col_i32(blob, c_id)
            sid = self.rd.col_i32(blob, c_skill)
            if bid is None or bid <= 0 or sid not in skill_ids:
                continue
            name, name_src = self._resolve_name(BUFF_CLS, blob, zl)
            entry: Dict = {"buff_id": bid, "name": name, "name_src": name_src}
            if c_type is not None:
                entry["buff_type"] = self.rd.col_i32(blob, c_type)
            if c_atype is not None:
                entry["ability_type"] = self.rd.col_i32(blob, c_atype)
            if c_astype is not None:
                entry["ability_sub_type"] = self.rd.col_i32(blob, c_astype)
            if c_tags is not None:
                tags = self.rd.col_i32_array(zl, blob, c_tags)
                if tags:
                    entry["tags"] = tags
            result.setdefault(sid, []).append(entry)
        return result

    def buff_name(self, buff_id: int) -> str:
        idx = self._build_index(BUFF_CLS)
        ent = idx.get(int(buff_id))
        if not ent:
            return ""
        zl, blob = ent
        name, _ = self._resolve_name(BUFF_CLS, blob, zl)
        return name

    def buff_cluster(self, anchor_ids: List[int], radius: int = 100) -> List[Dict]:
        """Named buffs in the id-cluster around anchors (born/dead client buffs)."""
        anchors = [b for b in anchor_ids if b and b > 0]
        if not anchors:
            return []
        lo, hi = min(anchors) - radius, max(anchors) + radius
        idx = self._build_index(BUFF_CLS)
        out = []
        for bid, (zl, blob) in idx.items():
            if lo <= bid <= hi:
                name, src = self._resolve_name(BUFF_CLS, blob, zl)
                if name:
                    out.append({"buff_id": bid, "name": name, "name_src": src})
        return sorted(out, key=lambda b: b["buff_id"])

    # ── field (AOE ground) ───────────────────────────────────────────────────

    def field_for_skill(self, skill_id: int) -> List[Dict]:
        """Fields spawned by a skill (FieldTable.SkillId → match)."""
        try:
            idx = self._build_index(FIELD_CLS)
        except Exception:
            return []
        c_skill = self._col(FIELD_CLS, "SkillId")
        if c_skill is None:
            return []
        c_size = self._col(FIELD_CLS, "Size")
        c_name = self._col(FIELD_CLS, "Name")
        out = []
        for fid, (zl, blob) in idx.items():
            sid = self.rd.col_i64(blob, c_skill)
            if sid is None or int(sid) != int(skill_id):
                continue
            entry: Dict = {"field_id": fid}
            if c_name is not None:
                entry["name"] = self.rd.col_string(zl, blob, c_name)
            if c_size is not None:
                entry["size"] = self.rd.col_i32_array(zl, blob, c_size)
            out.append(entry)
        return out

    # ── main dump ────────────────────────────────────────────────────────────

    def dump_raid(self, dungeon_id: int, difficulty: Optional[int] = None) -> Dict:
        """Complete mechanics dump for one raid dungeon."""
        rows = self.all_raid_rows()
        exact = [r for r in rows if r["dungeon_id"] == dungeon_id]
        gids = {r["group_id"] for r in exact if r["group_id"]}
        family = [r for r in rows if r["group_id"] in gids] if gids else exact
        if not family:
            return {"dungeon_id": dungeon_id, "error": "no_raid_rows"}

        if difficulty is not None:
            sel = [r for r in family if r["difficulty"] == difficulty]
        else:
            sel = sorted(family, key=lambda r: r.get("difficulty") or 0, reverse=True)
        if not sel:
            return {"dungeon_id": dungeon_id, "error": "no_matching_difficulty"}
        row = sel[0]

        all_skill_ids: Set[int] = set()
        phases = []
        for phase_no, boss_id in enumerate(row["boss_ids"], 1):
            boss = self.boss_full_info(boss_id)
            if not boss:
                phases.append({"phase": phase_no, "boss_id": boss_id, "error": "no_monster_row"})
                continue
            all_skill_ids.update(boss["skill_ids"])

            skills = []
            for sid in boss["skill_ids"]:
                sk = self.skill_full_info(sid)
                if sk:
                    effects = self.skill_effects(sk.get("EffectIDs", []))
                    if effects:
                        sk["effects"] = effects
                    fields = self.field_for_skill(sid)
                    if fields:
                        sk["fields"] = fields
                    skills.append(sk)
                else:
                    skills.append({"id": sid, "error": "no_skill_row"})

            born_buff_names = {b: self.buff_name(b) for b in boss.get("born_client_buffs", [])}
            dead_buff_names = {b: self.buff_name(b) for b in boss.get("dead_client_buffs", [])}
            cluster_anchors = boss.get("born_client_buffs", []) + boss.get("dead_client_buffs", [])
            cluster = self.buff_cluster(cluster_anchors)

            phases.append({
                "phase": phase_no,
                "boss_id": boss_id,
                "boss": boss,
                "skills": skills,
                "born_buff_names": born_buff_names,
                "dead_buff_names": dead_buff_names,
                "mechanic_buff_cluster": cluster,
            })

        buff_reverse = self._buff_reverse_map(all_skill_ids)

        summary = self._build_summary(phases, buff_reverse)

        return {
            "dungeon_id": row["dungeon_id"],
            "group_id": row["group_id"],
            "difficulty": row["difficulty"],
            "name": row["name"],
            "desc": row["desc"],
            "boss_ids": row["boss_ids"],
            "family": [{
                "dungeon_id": r["dungeon_id"],
                "difficulty": r["difficulty"],
                "name": r["name"],
                "boss_ids": r["boss_ids"],
            } for r in family],
            "phases": phases,
            "buff_reverse_map": {str(k): v for k, v in buff_reverse.items()},
            "summary": summary,
            "generated_at": int(time.time()),
            "notes": list(self.notes),
        }

    def _build_summary(self, phases: List[Dict],
                       buff_reverse: Dict[int, List[Dict]]) -> Dict:
        """Summarize: mechanic skills, danger skills, phase transitions."""
        total_skills = 0
        mechanic_skills = []
        danger_skills = []
        normal_skills = []
        all_effect_buffs: List[Dict] = []

        for ph in phases:
            boss = ph.get("boss", {})
            for sk in ph.get("skills", []):
                if "error" in sk:
                    continue
                total_skills += 1
                sid = sk["id"]
                entry = {
                    "skill_id": sid,
                    "name": sk.get("name", ""),
                    "phase": ph["phase"],
                    "boss_id": ph["boss_id"],
                    "boss_name": boss.get("name", ""),
                    "category": sk.get("category", ""),
                }
                if sk.get("IsDangerSkill"):
                    danger_skills.append(entry)
                if sk.get("is_mechanic"):
                    mechanic_skills.append(entry)
                else:
                    normal_skills.append(entry)
                for eff in sk.get("effects", []):
                    for bid in eff.get("install_buffs", []):
                        all_effect_buffs.append({
                            "buff_id": bid,
                            "from_skill": sid,
                            "from_effect": eff["id"],
                        })

        return {
            "total_skills": total_skills,
            "mechanic_count": len(mechanic_skills),
            "danger_count": len(danger_skills),
            "normal_count": len(normal_skills),
            "mechanic_skills": mechanic_skills,
            "danger_skills": danger_skills,
            "effect_buffs": all_effect_buffs,
        }

    def dump_all_raids(self, target_dungeons: Optional[List[int]] = None) -> Dict:
        """Dump ALL known raid dungeons, or a specific set."""
        families = self.raid_families()
        raids = {}
        for gid, rows in families.items():
            dungeon_ids = sorted(set(r["dungeon_id"] for r in rows))
            if target_dungeons:
                dungeon_ids = [d for d in dungeon_ids if d in target_dungeons]
            if not dungeon_ids:
                continue
            best_diff = max(r["difficulty"] for r in rows)
            # use the highest difficulty of the primary dungeon
            primary = dungeon_ids[0]
            self.log(f"\n{'='*60}")
            self.log(f"[dump] Raid group {gid}: dungeons={dungeon_ids} best_diff={best_diff}")
            result = self.dump_raid(primary, difficulty=best_diff)
            if "error" not in result:
                raids[str(primary)] = result
                sm = result.get("summary", {})
                self.log(f"[dump]   {result.get('name','')}: "
                         f"{sm.get('total_skills',0)} skills, "
                         f"{sm.get('mechanic_count',0)} mechanics, "
                         f"{sm.get('danger_count',0)} danger skills")
        return {
            "raids": raids,
            "raid_count": len(raids),
            "generated_at": int(time.time()),
            "notes": list(self.notes),
        }


# ── apply: persist to name tables + skill store ─────────────────────────────

def _apply(result: Dict, log=print) -> Dict:
    report: Dict = {}
    from plugins.star_resonance_plugin.engines.boss_skill_store import BossSkillStore, KIND_SKILL
    store = BossSkillStore()
    name_rows: List[Dict] = []
    boss_names: Dict[int, str] = {}
    n_obs = 0

    for _did, raid in result.get("raids", {}).items():
        dungeon_id = int(raid["dungeon_id"])
        dungeon_name = raid.get("name") or ""
        for ph in raid.get("phases", []):
            boss = ph.get("boss", {})
            boss_id = int(ph.get("boss_id") or 0)
            boss_name = boss.get("name") or ""
            if boss_id > 0 and boss_name:
                boss_names[boss_id] = boss_name
            for sk in ph.get("skills", []):
                sid = int(sk.get("id") or 0)
                sname = sk.get("name") or ""
                if sid <= 0:
                    continue
                tags = ["config", "full_dump"]
                if sk.get("IsDangerSkill"):
                    tags.append("danger")
                if sk.get("is_mechanic"):
                    tags.append("mechanic")
                store.observe(scene_key=str(dungeon_id), dungeon_id=dungeon_id,
                              scene_name=dungeon_name, boss_base_id=boss_id,
                              boss_name=boss_name, obs_id=sid, name=sname,
                              kind=KIND_SKILL, tags=tags)
                n_obs += 1
                if sname:
                    name_rows.append({"text": sname, "confidence": "mem",
                                      "primary_match": {"id_space": "skill_id", "id": sid}})
            for b in ph.get("mechanic_buff_cluster", []):
                bid = int(b.get("buff_id") or 0)
                bname = b.get("name") or ""
                if bid > 0 and bname:
                    name_rows.append({"text": bname, "confidence": "mem",
                                      "primary_match": {"id_space": "buff_id", "id": bid}})

    saved = store.save(force=True)
    report["skill_store"] = {"observations": n_obs, "saved": saved}
    log(f"[apply] BossSkillStore: {n_obs} observations (saved={saved})")

    from plugins.star_resonance_plugin.net.tcp_name_cache import build_index_from_live_rows, sanitize_shared_cache
    raw = build_index_from_live_rows(
        {"rows": name_rows}, confidence={"high", "medium", "mem", "tcp", "static"})
    if boss_names:
        bucket = raw.setdefault("names", {}).setdefault("by_kind", {}).setdefault("monster", {})
        for bid, bname in boss_names.items():
            bucket[str(bid)] = {"text": bname, "confidence": "mem"}
    index = sanitize_shared_cache(raw)
    cache_path = os.path.join(_NAME_TABLES, "live_full_raid_dump_cache.json")
    os.makedirs(os.path.dirname(cache_path), exist_ok=True)
    with open(cache_path, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")
    from tools.tablekit.hybrid_name_tables import overlay_cache_into_existing_tables
    ov = overlay_cache_into_existing_tables(cache_path, write=True)
    changed = {k: v["changed"] for k, v in ov["kinds"].items() if v.get("changed")}
    report["name_overlay"] = {"cache": cache_path, "rows": len(name_rows),
                              "changed": changed}
    log(f"[apply] name overlay: {changed or 'no changes'} ({len(name_rows)} rows)")
    return report


# ── CLI ──────────────────────────────────────────────────────────────────────

def main(argv: Optional[List[str]] = None) -> int:
    _utf8_stdout()
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dungeon", type=int, default=None,
                   help="specific dungeon ID (default: all raids)")
    p.add_argument("--difficulty", type=int, default=None)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--apply", action="store_true",
                   help="persist to BossSkillStore + name tables")
    p.add_argument("--out", default=None)
    args = p.parse_args(argv)

    from mem_probe.il2cpp.static_dps_source import StaticDpsSource
    src = StaticDpsSource()
    _ = src.sr

    en = FullRaidEnumerator(src)
    en.prepare()

    if args.dungeon:
        result = {"raids": {str(args.dungeon): en.dump_raid(args.dungeon, args.difficulty)},
                  "raid_count": 1, "generated_at": int(time.time()), "notes": en.notes}
    else:
        result = en.dump_all_raids()

    # print summary
    print(f"\n{'='*60}")
    print(f"[dump] SUMMARY: {result['raid_count']} raids dumped")
    for did, raid in result.get("raids", {}).items():
        sm = raid.get("summary", {})
        print(f"\n  Dungeon {did} {raid.get('name','')} (diff={raid.get('difficulty','')})")
        for ph in raid.get("phases", []):
            boss = ph.get("boss", {})
            mechs = [s for s in ph.get("skills", [])
                     if s.get("is_mechanic") or s.get("IsDangerSkill")]
            print(f"    P{ph['phase']} {boss.get('name','')} (id={ph['boss_id']}): "
                  f"{len(ph.get('skills',[]))} skills, {len(mechs)} mechanics")
            for sk in mechs:
                danger = " [DANGER]" if sk.get("IsDangerSkill") else ""
                cat = sk.get("category", "")
                efx = len(sk.get("effects", []))
                print(f"      {sk['id']:<10} {sk.get('name',''):<20} "
                      f"cat={cat}{danger} effects={efx}")
        buffs = raid.get("buff_reverse_map", {})
        if buffs:
            print(f"    Buff reverse map: {sum(len(v) for v in buffs.values())} entries "
                  f"across {len(buffs)} skills")
        cluster_total = sum(len(ph.get("mechanic_buff_cluster", []))
                           for ph in raid.get("phases", []))
        if cluster_total:
            print(f"    Mechanic buff cluster: {cluster_total} named buffs")

    out_path = args.out or os.path.join(_EXPORTS, "full_raid_mechanics_dump.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(result, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"\n[dump] exported -> {out_path}")

    if args.apply:
        result["apply_report"] = _apply(result)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
            f.write("\n")
    elif not args.dry_run:
        print("[dump] pass --apply to persist, or --dry-run for export only")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
