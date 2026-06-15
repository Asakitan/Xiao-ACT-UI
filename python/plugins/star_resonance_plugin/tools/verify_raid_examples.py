# -*- coding: utf-8 -*-
"""verify_raid_examples - 机制示例全量校验 (离线, 静态缓存为权威)。

逐文件检查:
  1. 文件名 = {dungeon_id}_{图名}_机制示例.json 且图名在 raid_scene_names.json 中;
  2. profile.dungeon_id / target_name_pattern 与 raid_scene_names.json 的场次一致;
  3. 每条机制 detect.skill_ids ⊆ 该场全部 boss(全难度并集) 的实测技能集(boss_skills);
  4. 每条机制 detect.buff_ids ⊆ 该场 buff 段(buff_marker_fill.BOSS_BUFF_SEGMENTS);
  5. skill/buff id 在名字缓存里有名(检测得到的都是真 id)。
退出码非 0 = 有违规。
"""
from __future__ import annotations

import glob
import json
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from engines.buff_marker_fill import BOSS_BUFF_SEGMENTS               # noqa: E402

_DIR = os.path.join(_ROOT, "assets", "boss_raids")
_CACHE = os.path.join(_ROOT, "assets", "name_tables", "static_id_name_cache.json")
_SCENES = os.path.join(_DIR, "raid_scene_names.json")


def main():
    log = lambda m: print(m, flush=True)
    cache = json.loads(open(_CACHE, encoding="utf-8").read())
    scenes = json.loads(open(_SCENES, encoding="utf-8").read())
    boss_skills = {int(k): {int(s) for s in v} for k, v in cache.get("boss_skills", {}).items()}
    skill_names = cache.get("skill", {})
    buff_names = cache.get("buff", {})

    # 图名 → (dungeon_id, boss_pattern, 全难度boss技能并集)
    enc_by_map = {}
    for did, info in scenes.items():
        if did.startswith("_"):
            continue
        for enc in info["encounters"]:
            skills = set()
            missing_boss = [b for b in enc["boss_ids"] if b not in boss_skills]
            for b in enc["boss_ids"]:
                skills |= boss_skills.get(b, set())
            enc_by_map[enc["map"]] = (int(did), enc["boss"], skills, missing_boss)

    bad = 0
    files = sorted(glob.glob(os.path.join(_DIR, "*机制示例.json")))
    for fp in files:
        base = os.path.basename(fp)
        m = re.match(r"^(\d+)_(.+)_机制示例\.json$", base)
        errs = []
        if not m:
            errs.append("文件名不符合 {id}_{图名}_机制示例.json")
            mapname, fdid = "", 0
        else:
            fdid, mapname = int(m.group(1)), m.group(2)
        enc = enc_by_map.get(mapname)
        if enc is None:
            errs.append("图名「%s」不在 raid_scene_names.json" % mapname)
        d = json.loads(open(fp, encoding="utf-8").read())
        prof = d.get("profile", d)
        pat = prof.get("target_name_pattern", "")
        segs = BOSS_BUFF_SEGMENTS.get(pat)
        if segs is None:
            errs.append("target_name_pattern「%s」无 buff 段注册" % pat)
        if enc:
            did, boss_pat, skills, missing_boss = enc
            if fdid != did:
                errs.append("文件名 dungeon=%d ≠ 场次 dungeon=%d" % (fdid, did))
            if int(prof.get("dungeon_id", 0)) != did:
                errs.append("profile.dungeon_id=%s ≠ %d" % (prof.get("dungeon_id"), did))
            if pat != boss_pat:
                errs.append("pattern「%s」≠ 场次 boss「%s」" % (pat, boss_pat))
            if missing_boss:
                errs.append("boss %s 不在 boss_skills 缓存(需游戏开重建)" % missing_boss)
        for mech in prof.get("mechanics", []):
            mid = mech.get("id", "?")
            det = mech.get("detect", {})
            for sid in det.get("skill_ids", []):
                if enc and enc[2] and int(sid) not in enc[2]:
                    errs.append("%s skill %s 不属于该场 boss 技能集" % (mid, sid))
                if str(sid) not in skill_names:
                    errs.append("%s skill %s 名字缓存无名" % (mid, sid))
            for bid in det.get("buff_ids", []):
                if segs and not any(lo <= int(bid) <= hi for lo, hi in segs):
                    errs.append("%s buff %s 不在该场 buff 段 %s" % (mid, bid, segs))
                if str(bid) not in buff_names:
                    errs.append("%s buff %s 名字缓存无名" % (mid, bid))
        if errs:
            bad += 1
            log("✗ %s" % base)
            for e in errs:
                log("    - %s" % e)
        else:
            n = len(prof.get("mechanics", []))
            nb = sum(1 for mm in prof.get("mechanics", []) if mm.get("detect", {}).get("buff_ids"))
            log("✓ %s  机制%d条(带buff绑定%d)" % (base, n, nb))
    log("\n%d/%d 文件通过" % (len(files) - bad, len(files)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
