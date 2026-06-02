"""refresh - 游戏更新后一键刷新基址 + 偏移库.

完整流水线:
  1. 探测当前游戏 game_key (检查是否已注册)
  2. 调 dump_tool: mem dump metadata + Il2CppDumper → dump.cs/script.json
  3. 调 dump_cs_parser → dump_cs_index.json
  4. 调 bundle_build (用预设 class 列表 + 1 阶展开)
  5. 调 bundle_store.register → 写入 store
  6. 清除旧 instance_cache (因为 klass_ptr 变了)

CLI:
    python -m tools.mem_probe.il2cpp.refresh
    python -m tools.mem_probe.il2cpp.refresh --force        # 已注册也重做
    python -m tools.mem_probe.il2cpp.refresh --skip-mem-dump  # 用现有 metadata
"""
from __future__ import annotations

import argparse
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(_HERE)))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from mem_probe.il2cpp import dump_tool, dump_cs_parser, bundle_build, bundle_store
from mem_probe.il2cpp import mem_dump_metadata as mdm
from mem_probe.il2cpp.instance_cache import clear_cache


# bundle 默认含的类 (combat / self / common). 加新 class 改这里.
DEFAULT_BUNDLE_CLASSES = [
    "Zproto.CharSerialize",
    "Zproto.UserFightAttr",
    "Zproto.UserFightAttrContainerArchive",
    "Zproto.CharSerializeContainerArchive",
    "Zproto.CharBaseInfo",
    "Zproto.SceneData",
    "Zproto.SceneLuaData",
    "Zproto.BuffDBInfo",
    "Zproto.BuffDBData",
    "Zproto.BuffInfo",
    "Zproto.BuffInfoSync",
    "Zproto.SkillCDInfo",
    "Zproto.SyncDamageInfo",
    "Zproto.SyncHitInfo",
    "Zproto.ClientHitInfo",
    "Zproto.ClientHitPartInfo",
    "Zproto.UseSkill",
    "Zproto.UseSkillParam",
]


def main(argv=None):
    ap = argparse.ArgumentParser(description="刷新游戏基址 + 偏移库")
    ap.add_argument("--game-dir", default=r"E:\星痕共鸣(2001991)")
    ap.add_argument("--force", action="store_true",
                    help="即使当前版本已注册也重新 dump")
    ap.add_argument("--skip-mem-dump", action="store_true")
    ap.add_argument("--expand-depth", type=int, default=1)
    args = ap.parse_args(argv)

    print("=" * 60)
    print("Step 0: 探测当前游戏版本")
    print("=" * 60)
    info = bundle_store.compute_running_game_key()
    if info is None:
        print("[fail] 无法探测 — Star.exe 未运行?", file=sys.stderr)
        return 1
    key, ga_path, ga_size = info
    print(f"  game_key = {key[:32]}...")
    print(f"  ga_path  = {ga_path}")
    print(f"  ga_size  = {ga_size:,}")

    existing = bundle_store.find_bundle_for_key(key)
    if existing and not args.force:
        print(f"\n[OK] 当前版本已有 bundle: {existing}")
        print("  使用 --force 强制重新 dump.")
        return 0
    if existing:
        print(f"  [force] 已有 {existing}, 强制重做")

    # Step 1+2: dump_tool 完成 mem-dump + Il2CppDumper
    t0 = time.time()
    print()
    print("=" * 60)
    print("Step 1+2: dump_tool (mem metadata + Il2CppDumper)")
    print("=" * 60)
    dump_args = ["--game-dir", args.game_dir]
    if args.skip_mem_dump:
        dump_args.append("--skip-mem-dump")
    rc = dump_tool.main(dump_args)
    if rc != 0:
        print(f"[fail] dump_tool rc={rc}", file=sys.stderr)
        return rc

    # 算 dump_id 路径 (mem_dump_metadata 用 sha8)
    out_dir = mdm._default_out_dir(args.game_dir)
    dump_id = os.path.basename(out_dir)
    dumper_out = os.path.join(out_dir, "dumper_out")
    dump_cs_path = os.path.join(dumper_out, "dump.cs")
    script_json = os.path.join(dumper_out, "script.json")
    dump_cs_idx = os.path.join(out_dir, "dump_cs_index.json")

    # Step 3: dump_cs_parser
    print()
    print("=" * 60)
    print(f"Step 3: dump_cs_parser → {dump_cs_idx}")
    print("=" * 60)
    classes = dump_cs_parser.parse_dump_cs(dump_cs_path)
    import json as _json
    with open(dump_cs_idx, "w", encoding="utf-8") as f:
        _json.dump(classes, f, ensure_ascii=False)
    print(f"  [OK] {len(classes):,} classes, {os.path.getsize(dump_cs_idx)/1024:.0f}KB")

    # Step 4: bundle_build
    bundle_path = os.path.join(_HERE, "_cache", "bundle.json")
    print()
    print("=" * 60)
    print(f"Step 4: bundle_build (depth={args.expand_depth})")
    print("=" * 60)
    bundle = bundle_build.build_bundle(
        script_json, dump_cs_idx, ga_path,
        dump_id, DEFAULT_BUNDLE_CLASSES, expand_depth=args.expand_depth,
    )
    os.makedirs(os.path.dirname(bundle_path), exist_ok=True)
    with open(bundle_path, "w", encoding="utf-8") as f:
        _json.dump(bundle, f, ensure_ascii=False, indent=2)
    print(f"  [OK] {bundle_path} ({os.path.getsize(bundle_path)/1024:.0f}KB, "
          f"{len(bundle['classes'])} classes, {len(bundle['klass_rva'])} RVAs)")

    # Step 5: register in bundle_store
    print()
    print("=" * 60)
    print("Step 5: bundle_store register")
    print("=" * 60)
    e = bundle_store.register_bundle(bundle_path)
    print(f"  [OK] registered {e.bundle_id} → store")

    # Step 6: 清旧 instance_cache (klass_ptr 变了)
    print()
    print("=" * 60)
    print("Step 6: 清除旧 instance_cache (klass_ptr 已变化)")
    print("=" * 60)
    clear_cache()
    print("  [OK] cache cleared")

    print()
    print(f"[完成] 总耗时 {time.time()-t0:.1f}s")
    print(f"下次 StaticDpsSource() 启动会自动用 bundle {e.bundle_id}")
    print(f"首次 get_self_snapshot() 需要 ~200s (重扫); 之后命中缓存 <1ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())

