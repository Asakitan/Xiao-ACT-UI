# -*- coding: utf-8 -*-
"""auto_fill_geometry - 游戏开时自动把读到的几何智能填进 boss 机制示例。

跑一次: 连游戏 → 读 FieldTable 领域几何(FieldGeometryReader) → 对 assets/boss_raids 所有示例
按名字相似度自动填 (auto_fill_from_fields, 高分且同boss才填, 不覆盖已填) → 写回 + 报告。
游戏关则提示。运行时瞬时招几何(skill_id 时间关联)由战斗采集器在打 boss 时自动填(更准)。
"""
from __future__ import annotations

import glob
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.geometry_auto_fill import auto_fill_from_fields        # noqa: E402

_DIR = os.path.join(_ROOT, "assets", "boss_raids")


def main():
    log = lambda m: print(m, flush=True)
    try:
        from mem_probe.il2cpp.static_dps_source import StaticDpsSource
        from mem_probe.il2cpp.mem_field_geometry_reader import FieldGeometryReader
        src = StaticDpsSource()
    except Exception as e:
        log("游戏未开/连不上: %s" % str(e)[:80])
        log("→ 游戏开着(哪怕房间)再跑, 才能读 FieldTable 领域几何自动填。")
        return 1
    fields = FieldGeometryReader(src).read_all()
    log("读到 %d 条 FieldTable 领域几何 (Size=半径米):" % len(fields))
    for fg in fields[:30]:
        log("  %-22s %s r=%.1f%s" % (fg["name"][:22], fg["shape"], fg["radius"],
                                     " inner=%.1f" % fg["inner"] if fg.get("inner") else ""))
    if not fields:
        log("→ 当前场景无领域几何加载(进对应本才有更多)。")
        return 0
    total = 0
    for f in sorted(glob.glob(os.path.join(_DIR, "*机制示例.json"))):
        d = json.load(open(f, encoding="utf-8"))
        prof = d.get("profile", d)
        filled = auto_fill_from_fields(prof, fields, min_score=0.5)
        if filled:
            json.dump(d, open(f, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
            log("\n★%s 自动填 %d 条:" % (os.path.basename(f), len(filled)))
            for x in filled:
                log("   %s ← 领域'%s' (相似度%.2f)" % (x["mech"], x["field"], x["score"]))
            total += len(filled)
    log("\n完成. 共自动填 %d 条机制几何。瞬时招几何待进本时战斗采集器按 skill_id 精确填。" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
