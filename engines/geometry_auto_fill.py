# -*- coding: utf-8 -*-
"""geometry_auto_fill - 自动把读到的几何智能填进机制壳子 (fill_geometry 的智能调度).

两种填法:
  1. **按 skill_id 精确填** (运行时时间关联, 最准): boss 放招 X 的瞬间场上新出现的几何实体
     = 招 X 的几何 → 直接填进 detect.skill_ids 含 X 的机制。fill_by_skill_id()。
  2. **按名字相似度填** (FieldTable 领域几何, best-effort): field 名 vs 机制名的中文 2-gram
     重叠打分, 高分(且 boss 标识一致)才填, 避免错填。auto_fill_from_fields()。

纯逻辑可测 (匹配/填入不碰内存); 内存读由 FieldGeometryReader / 运行时采集器提供。
"""
from __future__ import annotations

from typing import Dict, List, Optional


def _bigrams(s: str) -> set:
    s = "".join(ch for ch in (s or "") if ch.strip())
    return {s[i:i + 2] for i in range(len(s) - 1)} if len(s) >= 2 else ({s} if s else set())


def name_similarity(a: str, b: str) -> float:
    """中文 2-gram 重叠相似度 0~1 (Dice 系数)。"""
    ga, gb = _bigrams(a), _bigrams(b)
    if not ga or not gb:
        return 0.0
    inter = len(ga & gb)
    return 2.0 * inter / (len(ga) + len(gb))


def _set_geometry(mech: dict, shape: str, radius: float, inner: float = 0.0,
                  angle: float = 0.0, width: float = 0.0, center: str = "boss",
                  source: str = "runtime") -> None:
    inline = mech.setdefault("dodge", {}).setdefault("inline", {})
    inline["geometry"] = {"shape": str(shape), "radius": float(radius),
                          "inner": float(inner), "angle": float(angle),
                          "width": float(width), "center": str(center),
                          "source": str(source)}
    mech.pop("_needs_geometry", None)


def fill_by_skill_id(profile: dict, skill_id: int, *, shape: str, radius: float,
                     inner: float = 0.0, angle: float = 0.0, width: float = 0.0,
                     center: str = "boss", source: str = "runtime") -> int:
    """运行时时间关联结果: 把几何精确填进 detect.skill_ids 含 skill_id 的机制。返回填的条数。"""
    n = 0
    for m in (profile or {}).get("mechanics", []) or []:
        ids = ((m.get("detect") or {}).get("skill_ids")) or []
        if int(skill_id) in [int(i) for i in ids]:
            _set_geometry(m, shape, radius, inner, angle, width, center, source)
            n += 1
    return n


def auto_fill_from_fields(profile: dict, field_geoms: List[Dict],
                          min_score: float = 0.5) -> List[Dict]:
    """FieldTable 领域几何 → 名字相似度匹配机制, 高分才填。返回填入记录 [{mech, field, score}]。
    只填 source 未填(占位)的机制, 不覆盖已有(运行时/手填优先)。"""
    filled = []
    boss_pat = (profile or {}).get("target_name_pattern", "")
    for m in (profile or {}).get("mechanics", []) or []:
        inline = (m.get("dodge") or {}).get("inline") or {}
        geom = inline.get("geometry") or {}
        if geom.get("source"):           # 已填(运行时/手填)→ 不覆盖
            continue
        best, best_score = None, min_score
        for fg in field_geoms:
            # boss 标识一致才考虑 (避免跨 boss 错填)
            if fg.get("boss") and boss_pat and fg["boss"] not in boss_pat \
                    and boss_pat not in fg["boss"]:
                continue
            s = name_similarity(m.get("name", ""), fg.get("name", ""))
            if s >= best_score:
                best, best_score = fg, s
        if best is not None:
            _set_geometry(m, best.get("shape", "circle"), best.get("radius", 0.0),
                          best.get("inner", 0.0), source="reverse")
            filled.append({"mech": m.get("name"), "field": best.get("name"),
                           "score": round(best_score, 2)})
    return filled


__all__ = ["name_similarity", "fill_by_skill_id", "auto_fill_from_fields"]
