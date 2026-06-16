"""script_parser — 解析 Il2CppDumper 生成的 script.json.

用法:
    from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex
    si = ScriptIndex.load("...path/script.json")
    si.find_klass("Zproto.UserFightAttr")  # -> RVA of *_TypeInfo
    si.find_var("Zproto.UserFightAttr")    # -> RVA of *_var (Il2CppType*)

script.json 顶层结构 (Il2CppDumper v6.7.46):
    ScriptMethod: list of {Address, Name, Signature, TypeSignature}
    ScriptString: list of {Address, Value}
    ScriptMetadata: list of {Address, Name, Signature}    # ← klass / var / 字面量等
    ScriptMetadataMethod: list
    Addresses: list of int

ScriptMetadata.Name 后缀:
    "_TypeInfo"  → Il2CppClass*  位于 RVA Address (在 GA .data 段)
    "_var"       → Il2CppType*   (一般用作 typeof() 桥接)
    其它 (如 "MyClass.field_var") → 各种元数据指针
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Dict, Optional


@dataclass
class ScriptIndex:
    klass_rva: Dict[str, int]   # "Zproto.UserFightAttr" -> RVA
    type_var_rva: Dict[str, int]  # "Zproto.UserFightAttr" -> RVA of Il2CppType*

    @classmethod
    def load(cls, path: str) -> "ScriptIndex":
        with open(path, "r", encoding="utf-8") as f:
            sj = json.load(f)
        klass: Dict[str, int] = {}
        tvar: Dict[str, int] = {}
        for entry in sj.get("ScriptMetadata", ()):
            name = entry.get("Name", "")
            addr = entry.get("Address")
            if not name or addr is None:
                continue
            if name.endswith("_TypeInfo"):
                key = name[: -len("_TypeInfo")]
                klass[key] = addr
            elif name.endswith("_var"):
                key = name[: -len("_var")]
                tvar[key] = addr
        return cls(klass_rva=klass, type_var_rva=tvar)

    def find_klass(self, name: str) -> Optional[int]:
        """name 可以是带或不带命名空间. 优先精确匹配, 其次按短名搜索."""
        if name in self.klass_rva:
            return self.klass_rva[name]
        # 短名匹配 (类似 'UserFightAttr' 命中 'Zproto.UserFightAttr')
        cand = [k for k in self.klass_rva if k.split(".")[-1] == name]
        if len(cand) == 1:
            return self.klass_rva[cand[0]]
        return None

    def find_var(self, name: str) -> Optional[int]:
        if name in self.type_var_rva:
            return self.type_var_rva[name]
        cand = [k for k in self.type_var_rva if k.split(".")[-1] == name]
        if len(cand) == 1:
            return self.type_var_rva[cand[0]]
        return None

