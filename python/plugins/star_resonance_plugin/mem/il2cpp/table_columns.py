# -*- coding: utf-8 -*-
# table_columns - extract config-table row blob column offsets from getter thunks.
#
# A table row's serialized blob columns are NOT IL2CPP class fields, so the
# auto_offsets (live field table / dump bundle) path cannot resolve them. The
# authoritative source is the compiled property getter itself: every row getter
# is a tiny thunk
#
# call <get_proxy helper>
# xor  r8d, r8d
# mov  edx, <COLUMN>        ; or xor edx,edx for column 0
# mov  rcx, rax
# ...epilogue...
# jmp  <ReadProxy.ReadXxx>  ; reader picks the column TYPE
#
# so the column offset is the ``edx`` immediate and the column type follows from
# which ``ReadProxy.Read*`` the thunk tail-jumps to. ``dump.cs`` provides each
# getter's file Offset + RVA and the ReadProxy reader RVAs, so the whole
# extraction works offline on ``out/<dump_id>/`` artifacts.
#
# Resolution order used by callers:
# 1. versioned cache ``_cache/table_columns_<dump_id>.json``
# 2. fresh extraction from dump.cs + GameAssembly.dll (then cached)
# 3. ``FALLBACK`` curated literals
# plus a live ``self_check`` gate: when the extracted columns fail live
# plausibility the caller downgrades to the literals (with a log line).
#
# Offline CLI:
# python -m mem_probe.il2cpp.table_columns Bokura.RaidDungeonTableBase [more...]
from __future__ import annotations

import json
import os
import re
import struct
import sys
from typing import Dict, Iterable, List, Optional, Tuple

_HERE = os.path.dirname(os.path.abspath(__file__))
_OUT_ROOT = os.path.join(_HERE, "out")
_CACHE_DIR = os.path.join(_HERE, "_cache")

READPROXY_CLS = "Bokura.Table.ReadProxy"

# ReadProxy.Read* method name -> compact column type tag.
READER_TYPE = {
    "ReadLength": "length",
    "ReadInt32": "i32",
    "ReadInt64": "i64",
    "ReadSingle": "f32",
    "ReadBoolean": "bool",
    "ReadString": "string",
    "ReadMLString": "mlstring",
    "ReadVector2": "vec2",
    "ReadVector3": "vec3",
    "ReadIntNumber": "intnumber",
    "ReadInt32Array": "i32array",
    "ReadInt32Table": "i32table",
    "ReadInt64Array": "i64array",
    "ReadNumberArray": "numberarray",
    "ReadNumberTable": "numbertable",
    "ReadMLStringArray": "mlstringarray",
    "ReadMLStringTable": "mlstringtable",
    "ReadStringArray": "stringarray",
    "ReadStringTable": "stringtable",
    "ReadStringTripleArray": "stringtriplearray",
    "ReadVector2Array": "vec2array",
    "ReadVector3Array": "vec3array",
    "ReadKVIntInt": "kvintint",
    "ReadKVIntNumber": "kvintnumber",
    "ReadKVIntNumberArray": "kvintnumberarray",
    "ReadKVIntMLString": "kvintmlstring",
    "ReadKVIntString": "kvintstring",
    "ReadKVIntVector2": "kvintvec2",
    "ReadKVIntVector3": "kvintvec3",
}

# Curated literal columns (verified against out/fdc7111b getter thunks + live
# reads). Used when extraction artifacts are missing or the live self-check
# rejects the extracted set.
FALLBACK: Dict[str, Dict[str, Tuple[int, str]]] = {
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
        "BornSkillId": (0xE7, "i32"),
    },
    "Bokura.SkillTableBase": {
        "Id": (0x0, "i32"),
        "NameDesign": (0x4, "string"),
        "Name": (0xC, "mlstring"),
    },
    "Bokura.BuffTableBase": {
        "Id": (0x0, "i32"),
        "Name": (0x10, "mlstring"),
    },
}

