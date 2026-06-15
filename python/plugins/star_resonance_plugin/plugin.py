# -*- coding: utf-8 -*-
"""Star Resonance (星痕共鸣) game adapter plugin.

Phase 4 skeleton: registers menu categories + declares data sources.
Actual game code migration happens in Phase 5A-E.
"""

from __future__ import annotations

_ctx = None


def _build_auto_items():
    """「自动」menu category items (AutoKey control)."""
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    items = []
    try:
        ak_engine = _ctx.get_engine('auto_key_engine')
        ak_on = bool(ak_engine and getattr(ak_engine, 'running', False))
    except Exception:
        ak_on = False
    items.append({
        'icon': '⚡',
        'label': f'AutoKey: {"ON" if ak_on else "OFF"}',
        'command': getattr(owner, '_toggle_auto_script', lambda: None),
    })
    items.append({
        'icon': '◆',
        'label': 'AutoKey Quick Panel',
        'command': getattr(owner, '_toggle_autokey_panel', lambda: None),
    })
    items.append({
        'icon': '◇',
        'label': 'AutoKey Detail Editor',
        'command': getattr(owner, '_toggle_autokey_detail_panel', lambda: None),
    })
    return items


def _build_boss_items():
    """「Boss」menu category items (BossRaid control)."""
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    items = []
    try:
        br_engine = _ctx.get_engine('boss_raid_engine')
        br_on = bool(br_engine and getattr(br_engine, 'running', False))
    except Exception:
        br_on = False
    items.append({
        'icon': '⚔',
        'label': f'BossRaid: {"ON" if br_on else "OFF"}',
        'command': getattr(owner, '_toggle_boss_raid', lambda: None),
    })
    items.append({
        'icon': '▸',
        'label': '下一阶段',
        'command': getattr(owner, '_boss_raid_next_phase', lambda: None),
    })
    items.append({
        'icon': '◆',
        'label': 'BossRaid Quick Panel',
        'command': getattr(owner, '_toggle_bossraid_panel', lambda: None),
    })
    items.append({
        'icon': '◇',
        'label': 'BossRaid Detail Editor',
        'command': getattr(owner, '_toggle_bossraid_detail_panel', lambda: None),
    })
    return items


def _build_burst_items():
    """「Burst」menu category items (skill readiness)."""
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    burst_on = bool(_ctx.setting('burst_enabled') if _ctx else True)
    items = [
        {
            'icon': '◆',
            'label': f'爆发提示: {"ON" if burst_on else "OFF"}',
            'command': getattr(owner, '_toggle_burst_enabled', lambda: None),
        },
    ]
    return items


def _build_panel_items():
    """「面板」menu category items (game-specific panels)."""
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    dps_on = bool(_ctx.setting('dps_enabled') if _ctx else True)
    items = [
        {
            'icon': '◆',
            'label': f'DPS面板: {"ON" if dps_on else "OFF"}',
            'command': getattr(owner, '_toggle_dps_enabled', lambda: None),
        },
        {
            'icon': '◈',
            'label': 'Commander',
            'command': getattr(owner, '_toggle_commander_panel', lambda: None),
        },
    ]
    return items


def on_load(ctx):
    """Plugin entry point — register menu categories and data sources."""
    global _ctx
    _ctx = ctx

    ctx.log('Star Resonance plugin loading (Phase 4 skeleton)')

    ctx.set_defaults({
        'mem_data_source': 'tcp',
        'dps_enabled': True,
        'burst_enabled': True,
        'buffmon_enabled': True,
        'boss_bar_mode': 'boss_raid',
        'sound_enabled': True,
    })


def on_enable():
    if _ctx is None:
        return
    _ctx.log('Star Resonance plugin enabled')


def on_disable():
    if _ctx is None:
        return
    _ctx.log('Star Resonance plugin disabled')


def on_unload():
    global _ctx
    if _ctx is not None:
        _ctx.log('Star Resonance plugin unloading')
    _ctx = None
