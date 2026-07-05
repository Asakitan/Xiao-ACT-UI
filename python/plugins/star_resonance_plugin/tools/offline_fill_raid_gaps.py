# -*- coding: utf-8 -*-
# offline_fill_raid_gaps - 离线填充raid示例中的未绑定mechanic skill/buff。
#
# 纯离线: 从已有name table交叉分析缺口, 直接写入raid示例JSON。
#
# Usage:
# python -m tools.offline_fill_raid_gaps --dry-run     # 仅打印变更
# python -m tools.offline_fill_raid_gaps --apply        # 写入文件
from __future__ import annotations

import argparse
import copy
import json
import os
import sys
import time
import uuid
from typing import Dict, List, Optional, Set

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_BR = os.path.join(_ROOT, "assets", "boss_raids")


def _utf8():
    try:
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    except Exception:
        pass


def _mech_id() -> str:
    return f"mech_{uuid.uuid4().hex[:8]}"


def _make_mechanic(name: str, skill_ids: List[int] = None, buff_ids: List[int] = None,
                   source: str = "any", kind: str = "", notes: str = "",
                   countdown_s: float = 4.0, tts: str = "",
                   dodge_enabled: bool = False, dodge_dir: str = "",
                   phase_ids: List[str] = None) -> dict:
    return {
        "id": _mech_id(),
        "name": name,
        "kind": kind or name,
        "enabled": True,
        "notes": notes,
        "color": "",
        "phase_ids": phase_ids or [],
        "detect": {
            "skill_ids": skill_ids or [],
            "buff_ids": buff_ids or [],
            "source": source,
            "boss_base_id": 0,
            "hp_pct": 0.0,
            "time_into_phase_s": 0.0,
            "time_into_fight_s": 0.0,
            "repeat_interval_s": 0.0,
            "event": "",
        },
        "alert": {
            "enabled": True,
            "banner_text": name[:14],
            "tts_text": tts or name,
            "alert_type": "both",
            "countdown_s": countdown_s,
            "pre_warn_s": 2.0,
            "cooldown_s": 4.0,
            "sound": "boss_alert",
        },
        "dodge": {
            "enabled": dodge_enabled,
            "linkage_id": "",
            "inline": {
                "direction": dodge_dir,
                "move_ms": 2000,
                "fallback_direction": "back",
                "geometry": {"shape": "", "radius": 0.0, "source": ""},
            },
        },
    }


# ── changes per file ─────────────────────────────────────────────────────────

