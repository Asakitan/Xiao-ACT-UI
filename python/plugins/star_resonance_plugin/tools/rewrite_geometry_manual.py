# -*- coding: utf-8 -*-
# 手动根据机制语义重写所有raid示例的geometry和dodge参数。
#
# 基于:
# 1. 技能名语义(横扫=半场扇形, 分摊=聚集圈, 吐息=锥形等)
# 2. AI范围数据(optMax作为有效半径)
# 3. BulletShape几何(扇形角度/线形宽度)
# 4. MMO通用机制尺寸常识
#
# 规则:
# - 已有source != "" 的几何不覆盖(认为是手动验证过的)
# - 不改notes/alert/detect(保留现有的手写内容)
# - 只改 geometry + 部分dodge方向
from __future__ import annotations

import json
import os
import sys
import re
from typing import Dict, List, Optional, Tuple

sys.stdout.reconfigure(encoding="utf-8")
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BR = os.path.join(_ROOT, "assets", "boss_raids")
_EXP = os.path.join(_ROOT, "exports", "boss_raids")

# ── 按技能名/机制类型的标准几何 ─────────────────────────────────────────────

def _infer_geometry(mech: dict, ai: List[float] = None) -> Tuple[dict, str]:
    # 根据机制名/kind/技能名推断正确的geometry和dodge direction。
    # 返回 (geometry_dict, dodge_direction)。
    name = mech.get("name", "")
    kind = mech.get("kind", "")
    det = mech.get("detect", {})
    existing_dir = mech.get("dodge", {}).get("inline", {}).get("direction", "")

    ai_r = 0.0
    if ai and len(ai) >= 4:
        ai_r = ai[2] if ai[2] > 0 else (ai[3] if 0 < ai[3] < 100 else 0)

    # ── 全场/毁灭 ──
    if any(kw in name for kw in ["全场", "全团", "毁灭", "无尽寒冬", "史诗狂怒"]):
        return ({"shape": "circle", "radius": 100.0, "center": "boss",
                 "source": "semantic"}, "")

    # ── 分摊(集合) ──
    if any(kw in name for kw in ["分摊"]) and "领地" not in name:
        r = ai_r if ai_r > 2 else 6.0
        return ({"shape": "circle", "radius": r, "center": "self",
                 "source": "semantic"}, "goto_teammate")

    # ── 分散/扩散 ──
    if any(kw in name for kw in ["分散", "扩散"]):
        r = ai_r if ai_r > 2 else 10.0
        return ({"shape": "circle", "radius": r, "center": "self",
                 "source": "semantic"}, "away_nearest")

    # ── 致死/死刑/秒杀 ──
    if any(kw in name for kw in ["致死", "死刑", "秒杀", "宣告", "死缓"]):
        r = ai_r if ai_r > 1 else 4.0
        return ({"shape": "circle", "radius": r, "center": "boss",
                 "source": "semantic"}, "")

    # ── 横扫/扫尾 (半场扇形) ──
    if any(kw in name for kw in ["横扫", "扫尾", "激光"]) and "半场" in name:
        r = ai_r if ai_r > 3 else 20.0
        return ({"shape": "cone", "radius": r, "angle": 180.0,
                 "center": "boss", "source": "semantic"}, "away_boss")

    # ── 吐息/龙息 (前方锥形) ──
    if any(kw in name for kw in ["吐息", "龙息", "喷吐", "喷洒"]):
        r = ai_r if ai_r > 3 else 15.0
        return ({"shape": "cone", "radius": r, "angle": 90.0,
                 "center": "boss", "source": "semantic"}, "away_boss")

    # ── 顺劈/横扫(不含半场) ──
    if any(kw in name for kw in ["横扫", "扫尾", "顺劈", "挥砍"]):
        r = ai_r if ai_r > 2 else 8.0
        return ({"shape": "cone", "radius": r, "angle": 120.0,
                 "center": "boss", "source": "semantic"}, "away_boss")

    # ── 砸地/重击/拍地 (boss脚下圈) ──
    if any(kw in name for kw in ["砸地", "重击", "拍地", "碎地", "跳砸", "蓄力下砸"]):
        r = ai_r if ai_r > 2 else 8.0
        return ({"shape": "circle", "radius": r, "center": "boss",
                 "source": "semantic"}, "away_boss")

    # ── 环形/崩石环 (环形AOE) ──
    if any(kw in name for kw in ["环形", "崩石环"]):
        r = ai_r if ai_r > 5 else 15.0
        inner = r * 0.3
        return ({"shape": "ring", "radius": r, "inner": round(inner, 1),
                 "center": "boss", "source": "semantic"}, "away_nearest")

    # ── 冲锋 ──
    if any(kw in name for kw in ["冲锋", "冲", "突进"]):
        r = ai_r if ai_r > 3 else 25.0
        return ({"shape": "line", "radius": r, "width": 4.0,
                 "center": "boss", "source": "semantic"}, "away_boss")

    # ── 贯穿/激光/光束/轨道炮 (直线) ──
    if any(kw in name for kw in ["贯穿", "激光", "光束", "光线", "轨道", "射线", "毁灭光线"]):
        r = ai_r if ai_r > 5 else 30.0
        return ({"shape": "line", "radius": r, "width": 3.0,
                 "center": "boss", "source": "semantic"}, "away_boss")

    # ── 点名/光柱/标记 (点目标圈) ──
    if any(kw in name for kw in ["点名", "光柱", "标记", "闪击"]):
        r = ai_r if ai_r > 2 else 6.0
        return ({"shape": "circle", "radius": r, "center": "self",
                 "source": "semantic"}, "away_nearest")

    # ── 连线 ──
    if any(kw in name for kw in ["连线", "连结"]):
        return ({"shape": "line", "radius": 20.0, "width": 2.0,
                 "center": "self", "source": "semantic"}, "away_nearest")

    # ── 十字 ──
    if any(kw in name for kw in ["十字"]):
        r = ai_r if ai_r > 5 else 15.0
        return ({"shape": "cross", "radius": r, "width": 3.0,
                 "center": "boss", "source": "semantic"}, "away_nearest")

    # ── 陨石/陨星/光陨 (落点圈) ──
    if any(kw in name for kw in ["陨石", "陨星", "光陨", "飞石"]):
        r = ai_r if ai_r > 2 else 7.0
        return ({"shape": "circle", "radius": r, "center": "self",
                 "source": "semantic"}, "away_nearest")

    # ── 压团血/AOE ──
    if any(kw in name for kw in ["压团", "aoe", "AOE", "填充"]):
        r = ai_r if ai_r > 2 else 10.0
        return ({"shape": "circle", "radius": r, "center": "boss",
                 "source": "semantic"}, "away_boss")

    # ── 地震波/裂石风暴 ──
    if any(kw in name for kw in ["地震", "震荡", "风暴", "裂石"]):
        r = ai_r if ai_r > 5 else 15.0
        return ({"shape": "circle", "radius": r, "center": "boss",
                 "source": "semantic"}, "away_boss")

    # ── 冰霜/冰封/零度 (boss中心圈) ──
    if any(kw in name for kw in ["冰霜", "冰封", "零度"]):
        r = ai_r if ai_r > 3 else 12.0
        return ({"shape": "circle", "radius": r, "center": "boss",
                 "source": "semantic"}, "away_boss")

    # ── 虚蚀球/子弹/旋转子弹 (弹幕) ──
    if any(kw in name for kw in ["子弹", "虚蚀球", "回旋"]):
        r = ai_r if ai_r > 3 else 12.0
        return ({"shape": "circle", "radius": r, "center": "boss",
                 "source": "semantic"}, "away_nearest")

    # ── 大炎戒/火环 (大范围圈) ──
    if any(kw in name for kw in ["炎戒", "火环", "大炎"]):
        r = ai_r if ai_r > 5 else 15.0
        return ({"shape": "ring", "radius": r, "inner": round(r * 0.4, 1),
                 "center": "boss", "source": "semantic"}, "away_nearest")

    # ── 净化之光(扩散/收缩) ──
    if "净化" in name:
        r = 12.0
        if "扩散" in name:
            return ({"shape": "ring", "radius": r, "inner": 0.0,
                     "center": "boss", "source": "semantic"}, "away_nearest")
        elif "收缩" in name:
            return ({"shape": "ring", "radius": r, "inner": r * 0.6,
                     "center": "boss", "source": "semantic"}, "away_boss")
        return ({"shape": "ring", "radius": r, "inner": 0.0,
                 "center": "boss", "source": "semantic"}, "away_nearest")

    # ── 共鸣/果实/召唤/特殊 (不需要几何) ──
    if any(kw in name for kw in ["共鸣", "果实", "召唤", "相位", "传送",
                                  "吸收", "誓死", "承诺", "衰减", "驱逐",
                                  "密码", "弹球", "时停", "魔方",
                                  "锤地", "撞墙"]):
        return ({}, existing_dir)

    # ── fallback: 用AI range ──
    if ai_r > 1:
        return ({"shape": "circle", "radius": ai_r, "center": "boss",
                 "source": "ai_range"}, existing_dir)

    return ({}, existing_dir)


