# -*- coding: utf-8 -*-
"""gen_raid_mechanics - 从 static_id_name_cache.json 的 boss 技能名, 按关键词分类机制,
为每个 raid 本生成"示例机制 JSON"(assets/boss_raids/<本>_机制示例.json)。

为什么靠名字关键词: 实测 SkillTable 的 IsDangerSkill/IsAoe/SkillRangeType 对 boss 机制技能
几乎全 0 (无区分度), 但技能名(NameDesign)极清晰('炎光环形aoe'/'光龙吐息'/'神之放逐'...)。
同一 boss 角色跨 3 难度技能 id 相同 → detect.skill_ids + boss_base_id=0 一个示例覆盖全难度。

生成的是**可用起点**(检测+TTS+横幅+按类躲法方向已配, 默认关躲避), 用户可再精修 notes/躲法。
"""
from __future__ import annotations

import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.boss_raid_engine import normalize_profile               # noqa: E402

_CACHE = os.path.join(_ROOT, "assets", "name_tables", "static_id_name_cache.json")
_OUT_DIR = os.path.join(_ROOT, "assets", "boss_raids")

# 明确的非机制 (普攻/位移/预备动作) → 跳过, 不生成
_SKIP = ("普攻", "转向", "瞬移", "传送用", "传送", "待机", "跳跃", "吼叫", "向后",
         "原地", "左转", "右转", "走位", "归位", "通用", "初始", "用)", "切换")

# 分类规则 (有序=优先级, 先特定后通用): (关键词, kind, 躲避方向, TTS简名, 卡片说明, 倒计时s, 颜色)
_RULES = [
    (("分摊", "领地分摊", "集合"), "stack", "goto_teammate", "分摊抱团",
     "集合抱团一起分摊, 人越多每人吃的伤害越少。", 5, "#3aa0ff"),
    (("全场", "压团", "全屏", "满屏"), "raidwide", "", "全场AOE",
     "全场范围伤害, 无法靠走位躲——抱团减伤或按读条机制处理。", 6, "#c0392b"),
    (("环形", "环型", "圆环"), "ring", "away_nearest", "环形AOE",
     "环形/圆形AOE, 远离圈中心出圈。", 5, "#ef684e"),
    (("吐息", "喷吐", "龙息", "喷"), "breath", "away_boss", "吐息",
     "锥形吐息, 侧身移出 Boss 正面扇形。", 5, "#e67e22"),
    (("横扫", "半场", "扫"), "sweep", "away_boss", "横扫",
     "横扫半场, 远离扫击侧或绕到 Boss 背后。", 5, "#e67e22"),
    (("开冲", "角斗", "冲锋", "突进", "冲撞", "冲刺"), "charge", "away_boss", "冲锋",
     "直线冲锋, 朝垂直方向闪避离开冲锋路径。", 5, "#f39c12"),
    (("十字",), "cross", "away_nearest", "十字AOE",
     "十字AOE, 站到十字之间的安全缝隙。", 6, "#9b59b6"),
    (("扩散", "分散"), "spread", "away_nearest", "分散",
     "分散点名, 与队友拉开站位。", 6, "#8e44ad"),
    (("点名", "光柱", "标记", "累刑", "宣告", "审判", "刻度"), "marker", "away_nearest", "点名",
     "点名机制, 远离人群并按指引处理。", 6, "#8e44ad"),
    (("放逐",), "banish", "away_boss", "放逐",
     "放逐地波, 远离 Boss 并注意走位。", 8, "#16a085"),
    (("召唤", "连线", "连结", "连结试炼", "召唤小怪"), "summon", "", "召唤连线",
     "召唤/连线机制, 优先处理目标或站位连线。", 8, "#27ae60"),
    (("致死", "死亡计时", "终焉", "坍缩", "残响", "终末", "终焉交响"), "lethal", "", "致死机制",
     "致死机制, 严格按读条/指引执行, 失误即团灭。", 8, "#c0392b"),
    (("时停", "时空", "停滞", "相位"), "timestop", "", "时停",
     "时停/相位, 注意时停结束后的延时点名先结算。", 8, "#2c3e50"),
    (("守护", "踩塔", "果实", "净化", "收缩"), "special", "away_nearest", "",
     "特殊机制, 留意地面预警圈处理。", 6, "#34495e"),
    (("陨", "坠", "落", "轨道炮", "炮"), "fall", "away_nearest", "落点AOE",
     "落地/炮击AOE, 远离红色落点。", 5, "#d35400"),
    (("aoe", "波", "爆", "光", "焰", "炎", "雷", "冰", "毒"), "aoe", "away_nearest", "AOE",
     "范围AOE伤害, 远离/出圈。", 5, "#ef684e"),
]


def _classify(name: str):
    low = name
    if any(s in low for s in _SKIP):
        return None
    for kws, kind, direction, tts, note, cd, color in _RULES:
        if any(k.lower() in low.lower() for k in kws):
            return {"kind": kind, "direction": direction, "tts": tts or name,
                    "note": note, "cd": cd, "color": color}
    return None


