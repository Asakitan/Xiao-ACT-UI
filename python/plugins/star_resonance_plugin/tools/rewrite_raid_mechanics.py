# -*- coding: utf-8 -*-
# rewrite_raid_mechanics - 用全量dump数据重写raid机制示例的几何/检测/躲避链路。
#
# 数据源:
# exports/boss_raids/full_raid_mechanics_dump.json   (在线技能全量)
# exports/boss_raids/deep_probe_results.json         (弹道形状/buff详情)
# exports/boss_raids/raid_skill_geometry.json         (AI范围)
# assets/name_tables/buff.json                       (buff名字)
# assets/boss_raids/*机制示例.json                     (现有示例)
#
# 链路:
# Skill name ←→ Bullet name (fuzzy match) → BulletShape → geometry
# AI range → fallback radius
# Buff data → detect.buff_ids enrichment
# ShapeType → dodge direction
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_BR = os.path.join(_ROOT, "assets", "boss_raids")
_EXP = os.path.join(_ROOT, "exports", "boss_raids")
_NT = os.path.join(_ROOT, "assets", "name_tables")


def _utf8():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


# ── shape type → geometry ────────────────────────────────────────────────────

SHAPE_TYPE_NAME = {0: "circle", 1: "line", 2: "cone", 3: "ring", 4: "complex"}

# dodge direction by shape
SHAPE_DODGE = {
    "circle": "away_boss",
    "line": "away_nearest",
    "cone": "away_boss",
    "ring": "away_nearest",
    "complex": "away_boss",
}


def _shape_to_geometry(stype: int, sdata: List[float]) -> Dict:
    # Convert BulletShape (type, data) → geometry dict.
    shape_name = SHAPE_TYPE_NAME.get(stype, "")
    if not shape_name or not sdata:
        return {}
    if stype == 0:
        r = sdata[0] if sdata else 0
        if r <= 0:
            return {}
        return {"shape": "circle", "radius": round(r, 1), "source": "bullet_shape"}
    elif stype == 1:
        w = sdata[0] if len(sdata) > 0 else 0
        h = sdata[1] if len(sdata) > 1 else 0
        depth = sdata[2] if len(sdata) > 2 else 0
        if w <= 0.2 and depth > 2:
            return {"shape": "line", "width": round(w, 1),
                    "radius": round(depth, 1), "source": "bullet_shape"}
        if h > 1:
            return {"shape": "line", "width": round(w, 1),
                    "radius": round(max(h, depth), 1), "source": "bullet_shape"}
        return {}
    elif stype == 2:
        r = sdata[0] if len(sdata) > 0 else 0
        angle = sdata[1] if len(sdata) > 1 else 0
        reach = sdata[2] if len(sdata) > 2 else 0
        if angle > 0:
            return {"shape": "cone", "radius": round(max(r, reach), 1),
                    "angle": round(angle, 1), "source": "bullet_shape"}
        return {}
    elif stype == 3:
        inner = sdata[0] if len(sdata) > 0 else 0
        outer = sdata[1] if len(sdata) > 1 else 0
        if outer > 0:
            return {"shape": "ring", "inner": round(inner, 1),
                    "radius": round(outer, 1), "source": "bullet_shape"}
        return {}
    elif stype == 4 and len(sdata) >= 3:
        r = max(sdata[2], sdata[3]) if len(sdata) > 3 else sdata[2]
        if r > 1:
            return {"shape": "circle", "radius": round(r, 1), "source": "bullet_shape"}
    return {}


def _ai_range_to_geometry(ai: List[float]) -> Dict:
    # AiReleaseSkillRange [min, optMin, optMax, max] → rough geometry.
    if not ai or len(ai) < 4:
        return {}
    opt_max = ai[2]
    rng_max = ai[3]
    r = opt_max if opt_max > 0 else rng_max
    if r <= 0 or r > 200:
        return {}
    if rng_max == -1 or rng_max >= 100:
        return {"shape": "circle", "radius": round(r, 1), "source": "ai_range_raidwide"}
    return {"shape": "circle", "radius": round(r, 1), "source": "ai_range"}


# ── bullet ↔ skill name matching ─────────────────────────────────────────────

def _normalize(s: str) -> str:
    return re.sub(r"[（）\(\)·\-—_ 　【】]", "", s.lower())


