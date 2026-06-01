# -*- coding: utf-8 -*-
"""
SAO-UI 接口冒烟测试
Smoke test for every importable module + key public surfaces.

Strategy:
  Phase A — Import every .py module (top-level + gui_modules/).
  Phase B — Verify Cython .pyd modules expose their documented API.
  Phase C — Smoke-call a representative entry point of every key subsystem
            with safe dummy inputs (no network, no Tk, no game window).
  Phase D — Iterate all 19 SAOPlayerGUI mixins, confirm class + method count.

Exit code 0 if every probe passes, 1 otherwise.
"""

from __future__ import annotations

import importlib
import sys
import time
import traceback
from pathlib import Path
from typing import List, Tuple

# Add repo root to sys.path
HERE = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(HERE))

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
    """Run fn(*args, **kwargs); record OK/FAIL with traceback head."""
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
TOPLEVEL_MODULES = [
    "auto_key_engine", "automation", "boss_autokey_linkage", "boss_raid_engine",
    "character_profile", "config", "dps_tracker", "game_state",
    "render.gpu_capture", "render.gpu_compositor", "render.gpu_overlay_window", "render.gpu_renderer",
    "hide_seek_engine", "main", "render.overlay_render_worker", "render.overlay_scheduler",
    "render.overlay_subpixel", "packet_bridge", "packet_capture", "packet_parser",
    "utils.perf_probe", "vision.recognition", "render.render_capture_sync", "sao_gui", "utils.sao_sound",
    "sao_theme", "sao_updater", "sao_web_panel_common", "sao_webview",
    "vision.skill_recognition", "render.skillfx_jit", "render.skillfx_pipeline", "update_apply",
    "vision.vision_accel", "utils.window_effects", "utils.window_locator",
]

GUI_MODULES = [
    "gui_modules.sao_child_bar_gpu", "gui_modules.sao_gui_actions_mixin",
    "gui_modules.sao_gui_alert", "gui_modules.sao_gui_autokey",
    "gui_modules.sao_gui_bosshp", "gui_modules.sao_gui_bossraid",
    "gui_modules.sao_gui_buffmon", "gui_modules.sao_gui_commander",
    "gui_modules.sao_gui_damage_events_mixin", "gui_modules.sao_gui_dialogs_mixin",
    "gui_modules.sao_gui_dps", "gui_modules.sao_gui_dps_theme_mixin",
    "gui_modules.sao_gui_engine_lifecycle_mixin", "gui_modules.sao_gui_engine_toggles_mixin",
    "gui_modules.sao_gui_fisheye_mixin", "gui_modules.sao_gui_float_handlers_mixin",
    "gui_modules.sao_gui_float_hp_mixin", "gui_modules.sao_gui_hp",
    "gui_modules.sao_gui_lifecycle_mixin", "gui_modules.sao_gui_link_animation_mixin",
    "gui_modules.sao_gui_menu_hud", "gui_modules.sao_gui_menu_mixin",
    "gui_modules.sao_gui_misc_mixin", "gui_modules.sao_gui_packet_callbacks_mixin",
    "gui_modules.sao_gui_panel_fx_mixin", "gui_modules.sao_gui_panels_mixin",
    "gui_modules.sao_gui_profile_editors", "gui_modules.sao_gui_session_mixin",
    "gui_modules.sao_gui_skillfx", "gui_modules.sao_gui_state_mixin",
    "gui_modules.sao_gui_status_updater_mixin", "gui_modules.sao_hotkey_manager",
    "gui_modules.sao_left_info_gpu", "gui_modules.sao_menu_bar_gpu",
    "gui_modules.sao_menu_hud", "gui_modules.sao_menu_left_stack",
    "gui_modules.sao_panel_ui", "gui_modules.sao_player_panel",
    "gui_modules.sao_session_players_panel", "gui_modules.settings_manager",
]

CYTHON_MODULES = [
    "_sao_cy_packet", "_sao_cy_combat", "_sao_cy_pixels",
    "_sao_cy_skillfx", "_sao_cy_uihelpers",
]

