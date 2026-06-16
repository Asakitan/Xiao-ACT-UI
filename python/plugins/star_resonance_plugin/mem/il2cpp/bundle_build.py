"""bundle_build - 把 script.json + dump_cs_index 子集 + GA 校验和打包.

dump_cs_index.json (15.5MB) 和 script.json (248MB) 太大, 不适合分发.
本工具按"需要的类列表"抽取最小子集, 输出单一 JSON bundle.

bundle 结构:
  {
    "meta": {
        "dump_id": "ef9ef95a",
        "ga_module": "GameAssembly.dll",
        "ga_size": <int>,
        "ga_sha256_first_1mb": "<hex>",   // 用于运行时校验 dump 是否还匹配
        "built_at": <unix>,
        "il2cpp_version": "31",
    },
    "klass_rva": { "Zproto.CharSerialize": <rva>, ... },
    "type_rva":  { "Zproto.UserFightAttr": <rva>, ... },     // typeof 桥
    "classes": {
        "Zproto.CharSerialize": {
            "namespace": "Zproto",
            "type_def_index": 10518,
            "fields": [{name, type, offset, is_static}, ...]
        },
        ...
    }
  }
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from typing import Iterable, List, Set

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.il2cpp.dump_cs_parser import DumpCsIndex


# 默认需要的类: 玩家自身 + 战斗属性容器. 后续按需扩.
DEFAULT_CLASSES: List[str] = [
    "Zproto.CharSerialize",
    "Zproto.UserFightAttr",
    "Zproto.UserFightAttrContainerArchive",
    "Zproto.CharSerializeContainerArchive",
    "Zproto.CharBaseInfo",
    "Zproto.RoleLevel",
    "Zproto.SkillCDInfo",
    "Zproto.EnergyItem",
    "Zproto.SeasonMedalInfo",
    "Zproto.MedalHole",
    "Zproto.ProfessionList",
    "Zproto.SceneData",
    "Zproto.SceneLuaData",
    # buff / skill / damage wire types (TCP parser + boss-skill aggregation)
    "Zproto.BuffDBInfo",
    "Zproto.BuffDBData",
    "Zproto.BuffInfo",
    "Zproto.BuffInfoSync",
    "Zproto.SyncDamageInfo",
    "Zproto.SyncHitInfo",
    "Zproto.ClientHitInfo",
    "Zproto.ClientHitPartInfo",
    "Zproto.UseSkill",
    "Zproto.UseSkillParam",
    # Panda.ZGame runtime classes the live mem readers resolve field offsets from
    # by name (auto_offsets). Curating them lets DumpCsIndex.field_offset self-heal
    # these readers on a game patch instead of falling back to hardcoded literals:
    #   ZEntityMgr/ZEntity/ZAttrCollection -> mem_entity_mgr / _provider / _combat / _attr
    #   DamageDataMgr/DamageData           -> mem_damage_reader
    #   ZStateMachine/BuffComp/BuffItem    -> mem_boss_action_reader
    "Panda.ZGame.ZEntityMgr",
    "Panda.ZGame.ZClientEntityMgr",          # mem_entity_mgr client registry
    "Panda.ZGame.ServerStateEntityMgr",       # server entity registry singleton
    "Panda.ZGame.ZEntity",
    "Panda.ZGame.PlayerEnt",                 # PlayerEnt field offsets
    "Panda.ZGame.BattleResComp",             # boss combat resource component (HP probe)
    "Panda.ZGame.ZAttrCollection",
    "Panda.ZGame.ZAttrCacheSlim",            # cacheSlim sublist inline layout
    "Panda.ZGame.DamageDataMgr",
    "Panda.ZGame.DamageData",
    "Panda.ZGame.ZStateMachine",
    "Panda.ZGame.BuffComp",
    "Panda.ZGame.BuffItem",
    "Panda.ZGame.PlayerHpWatcher",           # sCheckAttrIndex_ static (HP attr keys)
    "Panda.ZGame.CameraManager",             # mem_camera_reader singleton
    "Panda.ZGame.SceneConfigMgr",            # scene/map singleton (mem_map_name_reader anchor)
    # HUD GM render (debug overlay anchor used by some probes)
    "Panda.Hud.HudGmRender",
    "Panda.Hud.HudGm",
    # Localization + table registry that the name/boss/map readers resolve offsets
    # from by name (used by mem_string_pool / mem_config_table_reader / mem_map_name_reader /
    # mem_field_geometry_reader / mem_boss_skill_state_reader).
    "Panda.Module.StringPoolRuntimeImpl",
    "Bokura.MonsterTableBase",
    "Bokura.SceneTableBase",
    "Bokura.RaidDungeonTableBase",
    "Bokura.SkillTableBase",
    "Bokura.BuffTableBase",
    "Bokura.FieldTableBase",
    "Bokura.NpcTableBase",
    "Bokura.ItemTableBase",
    "Bokura.DungeonTableBase",
    "Bokura.Table.ReadProxy",                # generic row reader used by table_columns
    "Table.Utility.TableProxyManager",        # table registry singleton
]


_GENERIC_REF_RE = __import__('re').compile(r'<\s*([A-Za-z_][\w`]*)(?:\s*,\s*([A-Za-z_][\w`]*))?\s*>')


def _extract_generic_refs(type_str: str) -> List[str]:
    """Pull all reference type names out of generic signatures.

    Examples:
        RepeatedField<SkillCDInfo>          -> ['SkillCDInfo']
        MapField<uint, EnergyInfo>          -> ['EnergyInfo']
        List<RepeatedField<Zproto.SkillCD>> -> ['Zproto.SkillCD']
        EnergyItem                            -> ['EnergyItem']
    """
    if not type_str:
        return []
    base = type_str.split("<", 1)[0].strip("[] ")
    out = [base] if base and base[0].isupper() else []
    for m in _GENERIC_REF_RE.finditer(type_str):
        for grp in m.groups():
            if grp and grp[0].isupper():
                out.append(grp)
    return out


def expand_referenced_classes(dci: DumpCsIndex, seeds: Iterable[str],
                              max_depth: int = 1) -> Set[str]:
    """从种子类的字段类型递归展开 (引用类型才进 dci) - 一阶即可."""
    result: Set[str] = set()
    queue = list(seeds)
    depth = {s: 0 for s in seeds}
    while queue:
        name = queue.pop(0)
        if name in result:
            continue
        c = dci.find_class(name)
        if c is None:
            continue
        result.add(name)
        if depth.get(name, 0) >= max_depth:
            continue
        for f in c["fields"]:
            for t in _extract_generic_refs(f["type"]):
                ref = dci.find_class(t)
                if ref is None:
                    continue
                full = f"{ref['namespace']}.{ref['name']}" if ref["namespace"] else ref["name"]
                if full not in result and full not in depth:
                    queue.append(full)
                    depth[full] = depth.get(name, 0) + 1
    return result


def build_bundle(script_json: str, dump_cs_json: str, ga_path: str,
                 dump_id: str, classes: Iterable[str],
                 expand_depth: int = 0) -> dict:
    print(f"[bundle] loading script.json...", file=sys.stderr)
    si = ScriptIndex.load(script_json)
    print(f"[bundle] loading dump_cs_index...", file=sys.stderr)
    dci = DumpCsIndex.load_from_json(dump_cs_json)

    needed = set(classes)
    if expand_depth > 0:
        needed = expand_referenced_classes(dci, needed, max_depth=expand_depth)
    print(f"[bundle] {len(needed)} classes after expansion", file=sys.stderr)

    klass_rva = {}
    type_rva = {}
    classes_out = {}
    for name in sorted(needed):
        c = dci.find_class(name)
        if c is None:
            print(f"  [warn] {name} not in dump_cs_index", file=sys.stderr)
            continue
        full = f"{c['namespace']}.{c['name']}" if c["namespace"] else c["name"]
        classes_out[full] = {
            "namespace": c["namespace"],
            "type_def_index": c["type_def_index"],
            "fields": c["fields"],
        }
        krva = si.find_klass(full) or si.find_klass(c["name"])
        if krva is not None:
            klass_rva[full] = krva
        trva = si.find_var(full) or si.find_var(c["name"])
        if trva is not None:
            type_rva[full] = trva

    # GA validation hash
    ga_size = 0
    ga_hash = ""
    if os.path.isfile(ga_path):
        ga_size = os.path.getsize(ga_path)
        h = hashlib.sha256()
        with open(ga_path, "rb") as f:
            h.update(f.read(1 * 1024 * 1024))
        ga_hash = h.hexdigest()

    return {
        "meta": {
            "dump_id": dump_id,
            "ga_module": "GameAssembly.dll",
            "ga_size": ga_size,
            "ga_sha256_first_1mb": ga_hash,
            "built_at": int(time.time()),
            "schema_version": 1,
        },
        "klass_rva": klass_rva,
        "type_rva": type_rva,
        "classes": classes_out,
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dump-id", default="ef9ef95a")
    p.add_argument("--script-json")
    p.add_argument("--dump-cs-json")
    p.add_argument("--ga-path", help="GameAssembly.dll path for hash. Optional.")
    p.add_argument("--classes", nargs="*", default=DEFAULT_CLASSES)
    p.add_argument("--expand-depth", type=int, default=0)
    p.add_argument("--out", required=True)
    args = p.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))
    sj = args.script_json or os.path.join(here, "out", args.dump_id, "dumper_out", "script.json")
    dj = args.dump_cs_json or os.path.join(here, "out", args.dump_id, "dump_cs_index.json")
    ga = args.ga_path or os.path.join(here, "out", args.dump_id, "dumper_out", "..", "GameAssembly.dll")

    bundle = build_bundle(sj, dj, ga, args.dump_id, args.classes, args.expand_depth)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(bundle, f, ensure_ascii=False, indent=2)
    sz = os.path.getsize(args.out)
    print(f"[OK] {args.out} ({sz:,} bytes)")
    print(f"  klass_rva: {len(bundle['klass_rva'])}")
    print(f"  type_rva : {len(bundle['type_rva'])}")
    print(f"  classes  : {len(bundle['classes'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