def _match_score(skill_name: str, bullet_name: str) -> float:
    # Fuzzy match score between a skill name and bullet name.
    sn = _normalize(skill_name)
    bn = _normalize(bullet_name)
    if not sn or not bn:
        return 0.0
    if sn in bn or bn in sn:
        return 0.9
    common = sum(1 for c in sn if c in bn)
    return common / max(len(sn), len(bn))


def build_bullet_shape_map(deep: Dict) -> Dict[str, Dict]:
    # bullet_name → best shape geometry.
    shapes = deep.get("bullet_shapes", {})
    bullets = deep.get("raid_bullets", {})
    result: Dict[str, Dict] = {}
    for bid_s, b in bullets.items():
        bid = int(bid_s)
        bname = b.get("name", "")
        if not bname:
            continue
        best_geom = {}
        for variant in range(20):
            for mul in (100, 10):
                shape_id = str(bid * mul + variant)
                if shape_id in shapes:
                    sh = shapes[shape_id]
                    geom = _shape_to_geometry(sh["type"], sh["data"])
                    if geom and (not best_geom or
                                 geom.get("radius", 0) > best_geom.get("radius", 0)):
                        best_geom = geom
                        best_geom["_bullet_id"] = bid
                        best_geom["_shape_id"] = int(shape_id)
        if best_geom:
            result[bname] = best_geom
    return result


def find_geometry_for_skill(skill_name: str, bullet_map: Dict[str, Dict],
                            ai_range: Optional[List[float]] = None) -> Dict:
    # Find best geometry for a skill: bullet shape match → AI range fallback.
    best_score = 0.3
    best_geom = {}
    for bname, geom in bullet_map.items():
        score = _match_score(skill_name, bname)
        if score > best_score:
            best_score = score
            best_geom = dict(geom)
    if best_geom:
        best_geom.pop("_bullet_id", None)
        best_geom.pop("_shape_id", None)
        return best_geom
    if ai_range:
        return _ai_range_to_geometry(ai_range)
    return {}


# ── mechanic rewriter ────────────────────────────────────────────────────────

def rewrite_file(filepath: str, dump: Dict, deep: Dict, geom_data: Dict,
                 buffs: Dict, dry_run: bool) -> int:
    # Rewrite a single raid example file. Returns change count.
    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)

    prof = data.get("profile", {})
    mechs = prof.get("mechanics", [])
    fn = os.path.basename(filepath)
    changes = 0

    bullet_map = build_bullet_shape_map(deep)

    # Build skill metadata lookup from dump
    skill_meta = {}
    for did, raid in dump.get("raids", {}).items():
        for ph in raid.get("phases", []):
            for sk in ph.get("skills", []):
                skill_meta[sk["id"]] = sk

    # Build AI range lookup from geom_data
    ai_ranges = {}
    for sk in geom_data.get("skills", []):
        for eff in sk.get("effects", []):
            ai = eff.get("ai", [])
            if ai and any(v != 0 for v in ai):
                ai_ranges[sk["id"]] = ai

    # Build buff enrichment map: mechanic buffs we know about
    raid_buffs = deep.get("raid_buff_data", {})

    for mech in mechs:
        det = mech.get("detect", {})
        dodge = mech.get("dodge", {})
        inline = dodge.get("inline", {})
        geom = inline.get("geometry", {})
        alert = mech.get("alert", {})
        mname = mech.get("name", "")

        # ── 1. Fill geometry ──────────────────────────────────────────
        if not geom.get("source") or geom.get("source") == "":
            skill_ids = det.get("skill_ids", [])
            best_geom = {}

            for sid in skill_ids:
                sm = skill_meta.get(sid, {})
                sname = sm.get("name", mname)
                ai = ai_ranges.get(sid)
                g = find_geometry_for_skill(sname, bullet_map, ai)
                if g and (not best_geom or
                          g.get("radius", 0) > best_geom.get("radius", 0)):
                    best_geom = g

            if not best_geom and skill_ids:
                for sid in skill_ids:
                    ai = ai_ranges.get(sid)
                    if ai:
                        g = _ai_range_to_geometry(ai)
                        if g:
                            best_geom = g
                            break

            if best_geom:
                for k, v in best_geom.items():
                    geom[k] = v
                if "geometry" not in inline:
                    inline["geometry"] = geom
                changes += 1

        # ── 2. Set dodge direction from geometry shape ────────────────
        if dodge.get("enabled") and not inline.get("direction"):
            shape = geom.get("shape", "")
            if shape in SHAPE_DODGE:
                inline["direction"] = SHAPE_DODGE[shape]
                changes += 1

        # ── 3. Enrich buff_ids from raid_buff_data ────────────────────
        existing_buffs = set(det.get("buff_ids", []))
        keywords = _extract_keywords(mname)
        if keywords:
            for bid_s, bdata in raid_buffs.items():
                bid = int(bid_s)
                if bid in existing_buffs:
                    continue
                bname = bdata.get("name", "")
                if not bname:
                    continue
                vis = bdata.get("visible", 0)
                if vis == 2:
                    continue
                if bdata.get("buff_type", 0) == 0 and vis == 2:
                    continue
                for kw in keywords:
                    if kw in bname and det.get("source", "any") in ("any", "boss"):
                        det.setdefault("buff_ids", []).append(bid)
                        existing_buffs.add(bid)
                        changes += 1
                        break

        # ── 4. Fill countdown from bullet duration ────────────────────
        if not alert.get("countdown_s") or alert["countdown_s"] <= 0:
            for sid in det.get("skill_ids", []):
                sm = skill_meta.get(sid, {})
                for eff in sm.get("effects", []):
                    dur = eff.get("duration")
                    if dur and dur > 0.5:
                        alert["countdown_s"] = min(round(dur, 1), 8.0)
                        changes += 1
                        break

        # ── 5. Add notes with mechanic data ───────────────────────────
        notes = mech.get("notes", "")
        for sid in det.get("skill_ids", []):
            sm = skill_meta.get(sid, {})
            if sm.get("desc") and sm["desc"] not in (notes or ""):
                extra = sm["desc"][:60]
                if notes:
                    mech["notes"] = notes + "\n" + extra
                else:
                    mech["notes"] = extra
                changes += 1
                break

    if changes > 0 and not dry_run:
        data["profile"]["mechanics"] = mechs
        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.write("\n")

    return changes


