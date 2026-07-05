# -*- coding: utf-8 -*-
# Offline test for table_columns thunk extraction against real dump artifacts.
#
# Uses out/fdc7111b (dump.cs + GameAssembly.dll) and asserts the extracted
# column offsets/types match the independently verified layout. Skips (exit 0)
# when the artifacts are not present on this machine.
#
# Run: python -m mem_probe.il2cpp.test_table_columns
import os

from plugins.star_resonance_plugin.mem.il2cpp.table_columns import (
    parse_classes_getters, extract_columns, READPROXY_CLS, FALLBACK,
)

_HERE = os.path.dirname(os.path.abspath(__file__))
_DUMP_DIR = os.path.join(_HERE, "out", "fdc7111b")

CLASSES = ["Bokura.RaidDungeonTableBase", "Bokura.MonsterTableBase",
           "Bokura.SkillTableBase", "Bokura.BuffTableBase"]

EXPECT = {
    "Bokura.RaidDungeonTableBase": {
        "DungeonId": (0x0, "i32"),
        "Difficult": (0x4, "i32"),
        "GroupId": (0x8, "i32"),
        "Name": (0xC, "mlstring"),
        "BossId": (0x18, "i32array"),
    },
    "Bokura.MonsterTableBase": {
        "Id": (0x0, "i32"),
        "Name": (0x4, "mlstring"),
        "SkillIds": (0x1C, "i32array"),
    },
    "Bokura.SkillTableBase": {
        "Id": (0x0, "i32"),
        "Name": (0xC, "mlstring"),
    },
    "Bokura.BuffTableBase": {
        "Id": (0x0, "i32"),
        "Name": (0x10, "mlstring"),
    },
}


def main():
    dump_cs = os.path.join(_DUMP_DIR, "dump.cs")
    ga = os.path.join(_DUMP_DIR, "GameAssembly.dll")
    if not (os.path.isfile(dump_cs) and os.path.isfile(ga)):
        print(f"SKIP: artifacts missing under {_DUMP_DIR}")
        return 0

    failures = 0

    def check(label, got, want):
        nonlocal failures
        ok = got == want
        print(f"{'OK ' if ok else 'FAIL'}  {label}: got={got!r} want={want!r}")
        failures += (0 if ok else 1)

    getters, reader_map = parse_classes_getters(dump_cs, CLASSES)
    check("reader_map has core readers",
          all(n in reader_map.values()
              for n in ("ReadInt32", "ReadMLString", "ReadInt32Array")), True)
    for cls in CLASSES:
        cols = extract_columns(ga, getters.get(cls, {}), reader_map)
        check(f"{cls}: getters parsed", bool(getters.get(cls)), True)
        for prop, want in EXPECT[cls].items():
            check(f"{cls}.{prop}", cols.get(prop), want)
        # curated literals must agree with extraction (guards FALLBACK drift)
        for prop, want in FALLBACK.get(cls, {}).items():
            check(f"{cls}.{prop} == FALLBACK", cols.get(prop), want)

    # BornSkillId is needed by the raid enumeration; assert it extracts as i32
    cols_m = extract_columns(ga, getters["Bokura.MonsterTableBase"], reader_map)
    born = cols_m.get("BornSkillId")
    check("MonsterTableBase.BornSkillId type", born and born[1], "i32")

    print("RESULT:", "ALL GREEN" if failures == 0 else f"{failures} FAILED")
    return failures


if __name__ == "__main__":
    raise SystemExit(1 if main() else 0)