# getter thunk tail: xor r8d,r8d ; (xor edx,edx | mov edx,imm32) ; mov rcx,rax ;
# short epilogue ; jmp rel32 <reader>. xor reg,reg appears as opcode 0x31 or 0x33.
_THUNK_RE = re.compile(
    rb"\x45[\x31\x33]\xc0"                       # xor r8d, r8d
    rb"(?:[\x31\x33]\xd2|\xba(?P<imm>.{4}))"     # xor edx,edx | mov edx, imm32
    rb"\x48\x8b\xc8"                             # mov rcx, rax
    rb"(?P<epi>.{0,16}?)"                        # add rsp,..; pop ..
    rb"\xe9(?P<rel>.{4})",                       # jmp rel32
    re.DOTALL,
)

_NS_RE = re.compile(r"^//\s*Namespace:\s*(?P<ns>.*)$")
_CLASS_RE = re.compile(r"^[\w\s\[\]]*?(?:class|struct)\s+(?P<name>[A-Za-z_]\w*)\s*[:/]")
_RVA_RE = re.compile(r"^\s*//\s*RVA:\s*0x(?P<rva>[0-9A-Fa-f]+)\s+Offset:\s*0x(?P<off>[0-9A-Fa-f]+)")
_GETTER_RE = re.compile(r"^\s*(?:public|private|internal|protected)?[\w\s<>,.\[\]`]*?\bget_(?P<prop>\w+)\s*\(\s*\)")
_READER_RE = re.compile(r"^\s*public\s+[\w<>,.\[\]]+\s+(?P<name>Read\w+)\s*\(int offset\)")


# ── dump dir selection ───────────────────────────────────────────────────────

def _find_dump_cs(d: str) -> str:
    for cand in (os.path.join(d, "dump.cs"), os.path.join(d, "dumper_out", "dump.cs")):
        if os.path.isfile(cand):
            return cand
    return ""


def _find_ga(d: str) -> str:
    for cand in (os.path.join(d, "GameAssembly.dll"),
                 os.path.join(d, "dumper_out", "GameAssembly.dll")):
        if os.path.isfile(cand):
            return cand
    return ""


def select_dump_dir(out_root: str = _OUT_ROOT) -> Optional[Tuple[str, str]]:
    # Pick the dump dir to extract from. Returns (dir, dump_id) or None.
    #
    # Preference: the version matching the RUNNING game (via bundle_store), when
    # its out/<dump_id>/ dir holds both dump.cs and GameAssembly.dll; otherwise
    # the newest out/ dir that has both artifacts.
    try:
        from plugins.star_resonance_plugin.mem.il2cpp.bundle_store import find_bundle_for_running_game
        hit = find_bundle_for_running_game()
        if hit:
            with open(hit[0], "r", encoding="utf-8") as f:
                meta = (json.load(f).get("meta") or {})
            dump_id = str(meta.get("dump_id") or "")
            d = os.path.join(out_root, dump_id)
            if dump_id and _find_dump_cs(d) and _find_ga(d):
                return d, dump_id
    except Exception:
        pass
    best: Optional[Tuple[float, str, str]] = None
    try:
        names = os.listdir(out_root)
    except OSError:
        return None
    for name in names:
        d = os.path.join(out_root, name)
        if not os.path.isdir(d):
            continue
        dc, ga = _find_dump_cs(d), _find_ga(d)
        if not (dc and ga):
            continue
        mt = max(os.path.getmtime(dc), os.path.getmtime(ga))
        if best is None or mt > best[0]:
            best = (mt, d, name)
    return (best[1], best[2]) if best else None


# ── dump.cs streaming parse ──────────────────────────────────────────────────

