"""把 Il2CppDumper 的 dump.cs + script.json 转为我们运行时用的 metadata.json.

dump.cs 格式 (relevant 行):
    public class Foo : Bar // TypeDefIndex: 1234
    {
        // Fields
        public int CurHp; // 0x10
        private static int _instance; // 0x0
        public const int Bar = 5;       // 无 offset, 跳过
        ...
    }

script.json sections:
    ScriptMetadata: [{Address, Name, Signature}]
        Name 形如 "Foo_TypeInfo"  → Il2CppClass*  (Address 是 GameAssembly.dll 内 RVA)
        Name 形如 "Foo_var"       → Il2CppType*

输出 metadata.json schema:
{
  "version": 1,
  "metadata_version": 31,
  "game_assembly_sha8": "...",
  "type_info_map": {                  # 全量保留 (轻量, 字典)
      "<ClassFullName>": <RVA int>,
      ...
  },
  "classes": {                        # 仅按 --class-regex 过滤
      "<ClassFullName>": {
          "type_def_index": int,
          "base": str | null,
          "fields": [{"name": str, "offset": int, "type": str, "static": bool}, ...]
      },
      ...
  }
}

CLI:
    python -m tools.mem_probe.il2cpp.metadata_builder --dumper-out <dir>
    python -m tools.mem_probe.il2cpp.metadata_builder --dumper-out <dir> --class-regex "Player|Hero|Avatar"
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from typing import Dict, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))


# ─────────── dump.cs streaming parser ───────────

# class header:  public class Foo // TypeDefIndex: 123
#                public class Foo : Bar, IBaz // TypeDefIndex: 123
#                internal class Foo<T> : ... // TypeDefIndex: 123
_RE_CLASS_HEADER = re.compile(
    r"^\s*(?:public|private|internal|protected|sealed|abstract|static)\s*"
    r"(?:public|private|internal|protected|sealed|abstract|static\s+)*"
    r"(?:class|struct|interface)\s+"
    r"([\w\.<>`,\s]+?)"               # class name (group 1) — 含泛型
    r"(?:\s*:\s*([\w\.<>`,\s]+?))?"   # base : ... (group 2, optional)
    r"\s*//\s*TypeDefIndex:\s*(\d+)\s*$"  # group 3 = type def index
)

# field line (含 offset):
#   public int CurHp; // 0x10
#   private readonly List<int> Foo; // 0x40
#   public static MyType Instance; // 0x0
_RE_FIELD = re.compile(
    r"^\s*"
    r"(?:public|private|internal|protected)\s+"
    r"((?:[\w\s])*?)"                # modifiers (group 1) — readonly/static/const
    r"\s*"
    r"([\w\.<>`,\[\]\s\?]+?)"         # type (group 2)
    r"\s+(\w+)\s*;\s*"                # name (group 3)
    r"//\s*0x([0-9a-fA-F]+)\s*$"      # offset (group 4)
)

# image line: // Image N: Foo.dll - 1234
# We don't currently use namespace from the image system; class name in dump.cs
# already has full dotted name when it's a nested namespace.

_SECTION_FIELDS = "// Fields"
_SECTION_PROPERTIES = "// Properties"
_SECTION_METHODS = "// Methods"
_NAMESPACE_PREFIX = "// Namespace:"
_IMAGE_PREFIX = "// Image "


def parse_dump_cs(path: str, class_regex: Optional[re.Pattern]) -> Dict[str, dict]:
    """流式解析 dump.cs. 仅保留匹配 class_regex 的类的字段."""
    classes: Dict[str, dict] = {}
    cur: Optional[dict] = None
    in_fields = False
    line_count = 0
    skipped_no_field = 0
    cur_namespace = ""  # 最近一次见到的 // Namespace: X

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line_count += 1
            stripped = line.rstrip("\n")

            # 跟踪 namespace (每个 class 前一行)
            s_lstrip = stripped.lstrip()
            if s_lstrip.startswith(_NAMESPACE_PREFIX):
                cur_namespace = s_lstrip[len(_NAMESPACE_PREFIX):].strip()
                continue

            # 检测 class 头
            m = _RE_CLASS_HEADER.match(stripped)
            if m:
                # 收尾上一个 class
                if cur is not None and not cur["fields"]:
                    skipped_no_field += 1
                cur = None
                in_fields = False
                short_name = m.group(1).strip()
                full_name = f"{cur_namespace}.{short_name}" if cur_namespace else short_name
                base = (m.group(2) or "").strip() or None
                tdi = int(m.group(3))
                if class_regex is not None and not class_regex.search(full_name):
                    continue
                cur = {
                    "type_def_index": tdi,
                    "base": base,
                    "namespace": cur_namespace,
                    "short_name": short_name,
                    "fields": [],
                }
                classes[full_name] = cur
                continue

            if cur is None:
                continue

            # section markers
            if _SECTION_FIELDS in stripped:
                in_fields = True
                continue
            if _SECTION_PROPERTIES in stripped or _SECTION_METHODS in stripped:
                in_fields = False
                continue

            if not in_fields:
                continue

            mf = _RE_FIELD.match(stripped)
            if not mf:
                continue
            mods_raw = mf.group(1) or ""
            ftype = mf.group(2).strip()
            fname = mf.group(3).strip()
            foff = int(mf.group(4), 16)
            is_static = "static" in mods_raw
            cur["fields"].append({
                "name": fname,
                "offset": foff,
                "type": ftype,
                "static": is_static,
            })

    print(f"[dump.cs] 行数={line_count:,}  收集类={len(classes)}  (空字段类略去)")
    return classes


# ─────────── script.json streaming parser (只取 ScriptMetadata 中的 *_TypeInfo) ───────────

# {"Address": <num>, "Name": "...", "Signature": "Il2CppClass*"}  这是 _TypeInfo
# {"Address": <num>, "Name": "...", "Signature": "Il2CppType*" }  这是 _var
_RE_METADATA_ENTRY = re.compile(
    rb'\{\s*"Address"\s*:\s*(\d+)\s*,\s*"Name"\s*:\s*"((?:[^"\\]|\\.)*)"\s*,\s*"Signature"\s*:\s*"Il2CppClass\*"\s*\}'
)
_SUFFIX_TYPEINFO = "_TypeInfo"


def parse_script_json_typeinfo(path: str) -> Dict[str, int]:
    """从 script.json 流式抽取所有 *_TypeInfo. 返回 {full_class_name: rva}."""
    type_info: Dict[str, int] = {}
    # 先定位 ScriptMetadata 段, 再增量扫. script.json 是单行 JSON, 数百 MB.
    target_marker = b'"ScriptMetadata"'
    with open(path, "rb") as f:
        size = os.path.getsize(path)
        # 简单: mmap 整个文件, 找 ScriptMetadata 起点, 然后 regex 在尾段扫
        import mmap
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            i = mm.find(target_marker)
            if i < 0:
                print("[warn] 未找到 ScriptMetadata 段")
                return type_info
            # ScriptMetadata 段一直到文件结尾 (它是最后一个数组之一)
            # 直接 regex 扫到结尾
            for m in _RE_METADATA_ENTRY.finditer(mm, i):
                addr = int(m.group(1))
                name = m.group(2).decode("utf-8", errors="replace")
                if not name.endswith(_SUFFIX_TYPEINFO):
                    continue
                # 去掉 _TypeInfo 后缀, 还原原始 (CLR-mangled) 类名
                base = name[: -len(_SUFFIX_TYPEINFO)]
                # 如果同名出现多次 (泛型实例化), 保留第一个 (一般非泛型基底)
                if base not in type_info:
                    type_info[base] = addr
    print(f"[script.json] 收集 *_TypeInfo: {len(type_info):,}")
    return type_info


# ─────────── 关联: dump.cs 的类名 (CLR style) vs script.json 的 mangled 名 ───────────
#
# script.json 中的 Name 用 CLR 全名 + IL2CPP mangling, 例如:
#   "System.Collections.Generic.List`1<System.Int32>_TypeInfo"
# dump.cs 中的 class 名用 C# 简化形式, 例如:
#   "List<int>"
# 完美对齐很难; 这里只做朴素对齐: 同名直接匹配, 否则跳过.
# 后续 resolver 可对单个类做更精细的查找.


def align_typeinfo(classes: Dict[str, dict], typeinfo_map: Dict[str, int]) -> int:
    """将 typeinfo RVA 写入 classes 字典. 返回成功对齐的数量."""
    aligned = 0
    for cls_name, info in classes.items():
        if cls_name in typeinfo_map:
            info["type_info_rva"] = typeinfo_map[cls_name]
            aligned += 1
    return aligned


# ─────────── GameAssembly.dll sha ───────────


def game_assembly_sha8(path: str) -> str:
    if not os.path.isfile(path):
        return "unknown"
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read(64 * 1024 * 1024))
    return h.hexdigest()[:8]


# ─────────── CLI ───────────


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Build metadata.json from Il2CppDumper output")
    ap.add_argument("--dumper-out", required=True, help="dump.cs / script.json 所在目录")
    ap.add_argument("--game-assembly", default=r"E:\星痕共鸣(2001991)\GameAssembly.dll")
    ap.add_argument("--class-regex", default=None,
                    help="只导出匹配此 regex 的类的字段 (默认: 全量, 慎用)")
    ap.add_argument("--out", default=None, help="输出 metadata.json 路径 (默认: <dumper-out>/../metadata.json)")
    args = ap.parse_args(argv)

    dump_cs = os.path.join(args.dumper_out, "dump.cs")
    script_json = os.path.join(args.dumper_out, "script.json")
    if not os.path.isfile(dump_cs):
        print(f"[fail] 找不到 {dump_cs}", file=sys.stderr); return 1
    if not os.path.isfile(script_json):
        print(f"[fail] 找不到 {script_json}", file=sys.stderr); return 1

    cls_re = re.compile(args.class_regex) if args.class_regex else None
    if cls_re is None:
        print("[warn] 未指定 --class-regex, 将全量解析 dump.cs (慢, 输出大)")

    print(f"[1/3] 解析 dump.cs {os.path.getsize(dump_cs)/1024/1024:.1f}MB ...")
    t0 = time.time()
    classes = parse_dump_cs(dump_cs, cls_re)
    print(f"      用时 {time.time()-t0:.1f}s")

    print(f"[2/3] 解析 script.json (TypeInfo 段, ~{os.path.getsize(script_json)/1024/1024:.0f}MB) ...")
    t0 = time.time()
    typeinfo_map = parse_script_json_typeinfo(script_json)
    print(f"      用时 {time.time()-t0:.1f}s")

    print(f"[3/3] 对齐 typeinfo RVA ...")
    aligned = align_typeinfo(classes, typeinfo_map)
    print(f"      对齐 {aligned}/{len(classes)} 个类")

    out_path = args.out or os.path.join(os.path.dirname(args.dumper_out.rstrip(os.sep)), "metadata.json")
    sha8 = game_assembly_sha8(args.game_assembly)
    payload = {
        "version": 1,
        "metadata_version": 31,
        "game_assembly_sha8": sha8,
        "class_regex": args.class_regex,
        "type_info_map": typeinfo_map,   # 全量
        "classes": classes,              # 按 regex 过滤
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, separators=(",", ":"))
    sz = os.path.getsize(out_path) / 1024 / 1024
    print(f"[ok] -> {out_path}  ({sz:.2f}MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
