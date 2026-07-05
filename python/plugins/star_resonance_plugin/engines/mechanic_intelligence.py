# -*- coding: utf-8 -*-
# mechanic_intelligence - boss 机制智能分析器: 招名 → 应对策略 (本地, 无需几何).
#
# ★为什么靠招名: 实测 boss 机制几何(半径/形状)不在可静态读的配置表(在AI脚本/prefab里),
# 但**机制类型从官方招名 100% 能判断**(炎光环形aoe=圆圈远离, 领地分摊=集合抱团, 角斗-开冲=
# 冲锋侧闪)。配合 curSkillId_ 实时读 boss 在出什么招 → 自动分类应对, 任意 boss 任意招不用手配。
#
# 输出策略不依赖几何半径: 躲避走招式生命周期判停(这招结束即停), 集合走靠拢队友(队友位可读)。
# 分类是**可解释的分层规则**, 不是机械关键词: 先排非机制(普攻/位移), 再按机制语义分类, 带置信度。
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Tuple


@dataclass
class MechAdvice:
    category: str          # 机制大类
    action: str            # 躲避动作: away_nearest/away_boss/goto_teammate/'' (none=只提示)
    tts: str               # 语音简名
    advice: str            # 一句话躲法
    confidence: str        # high/medium/low
    is_mechanic: bool      # False = 普攻/位移, 不该提示
    needs_geometry: bool = False   # 是否需要"实际范围"才能精准躲 (away_*类圆/锥/线圈)
    suggested_shape: str = ""      # 建议形状(壳子占位): circle/ring/cone/line/cross


# category → 默认建议形状 (壳子里 geometry.shape 的占位; 实际形状以填入数据为准)
_CATEGORY_SHAPE = {
    "ring": "ring", "wave": "circle", "fall": "circle", "marker": "circle",
    "charge": "line", "breath": "cone", "sweep": "cone", "slam": "circle",
    "line": "line", "cross": "cross", "knockup": "circle", "spread": "circle",
}


# 明确的非机制 (普攻/位移/预备/演出) → 不提示
_NON_MECH = (
    "普攻", "普通攻击", "二连", "三连", "连击", "右爪", "左爪", "拳击", "爪击",
    "转向", "原地转向", "左转", "右转", "瞬移", "传送用", "传送", "走路", "走位",
    "位移", "追击位移", "跳跃", "待机", "初始待机", "吼叫", "通用吼叫", "切天气吼叫",
    "向前", "向后", "起手", "演绎", "入场", "趴着", "表演", "结束演绎", "巨大化蓄力",
    "挣扎", "结束", "芒辉之种",
)

