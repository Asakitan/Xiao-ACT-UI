"""End-to-end rebuild: dump metadata, run Il2CppDumper, build bundle.

Steps:
  1. Resolve current Star.exe PID + GameAssembly.dll path/sha
  2. Dump decrypted global-metadata.dat from the running process
  3. Copy GameAssembly.dll into the dumper out dir
  4. Run Il2CppDumper.exe (net6 self-contained) -> script.json + dump.cs
  5. Run mem_probe.il2cpp.bundle_build to produce a new bundle.json under
     mem_probe/il2cpp/out/<game_key8>/bundle.json
  6. Register the bundle in the store
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from typing import Optional

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.dirname(ROOT))

from plugins.star_resonance_plugin.mem.il2cpp.bundle_store import (  # noqa: E402
    compute_running_game_key, register_bundle,
)
from plugins.star_resonance_plugin.mem.il2cpp.bundle_build import (  # noqa: E402
    DEFAULT_CLASSES, build_bundle, expand_referenced_classes,
)
from plugins.star_resonance_plugin.mem.il2cpp.dump_cs_parser import DumpCsIndex  # noqa: E402
from plugins.star_resonance_plugin.mem.il2cpp.script_parser import ScriptIndex  # noqa: E402
from plugins.star_resonance_plugin.mem.il2cpp.mem_dump_metadata import (  # noqa: E402
    find_metadata_in_process, dump_metadata,
)
from ..process import StarProcess  # noqa: E402


DUMPER_DIR = os.path.join(ROOT, 'mem_probe', 'il2cpp', 'tools', 'Il2CppDumper')
DUMPER_EXE = os.path.join(DUMPER_DIR, 'Il2CppDumper.exe')
OUT_BASE = os.path.join(ROOT, 'mem_probe', 'il2cpp', 'out')


def _dump_metadata(out_dir: str) -> Optional[str]:
    info = compute_running_game_key()
    if info is None:
        return None
    pm = StarProcess()
    candidates = find_metadata_in_process(pm, verbose=False)
    if not candidates:
        pm.close()
        return None
    addr, version, total = candidates[0]
    print(f'[dump] picked addr=0x{addr:X} version={version} total={total/1024/1024:.2f}MB')
    md_path = os.path.join(out_dir, 'global-metadata.dat')
    dump_metadata(pm, addr, total, md_path)
    pm.close()
    return md_path


def _run_dumper(ga_path: str, md_path: str, out_dir: str) -> bool:
    cmd = [DUMPER_EXE, ga_path, md_path, out_dir]
    print('[dumper]', ' '.join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if proc.stdout:
        print(proc.stdout[-2000:])
    if proc.returncode != 0:
        print('[dumper stderr]', proc.stderr[-2000:])
        return False
    return True


def _build_bundle(out_dir: str, ga_path: str) -> Optional[str]:
    script_path = os.path.join(out_dir, 'script.json')
    dump_path = os.path.join(out_dir, 'dump.cs')
    if not (os.path.isfile(script_path) and os.path.isfile(dump_path)):
        print('[bundle] missing script.json or dump.cs')
        return None
    si = ScriptIndex.load(script_path)
    print(f'[bundle] ScriptIndex klass_rva: {len(si.klass_rva)}; type_var: {len(si.type_var_rva)}')
    print('[bundle] parsing dump.cs (70MB, ~1 minute)...', flush=True)
    dci = DumpCsIndex.from_dump_cs(dump_path)
    print(f'[bundle] DumpCsIndex: {len(dci.classes)} classes', flush=True)
    # Persist a JSON snapshot so build_bundle() can read it
    dci_json = os.path.join(out_dir, 'dump_cs_index.json')
    with open(dci_json, 'w', encoding='utf-8') as f:
        json.dump(dci.classes, f, ensure_ascii=False)
    print(f'[bundle] wrote {dci_json} ({os.path.getsize(dci_json)/1024:.1f}KB)', flush=True)
    classes = set(DEFAULT_CLASSES)
    extra = expand_referenced_classes(dci, list(classes), max_depth=1)
    classes.update(extra)
    print(f'[bundle] {len(classes)} classes after expansion', flush=True)
    out_bundle = os.path.join(out_dir, 'bundle.json')
    info = compute_running_game_key()
    dump_id = info[0][:8] if info else 'unknown'
    bundle = build_bundle(
        script_json=script_path,
        dump_cs_json=dci_json,
        ga_path=ga_path,
        dump_id=dump_id,
        classes=sorted(classes),
        expand_depth=0,
    )
    with open(out_bundle, 'w', encoding='utf-8') as f:
        json.dump(bundle, f, ensure_ascii=False, indent=2)
    print(f'[bundle] wrote {out_bundle} ({os.path.getsize(out_bundle):,} bytes)')
    if not os.path.isfile(out_bundle):
        return None
    return out_bundle


def rebuild() -> Optional[str]:
    if not os.path.isfile(DUMPER_EXE):
        print(f'[ERR] Il2CppDumper.exe not found at {DUMPER_EXE}')
        return None
    info = compute_running_game_key()
    if info is None:
        print('[ERR] cannot find running Star.exe with GameAssembly.dll')
        return None
    game_key, ga_path, ga_size = info
    out_dir = os.path.join(OUT_BASE, game_key[:8])
    os.makedirs(out_dir, exist_ok=True)
    print(f'[plan] game_key={game_key[:16]}...  out_dir={out_dir}')

    md_path = _dump_metadata(out_dir)
    if md_path is None:
        print('[ERR] metadata dump failed')
        return None
    print(f'[dump] wrote {os.path.getsize(md_path)/1024/1024:.2f}MB -> {md_path}')

    local_ga = os.path.join(out_dir, 'GameAssembly.dll')
    if os.path.abspath(ga_path) != os.path.abspath(local_ga):
        shutil.copy(ga_path, local_ga)
    print(f'[stage] GameAssembly.dll -> {local_ga}')

    if not _run_dumper(local_ga, md_path, out_dir):
        return None
    for fname in ('dump.cs', 'script.json', 'il2cpp.h', 'stringliteral.json'):
        p = os.path.join(out_dir, fname)
        if os.path.isfile(p):
            print(f'[dumper] {fname}: {os.path.getsize(p):,}')
        else:
            print(f'[dumper] MISSING: {fname}')

    bundle_path = _build_bundle(out_dir, local_ga)
    if bundle_path is None:
        return None
    print(f'[bundle] produced {bundle_path}')

    with open(bundle_path, 'r', encoding='utf-8') as f:
        meta = json.load(f)['meta']
    print('[bundle] meta =', meta)
    try:
        entry = register_bundle(bundle_path)
        print(f'[bundle] registered: id={entry.bundle_id} classes={entry.classes}')
    except Exception as e:
        print('[bundle] register failed:', e)
    return bundle_path


if __name__ == '__main__':
    rebuild()
