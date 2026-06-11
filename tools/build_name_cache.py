# -*- coding: utf-8 -*-
"""build_name_cache - 离线把全配置表 (skill/buff/monster) 的 id→中文名读出来, 落本地静态
缓存 (assets/name_tables/static_id_name_cache.json), 只读内存, 房间里/不打 boss 也能跑。

名字优先级: 本地化 Name(mlid 走 StringPoolBridge) > NameDesign(原始字符串, boss 内部技能用
这个, 如 '炎光角斗-开冲')。另解析指定 boss(MonsterTable.SkillIds/BornSkillId) 的技能名。

用法:
  python -m tools.build_name_cache                 # 全表枚举 + 写缓存
  python -m tools.build_name_cache --boss 102800 102801   # 另输出这些 boss 的技能名
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp.static_dps_source import StaticDpsSource        # noqa: E402
from mem_probe.il2cpp import table_columns                           # noqa: E402
from mem_probe.il2cpp.mem_config_table_reader import (               # noqa: E402
    MemConfigTableReader, TABLE_CLASS)
from mem_probe.il2cpp.mem_string_pool import StringPoolBridge        # noqa: E402

_CACHE = os.path.join(_ROOT, "assets", "name_tables", "static_id_name_cache.json")
# 每表用哪些列做名字 (有就按序取第一个非空)
_NAME_COLS = {
    "skill": ["Name", "NameDesign", "SkillLabel"],
    "buff": ["Name", "NameDesign"],
    "monster": ["Name"],
}


def _resolver(rd: MemConfigTableReader, pool: StringPoolBridge, cols: dict):
    cmap = {k: v[0] for k, v in cols.items()}
    mlid_cols = {k for k, (off, typ) in cols.items() if typ == "mlstring"}

    def name_of(zl: int, blob: int, keys) -> str:
        for k in keys:
            off = cmap.get(k)
            if off is None:
                continue
            if k in mlid_cols or k == "Name":
                s = pool.resolve(rd.col_mlid(blob, off))
            else:
                s = rd.col_string(zl, blob, off)
            if s:
                return s
        return ""
    return name_of, cmap


def enumerate_table(rd, pool, kind: str, log) -> dict:
    cls = TABLE_CLASS[kind]
    cols = table_columns.load_columns([cls], log=lambda *a: None)[cls]
    name_of, cmap = _resolver(rd, pool, cols)
    idc = cmap["Id"]
    out, n, t0 = {}, 0, time.time()
    for rp, zl, blob in rd.iter_rows(cls, limit=200000):
        rid = rd.col_i32(blob, idc)
        if not rid:
            continue
        nm = name_of(zl, blob, _NAME_COLS[kind])
        if nm:
            out[int(rid)] = nm
        n += 1
    log("[name-cache] %-8s rows=%d named=%d (%.1fs)"
        % (kind, n, len(out), time.time() - t0))
    return out


def boss_skills(rd, pool, boss_ids, log) -> dict:
    """MonsterTable[boss].SkillIds + BornSkillId → 各技能 id→名 (走已建的 skill 索引)。"""
    mcls, scls = TABLE_CLASS["monster"], TABLE_CLASS["skill"]
    mcols = {k: v[0] for k, v in table_columns.load_columns([mcls], log=lambda *a: None)[mcls].items()}
    scols = table_columns.load_columns([scls], log=lambda *a: None)[scls]
    sname, scmap = _resolver(rd, pool, scols)
    # skill 索引 id→(zl,blob) 以便按 id 取名
    sidx = {}
    for rp, zl, blob in rd.iter_rows(scls, limit=200000):
        sid = rd.col_i32(blob, scmap["Id"])
        if sid:
            sidx[int(sid)] = (zl, blob)
    res = {}
    for rp, zl, blob in rd.iter_rows(mcls, limit=200000):
        mid = rd.col_i32(blob, mcols["Id"])
        if int(mid or 0) not in set(boss_ids):
            continue
        ids = list(rd.col_i32_array(zl, blob, mcols.get("SkillIds")) or [])
        born = rd.col_i32(blob, mcols.get("BornSkillId"))
        if born:
            ids.append(int(born))
        entry = {}
        for sid in sorted(set(int(i) for i in ids if i)):
            zb = sidx.get(sid)
            entry[sid] = sname(zb[0], zb[1], _NAME_COLS["skill"]) if zb else ""
        res[int(mid)] = entry
        log("[name-cache] boss %d: %d skills" % (mid, len(entry)))
    return res


def enumerate_raids(rd, pool, log) -> dict:
    """RaidDungeonTable → {dungeon_id: {name, difficulty, group_id, bosses[]}}; 只留有名
    有 boss 的真 raid 行 (滤掉空名/活动占位行)。返回 (raids, all_boss_ids)。"""
    cls = TABLE_CLASS["raid_dungeon"]
    C = {k: v[0] for k, v in table_columns.load_columns([cls], log=lambda *a: None)[cls].items()}
    raids, all_boss = {}, set()
    for rp, zl, blob in rd.iter_rows(cls, limit=20000):
        did = rd.col_i32(blob, C.get("DungeonId"))
        if not did:
            continue
        nm = pool.resolve(rd.col_mlid(blob, C.get("Name")))
        bosses = [int(b) for b in (rd.col_i32_array(zl, blob, C.get("BossId")) or []) if b]
        if not nm or not bosses:
            continue                      # 滤掉占位/活动行
        raids[int(did)] = {
            "name": nm, "difficulty": rd.col_i32(blob, C.get("Difficult")),
            "group_id": rd.col_i32(blob, C.get("GroupId")), "bosses": bosses,
        }
        all_boss.update(bosses)
    log("[name-cache] raid 行=%d, 去重 boss=%d" % (len(raids), len(all_boss)))
    return raids, all_boss


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--boss", type=int, nargs="*", default=None,
                    help="额外指定 boss id; 不给则自动发现全部 raid boss")
    ap.add_argument("--out", default=_CACHE)
    args = ap.parse_args()
    log = lambda m: print(m, flush=True)

    src = StaticDpsSource()
    rd = MemConfigTableReader(src)
    pool = StringPoolBridge(src)
    t0 = time.time()
    if not pool.build():
        log("[name-cache] string pool 未定位 — 中止")
        return 2
    log("[name-cache] string pool ready (%.1fs)" % (time.time() - t0))

    cache = {}
    for kind in ("skill", "buff", "monster"):
        cache[kind] = {str(k): v for k, v in enumerate_table(rd, pool, kind, log).items()}

    # 自动发现 3 个 raid 的全部 boss + 各 boss 技能
    raids, all_boss = enumerate_raids(rd, pool, log)
    boss_ids = sorted(set(all_boss) | set(args.boss or []))
    bs = boss_skills(rd, pool, boss_ids, log) if boss_ids else {}
    mnames = cache["monster"]      # boss 名直接取 monster 段
    cache["raids"] = {str(d): {**v, "boss_names": [mnames.get(str(b), "") for b in v["bosses"]]}
                      for d, v in raids.items()}
    cache["boss_skills"] = {str(b): {str(k): v for k, v in d.items()} for b, d in bs.items()}

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(cache, f, ensure_ascii=False, indent=1)
    log("[name-cache] 写入 %s (skill=%d buff=%d monster=%d raid=%d boss=%d)"
        % (args.out, len(cache["skill"]), len(cache["buff"]), len(cache["monster"]),
           len(cache["raids"]), len(cache["boss_skills"])))
    # 按 raid 名分组汇总
    from collections import defaultdict
    by_name = defaultdict(set)
    for d, v in raids.items():
        by_name[v["name"]].update(v["bosses"])
    for rname, bset in by_name.items():
        log("\n== raid 《%s》 boss(%d) ==" % (rname, len(bset)))
        for b in sorted(bset):
            d = bs.get(b, {})
            log("  boss %d %s — %d 技能" % (b, mnames.get(str(b), ""), len(d)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
