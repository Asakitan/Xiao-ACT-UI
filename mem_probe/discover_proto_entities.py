"""CLI: scan all Zproto.Entity instances + decode their attributes.

Uses dump.cs RVAs (no value-anchor guesswork). Walks
Entity → AttrCollection → RepeatedField<Attr> → Attr → ByteString → varint.

Persists klass anchor to anchors.json on success so that the entity_watcher
can use proto-mode reading.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import Counter

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO_AUTO = os.path.dirname(_HERE)
if _SAO_AUTO not in sys.path:
    sys.path.insert(0, _SAO_AUTO)

from mem_probe.proto_entity_locator import (
    ENTITY_KLASS_RVA, ATTRCOLLECTION_KLASS_RVA, ATTR_KLASS_RVA,
    ATTR_HP, ATTR_MAX_HP, ATTR_EXTINCTION, ATTR_MAX_EXTINCTION,
    ATTR_BREAKING_STAGE, ENT_TYPE_CHAR, ENT_TYPE_MONSTER,
    discover_all_entities, klass_ptr_for, _ga_base,
)
from mem_probe.locator import ANCHORS_PATH


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Discover all Zproto.Entity instances via klass scan + AttrCollection walk")
    ap.add_argument("--anchors", default=ANCHORS_PATH)
    ap.add_argument("--ent-type", choices=("any", "monster", "char"), default="any")
    ap.add_argument("--limit", type=int, default=20, help="Max entities to print")
    args = ap.parse_args(argv)

    print("[proto-entity] attaching ...")
    from mem_probe.locator import SmartLocator
    sl = SmartLocator()
    pm = sl.pm
    print(f"[proto-entity] attached pid={pm.pid}")

    ga_base = _ga_base(pm)
    print(f"[proto-entity] GA base = 0x{ga_base:X}")
    print(f"[proto-entity] Entity klass = 0x{ga_base + ENTITY_KLASS_RVA:X}")

    type_filter = None
    if args.ent_type == "monster":
        type_filter = ENT_TYPE_MONSTER
    elif args.ent_type == "char":
        type_filter = ENT_TYPE_CHAR

    snapshots = discover_all_entities(
        pm,
        ent_type_filter=type_filter,
        decode_attrs=True,
        attr_filter={ATTR_HP, ATTR_MAX_HP, ATTR_EXTINCTION,
                     ATTR_MAX_EXTINCTION, ATTR_BREAKING_STAGE},
    )

    if not snapshots:
        print("[proto-entity] no entities found — Entity klass RVA may be wrong "
              "or no entities loaded")
        return 2

    # Type histogram
    types = Counter(s.ent_type for s in snapshots)
    print(f"\n[proto-entity] entity types:")
    for t, n in types.most_common():
        type_name = {0: "Char", 1: "Monster"}.get(t, f"type{t}")
        print(f"   {type_name}({t}): {n}")

    print(f"\n[proto-entity] sample snapshots (top {args.limit}):")
    for snap in snapshots[:args.limit]:
        type_name = {0: "Char", 1: "Monster"}.get(snap.ent_type, f"type{snap.ent_type}")
        hp = snap.attrs.get(ATTR_HP)
        max_hp = snap.attrs.get(ATTR_MAX_HP)
        ext = snap.attrs.get(ATTR_EXTINCTION)
        max_ext = snap.attrs.get(ATTR_MAX_EXTINCTION)
        brk = snap.attrs.get(ATTR_BREAKING_STAGE)
        print(f"   {type_name:<8} addr=0x{snap.addr:X} uuid={snap.uuid:>10}  "
              f"hp={hp}/{max_hp}  ext={ext}/{max_ext}  brk={brk}  "
              f"n_attrs={len(snap.raw_attrs)}")

    # Persist Entity anchor to anchors.json (proto-mode)
    try:
        with open(args.anchors, "r", encoding="utf-8") as f:
            anchors = json.load(f)
    except (OSError, json.JSONDecodeError):
        anchors = {}
    sl_block = anchors.setdefault("smart_locator", {})
    nested = sl_block.setdefault("anchors", {})
    nested["entity_collection"] = {
        "mode": "proto",
        "ga_base": f"0x{ga_base:X}",
        "entity_klass_rva": ENTITY_KLASS_RVA,
        "entity_klass_ptr": f"0x{ga_base + ENTITY_KLASS_RVA:X}",
        "attrcoll_klass_rva": ATTRCOLLECTION_KLASS_RVA,
        "attr_klass_rva": ATTR_KLASS_RVA,
        "monster_count": types.get(ENT_TYPE_MONSTER, 0),
        "char_count": types.get(ENT_TYPE_CHAR, 0),
        "discovered_via": "proto_il2cpp_dump",
        "discovered_at": time.time(),
        # IL2CPP layout offsets (constant — written for documentation/sanity)
        "layout": {
            "entity_uuid_off":     0x10,
            "entity_enttype_off":  0x18,
            "entity_attrs_off":    0x20,
            "attrcoll_attrs_off":  0x18,
            "repfield_array_off":  0x10,
            "repfield_count_off":  0x18,
            "array_data_off":      0x20,
            "attr_id_off":         0x10,
            "attr_rawdata_off":    0x18,
            "bytestr_obj_off":     0x18,
            "bytestr_start_off":   0x20,
            "bytestr_length_off":  0x24,
        },
        "attr_ids": {
            "HP":             ATTR_HP,
            "MAX_HP":         ATTR_MAX_HP,
            "EXTINCTION":     ATTR_EXTINCTION,
            "MAX_EXTINCTION": ATTR_MAX_EXTINCTION,
            "BREAKING_STAGE": ATTR_BREAKING_STAGE,
        },
    }
    sl_block["schema_version"] = max(2, int(sl_block.get("schema_version", 0) or 0))
    sl_block["entity_collection_set_at"] = time.time()
    with open(args.anchors, "w", encoding="utf-8") as f:
        json.dump(anchors, f, ensure_ascii=False, indent=2)
    print(f"\n[proto-entity] persisted entity_collection anchor → {args.anchors}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
