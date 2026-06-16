# -*- coding: utf-8 -*-
"""observe_combat - 战斗中采样实体/区域(zone)/位置, 抓 boss 施法时新增的预警圈.

红圈/AOE 预警是 boss 施法时新增的 ZoneEnt(zoneDict_) 或召唤怪(monsterDict_)。本工具
~4Hz 采样 bossDict/monsterDict/zoneDict/levelEntityDict, 记录"新出现"的实体(按 uuid)
+ 类名 + BaseId + 世界坐标 + 到玩家距离 + ZoneEnt 的 zoneType_/collider, 落 JSONL 供逆向。

用法 (开着游戏在 boss 房, 跑这个然后开打):
  python -m tools.observe_combat --seconds 120
out: exports/combat_observe.jsonl  (每条新实体一行)
"""
from __future__ import annotations

import json
import math
import os
import struct
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_mgr import EntityMgrReader
from plugins.star_resonance_plugin.mem.il2cpp.mem_player_position_reader import PlayerPositionReader

_MINP, _MAXP = 0x10000, 0x7FFF_FFFF_FFFF
# ZEntityMgr dict 偏移 (实测): 见 zoneDict_=0x90 levelEntityDict_=0x98
DICTS = [(0x60, "boss"), (0x68, "monster"), (0x90, "zone"), (0x98, "levelEnt")]


def _kn(pm, o):
    try:
        kp = pm.read_u64(o)
        np = pm.read_u64(kp + 0x10)
        b = pm.read_bytes(np, 64)
        s = b.split(b"\x00", 1)[0]
        return s.decode("ascii", "replace") if s else ""
    except Exception:
        return ""


def _zone_collider_center(pm, lfr, ent):
    """ZoneEnt → compList_ 里的 ZoneComp → collider_ → 试读 Unity collider 世界中心。
    native collider 数据偏移脆弱, 失败返回 None (仅作辅助标注)。"""
    try:
        cl = pm.read_u64(ent + 0x60) or 0   # compList_
        if not (_MINP <= cl <= _MAXP):
            return None, -1
        zonecomp = 0
        for i in range(16):
            cp = pm.read_u64(cl + 0x20 + i * 8) or 0
            if _MINP <= cp <= _MAXP and _kn(pm, cp) == "ZoneComp":
                zonecomp = cp
                break
        if not zonecomp:
            return None, -1
        ztype = pm.read_i32(ent + 0x118)
        # collider_@0x50 → BoxCollider/SphereCollider; bounds 世界中心走 native, 略
        return None, (ztype if ztype is not None else -1)
    except Exception:
        return None, -1


def main(argv=None):
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=120.0)
    ap.add_argument("--hz", type=float, default=4.0)
    args = ap.parse_args(argv)

    src = StaticDpsSource()
    _ = src.sr
    pm = src.sr.pm
    emr = EntityMgrReader(src)
    rd = PlayerPositionReader(src)
    from plugins.star_resonance_plugin.mem.il2cpp.live_field_resolver import LiveFieldResolver
    lfr = LiveFieldResolver(pm, klass_resolver=getattr(src.sr, "resolve_klass", None))

    mgr = emr.locate(0)
    if not mgr:
        print("[observe] ZEntityMgr not found")
        return 1
    player = pm.read_u64(mgr + 0x18)
    out_path = os.path.join(_ROOT, "exports", "combat_observe.jsonl")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    fout = open(out_path, "w", encoding="utf-8")
    print(f"[observe] 采样 {args.seconds:.0f}s @ {args.hz}Hz → {out_path}\n"
          f"[observe] 现在去打 boss! 新出现的实体/圈会实时打印 + 落盘。")

    seen = set()
    period = 1.0 / max(1.0, args.hz)
    t0 = time.time()
    n_new = 0
    while time.time() - t0 < args.seconds:
        try:
            pp = rd.read_entity_pos(player)
            for off, tag in DICTS:
                d = pm.read_u64(mgr + off) or 0
                for key, val in emr._read_dict_entries(d, max_entries=64):
                    if not val or key in seen:
                        continue
                    seen.add(key)
                    cls = _kn(pm, val)
                    baseid = (pm.read_i64(val + 0xE0) or 0) & 0xFFFFFFFF
                    pos = rd.read_entity_pos(val)
                    dist = (math.hypot(pp[0] - pos[0], pp[2] - pos[2])
                            if pp and pos else None)
                    ztype = -1
                    if cls == "ZoneEnt":
                        _, ztype = _zone_collider_center(pm, lfr, val)
                    rec = {
                        "t": round(time.time() - t0, 2), "dict": tag, "uuid": key,
                        "cls": cls, "baseid": baseid,
                        "pos": [round(c, 1) for c in pos] if pos else None,
                        "dist": round(dist, 1) if dist is not None else None,
                        "zoneType": ztype,
                        "player": [round(c, 1) for c in pp] if pp else None,
                    }
                    fout.write(json.dumps(rec, ensure_ascii=False) + "\n")
                    fout.flush()
                    n_new += 1
                    print("  +%5.1fs [%s] %s base=%d pos=%s dist=%s zt=%s"
                          % (rec["t"], tag, cls, baseid, rec["pos"], rec["dist"], ztype))
        except Exception as e:
            print("[observe] tick err:", e)
        time.sleep(period)
    fout.close()
    print(f"\n[observe] 完成: {n_new} 个新实体记录 → {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
