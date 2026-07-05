# -*- coding: utf-8 -*-
# Element / damage-property metadata for the ACT element breakdown.
#
# Maps the opaque ``element`` int carried by damage events (proto
# ``EDamageProperty``: 0 GENERAL, 1 FIRE, 2 WATER, 3 ELECTRICITY, 4 WOOD,
# 5 WIND, 6 ROCK, 7 LIGHT, 8 DARK) to ``{name(CN), color(hex), icon(emoji)}``.
#
# Loaded once and RAM-cached (lru_cache). The ``element.json`` asset is
# hand-curated and is intentionally NOT one of hybrid_name_tables._RUNTIME_KINDS,
# so the cross-repo name-table build/regenerate flow never touches it.
# A built-in fallback table keeps the feature working if the asset is missing.

from __future__ import annotations

import os
import json
from functools import lru_cache
from typing import Dict

def _resolve_element_asset() -> str:
    # element.json 路径解析 (onedir 友好)。
    #
    # 冻结 onedir 下 assets/ 被 build_release.bat 提升到 BASE_DIR(exe 顶层),
    # 而本模块 __file__ 落在 runtime/tools/tablekit/ → __file__ 相对的 runtime/assets
    # 已被搬空, 直接读会 FileNotFoundError 回退内置表。优先用 config.resource_path
    # (BASE_DIR 优先, BUNDLE_DIR 回退), 找不到再回退 __file__ 相对路径(dev 树/未冻结)。
    try:
        from config import resource_path  # BASE_DIR-first, BUNDLE_DIR fallback
        cand = resource_path('assets', 'name_tables', 'element.json')
        if os.path.isfile(cand):
            return cand
    except Exception:
        pass
    _root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    return os.path.join(_root, 'assets', 'name_tables', 'element.json')


_SAO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_ASSET = _resolve_element_asset()

# Built-in fallback mirrors the shipped element.json (proto EDamageProperty).
_FALLBACK: Dict[int, Dict[str, str]] = {
    0: {"name": "通用", "color": "#B0B8C4", "icon": "⚔"},
    1: {"name": "火", "color": "#FF6B35", "icon": "🔥"},
    2: {"name": "水", "color": "#2E9BFF", "icon": "💧"},
    3: {"name": "雷", "color": "#B45AFF", "icon": "⚡"},
    4: {"name": "木", "color": "#3FBF5F", "icon": "🌿"},
    5: {"name": "风", "color": "#46E0B0", "icon": "🌪"},
    6: {"name": "岩", "color": "#C8923C", "icon": "🪨"},
    7: {"name": "光", "color": "#FFD95A", "icon": "✨"},
    8: {"name": "暗", "color": "#9B6BD6", "icon": "🌑"},
}

_DEFAULT_COLOR = "#B0B8C4"


@lru_cache(maxsize=1)
def element_table() -> Dict[int, Dict[str, str]]:
    # Return the {element_id: {name, color, icon}} map (asset over fallback).
    table: Dict[int, Dict[str, str]] = {k: dict(v) for k, v in _FALLBACK.items()}
    try:
        with open(_ASSET, 'r', encoding='utf-8') as f:
            raw = json.load(f)
    except FileNotFoundError:
        return table
    except Exception:
        return table
    if isinstance(raw, dict):
        for key, val in raw.items():
            try:
                eid = int(key)
            except (TypeError, ValueError):
                continue  # skips "_comment" and any non-numeric keys
            if isinstance(val, dict) and val.get('name'):
                table[eid] = {
                    'name': str(val.get('name') or ''),
                    'color': str(val.get('color') or _DEFAULT_COLOR),
                    'icon': str(val.get('icon') or ''),
                }
    return table


def element_meta(element_id) -> Dict[str, str]:
    # Return {name, color, icon} for an element id, with a safe default.
    try:
        eid = int(element_id or 0)
    except (TypeError, ValueError):
        eid = 0
    meta = element_table().get(eid)
    if meta is not None:
        return meta
    return {'name': f'属性#{eid}', 'color': _DEFAULT_COLOR, 'icon': ''}


def element_name(element_id) -> str:
    # Return the CN element name for an element id.
    return element_meta(element_id)['name']