def parse_classes_getters(dump_cs_path: str, class_full_names: Iterable[str],
                          ) -> Tuple[Dict[str, Dict[str, Tuple[int, int]]], Dict[int, str]]:
    # One streaming pass over dump.cs.
    #
    # Returns ({class_full_name: {prop: (rva, file_offset)}},
    # {reader_rva: reader_method_name}) — the latter from the
    # ``Bokura.Table.ReadProxy`` struct block. Early-exits once every wanted
    # class block and the ReadProxy block have been consumed.
    want = {str(c) for c in class_full_names}
    by_short: Dict[str, str] = {}
    for full in want | {READPROXY_CLS}:
        ns, _, short = full.rpartition(".")
        by_short.setdefault(short, full)

    getters: Dict[str, Dict[str, Tuple[int, int]]] = {c: {} for c in want}
    reader_map: Dict[int, str] = {}
    done: set = set()
    cur_ns = ""
    cur_full: Optional[str] = None
    depth = 0
    pend: Optional[Tuple[int, int]] = None     # (rva, file_off) awaiting its method line

    with open(dump_cs_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if cur_full is None:
                m = _NS_RE.match(line)
                if m:
                    cur_ns = m.group("ns").strip()
                    continue
                m = _CLASS_RE.match(line)
                if m:
                    short = m.group("name")
                    full = f"{cur_ns}.{short}" if cur_ns else short
                    if full in want or full == READPROXY_CLS:
                        cur_full = full
                        depth = 0
                        pend = None
                continue
            # inside a wanted block
            if "{" in line:
                depth += line.count("{")
            if "}" in line:
                depth -= line.count("}")
                if depth <= 0:
                    done.add(cur_full)
                    cur_full = None
                    if want | {READPROXY_CLS} <= done:
                        break
                    continue
            m = _RVA_RE.match(line)
            if m:
                try:
                    pend = (int(m.group("rva"), 16), int(m.group("off"), 16))
                except ValueError:
                    pend = None
                continue
            if pend is None:
                continue
            if cur_full == READPROXY_CLS:
                m = _READER_RE.match(line)
                if m:
                    reader_map[pend[0]] = m.group("name")
                    pend = None
                    continue
            else:
                m = _GETTER_RE.match(line)
                if m:
                    prop = m.group("prop")
                    if prop != "proxy_":      # ref-ReadProxy accessor, not a column
                        getters[cur_full][prop] = pend
                    pend = None
                    continue
            if line.strip() and not line.lstrip().startswith("["):
                pend = None      # any other statement line consumes the RVA
    return getters, reader_map


def parse_class_getters(dump_cs_path: str, class_full_name: str,
                        ) -> Tuple[Dict[str, Tuple[int, int]], Dict[int, str]]:
    # Single-class convenience wrapper: ({prop: (rva, file_offset)}, reader_map).
    getters, reader_map = parse_classes_getters(dump_cs_path, [class_full_name])
    return getters.get(class_full_name, {}), reader_map


# ── GameAssembly thunk decode ────────────────────────────────────────────────

def extract_columns(ga_path: str, getters: Dict[str, Tuple[int, int]],
                    reader_map: Dict[int, str]) -> Dict[str, Tuple[int, str]]:
    # {prop: (rva, file_off)} -> {prop: (column_offset, type_tag)}.
    #
    # Reads <=0x100 bytes per getter at its file offset, matches the thunk byte
    # pattern, converts the tail-jmp rel32 to an RVA and requires it to land on a
    # known ReadProxy reader. Getters whose body does not match (non-thunk
    # getters, cached MLString wrappers, ...) are dropped.
    out: Dict[str, Tuple[int, str]] = {}
    with open(ga_path, "rb") as f:
        for prop, (rva, off) in getters.items():
            if off <= 0 or rva <= 0:
                continue
            f.seek(off)
            code = f.read(0x100)
            m = _THUNK_RE.search(code)
            if not m:
                continue
            imm = m.group("imm")
            col = struct.unpack("<i", imm)[0] if imm else 0
            if col < 0 or col > 0x4000:
                continue
            jmp_pos = m.start("rel") - 1               # the E9 byte
            rel = struct.unpack("<i", m.group("rel"))[0]
            target_rva = rva + jmp_pos + 5 + rel
            reader = reader_map.get(target_rva)
            if not reader:
                continue
            out[prop] = (col, READER_TYPE.get(reader, reader.lower()))
    return out


# ── versioned cache ──────────────────────────────────────────────────────────

def _cache_path(dump_id: str) -> str:
    return os.path.join(_CACHE_DIR, f"table_columns_{dump_id}.json")


def _load_cache(dump_id: str) -> Dict[str, Dict[str, Tuple[int, str]]]:
    try:
        with open(_cache_path(dump_id), "r", encoding="utf-8") as f:
            raw = json.load(f)
        out: Dict[str, Dict[str, Tuple[int, str]]] = {}
        for cls, cols in (raw.get("classes") or {}).items():
            out[cls] = {p: (int(v[0]), str(v[1])) for p, v in cols.items()}
        return out
    except Exception:
        return {}


def _save_cache(dump_id: str, classes: Dict[str, Dict[str, Tuple[int, str]]]) -> None:
    try:
        os.makedirs(_CACHE_DIR, exist_ok=True)
        payload = {"dump_id": dump_id,
                   "classes": {c: {p: list(v) for p, v in cols.items()}
                               for c, cols in classes.items()}}
        tmp = _cache_path(dump_id) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=1, sort_keys=True)
        os.replace(tmp, _cache_path(dump_id))
    except OSError:
        pass


