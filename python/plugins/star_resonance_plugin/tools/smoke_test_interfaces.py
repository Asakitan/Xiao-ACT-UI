# -*- coding: utf-8 -*-
# SAO-UI 接口冒烟测试
# Smoke test for every importable module + key public surfaces.
#
# Strategy:
# Phase A — Import every .py module (platform + Star Resonance plugin).
# Phase B — Verify Cython .pyd modules expose their documented API.
# Phase C — Smoke-call a representative entry point of every key subsystem
# with safe dummy inputs (no network, no Tk, no game window).
# Phase D — Iterate all SAOPlayerGUI mixins, confirm class + method count.
#
# Updated for the 5.0.0 platform/plugin split:
# * Platform tree:   sao_gui / sao_webview / gui_modules / render / act_platform / ...
# * Plugin tree:     plugins/star_resonance_plugin/{engines,net,vision,protocol,
# panels,render,cython,...}
# * Cython binaries: platform-side ``_sao_cy_uihelpers``/``_sao_cy_memscan``
# live next to ``main.py``; plugin-side ``_sao_cy_packet/
# _sao_cy_combat/_sao_cy_pixels/_sao_cy_skillfx/
# _sao_cy_sr_uihelpers`` live under
# ``plugins/star_resonance_plugin/cython/``.
#
# Exit code 0 if every probe passes, 1 otherwise.

from __future__ import annotations

import importlib
import os
import sys
import time
import traceback
from pathlib import Path
from typing import List, Tuple

# Add repo root to sys.path so we can import platform packages.
HERE = Path(__file__).resolve().parent.parent
PLATFORM_ROOT = HERE.parent.parent  # python/
if str(PLATFORM_ROOT) not in sys.path:
    sys.path.insert(0, str(PLATFORM_ROOT))

# Add Star Resonance Cython binary dir + protocol root so the bare-name
# imports (``import _sao_cy_packet``, etc.) resolve without going through
# plugin.on_load().
_PLUGIN_ROOT = PLATFORM_ROOT / "plugins" / "star_resonance_plugin"
for _sub in ("cython", "protocol"):
    _path = str(_PLUGIN_ROOT / _sub)
    if os.path.isdir(_path) and _path not in sys.path:
        sys.path.insert(0, _path)

OK = 0
FAIL = 0
SKIP = 0
RESULTS: List[Tuple[str, str, str]] = []  # (status, name, detail)


def record(status: str, name: str, detail: str = "") -> None:
    global OK, FAIL, SKIP
    if status == "OK":
        OK += 1
    elif status == "FAIL":
        FAIL += 1
    elif status == "SKIP":
        SKIP += 1
    RESULTS.append((status, name, detail))


def section(title: str) -> None:
    print(f"\n{'=' * 70}\n{title}\n{'=' * 70}")


def probe(name: str, fn, *args, **kwargs) -> None:
    # Run fn(*args, **kwargs); record OK/FAIL with traceback head.
    try:
        fn(*args, **kwargs)
        print(f"  OK    {name}")
        record("OK", name)
    except Exception as e:
        head = traceback.format_exc().strip().splitlines()[-1]
        print(f"  FAIL  {name}  -> {head}")
        record("FAIL", name, head)


# ---------------------------------------------------------------------------
# Module lists
# ---------------------------------------------------------------------------

# Platform-only modules (no game adapter coupling).
PLATFORM_TOPLEVEL_MODULES = [
    "config",
    "main",
    "sao_gui",
    "sao_webview",
    "sao_web_panel_common",
    "update_apply",
    "render.gpu_capture", "render.gpu_compositor", "render.gpu_overlay_window",
    "render.gpu_renderer", "render.overlay_render_worker",
    "render.overlay_scheduler", "render.overlay_subpixel",
    "render.render_capture_sync",
    "sao_theme",
    "act_platform",
    "ui_gpu",
    "updater.sao_updater",
    "utils.perf_probe", "utils.sao_sound", "utils.window_effects",
    "utils.window_locator",
]