def _apply_鸣角之野(mechs: List[dict]) -> List[dict]:
    # 双子: 11 uncovered skills + key buffs.
    changes = []

    mech_by_id = {m["id"]: m for m in mechs}

    # 1. 领地分摊 skill variants → add to existing
    for m in mechs:
        if m["id"] == "mech_10280011" or ("炎光领地分摊" in m["name"]):
            if 10280051 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10280051)
                changes.append(f"  + skill 10280051(领地分摊) → {m['name']}")
        if m["id"] == "mech_10280111" or ("幻华领地分摊" in m["name"]):
            if 10280151 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10280151)
                changes.append(f"  + skill 10280151(领地分摊) → {m['name']}")

    # 2. 死亡计时 → add to existing
    for m in mechs:
        if "死亡计时" in m["name"]:
            if 10281207 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10281207)
                changes.append(f"  + skill 10281207(死亡计时) → {m['name']}")

    # 3. 全场aoe variants → add to existing
    for m in mechs:
        if m.get("name") == "炎光全场aoe":
            if 10281254 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10281254)
                changes.append(f"  + skill 10281254(全场aoe) → {m['name']}")
        if m.get("name") == "幻华全场aoe":
            if 10281254 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10281254)
                changes.append(f"  + skill 10281254(全场aoe) → {m['name']}")

    # 4. 连线开始 → add to both 连线 mechanics
    for m in mechs:
        if "炎光之连线" in m["name"] or "炎光连线" in m["name"]:
            if 10281201 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10281201)
                changes.append(f"  + skill 10281201(连线开始) → {m['name']}")
        if "幻华之连线" in m["name"] or "幻华连线" in m["name"]:
            if 10281201 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10281201)
                changes.append(f"  + skill 10281201(连线开始) → {m['name']}")

    # 5. NEW: 领地致死 (炎光/幻华)
    mechs.append(_make_mechanic(
        "炎光领地致死", skill_ids=[10280052], buff_ids=[827144],
        kind="致死", tts="领地致死", countdown_s=3.0,
        notes="领地驱逐后未踩塔致死"))
    changes.append("  NEW 炎光领地致死 (skill:10280052 buff:827144)")

    mechs.append(_make_mechanic(
        "幻华领地致死", skill_ids=[10280152], buff_ids=[827147],
        kind="致死", tts="领地致死", countdown_s=3.0,
        notes="领地驱逐后未踩塔致死"))
    changes.append("  NEW 幻华领地致死 (skill:10280152 buff:827147)")

    # 6. NEW: 角斗失败致死
    mechs.append(_make_mechanic(
        "炎光角斗失败致死", skill_ids=[10280054],
        kind="致死", tts="角斗致死", countdown_s=2.0,
        notes="角斗判定失败 → 秒杀"))
    changes.append("  NEW 炎光角斗失败致死 (skill:10280054)")

    mechs.append(_make_mechanic(
        "幻华角斗失败致死", skill_ids=[10280153],
        kind="致死", tts="角斗致死", countdown_s=2.0,
        notes="角斗判定失败 → 秒杀"))
    changes.append("  NEW 幻华角斗失败致死 (skill:10280153)")

    # 7. NEW: 全场aoe但中间安全
    mechs.append(_make_mechanic(
        "全场aoe·中间安全", skill_ids=[10281250],
        kind="全场AOE", tts="中间安全", countdown_s=5.0,
        dodge_enabled=True, dodge_dir="goto_circle",
        notes="全场伤害但中央安全区"))
    changes.append("  NEW 全场aoe·中间安全 (skill:10281250)")

    # 8. NEW: 领地驱逐指令
    mechs.append(_make_mechanic(
        "领地驱逐指令", skill_ids=[10281205],
        kind="特殊", tts="驱逐指令", countdown_s=4.0,
        notes="领地驱逐 → 需要换场"))
    changes.append("  NEW 领地驱逐指令 (skill:10281205)")

    # 9. 致死判断buff → add to existing 致死 mechanics
    for m in mechs:
        if "炎光致死踩塔" in m["name"]:
            if 827124 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(827124)
                changes.append(f"  + buff 827124(炎光致死判断死亡) → {m['name']}")
        if "幻华致死踩塔" in m["name"]:
            if 827126 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(827126)
                changes.append(f"  + buff 827126(幻华致死判断死亡) → {m['name']}")

    # 10. 秒杀全场 buff → add to 全场aoe
    for m in mechs:
        if m.get("name") == "炎光全场aoe":
            if 827149 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(827149)
                changes.append(f"  + buff 827149(炎光秒杀全场) → {m['name']}")

    return changes


def _apply_石骸之庭(mechs: List[dict]) -> List[dict]:
    # 石头人: 2 uncovered skills.
    changes = []
    for m in mechs:
        # 石头人分摊 → 石头人压团血
        if "压团血" in m["name"] or ("分摊" in m["name"] and "石" in m.get("kind", "")):
            if 10290154 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10290154)
                changes.append(f"  + skill 10290154(石头人分摊) → {m['name']}")
        # 石头人点名崩石环 → 崩石环
        if "崩石环" in m["name"]:
            if 10290158 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10290158)
                changes.append(f"  + skill 10290158(石头人点名崩石环) → {m['name']}")
    # fallback: try matching by kind
    if not any("10290154" in str(m["detect"]["skill_ids"]) for m in mechs):
        for m in mechs:
            if m.get("kind") == "分摊" and "石头人" not in m["name"]:
                if 10290154 not in m["detect"]["skill_ids"]:
                    m["detect"]["skill_ids"].append(10290154)
                    changes.append(f"  + skill 10290154(石头人分摊) → {m['name']} [by kind]")
                    break
    if not any("10290158" in str(m["detect"]["skill_ids"]) for m in mechs):
        for m in mechs:
            if "环" in m.get("kind", "") or "环" in m.get("name", ""):
                if 10290158 not in m["detect"]["skill_ids"]:
                    m["detect"]["skill_ids"].append(10290158)
                    changes.append(f"  + skill 10290158(崩石环) → {m['name']} [by kind]")
                    break
    return changes


