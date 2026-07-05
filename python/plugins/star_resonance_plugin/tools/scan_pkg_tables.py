# -*- coding: utf-8 -*-
# scan_pkg_tables - 扫游戏 .pkg(UnityFS bundle) 定位配置表/几何资源 (离线, 游戏可关).
#
# .pkg = 24B自定义header + 标准 UnityFS bundle(无加密)。UnityPy 解每个 pkg 的对象类型分布 +
# TextAsset/MonoBehaviour 名字, 找含 table/config/skill/bullet/shape/zone 的 = 配置表所在 pkg。
# 多进程并行。输出 exports/pkg_table_scan.json (pkg → 类型分布 + 命中的配置表资源名)。
from __future__ import annotations

import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed

_CONTAINER = r"E:\星痕共鸣(2001991)\Star_Data\StreamingAssets\container"
_OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "exports", "pkg_table_scan.json")
_KW = ("table", "config", "skill", "bullet", "shape", "zone", "field", "effect",
       "Table", "Config", "Skill", "Bullet", "Shape", "Zone", "Field", "Effect")


def scan_one(path: str) -> dict:
    import UnityPy
    res = {"pkg": os.path.basename(path), "types": {}, "hits": [], "err": ""}
    try:
        data = open(path, "rb").read()
        uf = data.find(b"UnityFS")
        if uf < 0:
            res["err"] = "no UnityFS"
            return res
        env = UnityPy.load(data[uf:])
        for obj in env.objects:
            t = obj.type.name
            res["types"][t] = res["types"].get(t, 0) + 1
        # 命中配置表名 (TextAsset/MonoBehaviour/AssetBundle container)
        try:
            for path_name, _obj in (env.container or {}).items():
                if any(k in path_name for k in _KW):
                    res["hits"].append(path_name)
        except Exception:
            pass
        if not res["hits"]:
            for obj in env.objects:
                if obj.type.name in ("TextAsset", "MonoBehaviour"):
                    try:
                        d = obj.read()
                        nm = getattr(d, "m_Name", "") or ""
                        if any(k in nm for k in _KW):
                            res["hits"].append("%s:%s" % (obj.type.name, nm))
                    except Exception:
                        pass
                    if len(res["hits"]) >= 30:
                        break
    except Exception as e:
        res["err"] = str(e)[:80]
    return res


def main():
    pkgs = sorted(os.path.join(_CONTAINER, f) for f in os.listdir(_CONTAINER)
                  if f.endswith(".pkg"))
    print("[scan] %d 个 pkg, 多进程扫描..." % len(pkgs), flush=True)
    os.makedirs(os.path.dirname(_OUT), exist_ok=True)
    out, done, found = {}, 0, []
    with ProcessPoolExecutor(max_workers=min(8, (os.cpu_count() or 4))) as ex:
        futs = {ex.submit(scan_one, p): p for p in pkgs}
        for fut in as_completed(futs):
            r = fut.result()
            out[r["pkg"]] = r
            done += 1
            if r["hits"]:
                found.append(r["pkg"])
                print("[scan] ★%s 命中配置表: %s" % (r["pkg"], r["hits"][:5]), flush=True)
            if done % 30 == 0:
                print("[scan] %d/%d, 命中 pkg: %d" % (done, len(pkgs), len(found)), flush=True)
    json.dump(out, open(_OUT, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("[scan] 完成. 写 %s. 含配置表的 pkg(%d): %s" % (_OUT, len(found), found), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