# Star Resonance plugin modules — all game-specific code lives here.
SR_PLUGIN_MODULES = [
    # plugin entry + bridges
    "plugins.star_resonance_plugin.plugin",
    "plugins.star_resonance_plugin.webview_bridge",
    "plugins.star_resonance_plugin.entity_menu_bridge",
    # act_replay harness (moved into plugin tree in 5.0.0)
    "plugins.star_resonance_plugin.act_replay.harness",
    # engines
    "plugins.star_resonance_plugin.engines.auto_key_engine",
    "plugins.star_resonance_plugin.engines.automation",
    "plugins.star_resonance_plugin.engines.boss_autokey_linkage",
    "plugins.star_resonance_plugin.engines.boss_raid_engine",
    "plugins.star_resonance_plugin.engines.character_profile",
    "plugins.star_resonance_plugin.engines.combat_analytics",
    "plugins.star_resonance_plugin.engines.dps_history",
    "plugins.star_resonance_plugin.engines.dps_tracker",
    "plugins.star_resonance_plugin.engines.encounter_manager",
    "plugins.star_resonance_plugin.engines.game_state",
    # net + protocol
    "plugins.star_resonance_plugin.net.packet_bridge",
    "plugins.star_resonance_plugin.net.packet_capture",
    "plugins.star_resonance_plugin.protocol.packet_parser",
    # vision
    "plugins.star_resonance_plugin.vision.recognition",
    "plugins.star_resonance_plugin.vision.skill_recognition",
    # render (game-specific GPU pipeline)
    "plugins.star_resonance_plugin.render.skillfx_jit",
    "plugins.star_resonance_plugin.render.skillfx_pipeline",
]

# Platform GUI helper modules (generic widgets / mixins) + plugin panels.
GUI_MODULES = [
    # generic platform widgets (no game logic)
    "gui_modules.sao_child_bar_gpu",
    "gui_modules.sao_gui_dialogs_mixin",
    "gui_modules.sao_gui_fisheye_mixin",
    "gui_modules.sao_gui_float_handlers_mixin",
    "gui_modules.sao_gui_float_chrome_mixin",
    "gui_modules.sao_gui_lifecycle_mixin",
    "gui_modules.sao_gui_link_animation_mixin",
    "gui_modules.sao_gui_menu_hud",
    "gui_modules.sao_gui_menu_mixin",
    "gui_modules.sao_gui_misc_mixin",
    "gui_modules.sao_gui_panel_fx_mixin",
    "gui_modules.sao_gui_panels_mixin",
    "gui_modules.sao_gui_status_updater_mixin",
    "gui_modules.sao_hotkey_manager",
    "gui_modules.sao_left_info_gpu",
    "gui_modules.sao_menu_bar_gpu",
    "gui_modules.sao_menu_hud",
    "gui_modules.sao_panel_ui",
    "gui_modules.settings_manager",
    # game-specific panels (plugin-side)
    "plugins.star_resonance_plugin.panels.sao_gui_actions_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_act_aggregate",
    "plugins.star_resonance_plugin.panels.sao_gui_action_log",
    "plugins.star_resonance_plugin.panels.sao_gui_alert",
    "plugins.star_resonance_plugin.panels.sao_gui_autokey",
    "plugins.star_resonance_plugin.panels.sao_gui_bosshp",
    "plugins.star_resonance_plugin.panels.sao_gui_bossraid",
    "plugins.star_resonance_plugin.panels.sao_gui_buffmon",
    "plugins.star_resonance_plugin.panels.sao_gui_commander",
    "plugins.star_resonance_plugin.panels.sao_gui_damage_events_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_data_source_health",
    "plugins.star_resonance_plugin.panels.sao_gui_dps",
    "plugins.star_resonance_plugin.panels.sao_gui_dps_theme_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_engine_lifecycle_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_engine_toggles_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_graph_timeseries",
    "plugins.star_resonance_plugin.panels.sao_gui_hp",
    "plugins.star_resonance_plugin.panels.sao_gui_offline_import",
    "plugins.star_resonance_plugin.panels.sao_gui_packet_callbacks_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_profile_editors",
    "plugins.star_resonance_plugin.panels.sao_gui_report_export",
    "plugins.star_resonance_plugin.panels.sao_gui_skill_drilldown",
    "plugins.star_resonance_plugin.panels.sao_gui_skillfx",
    "plugins.star_resonance_plugin.panels.sao_gui_state_mixin",
    "plugins.star_resonance_plugin.panels.sao_gui_timeline_vcr",
    "plugins.star_resonance_plugin.panels.sao_menu_left_stack",
    "plugins.star_resonance_plugin.panels.sao_player_panel",
    "plugins.star_resonance_plugin.panels.sao_session_players_panel",
]

