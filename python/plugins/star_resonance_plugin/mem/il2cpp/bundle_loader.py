"""bundle_loader - 从轻量 bundle (build by bundle_build.py) 构造 StaticResolver.

bundle 加载比 script.json (248MB) + dump_cs_index (15MB) 快几个数量级,
适合打包到主程序里发布.
"""
from __future__ import annotations

import json
import os
import sys
from typing import Optional

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ..process import StarProcess
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
from plugins.star_resonance_plugin.mem.il2cpp.dump_cs_parser import DumpCsIndex
from plugins.star_resonance_plugin.mem.il2cpp.static_resolver import StaticResolver


def _bundle_to_script_index(bundle: dict) -> ScriptIndex:
    return ScriptIndex(
        klass_rva=dict(bundle.get("klass_rva", {})),
        type_var_rva=dict(bundle.get("type_rva", {})),
    )


def _bundle_to_dci(bundle: dict) -> DumpCsIndex:
    classes = {}
    for full, c in bundle.get("classes", {}).items():
        ns = c.get("namespace", "")
        name = full.rsplit(".", 1)[-1] if ns else full
        classes[full] = {
            "namespace": ns,
            "name": name,
            "kind": "class",
            "type_def_index": c.get("type_def_index", -1),
            "bases": "",
            "fields": c.get("fields", []),
        }
    return DumpCsIndex(classes)


def open_resolver_from_bundle(bundle_path: str,
                              process_name: Optional[str] = None) -> StaticResolver:
    with open(bundle_path, "r", encoding="utf-8") as f:
        bundle = json.load(f)
    si = _bundle_to_script_index(bundle)
    dci = _bundle_to_dci(bundle)
    pm = StarProcess(process_name)
    ga_name = bundle["meta"].get("ga_module", "GameAssembly.dll").lower()
    ga = next(m for m in pm.list_modules() if m.name.lower() == ga_name)
    # 注: bundle.meta.ga_size 是磁盘文件大小, 与 PE 加载后的 SizeOfImage 不一致.
    # 真正的版本校验靠 sha256_first_1mb (在 bundle_store 里做), 这里只信任 hash.
    return StaticResolver(pm, ga.base, si, dci)