MIXIN_NAMES = [
    ("gui_modules.sao_gui_session_mixin", "SAOPlayerGUISessionMixin"),
    ("gui_modules.sao_gui_state_mixin", "SAOPlayerGUIStateMixin"),
    ("gui_modules.sao_gui_menu_mixin", "SAOPlayerGUIMenuMixin"),
    ("gui_modules.sao_gui_fisheye_mixin", "SAOPlayerGUIFisheyeMixin"),
    ("gui_modules.sao_gui_actions_mixin", "SAOPlayerGUIActionsMixin"),
    ("gui_modules.sao_gui_engine_toggles_mixin", "SAOPlayerGUIEngineTogglesMixin"),
    ("gui_modules.sao_gui_dps_theme_mixin", "SAOPlayerGUIDpsThemeMixin"),
    ("gui_modules.sao_gui_panels_mixin", "SAOPlayerGUIPanelsMixin"),
    ("gui_modules.sao_gui_status_updater_mixin", "SAOPlayerGUIStatusUpdaterMixin"),
    ("gui_modules.sao_gui_dialogs_mixin", "SAOPlayerGUIDialogsMixin"),
    ("gui_modules.sao_gui_engine_lifecycle_mixin", "SAOPlayerGUIEngineLifecycleMixin"),
    ("gui_modules.sao_gui_packet_callbacks_mixin", "SAOPlayerGUIPacketCallbacksMixin"),
    ("gui_modules.sao_gui_float_hp_mixin", "SAOPlayerGUIFloatHpMixin"),
    ("gui_modules.sao_gui_float_handlers_mixin", "SAOPlayerGUIFloatHandlersMixin"),
    ("gui_modules.sao_gui_lifecycle_mixin", "SAOPlayerGUILifecycleMixin"),
    ("gui_modules.sao_gui_panel_fx_mixin", "SAOPlayerGUIPanelFxMixin"),
    ("gui_modules.sao_gui_link_animation_mixin", "SAOPlayerGUILinkAnimationMixin"),
    ("gui_modules.sao_gui_damage_events_mixin", "SAOPlayerGUIDamageEventsMixin"),
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
        "compute_skillfx_layout", "format_level_text", "ease_out_cubic",
        "lerp_clamped", "popup_origins",
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
section("Phase A — import top-level modules")
for mod in TOPLEVEL_MODULES:
    probe(mod, import_module, mod)

section("Phase A — import gui_modules/ helpers + mixins")
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
    assert isinstance(config.APP_VERSION, str)
    assert config.APP_VERSION_LABEL.startswith("v")
    rects = config.get_skill_slot_rects((0, 0, 1920, 1080))
    assert isinstance(rects, list)


def smoke_uihelpers_layout():
    import _sao_cy_uihelpers as h
    out = h.compute_skillfx_layout((0, 0, 1920, 1080), [], [])
    # may be None if no slots; just ensure no exception
    _ = out


def smoke_uihelpers_format():
    import _sao_cy_uihelpers as h
    s = h.format_level_text(60, 83, 1)
    assert isinstance(s, str)


def smoke_uihelpers_ease():
    import _sao_cy_uihelpers as h
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
    import game_state
    gs = game_state.GameState()
    # write some fields
    gs.window_rect = (0, 0, 1920, 1080)
    assert gs.window_rect == (0, 0, 1920, 1080)


def smoke_packet_parser():
    import packet_parser
    # Just verify exposed surface; do not parse real bytes
    assert hasattr(packet_parser, "PacketParser") or hasattr(packet_parser, "parse_packet")


def smoke_dps_tracker():
    import dps_tracker
    cls = getattr(dps_tracker, "DPSTracker", None) or getattr(dps_tracker, "DpsTracker", None)
    assert cls is not None
    inst = cls()
    _ = inst


def smoke_window_locator():
    from utils import window_locator
    loc = window_locator.WindowLocator()
    rect = loc.get_rect()  # may return None if game window missing
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
    from vision import skill_recognition
    # Real exports: SkillVisualTracker class + skill-slot rect helpers
    assert hasattr(skill_recognition, "SkillVisualTracker")
    assert callable(skill_recognition.get_skill_slot_rects)
    assert callable(skill_recognition.get_skill_slot_client_rects)


def smoke_recognition():
    from vision import recognition
    # Engine class expected
    assert any(hasattr(recognition, n) for n in
               ("RecognitionEngine", "Recognition", "Vision"))


def smoke_panel_ui_icon():
    from gui_modules import sao_panel_ui
    assert callable(getattr(sao_panel_ui, "_apply_window_icon", None))
    assert callable(getattr(sao_panel_ui, "_set_process_app_id", None))


probe("config.APP_VERSION + get_skill_slot_rects", smoke_config)
probe("_sao_cy_uihelpers.compute_skillfx_layout", smoke_uihelpers_layout)
probe("_sao_cy_uihelpers.format_level_text", smoke_uihelpers_format)
probe("_sao_cy_uihelpers ease / lerp", smoke_uihelpers_ease)
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