def _apply_蚀影之渊(mechs: List[dict]) -> List[dict]:
    # 蚀花: 6 uncovered skills + key buffs.
    changes = []

    for m in mechs:
        # 转阶段吼 + 转阶段地板 + 转阶段迷雾 → 终焉化身(转阶段mechanic)
        if "终焉" in m["name"] or "转阶段" in m.get("kind", ""):
            for sid, label in [(10300111, "转阶段吼"), (10300355, "转阶段地板伤害"),
                               (10300356, "转阶段迷雾")]:
                if sid not in m["detect"]["skill_ids"]:
                    m["detect"]["skill_ids"].append(sid)
                    changes.append(f"  + skill {sid}({label}) → {m['name']}")
            if 828144 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(828144)
                changes.append(f"  + buff 828144(战斗状态转阶段) → {m['name']}")

        # 致死 P2 variant → 致死 mechanic
        if m.get("name") == "致死" or "致死" in m["name"] and m.get("kind") == "致死":
            if 10300203 not in m["detect"]["skill_ids"]:
                m["detect"]["skill_ids"].append(10300203)
                changes.append(f"  + skill 10300203(致死P2) → {m['name']}")

    # NEW: 殷红断狱-千斩点名
    mechs.append(_make_mechanic(
        "绯狱千斩点名", skill_ids=[10300306], buff_ids=[828132],
        source="boss", kind="点名",
        tts="千斩点名", countdown_s=5.0,
        dodge_enabled=True, dodge_dir="away_boss",
        notes="殷红断狱阶段的绯狱千斩 → 被点名远离boss"))
    changes.append("  NEW 绯狱千斩点名 (skill:10300306 buff:828132)")

    # NEW: 密码破译
    mechs.append(_make_mechanic(
        "密码破译", skill_ids=[10300354],
        kind="特殊", tts="密码破译", countdown_s=6.0,
        notes="触发密码破译效果 → 特殊机制"))
    changes.append("  NEW 密码破译 (skill:10300354)")

    # NEW: 绯红剑影点名 (P1 + P2 variants as buff detection)
    mechs.append(_make_mechanic(
        "绯红剑影点名", buff_ids=[828107, 828124],
        source="self", kind="点名",
        tts="剑影点名", countdown_s=4.0,
        dodge_enabled=True, dodge_dir="away_nearest",
        notes="被绯红剑影点名 → 远离队友\nP1=828107 P2=828124"))
    changes.append("  NEW 绯红剑影点名 (buff:828107,828124)")

    return changes


# ── 13003 buff gap fills ─────────────────────────────────────────────────────

def _apply_永冻的异想天(mechs: List[dict]) -> List[dict]:
    # 冰龙: fill key mechanic buffs.
    changes = []
    for m in mechs:
        if "雪崩" in m["name"]:
            if 882112 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(882112)
                changes.append(f"  + buff 882112(雪崩连续打击点名) → {m['name']}")
        if "零度雪域" in m["name"]:
            for bid in [882153, 882156]:
                if bid not in m["detect"]["buff_ids"]:
                    m["detect"]["buff_ids"].append(bid)
                    changes.append(f"  + buff {bid} → {m['name']}")
        if "冰封" in m["name"] or "衰减" in m["name"]:
            if 882155 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(882155)
                changes.append(f"  + buff 882155(冰封之器点名衰减) → {m['name']}")
    return changes