# 分层分类规则 (有序=优先级, 先特定后通用):
#   (关键词元组, category, action, tts, advice, confidence)
_RULES: List[Tuple[Tuple[str, ...], str, str, str, str, str]] = [
    # —— 集合抱团 (分摊类) ——
    (("领地分摊", "分摊", "集合", "压团血", "压团", "共罪", "共罪之裁"),
     "stack", "goto_teammate", "集合分摊",
     "立刻向队友集合抱团一起分摊, 人越多每人吃越少。", "high"),
    # —— 分散站位 ——
    (("分散", "扩散", "大扩散", "小分散", "三角分散"),
     "spread", "away_nearest", "分散站位",
     "与队友拉开距离分散站位, 别重叠。", "high"),
    # —— 冲锋/方向性 (远离 Boss 正面) ——
    (("角斗", "开冲", "冲锋", "冲撞", "突进", "冲刺", "撞墙", "砸碎墙"),
     "charge", "away_boss", "冲锋",
     "Boss 直线冲锋, 朝垂直方向闪开冲锋路径。", "high"),
    (("吐息", "龙息", "喷吐", "喷"),
     "breath", "away_boss", "吐息",
     "锥形吐息, 侧身移出 Boss 正面扇形。", "high"),
    (("横扫", "扫尾", "顺劈", "撕裂", "回旋刀", "扫"),
     "sweep", "away_boss", "横扫",
     "横扫/劈砍, 远离 Boss 或绕到背后。", "high"),
    (("砸地", "拍地", "碎地", "重拳", "下砸", "跳砸", "蓄力下砸", "疯狂锤地", "锤地"),
     "slam", "away_boss", "砸地",
     "Boss 砸地冲击, 远离 Boss 周围。", "high"),
    # —— 直线/激光/连线 ——
    (("十字",), "cross", "away_nearest", "十字",
     "十字范围, 站到十字之间的缝隙。", "high"),
    (("激光", "光线", "光束", "毁灭光线", "轨道炮", "贯穿", "电磁脉冲", "电磁连线",
      "旋转子弹", "光束炮"),
     "line", "away_nearest", "直线激光",
     "直线/激光, 横向移出线的路径。", "high"),
    (("连线", "之连线", "连结", "电磁连线"),
     "link", "away_nearest", "连线",
     "玩家间连线, 按机制拉开或靠拢断线。", "medium"),
    # —— 落点/陨石/点名光柱 ——
    (("陨石", "陨星", "光陨", "飞石", "陨", "落"),
     "fall", "away_nearest", "陨石落点",
     "天降落点 AOE, 远离红色落点。", "high"),
    (("点名光柱", "光柱", "点名", "标记"),
     "marker", "away_nearest", "点名",
     "点名/光柱, 远离人群带到边上。", "high"),
    # —— 圆/环形/范围 AOE ——
    (("环形", "环型", "崩石环", "环形冲击", "大炎戒", "炎戒", "炎弧", "炎光环"),
     "ring", "away_nearest", "环形AOE",
     "环形/圆形 AOE, 远离圈中心出圈。", "high"),
    (("裂石风暴", "地震波", "震荡", "核心震荡", "冰霜冲击", "冲击", "波"),
     "wave", "away_nearest", "范围冲击",
     "范围冲击波, 远离爆心。", "medium"),
    # —— 点名死刑类 ——
    (("死刑", "死缓", "致死", "累刑", "宣告", "审判", "死亡计时"),
     "lethal", "", "致死机制",
     "致死/点名机制, 严格按指引处理, 失误团灭。", "high"),
    # —— 全屏不可躲 / 转阶段 ——
    (("全场", "满屏", "全团", "终焉", "终焉交响", "史诗狂怒", "狂怒", "无尽寒冬",
      "零度雪域", "时空坍缩", "时空的终焉", "毁灭", "终末"),
     "raidwide", "", "全屏大招",
     "全屏/不可躲大招, 抱团减伤或按读条处理。", "high"),
    # —— 召唤 ——
    (("召唤", "连线召唤", "召唤小怪", "召唤幻象", "召唤dummy", "召唤机械", "真实承诺",
      "幻影冲锋"),
     "summon", "", "召唤",
     "召唤增援/幻象, 优先集火召唤物。", "medium"),
    # —— 时停/相位 ——
    (("时停", "时空", "停滞", "相位", "相位映射"),
     "timestop", "", "时停",
     "时停/相位, 注意时停结束后延时点名先生效。", "medium"),
    # —— 守护/踩塔/吃球/领域 等特殊 ——
    (("誓死守护", "守护", "踩塔", "果实", "吸收水晶", "水晶", "净化之光", "净化",
      "领地共鸣", "虚空双契", "连结试炼", "试炼", "盾", "领域", "心神剥夺", "刻印",
      "蚀花刻印", "蚀心之种", "幻影湮灭", "终焉化身"),
     "special", "", "特殊机制",
     "特殊机制(守护/吃球/领域等), 按预警处理。", "low"),
    # —— 击飞 ——
    (("击飞", "重击", "跳跃重击", "砸碎最后的墙"),
     "knockup", "away_boss", "击飞",
     "击飞/眩晕, 远离并注意落点。", "medium"),
]


class MechanicClassifier:
    # 招名 → 应对策略。纯逻辑可测, 无内存/几何依赖。

    def classify(self, skill_name: str) -> MechAdvice:
        name = (skill_name or "").strip()
        if not name:
            return MechAdvice("unknown", "", name, "", "low", False)
        low = name.lower()
        # 先排非机制 (但"砸碎墙"等击飞要保留 → 非机制表不含它们)
        if any(k in name for k in _NON_MECH):
            return MechAdvice("non_mechanic", "", name, "", "high", False)
        for kws, cat, action, tts, advice, conf in _RULES:
            if any(k.lower() in low for k in kws):
                shape = _CATEGORY_SHAPE.get(cat, "")
                # 需要范围才能精准躲: away_* 类且有几何形状 (集合/全屏/召唤等不需要)
                needs_geo = bool(action.startswith("away") and shape)
                return MechAdvice(cat, action, tts, advice, conf, True,
                                  needs_geometry=needs_geo, suggested_shape=shape)
        # 没匹配: 未知机制, 只提示名字, 低置信
        return MechAdvice("unknown", "", name, "未知机制, 留意 Boss 动作。", "low", True)


