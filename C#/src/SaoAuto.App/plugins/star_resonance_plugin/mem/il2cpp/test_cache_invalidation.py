"""tests for instance_cache - 验证失效逻辑 (无 200s 扫描).

只测 validate_cache_entry 的鉴别能力, 不触发全堆扫.
"""
from __future__ import annotations

import os
import sys

_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from plugins.star_resonance_plugin.mem.il2cpp.static_dps_source import StaticDpsSource
from plugins.star_resonance_plugin.mem.il2cpp.instance_cache import (
    _DEFAULT_CACHE, _key, _load, _save, validate_cache_entry,
)
from ..process import StarProcessError

CLS = "Zproto.CharSerialize"
SENT_FIELD = "Attr"
SENT_CLS = "Zproto.UserFightAttr"
KEY = _key(CLS, SENT_FIELD)


def test_with_cache_mod(name: str, mutator) -> bool:
    """临时改 cache, 校验, 恢复. 返回 True = 校验拒绝(预期)."""
    cache = _load(_DEFAULT_CACHE)
    if KEY not in cache:
        print(f"  [SKIP {name}] cache 没有 baseline entry")
        return False
    backup = dict(cache[KEY])
    mutator(cache[KEY])
    _save(_DEFAULT_CACHE, cache)
    src = StaticDpsSource()
    try:
        ok = validate_cache_entry(src.sr, CLS, SENT_FIELD, SENT_CLS)
    finally:
        src.close()
    cache[KEY] = backup
    _save(_DEFAULT_CACHE, cache)
    return not ok


def main():
    print("[#0] ensure baseline cache exists")
    try:
        src = StaticDpsSource()
    except StarProcessError as e:
        print(f"  [SKIP] live Star.exe memory attach unavailable: {e}")
        print("  This cache-invalidation smoke needs PROCESS_QUERY|VM_READ access; deterministic mem tests should still run without it.")
        return 0
    try:
        try:
            resolver = src.sr
        except StarProcessError as e:
            print(f"  [SKIP] live Star.exe memory attach unavailable: {e}")
            print("  This cache-invalidation smoke needs PROCESS_QUERY|VM_READ access; deterministic mem tests should still run without it.")
            return 0
        if not validate_cache_entry(resolver, CLS, SENT_FIELD, SENT_CLS):
            print("  baseline cache invalid, doing full scan...")
            s = src.get_self_snapshot()
            assert s is not None, "scan failed"
        else:
            print("  baseline cache OK")
    finally:
        src.close()

    print("[#1] baseline validation (expect PASS)")
    src = StaticDpsSource()
    try:
        ok = validate_cache_entry(src.sr, CLS, SENT_FIELD, SENT_CLS)
    finally:
        src.close()
    assert ok, "baseline should validate"
    print("  PASS")

    print("[#2] corrupt pid (expect REJECT)")
    rejected = test_with_cache_mod("pid", lambda e: e.update({"pid": 999999}))
    assert rejected
    print("  PASS")

    print("[#3] corrupt obj (expect REJECT)")
    rejected = test_with_cache_mod("obj", lambda e: e.update({"obj": 0xDEADBEEF1234}))
    assert rejected
    print("  PASS")

    print("[#4] corrupt klass_ptr (expect REJECT)")
    rejected = test_with_cache_mod("klass_ptr", lambda e: e.update({"klass_ptr": 0xFEEDFACE}))
    assert rejected
    print("  PASS")

    print("[#5] corrupt ga_base (expect REJECT)")
    rejected = test_with_cache_mod("ga_base", lambda e: e.update({"ga_base": 0x1000}))
    assert rejected
    print("  PASS")

    print("[#6] post-mutation baseline (expect PASS)")
    src = StaticDpsSource()
    try:
        ok = validate_cache_entry(src.sr, CLS, SENT_FIELD, SENT_CLS)
    finally:
        src.close()
    assert ok
    print("  PASS")

    print("\n[OK] all 5 invalidation paths correctly rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

