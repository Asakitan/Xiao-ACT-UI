"""instance_cache - 缓存已找到的对象地址, 跨帧 / 跨脚本运行复用.

策略:
  - cache key = (pid, ga_base, class_name, sentinel_field)
  - 校验: 重新读 obj+0 == 当前 klass_ptr & sentinel_field deref 仍 == sentinel klass
  - pid 变 (游戏重启) → 自动失效, 触发全堆扫
  - 缓存文件: sao_auto/tools/mem_probe/il2cpp/_cache/instances.json
"""
from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, asdict
from typing import Dict, Optional

from plugins.star_resonance_plugin.mem.il2cpp.static_resolver import StaticResolver


_DEFAULT_CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "_cache", "instances.json")


@dataclass
class CacheEntry:
    obj: int
    sentinel_obj: int
    klass_ptr: int
    sentinel_klass: int
    pid: int
    ga_base: int
    saved_at: float


def _load(path: str) -> Dict[str, dict]:
    if not os.path.isfile(path):
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _save(path: str, data: Dict[str, dict]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)


def _key(class_name: str, sentinel_field: str) -> str:
    return f"{class_name}::{sentinel_field}"


def get_or_find_self(sr: StaticResolver, class_name: str,
                     sentinel_field: str, sentinel_class: str,
                     cache_path: str = _DEFAULT_CACHE,
                     force_rescan: bool = False) -> Optional[tuple]:
    """返回 (obj, sentinel_obj). 缓存命中时跳过 200s 全堆扫.

    校验流程:
      1. 加载 cache, 找匹配 key + pid + ga_base
      2. 重读 obj+0 → 必须等于当前 klass_ptr (实时解析)
      3. 重读 sentinel_field deref → 必须指向有效 sentinel_klass
      4. 任一失败 → 重新 find_self + 写盘
    """
    cache = _load(cache_path)
    key = _key(class_name, sentinel_field)
    cur_klass = sr.resolve_klass(class_name)
    cur_skass = sr.resolve_klass(sentinel_class)
    if cur_klass is None or cur_skass is None:
        return None

    if not force_rescan:
        e = cache.get(key)
        if e and e.get("pid") == sr.pm.pid and e.get("ga_base") == sr.ga \
                and e.get("klass_ptr") == cur_klass:
            obj = e["obj"]
            if sr.pm.read_u64(obj) == cur_klass:
                sf_off = sr.dci.field_offset(class_name, sentinel_field)
                if sf_off is not None:
                    sp = sr.pm.read_u64(obj + sf_off)
                    if sp and sr.pm.read_u64(sp) == cur_skass:
                        return (obj, sp)

    # miss → full scan
    hits = sr.find_self(class_name, sentinel_field, sentinel_class)
    if not hits:
        # remove stale entry so next call doesn't keep validating
        cache.pop(key, None)
        _save(cache_path, cache)
        return None

    obj, sp = hits[0]
    cache[key] = asdict(CacheEntry(
        obj=obj, sentinel_obj=sp,
        klass_ptr=cur_klass, sentinel_klass=cur_skass,
        pid=sr.pm.pid, ga_base=sr.ga,
        saved_at=time.time(),
    ))
    _save(cache_path, cache)
    return (obj, sp)


def clear_cache(cache_path: str = _DEFAULT_CACHE) -> None:
    if os.path.isfile(cache_path):
        os.remove(cache_path)


def validate_cache_entry(sr: StaticResolver, class_name: str,
                         sentinel_field: str, sentinel_class: str,
                         cache_path: str = _DEFAULT_CACHE) -> bool:
    """只做校验, 不扫描. True = 缓存仍有效, False = 应重扫."""
    cache = _load(cache_path)
    e = cache.get(_key(class_name, sentinel_field))
    if not e:
        return False
    cur_klass = sr.resolve_klass(class_name)
    cur_skass = sr.resolve_klass(sentinel_class)
    if cur_klass is None or cur_skass is None:
        return False
    if e.get("pid") != sr.pm.pid or e.get("ga_base") != sr.ga \
            or e.get("klass_ptr") != cur_klass:
        return False
    obj = e["obj"]
    if sr.pm.read_u64(obj) != cur_klass:
        return False
    sf_off = sr.dci.field_offset(class_name, sentinel_field)
    if sf_off is None:
        return False
    sp = sr.pm.read_u64(obj + sf_off)
    if not sp or sr.pm.read_u64(sp) != cur_skass:
        return False
    return True

