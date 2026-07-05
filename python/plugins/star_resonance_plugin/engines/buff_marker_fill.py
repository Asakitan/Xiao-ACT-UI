# -*- coding: utf-8 -*-
# buff_marker_fill - 机制 buff 智能填入 detect.buff_ids (离线, 静态名字缓存即可).
#
# boss 出招会给玩家/boss 挂机制标记 buff(点名/分摊/衰减...), TCP BuffInfoSync 实时下发,
# 引擎 detect.buff_ids 已支持 OR 匹配 → 填上 buff_ids 检测路径立刻可用。
#
# 三层填法 (按可信度):
# 1. CURATED: 逐 boss 人工对照表 (名字不同义/跨词序的配对, 分析定论) — 强制绑定。
# 2. 自动匹配: boss buff 段内, 名字归一(剥 boss 前缀/点名/标记等) 后
# 精确==1.0 / 包含=0.8 / 2-gram Dice; **逐 buff 对全机制取 argmax**(防 分摊/幻分摊 串绑),
# ≥0.8 才绑; 0.5~0.8 只进报告(待人工确认)不写入。
# 3. 黑名单 token: 天生/出生/中转/虚拟体/计数/弱点研究 等内部簿记 buff 不参与自动绑定。
#
# 合并不覆盖: 已有 buff_ids 保留, 只追加去重(13023终 已手工分析的不动)。
from __future__ import annotations

import re
from typing import Dict, List, Tuple

# ---- boss buff 段 (target_name_pattern → [(lo, hi)]) — 三 raid 勘察定论 ----
# 13023 悖与灾的机骸: 始=8291xx 继=8292xx 终=8293xx
# 13013: 双子(炎光/幻华)=8271xx 石头人(陈骸)=8272xx 蚀花残影=8281xx
# 13003: 冰龙(凛)=8821xx 虚蚀龙(骸)=88220x-88225x 光龙(光)=8823xx
BOSS_BUFF_SEGMENTS: Dict[str, List[Tuple[int, int]]] = {
    "悖与灾的机骸·始": [(829100, 829199)],
    "悖与灾的机骸·继": [(829200, 829299)],
    # 终(失序之骸) 复用继的轨道炮/衰减炸弹资产(实战文本: 中场大激光/黄色大扩散炸弹) → 段含 2xx
    "悖与灾的机骸·终": [(829200, 829349)],
    "煌之冠·阳炎|苍之冠·月影": [(827100, 827199)],   # 鸣角之野 双boss(炎光+幻华)合档
    "幻花的陈骸":      [(827200, 827299)],
    "蚀花的残影":      [(828100, 828160)],
    "凛·伊兹寇利基":   [(882100, 882199)],
    "骸·修洛特尔":     [(882200, 882259)],
    "光·托纳蒂乌":     [(882300, 882360)],
}

# 内部簿记/表现类 buff: 不参与自动绑定 (CURATED 可越过)
_BLACKLIST = ("天生", "出生", "初始", "中转", "转阶段", "无敌", "通信", "锁血",
              "客户端", "前端", "废弃", "测试", "弱点研究", "虚拟体", "计数",
              "中介", "伤痕", "表演", "dummy", "不可选中", "屏幕效果", "溶解")

# 每 boss 额外排除 token (双子档现已两侧齐全, 无需排除)
_FILE_EXCLUDE: Dict[str, Tuple[str, ...]] = {}

# 归一: 剥掉的 boss 前缀/角色词/修饰词 (匹配前双侧都剥)。
# 双子(炎光/幻华)的前缀故意不剥: 同档两侧机制镜像, 剥掉会歧义并列 → 双侧配对走 CURATED。
_STRIP_TOKENS = ("蚀花残影", "殷红断狱", "虚蚀龙", "石头人", "冰龙", "光龙",
                 "玩家", "装置", "点名", "标记", "被", "挂",
                 "buff", "之", "的")