_MECH_COLOR = {
    "stack": "#3aa0ff", "spread": "#8e44ad", "charge": "#f39c12", "breath": "#e67e22",
    "sweep": "#e67e22", "slam": "#d35400", "cross": "#9b59b6", "line": "#e74c3c",
    "link": "#16a085", "fall": "#d35400", "marker": "#8e44ad", "ring": "#ef684e",
    "wave": "#ef684e", "lethal": "#c0392b", "raidwide": "#c0392b", "summon": "#27ae60",
    "timestop": "#2c3e50", "special": "#34495e", "knockup": "#d35400", "unknown": "#7f8c8d",
}
_CD = {"raidwide": 8, "lethal": 8, "summon": 8, "timestop": 8, "stack": 5,
       "charge": 5, "breath": 5, "sweep": 5, "ring": 5, "cross": 6}


def build_mechanic_shell(skill_id: int, skill_name: str,
                         boss_base_id: int = 0) -> Optional[dict]:
    # 智能分析 → 机制壳子 (boss_raid schema 的原始 dict; 调用方再 normalize_mechanic)。
    # 需范围(needs_geometry)的自动躲避机制, dodge.inline.geometry 留**占位**(shape=建议形状,
    # radius=0, source='') —— 等几何到手用 fill_geometry 填入。非机制返回 None。
    a = MechanicClassifier().classify(skill_name)
    if not a.is_mechanic:
        return None
    geom = {"shape": a.suggested_shape if a.needs_geometry else "", "radius": 0.0,
            "inner": 0.0, "angle": 0.0, "width": 0.0, "center": "boss", "source": ""}
    return {
        "id": "mech_%d" % int(skill_id), "name": skill_name, "kind": a.category,
        "enabled": True, "color": _MECH_COLOR.get(a.category, "#68e4ff"),
        "notes": "机制：%s\n躲法：%s" % (skill_name, a.advice),
        "phase_ids": [],
        "detect": {"skill_ids": [int(skill_id)], "buff_ids": [], "source": "self",
                   "boss_base_id": int(boss_base_id), "hp_pct": 0, "time_into_phase_s": 0,
                   "time_into_fight_s": 0, "repeat_interval_s": 0, "event": ""},
        "alert": {"enabled": True, "banner_text": "%s · %s" % (a.tts, a.advice[:14]),
                  "tts_text": a.tts, "alert_type": "both",
                  "countdown_s": _CD.get(a.category, 5), "pre_warn_s": 2,
                  "cooldown_s": 4, "sound": "boss_alert"},
        "dodge": {"enabled": bool(a.action), "linkage_id": "",
                  "inline": {"direction": a.action, "move_ms": 4000 if a.action == "goto_teammate" else 2000,
                             "geometry": geom}},
        "_needs_geometry": a.needs_geometry,    # 壳子标记: 待填范围(消费端可据此提示)
        "_confidence": a.confidence,
    }


def fill_geometry(mechanic: dict, *, shape: str, radius: float, inner: float = 0.0,
                  angle: float = 0.0, width: float = 0.0, center: str = "boss",
                  source: str = "reverse") -> bool:
    # 把"实际范围"填进机制壳子的 geometry (几何到手后调用; 填后自动躲避按精确范围出圈)。
    # 成功返回 True。机制无 dodge.inline 时自动建。
    try:
        dodge = mechanic.setdefault("dodge", {})
        inline = dodge.setdefault("inline", {})
        inline["geometry"] = {"shape": str(shape), "radius": float(radius),
                              "inner": float(inner), "angle": float(angle),
                              "width": float(width), "center": str(center),
                              "source": str(source) or "reverse"}
        mechanic.pop("_needs_geometry", None)
        return True
    except Exception:
        return False


__all__ = ["MechanicClassifier", "MechAdvice", "build_mechanic_shell", "fill_geometry"]