def main():
    # Load AI ranges
    with open(os.path.join(_EXP, "raid_skill_geometry.json"), encoding="utf-8") as f:
        geom_raw = json.load(f)
    ai_map: Dict[int, List[float]] = {}
    for sk in geom_raw.get("skills", []):
        for eff in sk.get("effects", []):
            ai = eff.get("ai", [])
            if ai and any(v != 0 for v in ai):
                ai_map[sk["id"]] = ai
                break

    total = 0
    for fn in sorted(os.listdir(_BR)):
        if not fn.endswith("机制示例.json"):
            continue
        path = os.path.join(_BR, fn)
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        mechs = data.get("profile", {}).get("mechanics", [])
        changes = 0

        for mech in mechs:
            dodge = mech.get("dodge", {})
            inline = dodge.get("inline", {})
            geom = inline.get("geometry", {})

            if geom.get("source") and geom["source"] not in ("", "ai_range",
                                                               "ai_range_raidwide",
                                                               "bullet_shape"):
                continue

            det = mech.get("detect", {})
            skill_ids = det.get("skill_ids", [])
            best_ai = None
            for sid in skill_ids:
                if sid in ai_map:
                    best_ai = ai_map[sid]
                    break

            new_geom, new_dir = _infer_geometry(mech, best_ai)
            if not new_geom:
                continue

            changed = False
            for k, v in new_geom.items():
                if k == "source" or geom.get(k) == v:
                    continue
                geom[k] = v
                changed = True
            if changed:
                geom["source"] = new_geom.get("source", "semantic")
                inline["geometry"] = geom

            if new_dir and not inline.get("direction"):
                inline["direction"] = new_dir
                changed = True

            if changed:
                changes += 1

        if changes:
            data["profile"]["mechanics"] = mechs
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
                f.write("\n")
            print(f"[WRITTEN] {fn}: {changes} geometries")
        else:
            print(f"[OK] {fn}")
        total += changes

    print(f"\nTotal: {total} geometry fills")


if __name__ == "__main__":
    main()