def load_columns(class_full_names: Iterable[str], *, dump_dir: Optional[str] = None,
                 dump_id: Optional[str] = None, use_cache: bool = True,
                 log=print) -> Dict[str, Dict[str, Tuple[int, str]]]:
    # Columns for several classes: cache -> extraction -> FALLBACK (per class).
    #
    # Always returns an entry for every requested class (possibly the literal
    # fallback, possibly {}); the per-class source is logged.
    want = [str(c) for c in class_full_names]
    if dump_dir is None:
        sel = select_dump_dir()
        if sel:
            dump_dir, dump_id = sel
    if dump_id is None and dump_dir:
        dump_id = os.path.basename(os.path.normpath(dump_dir))

    cached = _load_cache(dump_id) if (use_cache and dump_id) else {}
    out: Dict[str, Dict[str, Tuple[int, str]]] = {}
    missing = [c for c in want if c not in cached]
    if missing and dump_dir:
        dc, ga = _find_dump_cs(dump_dir), _find_ga(dump_dir)
        if dc and ga:
            try:
                getters, reader_map = parse_classes_getters(dc, missing)
                for cls in missing:
                    cols = extract_columns(ga, getters.get(cls, {}), reader_map)
                    if cols:
                        cached[cls] = cols
                if use_cache and dump_id:
                    _save_cache(dump_id, cached)
            except Exception as e:
                if log:
                    log(f"[table_columns] extraction failed ({e!r}); literal fallback")
    for cls in want:
        if cached.get(cls):
            out[cls] = dict(cached[cls])
        else:
            out[cls] = dict(FALLBACK.get(cls, {}))
            if log:
                log(f"[table_columns] {cls}: using curated literal fallback")
    return out


# ── live plausibility self-check ─────────────────────────────────────────────