def _mechanic(sid: int, name: str, cls: dict) -> dict:
    direction = cls["direction"]
    inline = {"action_key": "", "action_label": "", "press_mode": "tap",
              "hold_ms": 600, "press_count": 1, "sequence": [], "delay_ms": 0,
              "lead_ms": 300, "cooldown_s": 3.0, "direction": direction,
              "move_ms": 4000 if direction == "goto_teammate" else 1500,
              "exit_margin_m": 6.0, "arrive_m": 2.0, "fallback_direction": "back"}
    return {
        "id": "mech_%d" % sid, "name": name, "kind": cls["kind"], "enabled": True,
        "color": cls["color"], "notes": "机制：%s\n躲法：%s" % (name, cls["note"]),
        "phase_ids": [],
        "detect": {"skill_ids": [sid], "buff_ids": [], "source": "self",
                   "boss_base_id": 0, "hp_pct": 0, "time_into_phase_s": 0,
                   "time_into_fight_s": 0, "repeat_interval_s": 0, "event": ""},
        "alert": {"enabled": True, "banner_text": "%s · %s" % (cls["tts"], cls["note"][:12]),
                  "tts_text": cls["tts"], "alert_type": "both", "countdown_s": cls["cd"],
                  "pre_warn_s": 2, "cooldown_s": 4, "sound": "boss_alert"},
        "dodge": {"enabled": bool(direction), "linkage_id": "", "inline": inline},
    }


def build_for_raid(cache: dict, raid_name: str, dungeon_id: int,
                   boss_id_by_role: dict, log) -> dict:
    mechs, skipped = [], []
    for role, bid in boss_id_by_role.items():
        skills = cache["boss_skills"].get(str(bid), {})
        for sid_s, nm in sorted(skills.items(), key=lambda kv: int(kv[0])):
            sid = int(sid_s)
            cls = _classify(nm or "")
            if cls is None:
                skipped.append((sid, nm))
                continue
            mechs.append(_mechanic(sid, nm, cls))
    profile = {
        "schema_version": 2,
        "profile": {
            "id": "boss_example_%d_auto" % dungeon_id,
            "schema_version": 2,
            "profile_name": "%s · 自动机制示例 (全难度通用)" % raid_name,
            "description": ("《%s》自动生成的机制示例: 含 %d 个 boss 的 %d 条机制, 检测全绑"
                            "真实技能 id(跨3难度通用), 按技能名分类配好 TTS 播报+横幅+定向躲避方向。"
                            "躲避默认关(需在机制页开「定向移动」/「自动走位」总开关, 自动WASD有账号"
                            "风险自担, F12急停)。这是可用起点, 躲法/说明可再精修。"
                            % (raid_name, len(boss_id_by_role), len(mechs))),
            "boss_total_hp": 0, "enrage_time_s": 600,
            "enrage": {"time_s": 600, "anchor": "fight", "phase_id": "",
                       "warn_threshold_s": 60, "urgent_threshold_s": 30,
                       "tts_milestones": [60, 30, 10]},
            "simple_mode": False, "target_name_pattern": "",
            "dungeon_id": dungeon_id, "difficulty": "nightmare", "source": "local",
            "phases": [{"id": "p1", "name": "全程", "trigger": {"type": "manual", "value": 0},
                        "timelines": []}],
            "mechanics": mechs,
        },
    }
    log("  《%s》dungeon=%d: %d 机制, 跳过 %d 普攻/位移" % (raid_name, dungeon_id, len(mechs), len(skipped)))
    return normalize_profile(profile["profile"]), profile


def main():
    log = lambda m: print(m, flush=True)
    cache = json.load(open(_CACHE, encoding="utf-8"))
    raids = cache.get("raids") or {}
    # 按 raid 名聚合所有难度的 dungeon; 每名取最高难度 dungeon_id + 角色去重(同名=同角色)
    from collections import defaultdict
    by_name = defaultdict(lambda: {"dungeons": [], "roles": {}})
    for did_s, v in raids.items():
        nm = v.get("name") or ""
        if not nm:
            continue
        g = by_name[nm]
        g["dungeons"].append((int(v.get("difficulty") or 0), int(did_s)))
        for bid, bname in zip(v.get("bosses") or [], v.get("boss_names") or []):
            # 角色名去重: 同名只留一个 boss id (技能跨难度相同)
            g["roles"].setdefault(bname or str(bid), int(bid))

    os.makedirs(_OUT_DIR, exist_ok=True)
    log("生成 %d 个 raid 的示例机制:" % len(by_name))
    for rname, g in by_name.items():
        dungeon_id = max(g["dungeons"])[1]    # 最高难度
        prof, raw = build_for_raid(cache, rname, dungeon_id, g["roles"], log)
        out = os.path.join(_OUT_DIR, "%d_%s_自动机制示例.json" % (dungeon_id, rname))
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"schema_version": 2, "profile": prof}, f, ensure_ascii=False, indent=1)
        log("    → %s (%d 机制)" % (os.path.basename(out), len(prof.get("mechanics") or [])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