CYTHON_MODULES = [
    "_sao_cy_packet", "_sao_cy_combat", "_sao_cy_pixels",
    "_sao_cy_skillfx", "_sao_cy_sr_uihelpers", "_sao_cy_uihelpers",
]

# Mixin module/class pairs — all game-state mixins now live in the plugin.
MIXIN_NAMES = [
    ("plugins.star_resonance_plugin.panels.sao_gui_state_mixin", "SAOPlayerGUIStateMixin"),
    ("gui_modules.sao_gui_menu_mixin", "SAOPlayerGUIMenuMixin"),
    ("gui_modules.sao_gui_fisheye_mixin", "SAOPlayerGUIFisheyeMixin"),
    ("plugins.star_resonance_plugin.panels.sao_gui_actions_mixin", "SAOPlayerGUIActionsMixin"),
    ("plugins.star_resonance_plugin.panels.sao_gui_engine_toggles_mixin", "SAOPlayerGUIEngineTogglesMixin"),
    ("plugins.star_resonance_plugin.panels.sao_gui_dps_theme_mixin", "SAOPlayerGUIDpsThemeMixin"),
    ("gui_modules.sao_gui_panels_mixin", "SAOPlayerGUIPanelsMixin"),
    ("gui_modules.sao_gui_status_updater_mixin", "SAOPlayerGUIStatusUpdaterMixin"),
    ("gui_modules.sao_gui_dialogs_mixin", "SAOPlayerGUIDialogsMixin"),
    ("plugins.star_resonance_plugin.panels.sao_gui_engine_lifecycle_mixin", "SAOPlayerGUIEngineLifecycleMixin"),
    ("plugins.star_resonance_plugin.panels.sao_gui_packet_callbacks_mixin", "SAOPlayerGUIPacketCallbacksMixin"),
    ("gui_modules.sao_gui_float_chrome_mixin", "SAOPlayerGUIFloatChromeMixin"),
    ("gui_modules.sao_gui_float_handlers_mixin", "SAOPlayerGUIFloatHandlersMixin"),
    ("gui_modules.sao_gui_lifecycle_mixin", "SAOPlayerGUILifecycleMixin"),
    ("gui_modules.sao_gui_panel_fx_mixin", "SAOPlayerGUIPanelFxMixin"),
    ("gui_modules.sao_gui_link_animation_mixin", "SAOPlayerGUILinkAnimationMixin"),
    ("plugins.star_resonance_plugin.panels.sao_gui_damage_events_mixin", "SAOPlayerGUIDamageEventsMixin"),
    ("gui_modules.sao_gui_misc_mixin", "SAOPlayerGUIMiscMixin"),
]

CYTHON_API = {
    "_sao_cy_packet": {
        "burst_ready", "compute_damage_key", "decode_fields",
        "decode_int32_from_raw", "decode_string_from_raw",
    },
    "_sao_cy_combat": {
        "attacker_is_self", "build_entity_snapshot", "is_monster_uuid",
        "is_player_uuid", "uuid_to_uid",
    },
    "_sao_cy_pixels": {
        "bgr_color_match_column_ratio", "gradient_edge_pct",
        "hsv_columns_and_hue_mask", "premultiply_bgra_ndarray",
    },
    "_sao_cy_skillfx": {"beam_rgba", "ring_layer_rgba", "ring_sweep_rgba"},
    "_sao_cy_uihelpers": {
        "panel_float_offsets", "popup_origins", "lerp_hex_color",
    },
    "_sao_cy_sr_uihelpers": {
        "compute_skillfx_layout", "format_level_text", "ease_out_cubic",
        "lerp_clamped", "pick_burst_trigger_slot",
    },
}


def import_module(name: str) -> None:
    importlib.import_module(name)


def check_cython_api(name: str, expected: set) -> None:
    m = importlib.import_module(name)
    missing = expected - set(dir(m))
    if missing:
        raise AssertionError(f"missing exports: {missing}")


def check_mixin(module: str, klass: str) -> None:
    m = importlib.import_module(module)
    cls = getattr(m, klass, None)
    if cls is None:
        raise AssertionError(f"class {klass} not found")
    methods = [a for a in dir(cls) if not a.startswith("__") and callable(getattr(cls, a, None))]
    if not methods:
        raise AssertionError(f"class {klass} has no methods")


