# -*- coding: utf-8 -*-
"""fill_buff_markers - 离线把机制 buff 智能填进 boss 机制示例的 detect.buff_ids。

数据源=assets/name_tables/static_id_name_cache.json 的 buff 全表(游戏关着也能跑)。
对 assets/boss_raids 全部示例: 人工对照表(CURATED)+段内自动匹配 → 追加去重写回 + 报告。
0.5~0.8 分的只列「待确认」不写入。
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

from plugins.star_resonance_plugin.engines.buff_marker_fill import apply_fill                     # noqa: E402

_DIR = os.path.join(_ROOT, "assets", "boss_raids")
_CACHE = os.path.join(_ROOT, "assets", "name_tables", "static_id_name_cache.json")


def main():
    log = lambda m: print(m, flush=True)
    with open(_CACHE, encoding="utf-8") as f:
        cache = json.loads(f.read())
    buff_names = {int(k): v for k, v in (cache.get("buff") or {}).items()}
    log("buff 名字缓存 %d 条" % len(buff_names))
    total = 0
    for fp in sorted(glob.glob(os.path.join(_DIR, "*机制示例.json"))):
        with open(fp, encoding="utf-8") as f:
            d = json.loads(f.read())
        prof = d.get("profile", d)
        mech_name = {m.get("id"): m.get("name") for m in prof.get("mechanics", [])}
        res = apply_fill(prof, buff_names)
        base = os.path.basename(fp)
        if res["filled"] or res["geometry"]:
            with open(fp, "w", encoding="utf-8") as f:
                json.dump(d, f, ensure_ascii=False, indent=1)
        log("\n=== %s ===" % base)
        for mid, new, rule in res["filled"]:
            names = ", ".join("%d %s" % (b, buff_names.get(b, "?")) for b in new)
            log("  ★[%s] %s ← %s (%s)" % (rule, mech_name.get(mid, mid), names, mid))
            total += len(new)
        for mid in res["geometry"]:
            log("  ◆几何 %s ← shape=line (直线点名角度系buff)" % mech_name.get(mid, mid))
        for mid, bid, bn, sc in res["suggest"]:
            log("  ?待确认 %s ←? %d %s (%.2f, 未写入)" % (mech_name.get(mid, mid), bid, bn, sc))
        if not (res["filled"] or res["geometry"] or res["suggest"]):
            log("  (无新增)")
    log("\n完成: 共写入 %d 个 buff 绑定。游戏开时可用 BuffTable.SkillId 列做 buff↔skill 精确互验。" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