def _extract_keywords(mech_name: str) -> List[str]:
    # Extract matching keywords from a mechanic name for buff lookup.
    kw_map = {
        "分摊": ["分摊"],
        "致死": ["致死", "秒杀", "死亡"],
        "连线": ["连线"],
        "点名": ["点名"],
        "衰减": ["衰减"],
        "环形": ["环形", "崩石环"],
        "横扫": ["横扫"],
        "吐息": ["吐息"],
        "冲锋": ["冲锋", "冲"],
        "陨石": ["陨石", "陨星", "光陨"],
        "全场": ["全场", "全屏"],
        "领地": ["领地"],
        "角斗": ["角斗"],
        "放逐": ["放逐"],
        "宣告": ["宣告"],
        "试炼": ["试炼"],
        "震荡": ["震荡"],
        "脉冲": ["脉冲"],
    }
    result = []
    for trigger, keywords in kw_map.items():
        if trigger in mech_name:
            result.extend(keywords)
    return result


def main(argv=None) -> int:
    _utf8()
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--apply", action="store_true")
    args = p.parse_args(argv)

    dump_path = os.path.join(_EXP, "full_raid_mechanics_dump.json")
    deep_path = os.path.join(_EXP, "deep_probe_results.json")
    geom_path = os.path.join(_EXP, "raid_skill_geometry.json")

    with open(dump_path, "r", encoding="utf-8") as f:
        dump = json.load(f)
    with open(deep_path, "r", encoding="utf-8") as f:
        deep = json.load(f)
    with open(geom_path, "r", encoding="utf-8") as f:
        geom_data = json.load(f)
    with open(os.path.join(_NT, "buff.json"), "r", encoding="utf-8") as f:
        buffs = json.load(f)

    total = 0
    for fn in sorted(os.listdir(_BR)):
        if not fn.endswith("机制示例.json"):
            continue
        path = os.path.join(_BR, fn)
        n = rewrite_file(path, dump, deep, geom_data, buffs, dry_run=not args.apply)
        if n:
            tag = "WRITTEN" if args.apply else "DRY-RUN"
            print(f"[{tag}] {fn}: {n} changes")
        else:
            print(f"[OK] {fn}: no changes needed")
        total += n

    print(f"\nTotal: {total} changes across {len(os.listdir(_BR))} files")
    if not args.apply and total > 0:
        print("Pass --apply to write")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