# ---------------------------------------------------------------------------
# Phase A — import every module
# ---------------------------------------------------------------------------
section("Phase A — import platform top-level modules")
for mod in PLATFORM_TOPLEVEL_MODULES:
    probe(mod, import_module, mod)

section("Phase A — import Star Resonance plugin modules")
for mod in SR_PLUGIN_MODULES:
    probe(mod, import_module, mod)

section("Phase A — import gui_modules/ + plugin panels")
for mod in GUI_MODULES:
    probe(mod, import_module, mod)

# ---------------------------------------------------------------------------
# Phase B — Cython API surface
# ---------------------------------------------------------------------------
section("Phase B — Cython .pyd modules public API")
for mod, api in CYTHON_API.items():
    probe(f"{mod} (exports: {len(api)})", check_cython_api, mod, api)

# ---------------------------------------------------------------------------
# Phase C — Smoke-call key entry points with safe dummy inputs
# ---------------------------------------------------------------------------
section("Phase C — Smoke-call key entry points")


def smoke_config():
    import config
    import settings_crypto
    from plugins.star_resonance_plugin import sr_config as srcfg
    import os
    import tempfile
    assert isinstance(config.APP_VERSION, str)
    assert config.APP_VERSION_LABEL.startswith("v")
    rects = srcfg.get_skill_slot_rects((0, 0, 1920, 1080))
    assert isinstance(rects, list)
    with tempfile.TemporaryDirectory() as root:
        path = os.path.join(root, "nested", "settings.json")
        sm = config.SettingsManager(path)
        sm.set("probe", "ok")
        sm.save()
        with open(path, "rb") as handle:
            decoded = settings_crypto.decode_settings(handle.read())
        assert decoded.get("probe") == "ok"


def smoke_uihelpers_layout():
    import _sao_cy_sr_uihelpers as h
    out = h.compute_skillfx_layout((0, 0, 1920, 1080), [], [])
    # may be None if no slots; just ensure no exception
    _ = out


def smoke_uihelpers_format():
    import _sao_cy_sr_uihelpers as h
    s = h.format_level_text(60, 83, 1)
    assert isinstance(s, str)


def smoke_uihelpers_ease():
    import _sao_cy_sr_uihelpers as h
    assert 0.0 <= h.ease_out_cubic(0.5) <= 1.0
    assert h.lerp_clamped(0.0, 100.0, 0.5) == 50.0


def smoke_combat_uuid():
    import _sao_cy_combat as c
    uid = c.uuid_to_uid(36668136)
    assert isinstance(uid, int)


def smoke_combat_classify():
    import _sao_cy_combat as c
    emit, tier = c.classify_big_hit_tier(500000, 100000, 300000, 1000000)
    assert isinstance(emit, (bool, int))
    assert isinstance(tier, str)


def smoke_packet_decode_int():
    import _sao_cy_packet as p
    val = p.decode_int32_from_raw(b"\x01\x00\x00\x00")
    assert val == 1


def smoke_packet_burst_ready():
    import _sao_cy_packet as p
    out = p.burst_ready([], [])
    assert isinstance(out, (bool, int))


def smoke_pixels_match():
    import _sao_cy_pixels as px
    import numpy as np
    img = np.zeros((1, 100, 3), dtype=np.uint8)
    img[..., 2] = 255  # red column in BGR
    col_fill, overall = px.bgr_color_match_column_ratio(img, 0, 0, 255, 30.0)
    assert 0.0 <= overall <= 1.0


def smoke_pixels_premultiply():
    import _sao_cy_pixels as px
    import numpy as np
    arr = np.zeros((4, 4, 4), dtype=np.uint8)
    arr[..., 3] = 128
    arr[..., 0] = 200
    out = px.premultiply_bgra_ndarray(arr)
    # cpdef returns bytes per .pyx signature
    assert isinstance(out, (bytes, bytearray))
    assert len(out) == arr.size


def smoke_skillfx_beam():
    import _sao_cy_skillfx as s
    out = s.beam_rgba(40, 80)
    assert isinstance(out, (bytes, bytearray))
    assert len(out) == 40 * 80 * 4


def smoke_game_state():
    from plugins.star_resonance_plugin.engines import game_state
    gs = game_state.GameState()
    gs.window_rect = (0, 0, 1920, 1080)
    assert gs.window_rect == (0, 0, 1920, 1080)


