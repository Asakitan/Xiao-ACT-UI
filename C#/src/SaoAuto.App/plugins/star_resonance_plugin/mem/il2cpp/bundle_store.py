"""bundle_store - 版本化 bundle 仓库 (基址库 + 偏移库).

设计:
  ─ bundles/                                   ← 版本仓库根
       index.json                              ← 摘要: {ga_size, sha256: bundle_id}
       <bundle_id>.json                        ← 完整 bundle (按 ga 内容寻址)
       <bundle_id>.meta.json                   ← 创建时间, dump_id, 类列表

GA-key 计算:
  game_key = sha256(GameAssembly.dll[:1MB])  — 文件首 1MB 哈希就能锁定版本

工作流:
  1. 启动: 探测当前 Star.exe 的 GameAssembly.dll, 算 game_key
  2. 查 index.json: 命中 → 加载对应 bundle (~ms)
  3. 未命中: 提示用户 dump (或自动调 Il2CppDumper, 若 EXE 路径已配置)
  4. dump 完后用 bundle_build 生成新 bundle, 注册到 index.json
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_STORE = os.path.join(_HERE, "_cache", "bundles")


def compute_game_key(ga_path: str, prefix_bytes: int = 1024 * 1024) -> str:
    """对 GA 文件首 1MB 取 sha256, 作为版本指纹."""
    h = hashlib.sha256()
    with open(ga_path, "rb") as f:
        h.update(f.read(prefix_bytes))
    return h.hexdigest()


def compute_running_game_key() -> Optional[tuple]:
    """运行时探测 Star.exe 的 GameAssembly.dll 路径并算 game_key.

    返回 (game_key, ga_path, ga_size) or None.
    """
    try:
        import psutil  # type: ignore
    except ImportError:
        # 退化: 用 pymem 拿模块路径
        try:
            from ..process import StarProcess
            sp = StarProcess()
            try:
                ga = next(m for m in sp.list_modules() if m.name.lower() == "gameassembly.dll")
                # pymem 不直接给文件路径; 用 psutil 兜底, 否则只能 in-memory 取首 1MB
                blob = sp.read_bytes(ga.base, 1024 * 1024)
                if not blob:
                    return None
                key = hashlib.sha256(blob).hexdigest()
                return (key, "<in-memory>", ga.size)
            finally:
                sp.close()
        except Exception:
            return None

    # 用 psutil 找 Star.exe 进程对应的 GameAssembly.dll 文件
    for p in psutil.process_iter(["name", "exe"]):
        try:
            if (p.info.get("name") or "").lower() != "star.exe":
                continue
            exe_dir = os.path.dirname(p.info.get("exe") or "")
            for root, _dirs, files in os.walk(exe_dir):
                for f in files:
                    if f.lower() == "gameassembly.dll":
                        path = os.path.join(root, f)
                        return (compute_game_key(path), path, os.path.getsize(path))
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            continue
    return None


# ─────────── store ───────────

@dataclass
class BundleEntry:
    bundle_id: str
    game_key: str
    ga_size: int
    dump_id: str
    classes: int
    klass_rva: int
    created_at: int


def _index_path(store_dir: str) -> str:
    return os.path.join(store_dir, "index.json")


def _load_index(store_dir: str) -> Dict[str, dict]:
    p = _index_path(store_dir)
    if not os.path.isfile(p):
        return {}
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def _save_index(store_dir: str, idx: Dict[str, dict]) -> None:
    os.makedirs(store_dir, exist_ok=True)
    p = _index_path(store_dir)
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(idx, f, indent=2, ensure_ascii=False)
    os.replace(tmp, p)


def list_bundles(store_dir: str = _DEFAULT_STORE) -> List[BundleEntry]:
    idx = _load_index(store_dir)
    return [BundleEntry(**v) for v in idx.values()]


def find_bundle_for_key(game_key: str,
                        store_dir: str = _DEFAULT_STORE) -> Optional[str]:
    """返回匹配 game_key 的 bundle 文件路径, 没有则 None."""
    idx = _load_index(store_dir)
    e = idx.get(game_key)
    if not e:
        return None
    p = os.path.join(store_dir, f"{e['bundle_id']}.json")
    return p if os.path.isfile(p) else None


def find_bundle_for_running_game(store_dir: str = _DEFAULT_STORE) -> Optional[tuple]:
    """返回 (bundle_path, game_key, ga_path) or None.

    None 表示当前游戏版本没有对应 bundle, 需要 dump+build.
    """
    info = compute_running_game_key()
    if info is None:
        return None
    key, ga_path, _sz = info
    bp = find_bundle_for_key(key, store_dir)
    if bp:
        return (bp, key, ga_path)
    return None


def register_bundle(bundle_path: str, store_dir: str = _DEFAULT_STORE) -> BundleEntry:
    """把已生成的 bundle 文件注册进 store. 用 game_key 作为索引."""
    with open(bundle_path, "r", encoding="utf-8") as f:
        bundle = json.load(f)
    meta = bundle.get("meta", {})
    game_key = meta.get("ga_sha256_first_1mb")
    if not game_key:
        raise ValueError("bundle.meta.ga_sha256_first_1mb missing — bundle_build needs --ga-path")
    bundle_id = game_key[:16]

    os.makedirs(store_dir, exist_ok=True)
    dst = os.path.join(store_dir, f"{bundle_id}.json")
    shutil.copy(bundle_path, dst)

    entry = BundleEntry(
        bundle_id=bundle_id,
        game_key=game_key,
        ga_size=meta.get("ga_size", 0),
        dump_id=meta.get("dump_id", ""),
        classes=len(bundle.get("classes", {})),
        klass_rva=len(bundle.get("klass_rva", {})),
        created_at=meta.get("built_at", int(time.time())),
    )
    idx = _load_index(store_dir)
    idx[game_key] = entry.__dict__
    _save_index(store_dir, idx)

    # 同时写 meta sidecar
    with open(os.path.join(store_dir, f"{bundle_id}.meta.json"), "w", encoding="utf-8") as f:
        json.dump(entry.__dict__, f, indent=2, ensure_ascii=False)
    return entry


# ─────────── CLI ───────────

def _cli():
    import argparse
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="列出所有已注册 bundle")
    sub.add_parser("probe", help="探测当前游戏 game_key, 报告匹配状态")
    reg = sub.add_parser("register", help="把现有 bundle 注册进 store")
    reg.add_argument("bundle", help="bundle.json path")
    args = p.parse_args()

    if args.cmd == "list":
        bs = list_bundles()
        if not bs:
            print("(empty store)")
            return
        for b in bs:
            ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(b.created_at))
            print(f"  [{b.bundle_id}] dump={b.dump_id} classes={b.classes} "
                  f"rva={b.klass_rva} ga_size={b.ga_size} built={ts}")
            print(f"     game_key={b.game_key}")
        return

    if args.cmd == "probe":
        info = compute_running_game_key()
        if info is None:
            print("[FAIL] 无法探测当前游戏 (Star.exe 未运行 或 找不到 GA)")
            return
        key, path, sz = info
        print(f"  game_key = {key}")
        print(f"  ga_path  = {path}")
        print(f"  ga_size  = {sz}")
        bp = find_bundle_for_key(key)
        if bp:
            print(f"  [OK] 已匹配 bundle: {bp}")
        else:
            print(f"  [MISS] 此版本未注册. 需要重新 dump + build + register")
        return

    if args.cmd == "register":
        e = register_bundle(args.bundle)
        print(f"[OK] registered as {e.bundle_id} ({e.classes} classes)")
        return


if __name__ == "__main__":
    _cli()

