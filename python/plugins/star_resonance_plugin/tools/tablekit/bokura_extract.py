# bokura_extract - 诊断: 用 bokura_decode 的锚点对齐, 报告 m0.pkg 主名字池能对齐
# 出多少【高置信】id->中文名, 并与 crib 交叉验证。
#
# 为什么默认【不写入】assets/name_tables/:
# 穷尽逆向后实证 (见 bokura_decode 顶注 + README_bokura.md):
# m0.pkg 离线无法重建【完整且准确】的 id->name 表 ——
# * 池只装了部分名字 (技能名仅 20% 在池里);
# * 池里无 id 列、无名字偏移指针, 名字仅按 id 升序紧密拼接;
# * 因此对齐只能产出 crib 的一个【子集】(交叉验证 100% 一致, 但 0 新增),
# 对 NameResolver 没有覆盖提升, 且跳号/缺名处等长巧合会引入错位风险。
# => 写出去是冗余 + 有污染风险, 故本工具默认仅诊断。
# 传 --write 可强制写入子集 (一般不需要)。
#
# 用法:
# python -m tools.tablekit.bokura_extract            # 仅诊断, 不写文件
# python -m tools.tablekit.bokura_extract --write     # 强制写子集(不推荐)
from __future__ import annotations

import json
import os

from tools.tablekit.bokura_decode import (
    PKG_DEFAULT, open_pkg, build_gap_map, main_pool_gap, tokenize_gap, align_table,
)

_HERE = os.path.dirname(os.path.abspath(__file__))
_SAO = os.path.dirname(os.path.dirname(_HERE))
_REPO = os.path.dirname(_SAO)
_ASSETS = os.path.join(_SAO, "assets")
_OUT = os.path.join(_ASSETS, "name_tables")
_DATATOOLS_CN = os.path.join(_REPO, "StarResonanceDps", "DataTools", "Data", "CN")


def _load_crib_skill():
    with open(os.path.join(_ASSETS, "skill_names.json"), encoding="utf-8") as f:
        return {int(k): v for k, v in json.load(f).items() if v}


def _load_crib_monster():
    with open(os.path.join(_DATATOOLS_CN, "MonsterTable.json"), encoding="utf-8") as f:
        d = json.load(f)
    out = {}
    for k, v in d.items():
        nm = v.get("Name") if isinstance(v, dict) else None
        if nm:
            out[int(k)] = nm
    return out


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true",
                    help="把对齐子集写到 assets/name_tables/ (默认仅诊断, 不写)")
    args = ap.parse_args()

    f, mm = open_pkg(PKG_DEFAULT)
    try:
        gaps = build_gap_map(mm)
        gs, ge = main_pool_gap(gaps)
        toks = tokenize_gap(mm, gs, ge)
    finally:
        mm.close()
        f.close()
    print("main pool gap 0x%X..0x%X, %d tokens (%d unique)" % (
        gs, ge, len(toks), len(set(t for _, t in toks))))

    if args.write:
        os.makedirs(_OUT, exist_ok=True)

    for kind, crib in (("skill", _load_crib_skill()), ("monster", _load_crib_monster())):
        result, stats = align_table(toks, crib)
        print("[%s] crib=%d -> aligned subset=%d (crosscheck %d/%d=%.2f%%)" % (
            kind, len(crib), stats["result"],
            stats["crosscheck_agree"], stats["crosscheck_total"],
            stats["crosscheck_rate"]))
        print("       stats:", stats)
        if args.write:
            path = os.path.join(_OUT, "%s.json" % kind)
            ordered = {str(i): result[i] for i in sorted(result)}
            with open(path, "w", encoding="utf-8") as fp:
                json.dump(ordered, fp, ensure_ascii=False, indent=0)
            print("       (wrote subset -> %s)" % os.path.relpath(path, _SAO))

    print("[dungeon] NOT extractable offline: no crib to anchor against, and dungeon "
          "names are interleaved with narrative/quest text in the pool. "
          "Requires reading the runtime ZTable (IL2CPP) from game memory.")
    if not args.write:
        print("\n(诊断模式: 未写任何文件。对齐结果只是 crib 子集, 0 新增, 不写以免冗余/污染。)")


if __name__ == "__main__":
    main()
