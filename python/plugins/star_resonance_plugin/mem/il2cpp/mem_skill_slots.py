"""mem_skill_slots - 把内存读出的 SkillCD 列表转成 GameState.skill_slots 格式.

复用 packet_bridge._build_packet_skill_slots 的成熟逻辑 (CD 计算 / 槽位推断 /
普攻/职业/大招锚点等), 通过构造一个 stub PlayerData 输入.

对外接口:
    convert(skill_cds, profession_id, server_time_offset_ms=None) -> list[dict]

skill_cds: List[SkillCD] (来自 SelfSnapshot.skill_cds)
profession_id: int (来自 SelfSnapshot.profession_id)
"""
from __future__ import annotations

import os
import sys
import time
from typing import Iterable, List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

# 延迟导入 (packet_bridge 依赖较重)
_build_skill_slots = None
_PlayerData = None
_PROFESSION_NAMES = None


def _ensure_imports():
    global _build_skill_slots, _PlayerData, _PROFESSION_NAMES
    if _build_skill_slots is not None:
        return
    try:
        from plugins.star_resonance_plugin.net.packet_bridge import _build_packet_skill_slots as _b
    except Exception:
        from packet_bridge import _build_packet_skill_slots as _b
    from packet_parser import PlayerData as _P, PROFESSION_NAMES as _N
    _build_skill_slots = _b
    _PlayerData = _P
    _PROFESSION_NAMES = _N


def _make_stub_player(skill_cds: Iterable, profession_id: int,
                     server_time_offset_ms: Optional[float],
                     observed_at_ms: int) -> object:
    """构造一个最小可用的 PlayerData, 只填 _build_packet_skill_slots 需要的字段."""
    _ensure_imports()
    p = _PlayerData(uid=0)
    p.profession_id = int(profession_id or 0)
    p.profession = _PROFESSION_NAMES.get(p.profession_id, '')
    p.server_time_offset_ms = server_time_offset_ms
    p.skill_slot_map = {}
    p.slot_bar_map = {}
    p.skill_cd_map = {}
    p.skill_last_use_at = {}
    p.skill_seen_ids = []
    p._inferred_skill_count = 0
    # CD 修正全部置 0 (内存中目前没读 buff/attr 修正)
    p.attr_skill_cd = 0
    p.attr_skill_cd_pct = 0
    p.attr_cd_accelerate_pct = 0
    p.temp_attr_cd_pct = 0
    p.temp_attr_cd_fixed = 0
    p.temp_attr_cd_accel = 0

    for cd in skill_cds or ():
        slid = int(getattr(cd, 'skill_id', 0) or 0)
        if slid <= 0:
            continue
        cd_dict = {
            'duration': int(getattr(cd, 'duration_ms', 0) or 0),
            'begin_time': int(getattr(cd, 'begin_ms', 0) or 0),
            'valid_cd_time': int(getattr(cd, 'valid_cd_ms', 0) or 0),
            'charge_count': int(getattr(cd, 'charge_count', 0) or 0),
            'skill_cd_type': int(getattr(cd, 'cd_type', 0) or 0),
            'observed_at_ms': observed_at_ms,
            'last_vcd_update_ms': observed_at_ms,
            'max_charges': 1,
            'sub_cd_ratio': 0,
            'sub_cd_fixed': 0,
            'accelerate_cd_ratio': 0,
            'vcd_speed_ratio': 0.0,
        }
        p.skill_cd_map[slid] = cd_dict
        if slid not in p.skill_seen_ids:
            p.skill_seen_ids.append(slid)
    return p


def convert(skill_cds, profession_id: int = 0,
            server_time_offset_ms: Optional[float] = None) -> List[dict]:
    """SkillCD list → HUD skill_slots list. 失败返回 []."""
    if not skill_cds:
        return []
    try:
        _ensure_imports()
    except Exception as e:
        print(f"[mem_skill_slots] import failed: {e}", file=sys.stderr)
        return []
    observed_at_ms = int(time.time() * 1000)
    try:
        p = _make_stub_player(skill_cds, profession_id,
                              server_time_offset_ms, observed_at_ms)
        return _build_skill_slots(p) or []
    except Exception as e:
        import traceback
        print(f"[mem_skill_slots] convert failed: {e}", file=sys.stderr)
        traceback.print_exc()
        return []


def _selftest():
    from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
    src = StaticDpsSource()
    snap = src.get_extended_snapshot()
    if not snap:
        print("FAIL: no snap"); return
    print(f"profession={snap.profession_id} cds={len(snap.skill_cds or [])}")
    slots = convert(snap.skill_cds or [], snap.profession_id)
    print(f"slots ({len(slots)}):")
    for s in slots[:12]:
        print(f"  idx={s.get('index'):>2} sid={s.get('skill_id'):>5} "
              f"st={s.get('state'):>9} cd%={s.get('cooldown_pct'):.2f} "
              f"rem={s.get('remaining_ms'):>5}ms ch={s.get('charge_count')} "
              f"name={s.get('name','')}")


if __name__ == "__main__":
    _selftest()

