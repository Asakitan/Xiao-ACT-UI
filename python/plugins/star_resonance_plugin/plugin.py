# -*- coding: utf-8 -*-
"""Star Resonance (星痕共鸣) game adapter plugin.

Creates all game-specific engines, data sources, and overlay windows
during on_load, installing them onto the platform host via the plugin SDK.
The platform has ZERO imports from this plugin.
"""

from __future__ import annotations

_ctx = None
_engines_started = False

_GAME_SETTING_KEYS = (
    'mem_data_source', 'dps_enabled', 'burst_enabled', 'buffmon_enabled',
    'boss_bar_mode', 'sound_enabled', 'sound_volume', 'hp_ov_enabled',
    'watched_skill_slots', 'mech_banner_enabled', 'tts_enabled', 'tts_volume',
    'directional_dodge_enabled',
)

_GAME_SETTING_DEFAULTS = {
    'mem_data_source': 'tcp',
    'dps_enabled': True,
    'burst_enabled': True,
    'buffmon_enabled': True,
    'boss_bar_mode': 'boss_raid',
    'sound_enabled': True,
    'sound_volume': 70,
    'hp_ov_enabled': True,
    'tts_enabled': True,
    'tts_volume': 80,
}


def _owner_get(key, default=None):
    """Read a game setting from the owner's top-level settings (backward compat)."""
    if _ctx is None:
        return default
    owner = _ctx.engine.owner
    fn = getattr(owner, '_get_setting', None)
    if callable(fn):
        return fn(key, default)
    return default


def _ensure_toplevel_defaults(ctx):
    """Ensure game settings exist at the top level of settings.json.

    Old installs already have these keys at the top level — this just fills in
    any missing ones for fresh installs. The plugin reads/writes these through
    the owner's _get_setting/_set_setting so the platform and plugin always
    see the same values.
    """
    owner = ctx.engine.owner
    cfg = getattr(owner, '_cfg_settings_ref', None)
    if cfg is None:
        return
    changed = False
    for key, default in _GAME_SETTING_DEFAULTS.items():
        sentinel = object()
        if cfg.get(key, sentinel) is sentinel:
            cfg.set(key, default)
            changed = True
    if changed:
        try:
            cfg.save()
        except Exception:
            pass