def _apply_阴骨之地(mechs: List[dict]) -> List[dict]:
    # 虚蚀龙: fill key mechanic buffs.
    changes = []
    for m in mechs:
        if "分摊" in m["name"] and "虚蚀" in m.get("notes", m["name"]):
            if 882230 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(882230)
                changes.append(f"  + buff 882230(虚蚀龙-分摊) → {m['name']}")
        if "致死" in m["name"] and ("坍缩" in m.get("notes", "") or "T" in m["name"]):
            for bid in [882231, 882301]:
                if bid not in m["detect"]["buff_ids"]:
                    m["detect"]["buff_ids"].append(bid)
                    changes.append(f"  + buff {bid} → {m['name']}")
        if "双契" in m["name"] or "点名" in m["name"]:
            for bid in [882305, 882353]:
                label = "虚空双契-点名A" if bid == 882305 else "虚空双契-点名B"
                if bid not in m["detect"]["buff_ids"]:
                    m["detect"]["buff_ids"].append(bid)
                    changes.append(f"  + buff {bid}({label}) → {m['name']}")
    # NEW: 光陨点名
    has_光陨 = any("光陨" in m["name"] for m in mechs)
    if not has_光陨:
        mechs.append(_make_mechanic(
            "光陨点名", buff_ids=[882340],
            source="self", kind="陨石",
            tts="光陨", countdown_s=4.0,
            dodge_enabled=True, dodge_dir="away_nearest",
            notes="光陨 → 被点名远离队友"))
        changes.append("  NEW 光陨点名 (buff:882340)")
    return changes


def _apply_天启的神槛(mechs: List[dict]) -> List[dict]:
    # 光龙: fill key mechanic buffs.
    changes = []
    for m in mechs:
        if "终焉" in m["name"] or "秒杀" in m.get("kind", ""):
            if 882350 not in m["detect"]["buff_ids"]:
                m["detect"]["buff_ids"].append(882350)
                changes.append(f"  + buff 882350(时空的终焉-秒杀光柱) → {m['name']}")
    # NEW: 暗耀水晶连线
    has_连线 = any("水晶连线" in m["name"] or "暗耀" in m["name"] for m in mechs)
    if not has_连线:
        mechs.append(_make_mechanic(
            "暗耀水晶连线", buff_ids=[882317],
            source="any", kind="连线",
            tts="水晶连线", countdown_s=5.0,
            notes="暗耀水晶连线机制"))
        changes.append("  NEW 暗耀水晶连线 (buff:882317)")
    return changes


# ── main ─────────────────────────────────────────────────────────────────────

FILE_HANDLERS = {
    "13013_鸣角之野_机制示例.json": _apply_鸣角之野,
    "13013_石骸之庭_机制示例.json": _apply_石骸之庭,
    "13013_蚀影之渊_机制示例.json": _apply_蚀影之渊,
    "13003_永冻的异想天_机制示例.json": _apply_永冻的异想天,
    "13003_阴骨之地_机制示例.json": _apply_阴骨之地,
    "13003_天启的神槛_机制示例.json": _apply_天启的神槛,
}


def main(argv=None) -> int:
    _utf8()
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dry-run", action="store_true", help="only print changes")
    p.add_argument("--apply", action="store_true", help="write changes to files")
    args = p.parse_args(argv)

    total_changes = 0
    for filename, handler in FILE_HANDLERS.items():
        path = os.path.join(_BR, filename)
        if not os.path.exists(path):
            print(f"[SKIP] {filename} not found")
            continue
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)

        mechs = data.get("profile", {}).get("mechanics", [])
        before = len(mechs)
        changes = handler(mechs)

        if not changes:
            print(f"[OK] {filename}: no gaps found")
            continue

        print(f"\n[FILL] {filename}: {len(changes)} changes ({before} → {len(mechs)} mechanics)")
        for c in changes:
            print(c)
        total_changes += len(changes)

        if args.apply:
            data["profile"]["mechanics"] = mechs
            with open(path, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2)
                f.write("\n")
            print(f"  → WRITTEN to {path}")

    print(f"\nTotal: {total_changes} changes across {len(FILE_HANDLERS)} files")
    if not args.apply and total_changes > 0:
        print("Pass --apply to write changes to files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
