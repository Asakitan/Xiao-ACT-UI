"""dump_cs_parser - 流式解析 Il2CppDumper dump.cs, 抽 class -> field offsets.

dump.cs 格式:
  // Namespace: <ns>
  [Attribute]
  <modifiers> class|struct|enum|interface <Name> [: <bases>] // TypeDefIndex: NNN
  {
      // Fields
      <modifiers> <type> <name>; // 0xOFFSET
      ...
      // Methods
      // RVA: 0x... Offset: 0x... VA: 0x...
      <signature>
      ...
  }

输出 JSON:
  {
    "Zproto.CharSerialize": {
        "namespace": "Zproto",
        "type_def_index": 10518,
        "bases": "IMessage<CharSerialize>, ...",
        "fields": [
            {"name": "CharId", "type": "long", "offset": 16, "is_static": false, "modifiers": "public"},
            ...
        ]
    },
    ...
  }
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from typing import Dict, List, Optional


_CLASS_RE = re.compile(
    r'^(?P<mods>(?:public|private|internal|protected|static|sealed|abstract|readonly|partial|unsafe|\s)+)'
    r'(?P<kind>class|struct|enum|interface)\s+'
    r'(?P<name>[^\s:{<]+(?:<[^>]*>)?)'
    r'(?:\s*:\s*(?P<bases>[^/]+?))?'
    r'\s*//\s*TypeDefIndex:\s*(?P<tdi>\d+)\s*$'
)

# Field line: tab + <modifiers> <type> <name>; // 0xOFFSET
# Type can include generics <...>, arrays [], and dots. Name is plain ident or
# CompilerGenerated <Backing>k__BackingField.
_FIELD_RE = re.compile(
    r'^\t(?P<mods>(?:public|private|internal|protected|static|readonly|const|volatile|\s)+)'
    r'(?P<type>[^\s][^;]*?)\s+'
    r'(?P<name>[A-Za-z_<][\w<>`]*)\s*;\s*//\s*0x(?P<off>[0-9A-Fa-f]+)\s*$'
)

_NS_RE = re.compile(r'^//\s*Namespace:\s*(?P<ns>.*)$')


def parse_dump_cs(path: str, progress_every: int = 1_000_000) -> Dict[str, dict]:
    classes: Dict[str, dict] = {}
    cur_ns: str = ""
    cur_cls: Optional[dict] = None
    in_methods: bool = False
    brace_depth: int = 0  # depth INSIDE current class body
    t0 = time.time()
    line_no = 0

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line_no += 1
            if progress_every and line_no % progress_every == 0:
                print(f"  ...line {line_no:,} elapsed {time.time()-t0:.1f}s "
                      f"classes={len(classes)}", file=sys.stderr)

            stripped = line.rstrip("\n")

            if cur_cls is None:
                m = _NS_RE.match(stripped)
                if m:
                    cur_ns = m.group("ns").strip()
                    continue
                m = _CLASS_RE.match(stripped)
                if m:
                    name = m.group("name").strip()
                    full = f"{cur_ns}.{name}" if cur_ns else name
                    cur_cls = {
                        "namespace": cur_ns,
                        "name": name,
                        "kind": m.group("kind"),
                        "type_def_index": int(m.group("tdi")),
                        "bases": (m.group("bases") or "").strip(),
                        "fields": [],
                    }
                    classes[full] = cur_cls
                    in_methods = False
                    brace_depth = 0
                continue

            # inside a class
            if "{" in stripped:
                brace_depth += stripped.count("{")
            if "}" in stripped:
                brace_depth -= stripped.count("}")
                if brace_depth <= 0:
                    cur_cls = None
                    in_methods = False
                    continue

            if "// Methods" in stripped:
                in_methods = True
                continue
            if in_methods:
                continue

            m = _FIELD_RE.match(stripped)
            if m:
                mods = " ".join(m.group("mods").split())
                cur_cls["fields"].append({
                    "name": m.group("name"),
                    "type": m.group("type").strip(),
                    "offset": int(m.group("off"), 16),
                    "is_static": "static" in mods.split(),
                    "modifiers": mods,
                })

    print(f"[dump_cs] parsed {len(classes):,} classes in {time.time()-t0:.1f}s",
          file=sys.stderr)
    return classes


class DumpCsIndex:
    def __init__(self, classes: Dict[str, dict]):
        self.classes = classes
        # short-name -> [full_names]
        self._short: Dict[str, List[str]] = {}
        for full, c in classes.items():
            self._short.setdefault(c["name"], []).append(full)

    @classmethod
    def load_from_json(cls, path: str) -> "DumpCsIndex":
        with open(path, "r", encoding="utf-8") as f:
            return cls(json.load(f))

    @classmethod
    def from_dump_cs(cls, dump_cs_path: str) -> "DumpCsIndex":
        return cls(parse_dump_cs(dump_cs_path))

    def find_class(self, name: str) -> Optional[dict]:
        if name in self.classes:
            return self.classes[name]
        hits = self._short.get(name)
        if hits and len(hits) == 1:
            return self.classes[hits[0]]
        if hits:
            # 多重命名时优先 Zproto.*
            zproto = [h for h in hits if h.startswith("Zproto.")]
            if len(zproto) == 1:
                return self.classes[zproto[0]]
        return None

    def field_offset(self, class_name: str, field_name: str) -> Optional[int]:
        c = self.find_class(class_name)
        if not c:
            return None
        for f in c["fields"]:
            if f["name"] == field_name and not f["is_static"]:
                return f["offset"]
        return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dump-cs", required=True)
    p.add_argument("--out", required=True, help="output JSON path")
    p.add_argument("--verify", action="append", default=[],
                   help="ClassName:FieldName=expected_offset_hex (e.g. CharSerialize:CharId=10)")
    args = p.parse_args()

    classes = parse_dump_cs(args.dump_cs)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(classes, f, ensure_ascii=False)
    print(f"[OK] wrote {args.out} ({os.path.getsize(args.out):,} bytes)")

    idx = DumpCsIndex(classes)
    for spec in args.verify:
        cls_field, exp = spec.split("=")
        cls, field = cls_field.split(":")
        actual = idx.field_offset(cls, field)
        exp_int = int(exp, 16)
        ok = actual == exp_int
        got_str = f"0x{actual:X}" if actual is not None else "None"
        print(f"  verify {cls}.{field}: got {got_str} "
              f"expected 0x{exp_int:X}  {'OK' if ok else 'FAIL'}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