_STRIP_RE = re.compile("|".join(map(re.escape, _STRIP_TOKENS)), re.IGNORECASE)
_PUNCT_RE = re.compile(r"[-·_（）()\s]+")

# ---- 人工对照表: 自动匹配吃不下的配对 (mech_id 全局唯一) ----
# 来源: 三 raid buff 段逐条核对机制语义 (普=红/幻=紫延时; 词序倒置; 别名)
CURATED_BUFF_IDS: Dict[str, List[int]] = {
    # 13023始: 核心震荡球体(困难/噩梦); 电磁脉冲点名A/B/C
    "mech_10310010": [829102, 829103],
    "mech_10310011": [829104, 829105, 829106],
    # 13023继: 延时轨道炮/巨大延时轨道炮; 映射召唤(位置1/2); 普通点名(玩家挂); boss幻象+召唤幻象
    "mech_10320001": [829201, 829210],
    "mech_10320003": [829214, 829215],
    "mech_10320004": [829217],
    "mech_10320009": [829218, 829220],
    # 13013陈骸(石头人): 压团血=被分摊点名; 地震波=直线点名角度0/30/60/90/135+(M);
    # 飞石=落石/延迟落石点名; 最后的墙=被击飞眩晕; 锤地增伤; 吸收水晶=石墙连线吸能
    "mech_10290106": [827246],
    "mech_10290113": [827220, 827221, 827222, 827223, 827224, 827270],
    "mech_10290121": [827219, 827265],
    "mech_10290122": [827251],
    "mech_10290115": [827216],
    "mech_10290118": [827233],
    # 13013残影: 湮灭幻影点名(词序倒置); 蚀花烙印(刻印=烙印); 变身P2
    "mech_10300105": [828105],
    "mech_10300205": [828123],
    "mech_10300108": [828127],
    # 13013鸣角之野(双子): 炎光侧 — 领地共鸣虚拟体/分摊x2/连线系/领地衰减/角斗(共享buff只挂炎光侧防双告警)
    "mech_10280008": [827111],
    "mech_10280011": [827136, 827145],
    "mech_10280014": [827112, 827113, 827130, 827174, 827187],
    "mech_10280015": [827122],
    "mech_10280018": [827153, 827154],
    # 13013鸣角之野(双子): 幻华(月影)侧镜像
    "mech_10280107": [827110],
    "mech_10280111": [827137, 827148],
    "mech_10280114": [827116, 827117, 827131, 827175],
    "mech_10280115": [827123],
    # 13003凛: 冰矛假子弹预警&击飞; 幻影冲锋冰面标记; 冰封之器点名衰减
    "mech_101418": [882121],
    "mech_101414": [882164],
    "mech_101416": [882155],
}

# 人工几何提示: buff 名自带形状语义 (石头人地震波=直线点名→line)
CURATED_GEOMETRY: Dict[str, Dict] = {
    "mech_10290113": {"shape": "line", "source": "reverse"},
}


def normalize_name(s: str) -> str:
    # 剥 boss 前缀/角色词/标点 → 机制语义核心。
    s = _PUNCT_RE.sub("", str(s or ""))
    return _STRIP_RE.sub("", s)


def _bigrams(s: str) -> set:
    return {s[i:i + 2] for i in range(len(s) - 1)} if len(s) >= 2 else ({s} if s else set())


def match_score(mech_name: str, buff_name: str) -> float:
    # 归一后: 精确=1.0; 互相包含=0.8; 否则 2-gram Dice。
    a, b = normalize_name(mech_name), normalize_name(buff_name)
    if not a or not b or len(a) < 2 or len(b) < 2:
        return 0.0
    if a == b:
        return 1.0
    if a in b or b in a:
        return 0.8
    ga, gb = _bigrams(a), _bigrams(b)
    if not ga or not gb:
        return 0.0
    return 2.0 * len(ga & gb) / (len(ga) + len(gb))


def _is_blacklisted(buff_name: str, extra: Tuple[str, ...] = ()) -> bool:
    low = buff_name.lower()
    return any(t in low for t in _BLACKLIST) or any(t in buff_name for t in extra)


