# -*- coding: utf-8 -*-
# assemble_raid_examples - 把 per-boss 机制分析(workflow 输出) 组装成示例机制 JSON。
#
# 输入: raid-mechanics-per-boss-analysis workflow 的输出 (9 boss, 每 boss mechanics[])。
# 输出: 每个 boss 一个 assets/boss_raids/<dungeon>_<boss名>_机制示例.json。
# 机制 detect 绑真实 skill_id (boss_base_id=0 跨难度通用); 需范围的留 geometry 占位(壳子);
# 躲避方向/TTS/notes 用分析结果。normalize_profile 保证格式。
from __future__ import annotations

import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.engines.boss_raid_engine import normalize_profile               # noqa: E402
from plugins.star_resonance_plugin.engines.mechanic_intelligence import _MECH_COLOR                # noqa: E402

_OUT_DIR = os.path.join(_ROOT, "assets", "boss_raids")


def _mech_entry(m: dict, boss_id: int) -> dict:
    sid = int(m["skill_id"])
    nm = m.get("skill_name") or str(sid)
    cat = m.get("category") or "unknown"
    direction = m.get("dodge_direction") or ""
    tts = m.get("tts") or nm
    advice = m.get("advice") or ""
    geom = {"shape": (m.get("suggested_shape") or "") if m.get("needs_geometry") else "",
            "radius": 0.0, "inner": 0.0, "angle": 0.0, "width": 0.0,
            "center": "boss", "source": ""}
    inline = {"direction": direction,
              "move_ms": 4000 if direction == "goto_teammate" else 2000,
              "geometry": geom}
    return {
        "id": "mech_%d" % sid, "name": nm, "kind": cat, "enabled": True,
        "color": _MECH_COLOR.get(cat_key(cat), "#68e4ff"),
        "notes": "机制：%s\n躲法：%s" % (nm, advice),
        "phase_ids": [],
        "detect": {"skill_ids": [sid], "buff_ids": [], "source": "self",
                   "boss_base_id": 0, "hp_pct": 0, "time_into_phase_s": 0,
                   "time_into_fight_s": 0, "repeat_interval_s": 0, "event": ""},
        "alert": {"enabled": True, "banner_text": "%s · %s" % (tts, advice[:14]),
                  "tts_text": tts, "alert_type": "both",
                  "countdown_s": int(m.get("countdown_s") or 5), "pre_warn_s": 2,
                  "cooldown_s": 4, "sound": "boss_alert"},
        "dodge": {"enabled": bool(direction), "linkage_id": "", "inline": inline},
    }


def cat_key(cat: str) -> str:
    # 中文 category → _MECH_COLOR 的英文 key (模糊映射)。
    m = {"分摊": "stack", "全场": "raidwide", "环形": "ring", "冲锋": "charge",
         "吐息": "breath", "横扫": "sweep", "砸地": "slam", "十字": "cross",
         "点名": "marker", "落点": "fall", "召唤": "summon", "连线": "link",
         "致死": "lethal", "狂暴": "raidwide", "时停": "timestop", "特殊": "special",
         "击飞": "knockup", "扩散": "spread", "分散": "spread"}
    for k, v in m.items():
        if k in cat:
            return v
    return "unknown"


def build_profile(boss: dict) -> dict:
    mechs = [_mech_entry(m, boss["boss_id"]) for m in boss.get("mechanics", [])]
    geo_n = sum(1 for m in boss.get("mechanics", []) if m.get("needs_geometry"))
    prof = {
        "id": "boss_example_%d_auto" % boss["boss_id"],
        "schema_version": 2,
        "profile_name": "%s · 机制示例 (全难度通用)" % boss["boss_name"],
        "description": ("《%s》%s 机制示例: %d 条机制, 检测绑真实技能id(跨难度通用), 按真实"
                        "技能名分析配 TTS 播报+横幅+定向躲避; %d 条需自动躲避的机制已留几何"
                        "范围占位(壳子), 范围到手(进本读实体/逆向)用 fill_geometry 自动填→按精确"
                        "范围出圈, 未填则招式生命周期兜底。躲避默认关(机制页开总开关, F12急停)。"
                        % (boss["raid"], boss["boss_name"], len(mechs), geo_n)),
        "boss_total_hp": 0, "enrage_time_s": 0,
        "enrage": {"time_s": 0, "anchor": "fight", "phase_id": "",
                   "warn_threshold_s": 60, "urgent_threshold_s": 30,
                   "tts_milestones": [60, 30, 10]},
        "simple_mode": False, "target_name_pattern": boss["boss_name"],
        "dungeon_id": boss["dungeon_id"], "difficulty": "nightmare", "source": "local",
        "phases": [{"id": "p1", "name": "全程", "trigger": {"type": "manual", "value": 0},
                    "timelines": []}],
        "mechanics": mechs,
    }
    return normalize_profile(prof)


def main():
    if len(sys.argv) < 2:
        print("用法: python -m tools.assemble_raid_examples <workflow输出.json>")
        return 2
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    bosses = data.get("result", data) if isinstance(data, dict) else data
    os.makedirs(_OUT_DIR, exist_ok=True)
    for boss in bosses:
        prof = build_profile(boss)
        safe = boss["boss_name"].replace("·", "_").replace("/", "_")
        out = os.path.join(_OUT_DIR, "%d_%s_机制示例.json" % (boss["dungeon_id"], safe))
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"schema_version": 2, "profile": prof}, f,
                      ensure_ascii=False, indent=1)
        print("  → %s (%d 机制)" % (os.path.basename(out), len(prof.get("mechanics") or [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