def _init_game_engines(ctx):
    """Create GameState + PacketBridge + AutoKey + BossRaid + all overlays."""
    global _engines_started
    owner = ctx.engine.owner
    if owner is None or _engines_started:
        return
    _engines_started = True

    cfg = getattr(owner, '_cfg_settings_ref', None)
    if cfg is None:
        ctx.log('[SR] No cfg_settings_ref on owner, skipping engine init')
        return

    # ── GameStateManager ──
    try:
        from plugins.star_resonance_plugin.engines.game_state import GameStateManager
        state_mgr = GameStateManager()
        ctx.engine.set_owner_attr('_state_mgr', state_mgr)
        ctx.engine.set_owner_attr('_game_state', state_mgr)

        _on_update = getattr(owner, '_on_game_state_update', None)
        if callable(_on_update):
            state_mgr.subscribe(_on_update)
        state_mgr.load_cache(cfg)

        cached_name = state_mgr.state.player_name
        if cached_name:
            ctx.engine.set_owner_attr('_username', cached_name)
            disp = cached_name[:9] + '…' if len(cached_name) > 10 else cached_name
            ctx.engine.set_owner_attr('_hp_display_name', disp)
        ctx.log(f'[SR] GameState initialized (cached={cached_name!r})')
    except Exception as e:
        ctx.log(f'[SR] GameState FAILED: {e}')
        state_mgr = None

    # ── Sound settings ──
    try:
        from utils.sao_sound import set_sound_enabled, set_sound_volume
        set_sound_enabled(bool(cfg.get('sound_enabled', True)))
        set_sound_volume(int(cfg.get('sound_volume', 70) or 70))
    except Exception:
        pass

    # ── DPS / Encounter / History / Trigger 引擎 ──
    try:
        from plugins.star_resonance_plugin.engines.dps_tracker import DpsTracker
        from plugins.star_resonance_plugin.engines.dps_history import DpsHistoryStore
        from plugins.star_resonance_plugin.engines.encounter_manager import EncounterManager
        from plugins.star_resonance_plugin.engines.act_trigger_engine import ActTriggerEngine
        dps_history = DpsHistoryStore()
        dps_tracker = DpsTracker()
        encounter_mgr = EncounterManager()
        try:
            _rules = cfg.get('act_trigger_rules', []) or []
        except Exception:
            _rules = []
        trigger_engine = ActTriggerEngine(_rules)
        _finalize_hook = getattr(owner, '_on_dps_report_finalized', None)
        if callable(_finalize_hook):
            dps_tracker.register_finalized_hook(_finalize_hook)
        ctx.engine.set_owner_attr('_dps_history_store', dps_history)
        ctx.engine.set_owner_attr('_dps_tracker', dps_tracker)
        ctx.engine.set_owner_attr('_encounter_mgr', encounter_mgr)
        ctx.engine.set_owner_attr('_act_trigger_engine', trigger_engine)
        ctx.engine.register('dps_tracker', dps_tracker)
        ctx.engine.register('encounter_manager', encounter_mgr)
        ctx.engine.register('trigger_engine', trigger_engine)
        ctx.log('[SR] DPS/Encounter/Trigger engines initialized')
    except Exception as e:
        ctx.log(f'[SR] DPS engines FAILED: {e}')

    # ── PacketBridge (data source) ──
    packet_engine = None
    try:
        from plugins.star_resonance_plugin.net.packet_bridge import PacketBridge
        from act_platform.runtime import ensure_act_event_bus, ensure_act_plugin_manager

        data_mode = str(cfg.get('mem_data_source', 'tcp') or 'tcp').lower()
        packet_engine = PacketBridge(
            state_mgr, cfg,
            on_damage=getattr(owner, '_on_packet_damage', None),
            on_monster_update=getattr(owner, '_on_monster_update', None),
            on_boss_event=getattr(owner, '_on_boss_event', None),
            on_scene_change=getattr(owner, '_on_scene_change', None),
            on_skill_event=getattr(owner, '_on_skill_event', None),
            on_dungeon_event=getattr(owner, '_on_dungeon_event', None),
            data_source=data_mode,
            plugin_manager=ensure_act_plugin_manager(owner, load=False),
            event_bus=ensure_act_event_bus(owner),
        )
        packet_engine.start()
        ctx.engine.set_owner_attr('_packet_engine', packet_engine)
        ctx.engine.register('packet_bridge', packet_engine)

        dps_tracker = getattr(owner, '_dps_tracker', None)
        if dps_tracker:
            try:
                packet_engine.set_dps_tracker(dps_tracker)
            except Exception:
                pass
        ctx.log(f'[SR] PacketBridge started (mode={data_mode})')
    except Exception as e:
        ctx.log(f'[SR] PacketBridge FAILED: {e}')

    # ── Vision/Recognition engine ──
    try:
        from plugins.star_resonance_plugin.vision.recognition import RecognitionEngine
        vision = RecognitionEngine(state_mgr, cfg)
        vision.start()
        ctx.engine.set_owner_attr('_vision_engine', vision)
        ctx.log('[SR] RecognitionEngine started')
    except Exception as e:
        ctx.log(f'[SR] RecognitionEngine FAILED: {e}')

    # Update recognition active flag
    ctx.engine.set_owner_attr('_recognition_active', bool(
        getattr(owner, '_packet_engine', None) or getattr(owner, '_vision_engine', None)))

    # ── AutoKeyEngine ──
    try:
        from plugins.star_resonance_plugin.engines.auto_key_engine import AutoKeyEngine
        ak = AutoKeyEngine(
            state_mgr, cfg,
            extra_gate=lambda: bool(getattr(owner, '_recognition_active', False)),
        )
        ak.start()
        ctx.engine.set_owner_attr('_auto_key_engine', ak)
        ctx.engine.register('auto_key_engine', ak)
        burst_fn = getattr(owner, '_load_autokey_burst_actions', None)
        if callable(burst_fn):
            try:
                ak.set_burst_actions(burst_fn())
            except Exception:
                pass
        ctx.log('[SR] AutoKeyEngine started')
    except Exception as e:
        ctx.log(f'[SR] AutoKeyEngine FAILED: {e}')
        ak = None

    # ── BossRaid + AutoKeyLinkage ──
    try:
        from plugins.star_resonance_plugin.engines.boss_autokey_linkage import BossAutoKeyLinkage
        from plugins.star_resonance_plugin.engines.boss_raid_engine import BossRaidEngine
        from utils.sao_sound import play_sound

        linkage = BossAutoKeyLinkage(
            cfg,
            send_key=getattr(owner, '_send_linked_key', lambda *a, **k: None),
            on_log=lambda msg: ctx.log(msg),
            foreground_gate=ak.is_game_foreground if ak else (lambda: False),
            dodge_active_gate=lambda: bool(
                getattr(owner, '_auto_dodge_director', None)
                and owner._auto_dodge_director.is_active()),
        )
        ctx.engine.set_owner_attr('_boss_autokey_linkage', linkage)

        def _on_boss_alert(title, message):
            alert_ov = getattr(owner, '_alert_overlay', None)
            if alert_ov:
                try:
                    alert_ov.show_alert(title, message)
                except Exception:
                    pass
            try:
                linkage.on_boss_raid_alert(title, message)
            except Exception:
                pass

        br = BossRaidEngine(
            state_mgr, cfg,
            on_alert=_on_boss_alert,
            on_sound=lambda name: play_sound(name),
            on_boss_action=getattr(owner, '_on_boss_action_with_gate', None),
            on_mechanic=getattr(owner, '_on_mechanic_event', None),
        )
        ctx.engine.set_owner_attr('_boss_raid_engine', br)
        ctx.engine.register('boss_raid_engine', br)

        if packet_engine:
            try:
                packet_engine.set_boss_raid_engine(br)
            except Exception:
                pass
        ctx.log('[SR] BossRaid + AutoKeyLinkage started')
    except Exception as e:
        ctx.log(f'[SR] BossRaid FAILED: {e}')

    # ── Overlay windows (Entity/Tk mode only) ──
    root = getattr(owner, 'root', None)
    if root is None:
        return

    _get = lambda k, d=True: bool(getattr(owner, '_get_setting', lambda *a: d)(k, d))

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_dps import DpsOverlay
        ov = DpsOverlay(
            root, cfg,
            request_live_snapshot=getattr(owner, '_request_dps_live_snapshot', None),
            show_last_report=getattr(owner, '_request_dps_last_report', None),
            reset_dps=getattr(owner, '_reset_dps_tracker', None),
            has_last_report=getattr(owner, '_get_dps_last_report_available', None),
            request_entity_detail=getattr(owner, '_request_dps_entity_detail', None),
            list_history=getattr(owner, '_request_dps_history', None),
            export_last_report=getattr(owner, '_export_last_dps_report', None),
            alert=getattr(owner, '_show_entity_alert', None),
        )
        ctx.engine.set_owner_attr('_dps_overlay', ov)
        ctx.engine.set_owner_attr('_dps_enabled', _get('dps_enabled'))
        ctx.engine.set_owner_attr('_dps_visible', False)
        ctx.log('[SR] DPS overlay initialized')
    except Exception as e:
        ctx.log(f'[SR] DPS overlay FAILED: {e}')

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_bosshp import BossHpOverlay
        ctx.engine.set_owner_attr('_boss_hp_overlay', BossHpOverlay(root, cfg))
        ctx.log('[SR] BossHP overlay initialized')
    except Exception as e:
        ctx.log(f'[SR] BossHP overlay FAILED: {e}')

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_hp import HpOverlay
        hp = HpOverlay(root, cfg,
                       on_click=getattr(owner, '_hp_overlay_on_click', None),
                       on_menu=getattr(owner, '_hp_overlay_on_menu', None))
        ctx.engine.set_owner_attr('_hp_overlay', hp)
        if _get('hp_ov_enabled'):
            hp.show()
        ctx.log('[SR] HP overlay initialized')
    except Exception as e:
        ctx.log(f'[SR] HP overlay FAILED: {e}')

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_alert import AlertOverlay
        ctx.engine.set_owner_attr('_alert_overlay', AlertOverlay(root, cfg))
        ctx.log('[SR] Alert overlay initialized')
    except Exception as e:
        ctx.log(f'[SR] Alert overlay FAILED: {e}')

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_map_banner import MapBannerOverlay
        ctx.engine.set_owner_attr('_map_banner_overlay', MapBannerOverlay(root, cfg))
    except Exception:
        pass

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_mech_banner import MechBannerOverlay
        from plugins.star_resonance_plugin.engines.mechanic_alert_controller import MechanicAlertController
        from utils import sao_tts
        mb = MechBannerOverlay(root, cfg)
        ctx.engine.set_owner_attr('_mech_banner_overlay', mb)
        mac = MechanicAlertController(
            on_banner=mb.show_mechanic,
            banner_enabled_fn=lambda: _get('mech_banner_enabled'),
        )
        ctx.engine.set_owner_attr('_mech_alert_controller', mac)
        sao_tts.set_tts_enabled(bool(cfg.get('tts_enabled', True)))
        sao_tts.set_tts_volume(int(cfg.get('tts_volume', 80) or 80))
    except Exception:
        pass

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_skillfx import BurstReadyOverlay
        ctx.engine.set_owner_attr('_skillfx_overlay', BurstReadyOverlay(root, cfg))
    except Exception:
        pass

    try:
        from plugins.star_resonance_plugin.panels.sao_gui_buffmon import SelfBuffOverlay, BossBuffOverlay
        bm_on = _get('buffmon_enabled')
        sb = SelfBuffOverlay(root, cfg, hp_overlay=getattr(owner, '_hp_overlay', None))
        sb.set_enabled(bm_on)
        ctx.engine.set_owner_attr('_self_buff_overlay', sb)
        bb = BossBuffOverlay(root, cfg, boss_hp_overlay=getattr(owner, '_boss_hp_overlay', None))
        bb.set_enabled(bm_on)
        ctx.engine.set_owner_attr('_boss_buff_overlay', bb)
    except Exception:
        pass

    ctx.log('[SR] All overlays initialized')


