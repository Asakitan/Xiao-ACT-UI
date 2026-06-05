# -*- coding: utf-8 -*-
"""Element / damage-property metadata for the ACT element breakdown.

Maps the opaque ``element`` int carried by damage events (proto
``EDamageProperty``: 0 GENERAL, 1 FIRE, 2 WATER, 3 ELECTRICITY, 4 WOOD,
5 WIND, 6 ROCK, 7 LIGHT, 8 DARK) to ``{name(CN), color(hex), icon(emoji)}``.

Loaded once and RAM-cached (lru_cache). The ``element.json`` asset is
hand-curated and is intentionally NOT one of hybrid_name_tables._RUNTIME_KINDS,
so the cross-repo name-table build/regenerate flow never touches it.
A built-in fallback table keeps the feature working if the asset is missing.
"""

from __future__ import annotations

import os
import json
from functools import lru_cache
from typing import Dict

_SAO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
_ASSET = os.path.join(_SAO_ROOT, 'assets', 'name_tables', 'element.json')

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
    """Return the {element_id: {name, color, icon}} map (asset over fallback)."""
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
    """Return {name, color, icon} for an element id, with a safe default."""
    try:
        eid = int(element_id or 0)
    except (TypeError, ValueError):
        eid = 0
    meta = element_table().get(eid)
    if meta is not None:
        return meta
    return {'name': f'属性#{eid}', 'color': _DEFAULT_COLOR, 'icon': ''}


def element_name(element_id) -> str:
    """Return the CN element name for an element id."""
    return element_meta(element_id)['name']