def smoke_packet_parser():
    from plugins.star_resonance_plugin.protocol import packet_parser
    assert hasattr(packet_parser, "PacketParser") or hasattr(packet_parser, "parse_packet")


def smoke_dps_tracker():
    from plugins.star_resonance_plugin.engines import dps_tracker
    cls = getattr(dps_tracker, "DpsTracker", None)
    assert cls is not None


def smoke_window_locator():
    from utils import window_locator
    loc = window_locator.WindowLocator()
    rect = loc.get_rect()
    _ = rect


def smoke_sao_theme():
    import sao_theme
    assert callable(getattr(sao_theme, "ease_out", None))
    assert callable(getattr(sao_theme, "ease_in_out", None))
    assert callable(getattr(sao_theme, "lerp", None))
    assert sao_theme.lerp(0.0, 10.0, 0.5) == 5.0
    assert 0.0 <= sao_theme.ease_out(0.5) <= 1.0


def smoke_sao_sound():
    from utils import sao_sound
    # play_sound exists (don't actually play)
    assert callable(getattr(sao_sound, "play_sound", None))
    assert callable(getattr(sao_sound, "get_sao_font", None))


def smoke_settings_manager():
    from gui_modules import settings_manager
    cls = getattr(settings_manager, "Settings", None) or getattr(settings_manager, "SettingsManager", None)
    assert cls is not None


def smoke_skill_recognition():
    from plugins.star_resonance_plugin.vision import skill_recognition
    assert hasattr(skill_recognition, "SkillVisualTracker")
    assert callable(skill_recognition.get_skill_slot_rects)
    assert callable(skill_recognition.get_skill_slot_client_rects)


def smoke_recognition():
    from plugins.star_resonance_plugin.vision import recognition
    assert any(hasattr(recognition, n) for n in
               ("RecognitionEngine", "Recognition", "Vision"))


def smoke_panel_ui_icon():
    from gui_modules import sao_panel_ui
    assert callable(getattr(sao_panel_ui, "_apply_window_icon", None))
    assert callable(getattr(sao_panel_ui, "_set_process_app_id", None))


probe("config.APP_VERSION + get_skill_slot_rects", smoke_config)
probe("_sao_cy_sr_uihelpers.compute_skillfx_layout", smoke_uihelpers_layout)
probe("_sao_cy_sr_uihelpers.format_level_text", smoke_uihelpers_format)
probe("_sao_cy_sr_uihelpers ease / lerp", smoke_uihelpers_ease)
probe("_sao_cy_combat.uuid_to_uid", smoke_combat_uuid)
probe("_sao_cy_combat.classify_big_hit_tier", smoke_combat_classify)
probe("_sao_cy_packet.decode_int32_from_raw", smoke_packet_decode_int)
probe("_sao_cy_packet.burst_ready", smoke_packet_burst_ready)
probe("_sao_cy_pixels.bgr_color_match_column_ratio", smoke_pixels_match)
probe("_sao_cy_pixels.premultiply_bgra_ndarray", smoke_pixels_premultiply)
probe("_sao_cy_skillfx.beam_rgba", smoke_skillfx_beam)
probe("game_state.GameState", smoke_game_state)
probe("packet_parser surface", smoke_packet_parser)
probe("dps_tracker DPSTracker", smoke_dps_tracker)
probe("window_locator.WindowLocator", smoke_window_locator)
probe("sao_theme ease/lerp", smoke_sao_theme)
probe("sao_sound play_sound + get_sao_font", smoke_sao_sound)
probe("settings_manager Settings class", smoke_settings_manager)
probe("skill_recognition surface", smoke_skill_recognition)
probe("recognition engine surface", smoke_recognition)
probe("sao_panel_ui icon helpers", smoke_panel_ui_icon)

# ---------------------------------------------------------------------------
# Phase D — All 19 mixins
# ---------------------------------------------------------------------------
section("Phase D — SAOPlayerGUI mixins")
for module, klass in MIXIN_NAMES:
    probe(f"{klass}", check_mixin, module, klass)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
section("Summary")
print(f"  OK   : {OK}")
print(f"  FAIL : {FAIL}")
print(f"  SKIP : {SKIP}")
print(f"  Total: {OK + FAIL + SKIP}")

if FAIL:
    print("\nFailures:")
    for status, name, detail in RESULTS:
        if status == "FAIL":
            print(f"  - {name}: {detail}")
    sys.exit(1)
sys.exit(0)