# ── Menu category builders ──

def _build_auto_items():
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    ak = getattr(owner, '_auto_key_engine', None)
    ak_on = bool(ak and getattr(ak, 'running', False))
    return [
        {'icon': '⚡', 'label': f'AutoKey: {"ON" if ak_on else "OFF"}',
         'command': getattr(owner, '_toggle_auto_script', lambda: None)},
        {'icon': '◆', 'label': 'AutoKey Quick Panel',
         'command': getattr(owner, '_toggle_autokey_panel', lambda: None)},
        {'icon': '◇', 'label': 'AutoKey Detail Editor',
         'command': getattr(owner, '_toggle_autokey_detail_panel', lambda: None)},
    ]


def _build_boss_items():
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    br = getattr(owner, '_boss_raid_engine', None)
    br_on = bool(br and getattr(br, 'running', False))
    return [
        {'icon': '⚔', 'label': f'BossRaid: {"ON" if br_on else "OFF"}',
         'command': getattr(owner, '_toggle_boss_raid', lambda: None)},
        {'icon': '▸', 'label': '下一阶段',
         'command': getattr(owner, '_boss_raid_next_phase', lambda: None)},
        {'icon': '◆', 'label': 'BossRaid Quick Panel',
         'command': getattr(owner, '_toggle_bossraid_panel', lambda: None)},
        {'icon': '◇', 'label': 'BossRaid Detail Editor',
         'command': getattr(owner, '_toggle_bossraid_detail_panel', lambda: None)},
    ]