def self_check(reader, class_full_name: str, columns: Dict[str, Tuple[int, str]], *,
               pool=None, sample: int = 12, min_ratio: float = 0.5,
               expect: Optional[Dict[str, Tuple[int, int]]] = None,
               log=print) -> bool:
    # Validate extracted columns against live rows. ``reader`` is a
    # MemConfigTableReader; ``pool`` an optional StringPoolBridge for mlstring
    # probes. Per-row gates:
    # - column-0 i32 (the row id) > 0
    # - every ``expect`` prop value lies within its (lo, hi) range
    # - i32array columns decode with 0 <= count <= 512
    # Stale heap objects with recycled ReadProxy ids decode a FOREIGN table's
    # blob, so single bad rows are expected noise: the check passes when at least
    # ``min_ratio`` of sampled rows pass all gates AND at least one passing row
    # resolves an mlstring column to a CJK string (when ``pool`` is ready).
    # Returns False (caller downgrades to literals) on failure.
    rows = []
    try:
        # the validated loader walk yields clean serialized rows; the heap scan
        # (stale objects included) is only the fallback sample source
        for _key, zl, blob in reader.iter_rows_via_loader(class_full_name):
            rows.append((zl, blob))
            if len(rows) >= sample:
                break
        if not rows:
            for _row, zl, blob in reader.iter_rows(class_full_name, limit=sample):
                rows.append((zl, blob))
    except Exception as e:
        if log:
            log(f"[table_columns] self_check {class_full_name}: row iteration error {e!r}")
        return False
    if not rows:
        if log:
            log(f"[table_columns] self_check {class_full_name}: no live rows")
        return False

    id_cols = [p for p, (c, t) in columns.items() if c == 0 and t == "i32"]
    ml_cols = [(p, c) for p, (c, t) in columns.items() if t == "mlstring"]
    arr_cols = [(p, c) for p, (c, t) in columns.items() if t == "i32array"]
    need_cjk = bool(ml_cols and pool is not None and getattr(pool, "ready", False))
    passed = 0
    cjk_seen = not need_cjk
    for zl, blob in rows:
        ok = True
        for p in id_cols:
            v = reader.col_i32(blob, columns[p][0])
            if v is None or v <= 0:
                ok = False
                break
        if ok:
            for p, (lo, hi) in (expect or {}).items():
                if p not in columns:
                    continue
                v = reader.col_i32(blob, columns[p][0])
                if v is None or not (lo <= v <= hi):
                    ok = False
                    break
        if ok:
            for p, c in arr_cols:
                arr = reader.col_i32_array(zl, blob, c)
                if arr is None or len(arr) > 512:
                    ok = False
                    break
        if not ok:
            continue
        passed += 1
        if need_cjk and not cjk_seen:
            for p, c in ml_cols:
                s = pool.resolve(reader.col_mlid(blob, c))
                if s and any("一" <= ch <= "鿿" for ch in s):
                    cjk_seen = True
                    break
    ratio = passed / len(rows)
    if passed == 0 or ratio < min_ratio:
        if log:
            log(f"[table_columns] self_check {class_full_name}: only {passed}/"
                f"{len(rows)} sampled rows pass the gates")
        return False
    if not cjk_seen:
        if log:
            log(f"[table_columns] self_check {class_full_name}: no mlstring column "
                f"resolved to a CJK string on any passing row")
        return False
    return True


# ── offline CLI ──────────────────────────────────────────────────────────────

def _cli(argv: List[str]) -> int:
    import argparse
    p = argparse.ArgumentParser(description="offline table column extraction")
    p.add_argument("classes", nargs="+", help="full class names, e.g. Bokura.RaidDungeonTableBase")
    p.add_argument("--dump-dir", default=None, help="out/<dump_id> dir (default: auto-select)")
    p.add_argument("--no-cache", action="store_true")
    args = p.parse_args(argv)

    dump_dir = args.dump_dir
    dump_id = None
    if dump_dir is None:
        sel = select_dump_dir()
        if not sel:
            print("[table_columns] no dump dir with dump.cs + GameAssembly.dll found")
            return 1
        dump_dir, dump_id = sel
    print(f"[table_columns] dump_dir={dump_dir}")
    cols = load_columns(args.classes, dump_dir=dump_dir, dump_id=dump_id,
                        use_cache=not args.no_cache)
    for cls in args.classes:
        print(f"\n=== {cls} ===")
        for prop, (col, typ) in sorted(cols.get(cls, {}).items(), key=lambda kv: kv[1][0]):
            print(f"  0x{col:04X}  {typ:<14} {prop}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_cli(sys.argv[1:]))
