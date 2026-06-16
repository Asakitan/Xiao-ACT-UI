# -*- coding: utf-8 -*-
"""resolve_raid_scene_names - 读每场 raid 战斗的地图名 (游戏开着即可, 不用进图)。

实证结论(2026-06): 战斗子图名不在 SceneTable/SceneAreaTable, DungeonsTable.SceneID→
SceneTable.SubScene 链路为空(已弃用)。权威源=StringPoolRuntimeImpl 全池字符串:
  每周任务串「【每周】<本名>·<图名>」 — 与传送串「前往<图名>」双源一致,
  「鸣角之野」(双子场, 用户实证) 为校验锚。

用法: python tools/resolve_raid_scene_names.py [--apply]
  打印三个 raid 的「本名·图名」串与传送串; --apply 不覆盖人工整理的
  assets/boss_raids/raid_scene_names.json, 只写 exports/raid_scene_strings.json 供比对。
"""
from __future__ import annotations

import json
import os
import struct
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

_RAIDS = ("神龙枷所", "幻花沉眠之地", "遗忘幻梦之野")
_OUT = os.path.join(_ROOT, "exports", "raid_scene_strings.json")
_ANCHOR = "鸣角之野"


def main():
    log = lambda m: print(m, flush=True)
    apply_ = "--apply" in sys.argv
    try:
        from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
        from plugins.star_resonance_plugin.mem.il2cpp.mem_string_pool import StringPoolBridge
        src = StaticDpsSource()
    except Exception as e:
        log("游戏未开/连不上: %s" % str(e)[:100])
        return 1
    pool = StringPoolBridge(src)
    if not pool.build():
        log("string pool 未定位 — 中止")
        return 2
    pm = src.sr.pm
    arr, n = pool._pool_arr, pool._pool_len
    elems = pm.read_bytes(arr + 0x20, n * 8)
    ptrs = struct.unpack("<%dQ" % n, elems)

    def read_s(p):
        if not p:
            return ""
        try:
            ln = pm.read_i32(p + 0x10)
            if not (0 < ln < 256):
                return ""
            b = pm.read_bytes(p + 0x14, ln * 2)
            return b.decode("utf-16-le", "replace") if b else ""
        except Exception:
            return ""

    weekly, goto = [], []
    for p in ptrs:
        s = read_s(p)
        if not s:
            continue
        if s.startswith("【每周】") and any(r in s for r in _RAIDS) and "·" in s:
            weekly.append(s)
        elif s.startswith(("前往", "挑战")) and len(s) <= 12:
            goto.append(s)
    log("每周任务串 %d 条:" % len(weekly))
    for s in sorted(set(weekly)):
        log("  %s" % s)
    log("传送/挑战短串 %d 条 (人工比对用):" % len(set(goto)))
    for s in sorted(set(goto)):
        log("  %s" % s)
    ok = any(_ANCHOR in s for s in weekly)
    log("\n%s 校验锚「%s」%s" % ("✓" if ok else "⚠", _ANCHOR, "命中" if ok else "未命中 — 别直接采信!"))
    if apply_:
        os.makedirs(os.path.dirname(_OUT), exist_ok=True)
        with open(_OUT, "w", encoding="utf-8") as f:
            json.dump({"weekly": sorted(set(weekly)), "goto": sorted(set(goto))},
                      f, ensure_ascii=False, indent=1)
        log("已写 %s (人工比对 raid_scene_names.json 用)" % _OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