def _build_burst_items():
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    burst_on = bool(_owner_get('burst_enabled', True))
    return [
        {'icon': '◆', 'label': f'爆发提示: {"ON" if burst_on else "OFF"}',
         'command': getattr(owner, '_toggle_burst_enabled', lambda: None)},
    ]


def _build_panel_items():
    owner = _ctx.engine.owner if _ctx else None
    if owner is None:
        return []
    dps_on = bool(_owner_get('dps_enabled', True))
    return [
        {'icon': '◆', 'label': f'DPS面板: {"ON" if dps_on else "OFF"}',
         'command': getattr(owner, '_toggle_dps_enabled', lambda: None)},
        {'icon': '◈', 'label': 'Commander',
         'command': getattr(owner, '_toggle_commander_panel', lambda: None)},
    ]


def _inject_game_constants():
    """Inject game-specific modules and constants into platform at load time.

    Registers plugin-owned mem_access / unified_source into sys.modules under
    the ``mem_probe.*`` namespace so platform code that does
    ``from mem_probe.mem_access import MemAccess`` still works after the files
    moved from platform to plugin.
    """
    import sys
    from plugins.star_resonance_plugin.sr_config import (
        GAME_PROCESS_NAMES, GAME_WINDOW_KEYWORDS, GAME_MAIN_MODULE,
    )
    import config
    config.GAME_PROCESS_NAMES = GAME_PROCESS_NAMES
    config.GAME_WINDOW_KEYWORDS = GAME_WINDOW_KEYWORDS

    # ── Register game modules into mem_probe namespace ──
    try:
        import mem_probe
        from plugins.star_resonance_plugin.mem import mem_access as _ma
        from plugins.star_resonance_plugin.mem import unified_source as _us
        sys.modules['mem_probe.mem_access'] = _ma
        sys.modules['mem_probe.unified_source'] = _us
        mem_probe.mem_access = _ma
        mem_probe.unified_source = _us
    except Exception:
        pass

    try:
        from mem_probe.process import set_game_process_names
        set_game_process_names(GAME_PROCESS_NAMES)
    except Exception:
        pass

    # ── Inject game-specific factories into the now-registered mem_access ──
    try:
        from plugins.star_resonance_plugin.mem.mem_access import (
            set_game_main_module, set_ecr_factory,
        )
        set_game_main_module(GAME_MAIN_MODULE)
        from plugins.star_resonance_plugin.mem.il2cpp.mem_entity_combat import EntityCombatReader
        set_ecr_factory(EntityCombatReader)
    except Exception:
        pass

    # ── Inject bridge classes into unified_source ──
    try:
        from plugins.star_resonance_plugin.mem.unified_source import (
            set_bridge_classes, set_root_pointer_cache_fn,
        )
        from plugins.star_resonance_plugin.mem.il2cpp.mem_state_bridge import MemStateBridge
        from plugins.star_resonance_plugin.mem.il2cpp.mem_self_state_provider import MemSelfStateProvider
        set_bridge_classes(MemStateBridge, MemSelfStateProvider)

        def _rpc_health(bridge):
            src = getattr(getattr(bridge, "_entity_provider", None), "_src", None) \
                or getattr(getattr(bridge, "_provider", None), "_src", None)
            if src is None:
                return {"game_key": "", "entries": 0, "names": []}
            game_key = str(getattr(src, "game_key", "") or "")
            if not game_key:
                sr = getattr(src, "sr", None)
                meta = getattr(sr, "bundle_meta", None) if sr is not None else None
                if isinstance(meta, dict):
                    game_key = str(meta.get("ga_sha256_first_1mb") or "")
            if not game_key:
                return {"game_key": "", "entries": 0, "names": []}
            from plugins.star_resonance_plugin.mem.il2cpp import root_pointer_cache as _rpc
            return _rpc.coverage(game_key)

        set_root_pointer_cache_fn(_rpc_health)
    except Exception:
        pass