def auto_match_buffs(profile: dict, buff_names: Dict[int, str],
                     bind_threshold: float = 0.8,
                     report_threshold: float = 0.5) -> Dict[str, List]:
    # 段内逐 buff 对全机制 argmax 自动匹配。返回 {bound:[(mech_id,buff_id,buff_name,score)],
    # suggest:[同形, 0.5~0.8 待确认不写入]}。不修改 profile。
    pat = (profile or {}).get("target_name_pattern", "")
    segs = BOSS_BUFF_SEGMENTS.get(pat)
    mechs = (profile or {}).get("mechanics", []) or []
    if not segs or not mechs:
        return {"bound": [], "suggest": []}
    extra_excl = _FILE_EXCLUDE.get(pat, ())
    bound, suggest = [], []
    for bid, bname in buff_names.items():
        if not any(lo <= bid <= hi for lo, hi in segs):
            continue
        if _is_blacklisted(bname, extra_excl):
            continue
        scored = sorted(((match_score(m.get("name", ""), bname), m) for m in mechs),
                        key=lambda x: -x[0])
        best_score, best_m = scored[0]
        if len(scored) > 1 and abs(scored[1][0] - best_score) < 1e-9:
            continue                       # 并列最优 → 歧义, 跳过
        rec = (best_m.get("id", ""), bid, bname, round(best_score, 2))
        if best_score >= bind_threshold:
            bound.append(rec)
        elif best_score >= report_threshold:
            suggest.append(rec)
    return {"bound": bound, "suggest": suggest}


def apply_fill(profile: dict, buff_names: Dict[int, str]) -> Dict[str, List]:
    # CURATED + 自动匹配 → 合并写入 detect.buff_ids (追加去重, 不删已有)。
    # 返回 {filled:[(mech_id,added_ids,rule)], suggest:[...], geometry:[mech_id]}。
    res = auto_match_buffs(profile, buff_names)
    add_map: Dict[str, Dict[int, str]] = {}
    for mid, bid, _bn, _sc in res["bound"]:
        add_map.setdefault(mid, {})[bid] = "auto"
    for m in (profile or {}).get("mechanics", []) or []:
        mid = m.get("id", "")
        for bid in CURATED_BUFF_IDS.get(mid, []):
            add_map.setdefault(mid, {})[bid] = "curated"
    bound_pairs = {(mid, bid) for mid, ids in add_map.items() for bid in ids}
    res["suggest"] = [r for r in res["suggest"] if (r[0], r[1]) not in bound_pairs]
    filled, geom_filled = [], []
    for m in (profile or {}).get("mechanics", []) or []:
        mid = m.get("id", "")
        adds = add_map.get(mid)
        if adds:
            det = m.setdefault("detect", {})
            cur = [int(i) for i in (det.get("buff_ids") or [])]
            new = [b for b in sorted(adds) if b not in cur]
            if new:
                det["buff_ids"] = cur + new
                rule = "curated" if any(adds[b] == "curated" for b in new) else "auto"
                filled.append((mid, new, rule))
        geo = CURATED_GEOMETRY.get(mid)
        if geo:
            inline = m.setdefault("dodge", {}).setdefault("inline", {})
            g = inline.get("geometry") or {}
            if not g.get("source"):        # 不覆盖已填(运行时/手填)
                g.update({"shape": geo["shape"], "source": geo.get("source", "reverse")})
                for k in ("radius", "inner", "angle", "width"):
                    g.setdefault(k, 0.0)
                g.setdefault("center", "boss")
                inline["geometry"] = g
                geom_filled.append(mid)
    return {"filled": filled, "suggest": res["suggest"], "geometry": geom_filled}


__all__ = ["BOSS_BUFF_SEGMENTS", "CURATED_BUFF_IDS", "CURATED_GEOMETRY",
           "normalize_name", "match_score", "auto_match_buffs", "apply_fill"]