def on_load(ctx):
    """Plugin entry point — register menu categories and initialize game engines."""
    global _ctx
    _ctx = ctx

    _inject_game_constants()

    # Bootstrap runtime deps from plugin's own libs/vendor/requirements.txt
    ctx.ensure_requirements(install=True)

    _ensure_toplevel_defaults(ctx)

    from plugins.star_resonance_plugin.webview_bridge import install_webview_bridge
    install_webview_bridge(ctx)

    ctx.register_menu_category('自动', '⚡', _build_auto_items, priority=10)
    ctx.register_menu_category('Boss', '⚔', _build_boss_items, priority=20)
    ctx.register_menu_category('Burst', 'B', _build_burst_items, priority=30)
    ctx.register_menu_category('面板', '◈', _build_panel_items, priority=40)

    _init_game_engines(ctx)

    ctx.log('[SR] Star Resonance plugin loaded')


def on_enable():
    if _ctx is None:
        return
    _ctx.log('[SR] Star Resonance plugin enabled')


def on_disable():
    if _ctx is None:
        return
    _ctx.log('[SR] Star Resonance plugin disabled')


def on_unload():
    global _ctx, _engines_started
    if _ctx is not None:
        _ctx.log('[SR] Star Resonance plugin unloading')
    _ctx = None
    _engines_started = False
