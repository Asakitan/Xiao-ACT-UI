# -*- coding: utf-8 -*-
"""Shared configuration and settings helpers for SAO Auto."""

import filecmp
import json
import os
import sys
import tempfile
from typing import Any, Dict, List, Optional, Tuple

if getattr(sys, "frozen", False):
    # onedir + 模块化布局:
    #   BASE_DIR = exe 所在目录, 含 XiaoACTUI.exe / update.exe / web/ / assets/ / proto/ / runtime/
    #   BUNDLE_DIR = PyInstaller 解包根 (= contents_directory='runtime'), 仅作为最终回退
    BASE_DIR = os.path.dirname(sys.executable)
    BUNDLE_DIR = getattr(sys, '_MEIPASS', os.path.join(BASE_DIR, 'runtime'))
else:
    BUNDLE_DIR = os.path.dirname(os.path.abspath(__file__))
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# v2.1.2-h: onedir 下 sys.path 只含 runtime/, 但 build_release.bat 把
#   proto/ assets/ web/ 提升到 BASE_DIR (exe 顶层), 导致 `from proto import
#   star_resonance_pb2` ImportError (-> packet_parser 抓包链路死). 在 config
#   被任何模块 import 时立即把 BASE_DIR 加入 sys.path 头, 这是最早的修复点。
try:
    if BASE_DIR and BASE_DIR not in sys.path:
        sys.path.insert(0, BASE_DIR)
except Exception:
    pass


def _files_are_identical(left: str, right: str) -> bool:
    """Return True only when two files are byte-for-byte identical."""
    try:
        return filecmp.cmp(left, right, shallow=False)
    except Exception:
        return False


def _is_main_app_host() -> bool:
    """v2.1.2-k: 只有当宿主进程是 XiaoACTUI 主程序时, 才允许动 update.exe.

    update.exe 自己也会 import config (它被 PyInstaller 一起打包),
    如果在 update.exe 进程里跑 promote 逻辑, 会 rename/replace 自己,
    导致 update.exe 启动后 "凭空消失"。
    """
    if not getattr(sys, "frozen", False):
        return False
    try:
        exe_name = os.path.basename(sys.executable or "").lower()
    except Exception:
        return False
    # 任何带 update 字样的 helper 都跳过
    if "update" in exe_name:
        return False
    return True


def _promote_update_exe_new_early() -> bool:
    """Finalize update.exe.new at process bootstrap without touching old update.exe otherwise."""
    try:
        if not _is_main_app_host():
            return False
        staged = os.path.join(BASE_DIR, "update.exe.new")
        if not os.path.isfile(staged):
            return False
        target = os.path.join(BASE_DIR, "update.exe")
        if os.path.isfile(target) and _files_are_identical(target, staged):
            try:
                os.remove(staged)
            except Exception:
                pass
            print("[config] dropped identical update.exe.new", flush=True)
            return False
        if not os.path.isfile(target):
            os.replace(staged, target)
            print(f"[config] promoted update.exe.new -> {target}", flush=True)
            return True

        # Use a temp copy + atomic replace so the live helper stays untouched
        # until the staged file is fully materialized.
        # Retry up to 5 times with 0.5 s delays — the target may be transiently
        # locked by the dying update.exe process or antivirus scanning.
        import shutil
        import time as _time
        tmp_target = os.path.join(BASE_DIR, "update.exe.promoting")
        _MAX_RETRIES = 5
        for _attempt in range(_MAX_RETRIES):
            try:
                if os.path.exists(tmp_target):
                    os.remove(tmp_target)
            except Exception:
                pass
            try:
                shutil.copy2(staged, tmp_target)
                os.replace(tmp_target, target)
                try:
                    os.remove(staged)
                except Exception:
                    pass
                print(f"[config] replaced update.exe from update.exe.new -> {target}"
                      f" (attempt {_attempt + 1})", flush=True)
                return True
            except PermissionError:
                if _attempt < _MAX_RETRIES - 1:
                    _time.sleep(0.5)
                continue
        # All retries exhausted — clean up the .promoting leftover.
        try:
            if os.path.exists(tmp_target):
                os.remove(tmp_target)
        except Exception:
            pass
        print("[config] promote update.exe.new failed after retries (PermissionError)", flush=True)
        return False
    except Exception as e:
        print(f"[config] promote update.exe.new failed: {e}", flush=True)
        return False


def _promote_runtime_update_exe_early() -> bool:
    """v2.1.2-h: bootstrap 把 runtime/update.exe 提升到顶层.

    与 sao_updater.promote_runtime_update_exe 等价, 但放在 config 里
    保证最早被调用 (大多数模块都 import config). 解决用户反馈的
    "升级后 update.exe 没替换" — 之前依赖 sao_updater 的延迟 import
    路径, 在 webview/atexit 没触发时就跑不到。

    v2.1.2-k: 仅在主程序 (XiaoACTUI) 进程里运行, 防止 update.exe
    自己 promote 自己导致被删除。
    """
    try:
        if not _is_main_app_host():
            return False
        nested = os.path.join(BASE_DIR, "runtime", "update.exe")
        if not os.path.isfile(nested):
            return False
        target = os.path.join(BASE_DIR, "update.exe")
        # v2.1.3 修复: 当顶层 update.exe 已经存在时, 永远视其为权威 (full-package
        # 解压出的最新版), 嵌套 runtime/update.exe 一定是上一次 runtime-delta 的
        # 残留, 必须直接删掉, 绝不能拿 stale nested 覆盖 fresh top-level —
        # 之前的 size 比较 + os.replace 路径在升级链 (h→i→…→n) 中导致用户
        # 启动时看到 "update.exe 被删/回退" 的现象。
        if os.path.isfile(target):
            try:
                os.remove(nested)
                print(f"[config] dropped stale runtime/update.exe (top-level present)", flush=True)
            except Exception:
                pass
            return False
        # 顶层缺失 → 此时才把 nested 提升上来
        try:
            os.replace(nested, target)
            print(f"[config] promoted runtime/update.exe -> {target}", flush=True)
            return True
        except Exception as e:
            print(f"[config] promote update.exe failed: {e}", flush=True)
            return False
    except Exception:
        return False


try:
    _promote_update_exe_new_early()
except Exception:
    pass


try:
    _promote_runtime_update_exe_early()
except Exception:
    pass


def _promote_pending_replacements() -> int:
    """v2.1.2-j: 扫描 BASE_DIR 下所有 *.new 文件并 finalize.

    场景:
      - 旧 update.exe 用 os.replace 覆盖 SAOUI.ttf 失败 → 我们的新 update_apply
        把它 stage 到 SAOUI.ttf.new。
      - 旧 update.exe 处理 update.exe 自身时, 失败时 fallback 留下 update.exe.new
        (之前由 MoveFileEx DELAY_UNTIL_REBOOT 排队, 但用户要求重启前完成)。
    主程序 XiaoACTUI 启动到这里时, 之前持锁的进程已完全退出, 可以直接 rename。
    返回 finalize 成功的文件数。

    v2.1.2-k: 仅在主程序 (XiaoACTUI) 进程里运行 — update.exe 自己 import
    config 时若 finalize update.exe.new 会删除自己。
    """
    if not _is_main_app_host():
        return 0
    finalized = 0
    skip_dirs = {os.path.join(BASE_DIR, d) for d in ("backup", "staging", "temp", "exports")}
    try:
        for dirpath, dirnames, filenames in os.walk(BASE_DIR):
            # prune
            dirnames[:] = [d for d in dirnames if os.path.join(dirpath, d) not in skip_dirs]
            for fn in filenames:
                if not fn.endswith(".new"):
                    continue
                staged = os.path.join(dirpath, fn)
                target = staged[:-4]
                if not target:
                    continue
                if os.path.normcase(staged) == os.path.normcase(os.path.join(BASE_DIR, "update.exe.new")):
                    # update.exe.new is handled by the dedicated early bootstrap
                    # path above; keep it out of the generic .new finalizer.
                    continue
                try:
                    if os.path.isfile(target) and _files_are_identical(target, staged):
                        try:
                            os.remove(staged)
                        except Exception:
                            pass
                        continue
                    try:
                        os.replace(staged, target)
                        finalized += 1
                        print(f"[config] finalized pending replacement: {target}", flush=True)
                    except PermissionError:
                        # 目标仍被占用时保留 .new，等待下次启动再 finalize。
                        continue
                    except Exception as e:
                        print(f"[config] finalize failed for {target}: {e}", flush=True)
                except Exception:
                    pass
    except Exception:
        pass
    return finalized


try:
    _promote_pending_replacements()
except Exception:
    pass


def _cleanup_old_renamed_targets() -> int:
    """v2.1.2-n: 清理 schedule_apply_on_exit 留下的 ``<name>.old-<ts>`` 文件.

    主程序在退出前 rename 字体/DLL 让老 update.exe 能直接 os.replace,
    本进程持有的 GDI/loader handle 在主进程退出后释放, 重新启动时
    这些 .old-<ts> 文件已经无人持有, 可以安全删除避免堆积。"""
    if not _is_main_app_host():
        return 0
    import re as _re
    pattern = _re.compile(r"\.old-\d+$")
    cleaned = 0
    skip_dirs = {os.path.join(BASE_DIR, d) for d in ("backup", "staging", "temp", "exports")}
    try:
        for dirpath, dirnames, filenames in os.walk(BASE_DIR):
            dirnames[:] = [d for d in dirnames if os.path.join(dirpath, d) not in skip_dirs]
            for fn in filenames:
                if not pattern.search(fn):
                    continue
                full = os.path.join(dirpath, fn)
                try:
                    os.remove(full)
                    cleaned += 1
                except Exception:
                    pass
    except Exception:
        pass
    return cleaned


try:
    _cleanup_old_renamed_targets()
except Exception:
    pass


def _cleanup_orphan_swap_scripts() -> int:
    """v2.1.2-n: 清理 BASE_DIR 下残留的 _swap_update_*.cmd.

    update.exe 自己被覆盖时, _schedule_self_replace 会 spawn 一个 cmd 脚本,
    脚本末尾 `del /f /q "%~f0"` 应自删, 但偶尔 cmd.exe 没释放句柄就退出
    (用户截图能看到 _swap_update_<ts>.cmd 残留)。主程序启动时, 旧 update.exe
    及其 spawn 的 cmd 都已彻底退出, 直接清掉。"""
    if not _is_main_app_host():
        return 0
    cleaned = 0
    try:
        for fn in os.listdir(BASE_DIR):
            if not fn.startswith("_swap_update_") or not fn.endswith(".cmd"):
                continue
            try:
                os.remove(os.path.join(BASE_DIR, fn))
                cleaned += 1
            except Exception:
                pass
    except Exception:
        pass
    return cleaned


try:
    _cleanup_orphan_swap_scripts()
except Exception:
    pass

# 远程更新可写覆盖层 (可选, delta 直接写到 BASE_DIR 同名子目录, 这里仅用于 staging/backup/state)
RUNTIME_DIR = BASE_DIR
RUNTIME_PY_DIR = os.path.join(BASE_DIR, "runtime")           # 我们的 .py 与 Python DLL 同处 runtime/
RUNTIME_WEB_DIR = os.path.join(BASE_DIR, "web")
RUNTIME_ASSETS_DIR = os.path.join(BASE_DIR, "assets")
RUNTIME_PROTO_DIR = os.path.join(BASE_DIR, "proto")
RUNTIME_STAGING_DIR = os.path.join(BASE_DIR, "staging")
RUNTIME_BACKUP_DIR = os.path.join(BASE_DIR, "backup")
UPDATE_STATE_FILE = os.path.join(BASE_DIR, "update_state.json")


def _runtime_first(*parts: str) -> str:
    """返回资源路径: 优先 BASE_DIR (顶层模块化文件夹), 不存在则回退 BUNDLE_DIR."""
    if not parts:
        return BASE_DIR
    top = os.path.join(BASE_DIR, *parts)
    if os.path.exists(top):
        return top
    return os.path.join(BUNDLE_DIR, *parts)


def runtime_resource(*parts: str) -> str:
    return _runtime_first(*parts)


def resource_path(*parts: str) -> str:
    return _runtime_first(*parts)


# 只读资源 (优先 BASE_DIR 顶层文件夹, 回退 BUNDLE_DIR)
ASSETS_DIR = _runtime_first("assets")
SOUNDS_DIR = _runtime_first("assets", "sounds")
FONTS_DIR = _runtime_first("assets", "fonts")
WEB_DIR = _runtime_first("web")
# 可写数据 (exe 旁边)
TEMP_DIR = os.path.join(BASE_DIR, "temp")
SKILL_BASELINE_DIR = os.path.join(TEMP_DIR, "skill_startup")

# 远程更新服务地址 (可被 settings.json 中 update_host 覆盖). 留空表示禁用更新检查.
DEFAULT_UPDATE_HOST = "http://doi.sakisense.top:15018"
UPDATE_CHANNEL = "stable"
UPDATE_TARGET = "windows-x64"

WINDOW_TITLE = "SAO Auto - Game HUD"
WINDOW_SIZE = "900x980"
APP_VERSION = "3.2.13"
APP_VERSION_LABEL = f"v{APP_VERSION}"
# v3.2.13: panel-fx scheduler + link animations (rounds 64-65 of /loop).
#   sao_gui.py crosses below 1100 lines; cumulative reduction reaches
#   89.5%. MRO depth grows to 17 mixin layers.
#   Round 64: SAOPlayerGUIPanelFxMixin (210 lines mixin / 158 net out).
#     2 methods + 2 class attrs + 1 module-level helper:
#       _make_sao_panel_hud (22, module-level Canvas factory),
#       _attach_sao_panel_fx (62, register panel + sig cache + auto-
#         unregister on destroy + start shared tick),
#       _sao_fx_shared_tick (75, @_probe-decorated staticmethod;
#         90ms shared tick driving all panels via _CY_UI.sao_fx_coords).
#     Class attrs _sao_fx_panels and _sao_fx_after_id were declared on
#     FloatHpMixin since round 56 (vestigial — they had originally been
#     on SAOPlayerGUI proper and got carried along incidentally).
#     Round 64 relocates them to their proper home alongside the
#     methods that use them.
#     SAOPlayerGUI._sao_fx_* class-attr refs in the methods rewritten
#     to type(self)._sao_fx_* / type(self_ref)._sao_fx_* (semantics
#     identical via class-attr lookup; avoids the not-yet-defined
#     SAOPlayerGUI import cycle).
#     sao_gui.py: 2023 -> 1865.
#   Round 65: SAOPlayerGUILinkAnimationMixin (911 lines mixin / 850
#     net out). The single biggest extraction since round 41 (Fisheye,
#     1078). The full-screen SAO link-start/link-end overlay animations
#     + moderngl GPU draw helpers + exit-window enumerator.
#     12 methods (in source order):
#       _play_link_start (68 — top-level entry animation),
#       _init_entry_boot_gl (101 — moderngl context + GLSL shaders),
#       _draw_entry_boot_gl (31), _create_entry_overlay (30),
#       _draw_entry_overlay (107), _run_entry_animation (66),
#       _init_exit_pulse_gl (107), _draw_exit_pulse_gl (31),
#       _get_exit_banner (17), _create_exit_overlay (42),
#       _draw_exit_overlay (141 — biggest method in this cluster;
#         exit animation main loop),
#       _collect_exit_windows (110 — enumerates every visible Tk
#         Toplevel + ULW window for the exit fade).
#     Mixin imports: just time + tkinter at module level. moderngl,
#       PIL, gpu_overlay_window are inline-imported (matches pattern).
#     sao_gui.py: 1865 -> 1015.
#   SAOPlayerGUI MRO now has 17 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
#      FloatHp, FloatHandlers, Lifecycle, PanelFx, LinkAnimation,
#      State, Session). No name conflicts.
#   Cumulative refactor: 9682 -> 1015 = -8667 = -89.5%. sao_gui.py is
#   7985 lines below the user's 9000-line target. gui_modules/ now
#   holds 38 .py / ~29664 lines. **The refactor is approaching its
#   natural floor — most extractable cohesive clusters are out.**
# v3.2.12: menu widget reorg + lifecycle mixin (rounds 61-62 of /loop).
#   sao_gui.py crosses below 2100 lines; cumulative reduction reaches
#   79.1%. MRO depth grows to 15 mixin layers.
#   Round 61 (user-driven): the remaining 4 standalone "sao_*" GPU
#     widget files at the repo root moved into gui_modules/:
#       sao_menu_hud.py (1505 lines, the renderer hub),
#       sao_menu_bar_gpu.py (430), sao_left_info_gpu.py (892),
#       sao_child_bar_gpu.py (550). Total 3377 lines relocated.
#     Updates: 8 importer lines (sao_theme + 4 cross-imports within
#       moved files + 3 ui_gpu/*) and 4 new hiddenimports in
#       XiaoACTUI.spec (replacing the 1 old sao_menu_hud entry).
#     sao_menu_hud.py's `_BASE = os.path.dirname(os.path.abspath(__file__))`
#       fallback was rewritten to dirname(dirname(...)) since __file__
#       now resolves one level deeper.
#     Circular-import fix: the move exposed an existing latent cycle —
#       sao_theme → gui_modules.sao_menu_hud → gui_modules/__init__ →
#       sao_gui_menu_mixin → sao_theme.SAOPopUpMenu. Resolved by removing
#       the SAOPlayerGUI mixin re-exports from gui_modules/__init__.py.
#       The mixins are SAOPlayerGUI-internal — they're only imported via
#       full dotted path in sao_gui.py, so the re-exports were unused
#       convenience. The standalone helper classes (5 of them) that are
#       imported as `from gui_modules import X` stay re-exported.
#   Round 62: SAOPlayerGUILifecycleMixin (398 lines mixin / 324 net out).
#     7 methods covering ordered teardown + restore-on-startup:
#       _destroy_hp_alpha_strip_windows (9), _restore_panels (13),
#       _cleanup_entry_overlay (23), _cleanup_exit_overlay (24),
#       _finalize_close (128 — ordered destroy sequence: stop loops,
#         cancel after IDs, remove listeners, unbind hotkeys, stop
#         fisheye + boss-HP worker, close menu, stop engines, persist
#         cache, destroy overlays, quit mainloop),
#       _run_exit_animation (124 — confirm + fade-out + scheduled
#         _finalize_close after the animation),
#       _on_close (3 — top-level handler).
#     `SAOPlayerGUI._sao_fx_after_id` class-attr references in the
#     extracted block were rewritten to `type(self)._sao_fx_after_id`
#     so the mixin doesn't need to import the not-yet-defined class
#     (semantics identical via MRO/class-attr lookup).
#     sao_gui.py: 2347 -> 2023.
#   SAOPlayerGUI MRO now has 15 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
#      FloatHp, FloatHandlers, Lifecycle, State, Session). No name
#     conflicts.
#   Cumulative refactor: 9682 -> 2023 = -7659 = -79.1%. sao_gui.py is
#   6977 lines below the user's 9000-line target. gui_modules/ now
#   holds 36 .py / ~28543 lines.
# v3.2.11: float-handlers mixin + Win32 helpers move (rounds 58-59 of
#   /loop). sao_gui.py crosses below 2400 lines; cumulative reduction
#   reaches 75.8%. MRO depth grows to 14 mixin layers.
#   Round 58: SAOPlayerGUIFloatHandlersMixin (193 lines mixin / 137 net
#     out). 9 methods covering remaining float-button handlers + misc:
#       _float_click, _float_drag, _float_release, _float_enter,
#       _float_leave, _lift_float_loop, _raise_panel_window,
#       _arm_pending_combat_reset, _setup_hotkeys.
#     _create_floating_widget DEFERRED to round 59 (needs Win32 helpers
#     moved first to avoid circular import).
#     sao_gui.py: 2674 -> 2537.
#   Round 59 phase 1: 4 module-level Win32 helpers (63 lines total)
#     moved from sao_gui.py to gui_modules/sao_panel_ui.py — natural
#     cluster with the existing _apply_panel_style:
#       _get_icon_path, _apply_window_icon (iconbitmap + WM_SETICON),
#       _set_clickthrough_style (WS_EX_TRANSPARENT),
#       _disable_native_window_shadow (DWMNCRP_DISABLED).
#     sao_panel_ui.py grew from 195 -> ~280 lines.
#     sao_gui.py keeps a single re-import line so callers that still
#     reference the unqualified names work transparently.
#   Round 59 phase 2: _create_floating_widget (131 lines, the float
#     anchor Toplevel constructor) appended to FloatHandlersMixin.
#     Mixin imports expanded to include tkinter, ctypes, the 2 Win32
#     helpers, and get_cjk_font; mixin declares its own module-level
#     _user32 = ctypes.windll.user32 handle (ctypes.windll caches DLL
#     handles, so this is identical to sao_gui's _user32).
#     sao_gui.py: 2537 -> 2347.
#   SAOPlayerGUI MRO now has 14 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
#      FloatHp, FloatHandlers, State, Session). No name conflicts.
#   Cumulative refactor: 9682 -> 2347 = -7335 = -75.8%. sao_gui.py is
#   6653 lines below the user's 9000-line target. gui_modules/ now
#   holds 31 .py / ~25600 lines.
# v3.2.10: two more SAOPlayerGUI mixins extracted (rounds 55-56 of /loop).
#   sao_gui.py crosses below 2700 lines; cumulative reduction reaches
#   72.4%. MRO depth grows to 13 mixin layers.
#   Round 55: SAOPlayerGUIPacketCallbacksMixin (420 lines mixin / 357
#     net out). 9 methods covering the packet event callback surface:
#       _send_linked_key (29) — boss-raid alert → Win32 SendInput
#         keypress via auto_key_engine VK_NAME_MAP.
#       _on_packet_damage (66) — damage tick callback; updates the
#         boss-HP target lock + last-damage timestamp.
#       _is_dead_state (10, cython predicate),
#       _bump_boss_hp_target_hold (20),
#       _boss_monster_usable (18, cython + revive side effect),
#       _sync_boss_hp_revive_hold (7).
#       _on_monster_update (66) — monster update callback.
#       _on_boss_event (6) — delegates to boss_raid_engine.
#       _on_scene_change (136, biggest here) — scene transition;
#         arm pending combat reset + clear caches.
#     Cross-mixin refs still work: _boss_monster_usable used by
#     State mixin's _compute_boss_hp_delta; _send_linked_key used by
#     EngineLifecycle's _start_recognition; both resolved via MRO.
#     sao_gui.py: 3439 -> 3082.
#   Round 56: SAOPlayerGUIFloatHpMixin (469 lines mixin / 408 net out).
#     20 methods bundling HP overlay context handlers + float button +
#     breath animations + the 146-line motion blur effect:
#       HP overlay (6): _refresh_hp_layered, _reset_sta_offline_state,
#         _should_show_sta_offline, _hp_overlay_on_click,
#         _hp_overlay_restore_position, _hp_overlay_hide.
#       Float button + animations (14): _build_float_hud_items,
#         _set_float_alpha, _animate_float_hud, _start_float_breath,
#         _breath_step, _stop_float_breath, _attach_panel_float,
#         _panel_float_shared_tick (69, with @_probe.decorate),
#         _update_float_display/status/fname/title, _animate_float_to,
#         _play_motion_blur (146 — radial blur on menu open/close,
#         background thread screen grab + radial blur + main-thread fade).
#     Mixin needed `from perf_probe import probe as _probe` for the
#     @_probe.decorate('ui.panel_float.tick') on shared_tick (caught
#     by initial NameError, fixed before commit).
#     sao_gui.py: 3082 -> 2674.
#   SAOPlayerGUI MRO now has 13 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      StatusUpdater, Dialogs, EngineLifecycle, PacketCallbacks,
#      FloatHp, State, Session). No name conflicts.
#   Cumulative refactor: 9682 -> 2674 = -7008 = -72.4%. sao_gui.py is
#   6326 lines below the user's 9000-line target. gui_modules/ now
#   holds 30 .py / ~25252 lines.
# v3.2.9: two more SAOPlayerGUI mixins extracted (rounds 52-53 of /loop).
#   sao_gui.py crosses below 3500 lines; cumulative reduction reaches
#   64.5%. MRO depth grows to 11 mixin layers.
#   Round 52: SAOPlayerGUIDialogsMixin (268 lines mixin / 196 net out).
#     7-method grab-bag of UI-construction helpers:
#       _make_player_panel (34) — factory for SAO menu's left widget
#         (SAOMenuLeftStack: player panel + session-players panel).
#       _hp_overlay_on_menu (41) — right-click HP context menu.
#       _show_welcome_then_menu (14) — first-launch flow.
#       _show_entity_alert (13) — convenience around alert overlay
#         (referenced by Panels / EngineToggles / DpsTheme /
#         EngineLifecycle mixins; resolves via MRO).
#       _switch_to_webview_ui (22) — confirm + exit + hot restart.
#       _show_about (18) — about dialog with updater state hint.
#       _edit_profile (55) — profile editor with anti-double-open guard.
#     sao_gui.py: 3984 -> 3788.
#   Round 53: SAOPlayerGUIEngineLifecycleMixin (441 lines mixin / 349
#     net out). The heaviest single remaining cluster:
#       _stop_recognition_engines (32) — stops AutoKey/BossRaid/HideSeek/
#         packet/vision; clears refs.
#       _reconfigure_data_engines (80) — restarts packet+vision engines
#         for current mem_data_source setting; loads skill_names.json.
#       _start_recognition (204 lines — the biggest method here) — full
#         bring-up: GameStateManager + state + 30s cache thread + 7
#         overlay windows + AutoKey + BossRaid + linkage.
#       _persist_cached_identity_state (34) — write identity to game_cache.
#     __file__ fix: _reconfigure_data_engines's inline
#       `os.path.dirname(os.path.abspath(__file__))` for the
#       skill_names.json bundle was replaced with
#       resource_path('assets', 'skill_names.json'). __file__ now
#       resolves to gui_modules/ instead of the project root, so the
#       BUNDLE/BASE_DIR-aware resource_path() helper is the right path.
#     Mixin imports: os, json, AutoKeyEngine, BossAutoKeyLinkage,
#       BossRaidEngine, DpsTracker, play_sound, resource_path, + 6
#       overlay classes from gui_modules.
#     sao_gui.py: 3788 -> 3439.
#   SAOPlayerGUI MRO now has 11 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      StatusUpdater, Dialogs, EngineLifecycle, State, Session). No
#     name conflicts across 11 layers.
#   Cumulative refactor: 9682 -> 3439 = -6243 = -64.5%. sao_gui.py is
#   5561 lines below the user's 9000-line target. gui_modules/ now
#   holds 28 .py / ~24363 lines.
# v3.2.8: panel-UI helpers extracted + Status+Updater mixin (rounds 49-50
#   of /loop). sao_gui.py crosses below 4000 lines; cumulative reduction
#   reaches 58.9%.
#   Round 49: gui_modules/sao_panel_ui.py (195 lines) — pulls 10
#     module-level _SAO_PANEL_* color constants + 9 helper funcs out of
#     sao_gui.py into a focused utility module:
#       _apply_panel_style (DWM round-corner), _hex_rgba,
#       _make_panel_close_button (+ _close_btn_photo_cache),
#       _sao_panel_header, _bind_panel_drag, _sao_panel_body,
#       _sao_panel_hud_canvas, _sao_row, _sao_pill.
#     sao_gui.py keeps a single re-import line so all still-resident
#     panel handlers keep their unqualified usages.
#     This unblocks _toggle_status_panel and other panel handlers that
#     previously could not move out (would have been a circular import).
#     sao_gui.py: 4890 -> 4759. Cumulative refactor crosses -50.0%.
#   Round 50: gui_modules/sao_gui_status_updater_mixin.py (857 lines mixin
#     / 775 net out). 20 methods: 2 status-panel handlers + 18 updater
#     event-chain methods.
#     Status panel: _toggle_status_panel (85 lines build/destroy),
#       _update_status_panel (21 lines refresh-from-snapshot).
#     Updater event chain: _get_update_snapshot, _get_update_view
#       (139 lines — biggest single method here; snapshot → display tuple
#       formatter), _ensure_updater_listener, _on_update_snapshot
#       (root.after dispatch), _mark_update_popup_ready,
#       _build_update_popup_payload (58), _maybe_show_update_popup,
#       _start_update_download, _start_update_check, _skip_update_version,
#       _apply_downloaded_update, _resolve_update_action,
#       _set_update_button, _close_update_panel, _open_update_panel
#       (111 lines panel UI builder), _refresh_update_panel,
#       _check_for_updates_interactive (84), _prompt_update_available.
#     sao_gui.py: 4759 -> 3984.
#   SAOPlayerGUI MRO now has 9 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      StatusUpdater, State, Session). No method conflicts.
#   Cumulative refactor: 9682 -> 3984 = -5698 = -58.9%. sao_gui.py is
#   5016 lines below the user's 9000-line target. gui_modules/ now
#   holds 26 .py / ~23654 lines.
# v3.2.7: two more SAOPlayerGUI mixins extracted (rounds 46-47 of /loop).
#   sao_gui.py crosses below 4900 lines; cumulative reduction reaches
#   49.5%. MRO depth grows to 8 mixin layers.
#   Round 46: SAOPlayerGUIDpsThemeMixin (286 lines mixin / 226 net out).
#     16 DPS methods + 3 Theme methods bound by menu-refresh + overlay-
#     update pattern.
#     DPS surface (timeouts, snapshots, report availability, overlay show
#       paths, reset + toggle): _combat_damage_timeout_s,
#       _boss_hp_hold_timeout_s, _cancel_dps_idle_reset_after,
#       _schedule_dps_idle_reset_after_fade, _empty_dps_snapshot,
#       _get_dps_last_report_available, _sync_dps_report_availability,
#       _request_dps_live_snapshot, _get_dps_last_report,
#       _request_dps_last_report, _request_dps_entity_detail,
#       _show_dps_live_snapshot, _show_dps_last_report,
#       _reset_dps_tracker, _show_last_dps_report_menu, _toggle_dps_enabled.
#     Theme switcher: _toggle_panel_theme, _set_all_themes,
#       _apply_theme_to_overlay. References self._THEME_OVERLAY_MAP
#       (class attr on SAOPlayerGUI, resolved via MRO).
#     sao_gui.py: 5303 -> 5077.
#   Round 47: SAOPlayerGUIPanelsMixin (255 lines mixin / 187 net out).
#     11 methods covering Commander, panel visibility, small settings:
#     Commander: _toggle_commander_panel, _push_commander_data (48-line
#       packet-driven snapshot builder, GUI-level sig-cached).
#     Panel visibility: _toggle_hide_all_panels (51-line
#       snapshot+restore across 7 panel handles).
#     Recognition / sound / buffmon / boss-bar / mem-source / topmost:
#       _toggle_recognition_menu, _toggle_sound_enabled,
#       _adj_sound_volume, _toggle_buffmon_enabled, _cycle_boss_bar_mode,
#       _get_mem_data_source, _cycle_mem_data_source, _toggle_topmost.
#     Mixin imports: perf_probe.probe (for @_probe.decorate) +
#       CommanderPanel from the already-relocated gui_modules.
#     _toggle_status_panel deferred — references 10+ module-level
#     sao_gui UI helpers (_SAO_PANEL_HEADER_BG, _sao_panel_header,
#     _bind_panel_drag, etc.); moving it requires either pulling those
#     helpers along or introducing a circular import.
#     sao_gui.py: 5077 -> 4890.
#   SAOPlayerGUI MRO now has 8 mixin layers (in extraction order):
#     (Menu, Fisheye, Actions, EngineToggles, DpsTheme, Panels,
#      State, Session). No method conflicts across the chain.
#   Cumulative refactor: 9682 -> 4890 = -4792 = -49.5%. sao_gui.py is
#   4110 lines below the user's 9000-line target. gui_modules/ now
#   holds 24 .py / 22602 lines.
# v3.2.6: two more SAOPlayerGUI mixins extracted (rounds 43-44 of /loop).
#   sao_gui.py crosses below 5400 lines; cumulative reduction exceeds 45%.
#   Round 43: SAOPlayerGUIActionsMixin (214 lines mixin / 152 net out).
#     Bundles AutoKey + BossRaid clusters because they share the same
#     shape: toggle_panel + toggle_detail_panel + toggle_engine +
#     config_loader + config_saver + author_snapshot.
#     AutoKey (9 methods): _toggle_autokey_panel,
#       _toggle_autokey_detail_panel, _toggle_auto_script,
#       _auto_key_settings_ref, _auto_key_author_snapshot,
#       _load_auto_key_config, _save_auto_key_config,
#       _load_autokey_burst_actions, _save_autokey_burst_actions.
#     BossRaid (8 methods): _toggle_bossraid_panel,
#       _toggle_bossraid_detail_panel, _toggle_boss_raid,
#       _boss_raid_next_phase, _boss_raid_settings_ref,
#       _boss_raid_author_snapshot, _load_boss_raid_config,
#       _save_boss_raid_config.
#     sao_gui.py: 5582 -> 5430.
#   Round 44: SAOPlayerGUIEngineTogglesMixin (175 lines mixin / 127 net
#     out). Bundles HideSeek + Burst — two distinct engine on/off
#     clusters but the same toggle + refresh pattern.
#     HideSeek (6 methods): _toggle_hide_seek, _start_hide_seek,
#       _stop_hide_seek (instantiates HideSeekEngine + WindowLocator,
#       AlertOverlay persistent UI), _on_hide_seek_status,
#       _schedule_hide_seek_alert_refresh (50s refresh tick),
#       _refresh_hide_seek_alert.
#     Burst (5 methods): _pick_burst_trigger_slot (cython delegate),
#       _normalize_watched_skill_slots (cython), _reset_burst_tracking_state,
#       _toggle_burst_enabled, _toggle_burst_slot (preserves ≥1 slot
#       invariant).
#     sao_gui.py: 5430 -> 5303.
#   SAOPlayerGUI now has 6 mixin layers via MRO:
#     (MenuMixin, FisheyeMixin, ActionsMixin, EngineTogglesMixin,
#      StateMixin, SessionMixin).
#   Cumulative refactor: 9682 -> 5303 = -4379 = -45.2%. sao_gui.py is
#   3697 lines below the user's 9000-line target. gui_modules/ now
#   holds 22 .py / 22061 lines.
# v3.2.5: two more SAOPlayerGUI mixins extracted (rounds 40-41 of /loop).
#   sao_gui.py crosses below 5600 lines — cumulative reduction now
#   exceeds 42%.
#   Round 40: SAOPlayerGUIMenuMixin (566 lines) — the full SAO PopUpMenu
#     lifecycle. 17 methods, ~488 lines net out of sao_gui.py:
#       - construction: _setup_sao_menu, _build_menu_children (139),
#         _build_update_menu_label
#       - open/close: _toggle_sao_menu, _close_sao_menu_from_background,
#         _clear_sao_menu_close_pending
#       - hooks: _on_sao_menu_open, _on_sao_menu_close,
#         _dismiss_sao_menu_for_panel
#       - refresh: _refresh_menu_if_open (debounced), _refresh_menu_immediate,
#         _apply_menu_refresh_if_open
#       - sig caching: _compute_menu_refresh_signature (200 ms-cached, 78
#         lines), _get_menu_children_cached, _cancel_pending_menu_refresh
#       - persistence: _persist_entity_menu_state, _restore_entity_menu_state
#     sao_gui.py: 7431 -> 6943.
#   Round 41: SAOPlayerGUIFisheyeMixin (1421 lines) — the persistent GPU
#     fisheye overlay. 9 methods, 1362 lines net out, including the
#     SINGLE BIGGEST method in all of SAOPlayerGUI:
#       - _start_fisheye_overlay (1078 lines) — builds the full GPU
#         rendering pipeline (PIL capture -> numpy/GPU distortion ->
#         BGRA bytes -> GpuOverlayWindow + BgraPresenter @60fps) plus
#         the transparent Tk hit layer and Win32 z-order management
#       - _stop_fisheye_overlay (74) — daemon shutdown + GPU release
#       - _run_fisheye_entry (103) — entry-animation flow
#       - _fisheye_close_suppressed, _release_fisheye_input_zorder,
#         _destroy_fisheye_hit_layer, _start_fisheye_with_retry,
#         _any_panel_open, _maybe_stop_fisheye (smaller helpers)
#     sao_gui.py: 6943 -> 5582.
#   SAOPlayerGUI now inherits from 4 mixins via MRO:
#     (MenuMixin, FisheyeMixin, StateMixin, SessionMixin).
#   Cumulative refactor: 9682 -> 5582 = -4100 = -42.3%. sao_gui.py is
#   3418 lines below the user's 9000-line target. gui_modules/ now
#   holds 20 .py / 21672 lines.
# v3.2.4: file reorganization — ALL sao_gui_*.py satellite modules moved
#   into gui_modules/ (rounds 37-38 of /loop). Addresses the user's
#   "用文件夹来把所有的文件归类，包括以前的文件" objective.
#   Round 37 (3 small files): sao_gui_menu_hud (347 lines),
#     sao_gui_alert (457), sao_gui_commander (363) via git mv. Imports
#     updated in sao_gui.py + sao_theme.py + XiaoACTUI.spec. Total:
#     1167 lines moved.
#   Round 38 (8 files atomic, hub module dependency forced this):
#     sao_gui_dps.py (3188; hub — imported by 5 other overlay files),
#     sao_gui_hp.py (3418), sao_gui_bosshp.py (3074),
#     sao_gui_skillfx.py (1847), sao_gui_buffmon.py (1188),
#     sao_gui_profile_editors.py (1231), sao_gui_autokey.py (547),
#     sao_gui_bossraid.py (544). Total: 15037 lines moved.
#     5 cross-imports updated within gui_modules/ (the hub module
#     reference in hp / bosshp / skillfx / buffmon / alert).
#     6 importer lines updated in sao_gui.py.
#     4 lurking old-path imports caught + fixed:
#       - gui_modules/sao_gui_hp.py: `import sao_gui_hp as _mod`
#         self-import for theme setattr (would have ImportError at
#         runtime on theme-switch toggle)
#       - gui_modules/sao_gui_skillfx.py: same pattern
#       - tools/bench_compose.py, tools/spike_skillfx_ab.py (dev tools)
#   XiaoACTUI.spec hiddenimports: 11 entries updated to gui_modules.X
#   dotted paths, plus added GUI_MODULES_HIDDENIMPORTS =
#   collect_submodules('gui_modules') as a catch-all for future
#   additions.
#   Final state: 0 sao_gui_*.py files at repo root. gui_modules/ now
#   holds 19 .py files / 18885 lines covering the full SAO HUD /
#   overlay / panel layer + the 7 mixin/helper extractions from
#   earlier rounds. sao_gui.py: 7375 (unchanged — it's the importer,
#   not the source). Cumulative refactor still 9682 → 7375 = -2307
#   (-23.8%).
# v3.2.3: COMBAT-LAG FIX — boss-HP compute moved off the Tk main thread
#   (rounds 34-35 of /loop). Addresses the user's "重战斗卡顿" complaint.
#   Round 34 (same-thread refactor):
#     - Extracted the 260-line boss-HP inline compute from
#       _push_packet_overlays into a new mixin method
#       _compute_boss_hp_delta(gs, _pp_now). The helper returns the
#       overlay update dict or None; touches no Tk widget so it's
#       safe to call from any thread. Pure refactor, no behavior change.
#   Round 35 (the actual fix):
#     - Added a daemon worker thread (sao-boss-hp-worker) that consumes
#       (gs, now) snapshots from a single-slot latest-wins mailbox and
#       computes the overlay payload off the Tk main thread.
#     - The 5 Hz recognition loop now only does: enqueue snapshot (O(1))
#       + consume previous tick's payload + a single overlay.update()
#       call. The ~260 lines of bridge.get_monster() / sort / dict-build
#       / sig-hash compute happen on the worker thread.
#     - Latency tradeoff: boss-HP overlay payload is from PREVIOUS tick
#       (at most 200 ms behind), well below human visual detection
#       threshold for HP-bar changes.
#     - Five new mixin methods: _ensure_boss_hp_worker,
#       _boss_hp_worker_loop, _enqueue_boss_hp_compute,
#       _consume_boss_hp_payload, _stop_boss_hp_worker. Shutdown hook
#       added to SAOPlayerGUI._finalize_close before overlay teardown.
#     - Out-of-process smoke test verifies start / enqueue / consume /
#       stop semantics.
#   sao_gui.py: 7368 → 7375 (+7, just the _stop_boss_hp_worker hook in
#   _finalize_close). State mixin: 794 → 913 lines (+119, worker
#   scaffold). Cumulative refactor: 9682 → 7375 = -2307 = -23.8%.
# v3.2.2: sao_gui refactor reaches the mixin stage (rounds 31-32 of /loop).
#   The first two SAOPlayerGUI mixins are extracted — this is the
#   structural turning point: instead of pulling helper classes out
#   around SAOPlayerGUI, the class itself is now being sliced.
#     - gui_modules/sao_gui_session_mixin.py (208 lines) — 8 in-session
#       player roster helpers (_merge_session_player,
#       _sync_session_players_cache, _refresh_session_players_panel,
#       _toggle_session_players_panel, plus 4 smaller utilities). They
#       move as a unit because they call each other through self.
#     - gui_modules/sao_gui_state_mixin.py (770 lines) — the four
#       methods at the heart of the user's combat-lag complaint:
#       _on_game_state_update, _apply_fast_state_update,
#       _push_packet_overlays (HOT PATH; per-tick DPS/Boss/HP/SkillFX
#       push), and _recognition_loop (200 ms Tk-after-driven loop).
#       Isolating this cluster sets up the actual combat-lag fix:
#       round 34+ will introduce a daemon worker that consumes packet
#       snapshots off-main and re-enters Tk via root.after(0, ...).
#   SAOPlayerGUI now inherits from (SAOPlayerGUIStateMixin,
#   SAOPlayerGUISessionMixin); MRO resolves the 12 extracted methods
#   transparently. py_compile + import sao_gui both clean.
#   sao_gui.py shrinks 8388 -> 7368 (-1020 net since v3.2.1; cumulative
#   refactor: 9682 -> 7368 = -2314 = -23.9%). gui_modules/ now holds
#   978 lines of mixin code on top of the earlier 1338 lines of
#   extracted helper classes.
# v3.2.1: sao_gui refactor continues (rounds 27-post / 28 / 29).
#   Three more extractions to gui_modules/ + a dead-code purge:
#     - SAOPlayerPanel (471 lines) -> gui_modules/sao_player_panel.py.
#       This pushed sao_gui.py below the user's 9000-line target.
#     - SAOMenuLeftStack (117 lines) -> gui_modules/sao_menu_left_stack.py.
#     - SettingsManager (53 lines) -> gui_modules/settings_manager.py;
#       CONFIG_FILE resolved via parent-of-parent so dev-mode path
#       (sao_auto/settings.json) is preserved.
#   Dead-code purge: _update_layered_win (Win32 layered-window helper,
#   ~58 lines) + its 4 ctypes structs (_BLENDFUNCTION / _ULW_SIZE /
#   _ULW_POINT / _BITMAPINFOHEADER). Confirmed unused via repo-wide grep;
#   other ULW consumers all have their own copies. _user32 / _gdi32
#   signature setup stays since 32+ call sites in sao_gui still need it.
#   sao_gui.py shrinks 9091 -> 8388 lines (-703 net since v3.2.0;
#   cumulative refactor: 9682 -> 8388 = -1294 = -13.4%).
# v3.2.0: sao_gui structural refactor begins (rounds 25-26 of /loop).
#   Minor bump signals a visible structural change: sao_gui.py monolith
#   (9682 lines) is being split into gui_modules/* subpackage. First two
#   extractions land in this commit:
#     - gui_modules/sao_hotkey_manager.py (75 lines) — SAOHotkeyManager
#       moved out; sao_gui re-exports it for backward compatibility.
#     - gui_modules/sao_session_players_panel.py (598 lines) — the
#       SESSION PLAYERS left-stack panel + its private wheel-routing
#       registry helpers (_SESSION_WHEEL_ROOTS, _dispatch_session_wheel)
#       all moved together so the module is self-contained.
#   sao_gui.py shrinks 9682 -> 9091 lines (-591 net). Public class names
#   (`SAOHotkeyManager`, `SAOSessionPlayersPanel`) still importable from
#   sao_gui so existing call sites and external consumers unaffected.
# v3.1.9: sao_gui main-thread polish (rounds 21-post / 22 / 23 of /loop).
#   - _refresh_session_players_panel: inner _sync_session_players_cache
#     call now also passes min_interval=0.25 (matches the outer call from
#     _push_packet_overlays). The double-sync when menu was visible is
#     gone; both call sites share the 4 Hz cadence.
#   - _lift_float_loop: cadence bumped 150 ms -> 250 ms (~40% fewer
#     per-sec SetWindowPos calls when the SAO menu is open).
#   - _on_game_state_update: identity-compare gs.self_buffs against the
#     last cached reference to skip ov.update_buffs() when the bridge
#     hasn't pushed a fresh self_buffs list. Works because round-13's
#     shallow-copy snapshot keeps list refs stable across non-buff
#     state updates. Saves ~10-20 us per skipped call across 30-60 Hz.
# v3.1.8: sao_gui main-thread reduction (rounds 19-20 of a new targeted /loop).
#   - sao_gui._recognition_loop: character_profile.save_profile() (sync
#     settings.json read+write, 5-50 ms on slow disks) moved to a daemon
#     thread on first identity arrival. Main loop no longer pays the
#     one-frame hitch.
#   - sao_gui._stop_fisheye_overlay: worker_thread.join(2.0) + GPU window
#     destroy + presenter release moved to a daemon thread. Main thread
#     does only the light Tk-bound work + flips running[0]. Legacy Tk
#     Toplevel destroy re-dispatched back via root.after(0, ...) so
#     thread-affine widgets stay on the main loop.
#   - sao_gui._push_packet_overlays: bumped _sync_session_players_cache
#     min_interval from 0.05 -> 0.25 (effective throttle from never-firing
#     to 4 Hz). Cuts ~20-50 us/call * ~5 calls/sec main-thread work.
# v3.1.7: signature trim + throttle + deepcopy (rounds 15-post / 16 / 17 of perf /loop).
#   - recognition._row_independent_pct: dropped unused `hue` and
#     `fill_hue_ref` placeholder args (left over from round 4 cython
#     migration). Single caller updated.
#   - boss_raid_engine.BossRaidEngine.on_damage_event: replaced the
#     per-event _fire_entity_update_locked() (rebuilds full entity dict
#     each call) with a dirty-bit flip. _run_loop (4 Hz) drains the bit
#     via _maybe_flush_entity_update_locked() honouring a 10 Hz UI cap.
#     Saves 2.5-15 ms/sec of damage-handler time under heavy combat.
#   - auto_key_engine.AutoKeyEngine.get_status: deepcopy -> dict() shallow
#     copy on a flat 7-field primitive dict (~38x faster: 2605 -> 68 ns).
# v3.1.6: structural caching + cleanup (rounds 12-post / 13 / 14 of perf /loop).
#   - dps_tracker: dropped the now-dead _compute_damage_id and
#     _resolve_skill_key Python wrappers (rounds 1 and 10b inlined them
#     into _CY_COMBAT direct calls; no external callers in the repo).
#   - game_state.GameStateManager.update + load_cache: replaced the
#     dict-comp + dataclass __init__ snapshot path (47 fields, ~3.5 us)
#     with copy.copy(self._state) — ~1.0 us/call, 3.4x faster. Snapshot
#     independence + shallow-copy list semantics preserved.
#   - packet_bridge._publish_player_update: cached the 5 distinct
#     _use_packet_source() results at function entry, replacing 9
#     repeated calls. Also cached the lock-guarded self._state_mgr.state
#     (3 accesses -> 1). ~2-2.5 us + 2 lock acquires saved per publish.
# v3.1.5: DPS micro-opts + auto_key engine cache (rounds 10-11 of perf /loop).
#   - dps_tracker._build_snapshot_locked: hit_fx shallow-copy via dict()
#     replaces copy.deepcopy (flat dict of primitives, ~10x faster on
#     every UI poll).
#   - dps_tracker._process_event: inlined _CY_COMBAT.resolve_skill_key
#     call with a cached module-level skill-effect table; saves one
#     Python wrapper call per damage event (~1.3 us/event end-to-end).
#   - auto_key_engine.AutoKeyEngine._tick: cached normalize_auto_key_config
#     result keyed by raw-dict id + player identity tuple. The 20 Hz
#     engine tick now skips re-normalisation when settings + identity
#     unchanged. ~75x speedup on cache hits (17.4 us -> 231 ns/tick).
# v3.1.4: Cython per-bar numerics (rounds 7-8 of comprehensive perf /loop).
#   - _sao_cy_pixels.box_convolve5_same_f32: 5-wide moving-average
#     smoothing in nogil, replaces both np.convolve calls in
#     _detect_bar_pct (~3x faster).
#   - _sao_cy_pixels.find_last_above_threshold_f32: rightmost-above-
#     threshold scan with the redundant single-pixel-fill repair
#     mathematically eliminated; replaces the >=/any/where/max chain
#     in _detect_bar_pct (~27x faster).
#   - _sao_cy_pixels.compute_bar_col_score_f32: folds the 8-op
#     hue_delta / hue_bonus / col_score chain into one nogil pass,
#     eliminating ~7 temp float32 arrays per bar (~7x faster).
#   - End-to-end _detect_bar_pct: ~0.33 ms/call (down from ~0.9 baseline).
# v3.1.3: Cython per-bar recognition migration (rounds 4-5 of perf /loop).
#   - recognition._detect_stamina_pct now uses _sao_cy_pixels.
#     bgr_color_match_column_ratio — single nogil pass with squared-distance
#     test, no sqrt, ~14x faster than the numpy reference path.
#   - recognition._row_independent_pct now uses _sao_cy_pixels.
#     row_independent_fill_pct — per-row score + 3-wide convolution +
#     sub-pixel + quickselect median in one nogil block, ~15x faster.
#   - recognition._gradient_edge_pct now uses _sao_cy_pixels.gradient_edge_pct
#     — diff + 7-wide convolution + argmin + mid-score crossing in one nogil
#     block, ~4.3x faster with parity-exact output (delta=0 on 7/7 cases).
# v3.1.2: Cython hot-path migration (rounds 1-2 of comprehensive perf /loop).
#   - dps_tracker._compute_damage_id + skill_key fallback moved into
#     _sao_cy_combat.compute_damage_id / resolve_skill_key (~2-3x faster per
#     damage event, parity-verified across 8 cases).
#   - packet_parser._parse_dirty_stream sub-field header parsing collapsed
#     into _sao_cy_packet.parse_dirty_subfield_header (8 branches refactored,
#     ~53% faster per branch, ~50 lines of boilerplate removed).
# v2.2.12 — SAO menu HUD now drives a per-pixel-alpha layered window
# (UpdateLayeredWindow) composed off-thread on the heavy render lane,
# replacing the legacy chroma-key Toplevel + per-tick `geometry()` move
# (which forced un-vsync'd DWM region recomposites and was the dominant
# tearing source). Set `SAO_GPU_MENU_HUD=0` to fall back to the legacy
# canvas-native path for diagnostics.
USE_GPU_MENU_HUD = True
# v2.3.0 (2026-04 fix): The whole GLFW-backed GPU overlay family
# (menu bar fisheye painter, left info painter, menu HUD GPU window,
# child bar painter, skillfx GPU pump) used to be opt-in via
# `SAO_GPU_OVERLAY=1`. With that gate off, every GPU-window code path
# silently fell back to the Tk Canvas main-thread paint loop — which
# is exactly what the v2.2.16 → v2.3.0 "compose on the worker, present
# on GPU" rewrite was trying to fix. Default-ON so the per-tick
# fisheye / left info paint cost lands on the worker thread instead
# of the main loop. Set `SAO_GPU_OVERLAY=0` to force the legacy
# Tk-Canvas / ULW path (e.g. for diagnostics on machines whose driver
# refuses GLFW transparent windows).
USE_GPU_OVERLAY = True
# v3.0.3
#   Fix DPS overlay click-through after the second fade cycle. The
#   v3.0.2 fix marshalled the whole click-through toggle to the GLFW
#   pump as fire-and-forget commands; rapid fade_in / fade_out turns
#   queued up there and the second hide pulse landed visible-but-faded
#   while still grabbing clicks. v3.0.3 splits the toggle: the Win32
#   ex-style flip (WS_EX_TRANSPARENT) is applied synchronously from
#   the calling Tk thread (SetWindowLongPtrW is thread-safe), and only
#   the GLFW_MOUSE_PASSTHROUGH attribute mirror is queued to the pump.
# v3.0.2
#   Fix DPS overlay click-through while idle / faded out. The GPU
#   panel now flips GLFW_MOUSE_PASSTHROUGH (in addition to the Win32
#   ex-style) on the pump thread when fade_out() runs, and the tick
#   loop re-asserts pass-through every idle frame so a focus/activation
#   event can no longer leave the invisible panel quietly intercepting
#   clicks meant for the game window.
# v3.0.1
#   Minor bug fixes and performance improvements. Set up buff monitor.
# ── Buff 监视器 ──
# True: 在主控台输出 buff 数据流诊断日志 (FIRST data / FIRST rows / 全部被过滤掉等),
# 用于排查 "buff 栏没出来" 类问题。生产环境建议 False。
BUFFMON_DEBUG = True
# 'ultimate': 仅显示 奥义/幻想/职业大招 buff (默认, 严格过滤)
# 'all'     : 显示所有自身 buff (无过滤, 用于诊断或纯展示)
BUFFMON_SELF_FILTER = 'ultimate'
# True: 走 GPU presenter (GpuOverlayWindow + AsyncFrameWorker + BgraPresenter),
#       同 sao_left_info_gpu / sao_gui_skillfx 的渲染路径; 后台合成 + GL 上传, 主线程零开销。
# False: 走 ULW (UpdateLayeredWindow) 兼容路径, 主线程 PIL 合成。
# 自动 fallback: 当 GLFW 不可用或 GPU 窗口创建失败时, 自动切到 ULW。
USE_GPU_BUFFMON = True
# v3.0.0
#   Added a new memory mode for combat data, which is more direct and has 
#   lower latency than the previous implementation. This mode is enabled by default, 
#   but can be disabled via the saomenu.
# v2.5.25:
#     • Prevent BossHP raid/packet state from showing party members or stale
#       non-damaged units when no live self-damage target is locked.
# v2.5.24:
#     • Tighten Entity BossHP target gating: monster updates no longer adopt
#       untargeted NPCs/party units, packet BossHP stays visible only for a
#       self-damaged tracked target, and normal timeout/stable-hide can hide it
#       again after combat ends.
# v2.5.23:
#     • Keep Entity BossHP visible while packet boss data is valid instead of
#       hiding it after the startup/scene grace window; stable-hide no longer
#       suppresses a live packet-backed BossHP snapshot.
# v2.5.22:
#     • Add packet-capture self-healing for long idle stalls: bridge timeout
#       now clears the capture endpoint lock for re-detection and can request a
#       pcap handle restart when raw frames stop, matching SRDPS-style idle
#       reconnect behavior. Also align deferred DPS/BossHP reset with
#       resonance semantics and route wipe buff 510072 into soft restart.
# v2.5.21:
#     • Harden Entity DPS/BossHP recovery across map switches and mid-session
#       startup: replay early self dirty packets after UID confirmation, keep a
#       scene/startup damage grace window, force-show the first live DPS
#       snapshot, and allow packet BossHP data to wake during that grace.
# v2.5.20:
#     • Fix Entity DPS/BossHP recovery after map switches and mid-session
#       startup by keeping the post-restart damage callback alive and canceling
#       stale DPS idle-reset timers when fresh self combat damage arrives.
# v2.5.19:
#     • Restore close motion blur and move fisheye backdrop input off the GPU
#       fisheye window: the GPU backdrop is now click-through/render-only,
#       while a transparent Tk hit layer handles backdrop click/drag/wheel;
#       keep the popup GPU window and Tk shell raised above that hit layer.
# v2.5.18:
#     • Do not stack the fullscreen motion-blur close overlay on top of the
#       fisheye backdrop's own fade-out when closing by clicking the fisheye
#       background; ID/HP/menu closes still keep their normal close blur.
# v2.5.17:
#     • Preserve the fisheye backdrop window lifecycle during background-click
#       close: demote/click-through only, keep the GPU window visible for its
#       normal fade-out, and prevent stop-requested fade-out from rebounding
#       into fade-in so the backdrop can reopen cleanly without black flashes.
# v2.5.16:
#     • On fisheye-background menu close, synchronously hide the fisheye HWND,
#       demote it below topmost, and make it click-through before the popup
#       close animation runs, preventing background-click-only z-order steals.
# v2.5.15:
#     • Restore automatic DPS idle fade-out and live-damage reset after the
#       fade completes while keeping the last report, without regressing the
#       post-scene fresh-damage path that reopens DPS/BossHP after map switches.
# v2.5.14:
#     • Suppress fisheye restart/fade-in while a backdrop click is closing the
#       SAO menu, so stale menu-visible state cannot flash the black fisheye
#       background back to top or steal z-order during close.
# v2.5.12:
#     • Release SAO popup/fisheye input and z-order immediately when closing
#       by demoting their HWNDs below topmost and enabling click-through before
#       fade/worker teardown, preventing closing overlays from stealing clicks.
# v2.5.11:
#     • Keep BossHP visible during invincible / lock-HP boss mechanics while
#       DPS still has recent live damage, and protect self/team/known player
#       UIDs from the post-scene combat-target grace so teammates never become
#       fake boss-bar targets.
# v2.5.10:
#     • Keep DPS/BossHP recoverable after hard map switches by preserving the
#       DPS overlay window, re-opening live DPS on fresh damage, and adding a
#       short post-scene self-damage grace path for stale target classification.
#     • Prevent SAO menu backdrop/fisheye clicks from re-raising the popup while
#       closing, and strengthen DPS panel contrast, row glow, scanlines, and
#       list masking so combat text remains readable over bright game scenes.
# v2.5.9:
#     • Parse EnterScene payloads during map/server transitions so scene keys
#       and self entity UUID refresh correctly, and prevent delayed scene-hide
#       callbacks from suppressing fresh DPS/BossHP live updates after切图.
# v2.5.8:
#     • Move packet byte decoding, GUI layout/signature math, entity-menu
#       hit testing/animation helpers, and HP/DPS/BossHP pixel/formatting
#       hot paths into mandatory Cython helpers to reduce Python-frame
#       overhead without lowering overlay effects or frame cadence.
# v2.5.7:
#     • Align DPS/BossHP scene and dungeon transition handling with
#       StarResonanceDps / resonance-logs-cn, including deferred combat resets,
#       dungeon dirty target progress, EnterScene scene IDs, and richer damage
#       fields for stable all-map routing.
#     • Expand skill_names coverage from the upstream merged skill table and
#       switch DPS/HPS per-skill aggregation to resonance-logs-cn style
#       SkillFightLevelTable-backed damage IDs.
# v2.5.6:
#     • BossHP main-target picker uses the highest MAX_HP unit (the actual
#       boss) instead of the most-full hp_pct, so a 100 % overworld trash
#       next to a 28 % real boss no longer hijacks the bar.
#     • boss_hp_hold_timeout_s floor dropped from 180 s → 1 s; user-set
#       fade values now actually take effect when no damage is happening.
#     • Hard scene resets push BossHP / DPS hide() immediately instead of
#       deferring 120 ms behind a token check that stray packets could bump.
# v2.5.5:
#   Restore UID/POWER Session Players wheel and click-drag scrolling through
#   the fisheye backdrop, align target classification with StarResonanceDps
#   / resonance-logs-cn so BOSS HP / DPS keep routing across map changes,
#   and tighten BossHP fade behavior:
#     • Fisheye GpuOverlayWindow now forwards wheel + cursor + button events
#       to the Tk session_panel underneath (so wheel scroll and touch-style
#       drag both work even though the Tk shell is chroma-keyed transparent).
#     • Damage target classification (parser AoiSyncDelta SkillEffect path
#       and the GUI _normalize_damage_event_target_for_entity fallback) now
#       trusts UUID encoding over stale _monsters / _team_members / _players
#       caches, mirroring SRDPS `IsUuidPlayerRaw` / SRLOGS `EEntityType`.
# v2.5.4:
#   Route Entity SAOMenu backdrop clicks through the normal close animation:
#   popup empty-area clicks and full-screen fisheye-background clicks now
#   trigger the same menu-close/motion-blur/fisheye fade-out path instead of
#   leaking to the game or only being swallowed silently.
# v2.5.3:
#   Restore UID/POWER Session Players wheel input while the GPU panel is
#   visible, keep the fisheye backdrop as a background layer so popup buttons
#   remain the only interactive menu targets, swallow popup empty-area clicks,
#   and move self-damage unknown-target fallback into the parser so repeated
#   dungeon entries do not lose DPS/BossHP before GUI fallback can run.
# v2.5.0:
#   Make the Entity SAOMenu fisheye backdrop non-click-through so clicks on
#   the blurred background no longer leak to the game. Also make combat panel
#   reveal decisions more robust: DPS can open from an existing live snapshot
#   even if the dirty flag was already consumed, and BossHP no longer suppresses
#   the first confirmed self-damage target as a hidden baseline.
# v2.4.40:
#   Stop Entity SAOMenu GPU windows from click-through leaking to the game or
#   panels behind them. Interactive GpuOverlayWindow instances now explicitly
#   clear inherited Win32 WS_EX_TRANSPARENT / WS_EX_NOACTIVATE styles after
#   GLFW window creation, complementing the existing sticky GLFW hint reset.
# v2.3.0: GPU SDF shader pipeline for SkillFX (ring + beam + glow as a
# single fragment-shader pass). Replaces the old PIL/numpy compose that
# cost 60-90 ms per frame on the render worker; the GPU path runs the
# whole layer set in ~2-5 ms on the integrated GPU. Caption sprites
# still PIL (cached statically). Falls back to the CPU path on any
# pipeline failure. Set `SAO_SKILLFX_GPU=0` to force the legacy CPU
# path for diagnostics.
USE_GPU_SKILLFX = True
# v2.4.37:
#   Fix Entity HP overlay border/STA clipping caused by using SetWindowRgn
#   as an input hit region; keep the full window visible and use dynamic
#   click-through outside HP/ID hit zones instead. Also harden packet int32
#   varint decoding against over-wide combat AOI attr encodings.
# v2.4.36:
#   Fix Bosshp targets.
# v2.4.33:
#   Second pass of cython acceleration on the recognition loop and the
#   protobuf parser hot helpers, removing the remaining Python-side
#   "calculation stalls" reported on slower CPUs.
#     - `_sao_cy_uihelpers` adds `breath_offsets()` (60-fps float HUD sin
#       offsets) and `compute_skillfx_layout()` (window/viewport/slot rect
#       geometry for the BurstReady overlay).
#     - `_sao_cy_packet` adds `varint_to_int64`, `varint_to_int32`,
#       `decode_string_from_raw`, `decode_dirty_energy_value`,
#       `is_sane_attr_stamina_max`, `normalize_season_medal_level`,
#       `level_extra_source_priority`, `attrs_match_monster_hint`. These are
#       per-packet hot paths in `packet_parser.py`.
#     - `sao_gui._breath_step` and `_get_skillfx_layout` are now thin entries
#       that gather inputs and delegate; same for the corresponding parser
#       helpers.
#   Microbench (cp311, x64): breath_offsets ≈ 0.06 µs / call,
#   compute_skillfx_layout(9 slots) ≈ 5.2 µs / call,
#   decode_dirty_energy_value ≈ 0.10 µs / call.
# v2.4.32:
#   `dev_publish.py` / `dev_publish_gui.py` now smart-detect the recent
#   cython refactor pattern (added/edited/removed `_sao_cy_*.pyx` and the
#   matching `.pyd` artefacts). Highlights:
#     - `git_changed_files_with_status()` separates added / modified /
#       deleted, so deletes feed `manifest.removed_files` instead of being
#       silently dropped.
#     - `auto_rebuild_cython_if_stale()` detects a `.pyx` whose `.pyd` is
#       missing or older and runs `build_cython_ext.py build_ext --inplace`
#       in-place, then folds the freshly built `.pyd` into the change set.
#     - New `.pyx` sources unregistered in `build_cython_ext.py` raise a
#       visible warning during diagnose.
#     - Deleted `.py` / `.pyx` / `.pyd` / data files become orphan-cleanup
#       hints (`runtime/<x>.pyc`, `runtime/<x>.<EXT_SUFFIX>`, raw rel-paths)
#       embedded both in `manifest.removed_files` and a zip-level
#       `__remove_files__.json` sidecar.
#     - Pure-delete publishes still produce a `runtime-delta` zip with the
#       sidecar so the client can clean up without a body of new files.
#   `update_apply.py` reads either signal, backs each orphan into the
#   per-version `backup/__removed__/` tree before deletion, and refuses
#   paths that escape `base/` or target the launcher/update exe.
# v2.4.31:
#   New `_sao_cy_uihelpers` extension. The recognition-loop / panel-float
#   pure-logic helpers in `sao_gui.py` (`_pick_burst_trigger_slot`,
#   `_panel_float_shared_tick` sin offsets, `_format_level_text`,
#   `_normalize_watched_skill_slots`, `_is_dead_state`, `_boss_monster_usable`,
#   `_session_int`, `_format_session_power`) now route through cython.
#   Side-effecting parts (Tk/PIL widget calls, monster.is_dead revive flip)
#   stay in Python; the cython side returns intent flags only.
# v2.4.30:
#   Move `dps_tracker.SkillStats` / `EntityStats` and the per-tick snapshot
#   builder into `_sao_cy_combat`. `add_damage`, `add_heal`, `add_taken`,
#   `to_dict`, `build_entity_snapshot`, and big-hit FX tier classification now
#   run as Cython `cdef class` methods with C-typed fields. The Python module
#   re-exports the names so external imports stay stable.
# v2.4.29:
#   Fix DPS/BossHP not displaying stably and counting phantom HP/monsters after
#   map switches. SyncNearEntities Disappear of non-Dead types (FAR_AWAY,
#   REGION, TELEPORT, ENTER_VEHICLE, ENTER_RIDE) now evicts monsters from the
#   parser cache instead of letting them linger. Soft scene transitions and
#   restarts purge monsters whose `last_update` is older than 15 / 30 s, and
#   Entity + WebView soft-scene paths clear `_bb_recent_targets` /
#   `_bb_last_target_uuid` so the next damage event repopulates the boss bar
#   with the live target.
# v2.4.27:
#   Make Cython accelerators mandatory instead of optional: packet/combat,
#   pixel premultiply/alpha, packet capture frame parsing, and SkillFX math
#   kernels now fail fast if the matching _sao_cy*.pyd is missing. Runtime
#   Python/NumPy/Numba fallbacks were removed from those hot paths.
# v2.4.26:
#   Force Entity player UID/POWER panels onto the GPU overlay path, prewarm
#   their GLFW windows asynchronously, and avoid Tk fallback redraws that made
#   the first SAO menu open and large Session Players scrolls stutter.
# v2.4.24:
#   Keep the Session Players GPU panel attached to the SAO menu shell,
#   make Entity BossHP fixed-position/click-through, and treat the first
#   BossHP target sample as a hidden baseline so HP-stable targets do not
#   briefly pop before the auto-hide rule applies.
# v2.4.23:
#   Default the Entity Session Players panel to the GPU painter when the
#   shared GPU overlay gate is available, keep DPS/BossHP fade-out on exit,
#   and classify service-declared or already-registered monsters before the
#   player-like UUID fallback so repeated-instance and overworld targets keep
#   driving DPS/BossHP panels.
# v2.4.22:
#   Smooth the Entity Session Players panel by opening it from a collapsed
#   height only on first reveal. Repeated panel/menu button clicks now keep the
#   already-visible Session Players panel steady and only refresh row data,
#   matching the player info panel behavior in Entity and WebView.
# v2.4.21:
#   Align Session Players open timing with the player info panel in both
#   WebView and Entity UI. The right-side WebView panel now uses the same
#   one-second reveal cadence as leftInfo, and Entity starts the Session
#   Players animation alongside the player panel instead of racing ahead.
# v2.4.20:
#   Restore the Session Players open animation after making the panel
#   persistent. WebView now restarts the right-side panel animation whenever it
#   is shown/refreshed, instead of relying on the first `show` class transition.
# v2.4.19:
#   Make Session Players a persistent show/refresh panel instead of a toggle in
#   both Entity and WebView Saomenu, so it opens together with player info and
#   cannot disappear on repeated clicks. Session Players column headers now use
#   the normal UI font to avoid the small SAO-font stroke artifact near NAME.
# v2.4.18:
#   Keep WebView Session Players on its right-side Saomenu layout and reveal it
#   whenever menu data syncs while the menu is open. Entity Session Players now
#   defaults to the embedded panel below player info and keeps the live panel
#   reference so refreshes cannot early-return before painting.
# v2.4.17:
#   Restore the Saomenu Session Players panel in Entity and WebView UI, while
#   keeping the v2.4.16 GPU/lazy rendering optimizations. WebView -> Entity
#   switching now launches a fresh entity process after saving ui_mode/game_cache
#   so stale WebView/.NET window state cannot corrupt or block the new Saomenu.
# v2.4.15:
#   Restore Saomenu fisheye enter/exit dynamics in both Entity and WebView:
#   Entity now requests the GPU fade-out state instead of destroying the
#   overlay immediately, while WebView keeps its WebGL loop alive through the
#   close transition and animates blur/scale/distortion strength. Session
#   player panels now follow the menu motion with lightweight panel/row
#   animations.
# v2.4.14:
#   Fix Saomenu/session-player scalability and overlay input regressions.
#   Entity and WebView session-player panels now render large login-session
#   player lists lazily, with light version signatures so unchanged player
#   data no longer forces full sorting/DOM/Tk rebuilds when the menu is open.
#   Entity HP auto-hide now clips the native input region to the ID plate
#   instead of toggling whole-window click-through, keeping the ID panel
#   visible/clickable while hidden HP/STA pixels pass mouse input through.
#   Entity Saomenu fisheye now performs distortion/HUD shading in the final
#   GLFW GPU window and avoids the old per-frame FBO readback/CPU composite
#   path; normal screenshots can include the effect when the game-window
#   DXGI capture source is available.
# v2.4.13:
#   Add in-panel DPS detail mode for both Entity and WebView UI. The detail
#   view reuses live/report per-entity skill breakdowns, supports returning to
#   the compact list, and persists the resizable detailed panel size.
#   Saomenu now exposes an in-session player list sourced from the active
#   packet session, with WebView right-side placement and Entity menu-column
#   parity. BossHP/DPS packet display stability was hardened around revive /
#   server-switch edge cases, hidden HP panels stop intercepting clicks, and
#   Entity BossHP now mirrors the WebView main/secondary panel split.
#   Packet parsing gained a _sao_cy_packet helper for stable byte-level
#   decode/scan hotspots (made mandatory in v2.4.27), and dev_publish now
#   makes smarter full-package vs incremental-package decisions when the spec
#   changes. Skill names were refreshed from current SRDPS/SRLOGS Chinese
#   short-name tables so DPS skill breakdowns no longer show stale placeholders.
# v2.4.11:
#   Clean ABI-sensitive runtime dependency folders before applying full/runtime
#   refresh updates so stale NumPy/OpenCV files cannot make cv2 reject ndarray.
# v2.4.10:
#   Harden packaged Hide & Seek OpenCV calls with array diagnostics/fallbacks
#   while investigating onedir-only cv2/numpy runtime mismatches.
# v2.4.9:
#   Guard self-identity updates behind server-confirmed UID ownership so nearby
#   players cannot overwrite the cached/player-panel UID, name, level, or job.
# v2.4.7:
#   Fix packaged onedir Hide & Seek click execution. The hide_seek worker now
#   sets the same per-thread PerMonitorV2 DPI context as recognition, and mouse
#   clicks move the visible cursor before sending separate down/up packets so
#   frozen builds reliably click the matched screen coordinate.
# v2.4.6:
#   Align encounter reset behavior with upstream counters: same-dungeon
#   restarts now defer DPS/BossHP reset until the next real self damage, and
#   idle report generation no longer clears live totals during long mechanics.
# v2.4.5:
#   Preserve live DPS/BossHP during same-dungeon map/layer transitions and
#   long boss mechanic gaps. Parser now distinguishes hard scene resets from
#   soft in-instance transitions, while combat panels keep a longer idle
#   window before fading/resetting.
# v2.4.4:
#   Add Cython combat helpers for UUID classification, damage fallback,
#   self-attacker detection, and DPS target gating. As of v2.4.27 the compiled
#   helpers are mandatory so ABI mismatches fail fast instead of silently
#   returning to Python hot paths.
# v2.4.3:
#   Fix overworld / city-edge combat targets that only report HP loss or use
#   non-standard entity suffixes: DPS now counts player damage to non-player
#   combat targets, while BossHP only displays once usable packet HP exists.
#   Keep the packet hot path allocation-light and verify the existing Cython
#   pixel accelerator build stays healthy.
# v2.4.2:
#   Follow upstream DPS-counter behavior for broad-map combat targets:
#   player damage to any non-player target now counts for DPS, and non-player
#   entities carrying monster HP / break / hate attrs are tracked for BossHP.
# v2.4.1:
#   Add more Cython annotations and optimizations to the hotspots, 
#   further reducing CPU usage and improving frame stability, 
#   especially on lower-end machines.
#   Fix entity DPS/BossHP scene reset parity and dungeon sub-map detection:
#   same-scene retries and instanced mini-map layer changes now clear stale
#   BossHP/DPS state before accepting the next damage event.
# v2.4.0:
#   New Cython style for CPU optimized hotspots.
# v2.3.22:
#   Same-scene retry fixes for DPS/BossHP and HP hidden-click region parity.
# v2.3.20:
#   Entity menu / HP / DPS / BossHP GPU-overlay performance pass.
# v2.3.18:
#   General performance improvements and bug fixes.
# v2.3.17:
#   Fisheye worker: retry up to 3× (2 ms each) to acquire WGL lock, preventing worker starvation.
# v2.3.16:
#   Minor bug fixes and performance improvements.
# v2.3.15:
#   Entity GUI General fix.
# v2.3.14: 
#   Entity mode HUD panels decoupling, try to make render FPS
#   more stable by isolating the heavy works in a separate lane.
# v2.3.13:
#   Fiseye now use DXGI screenshot instead of mss.
# v2.3.10+:
#   HUD improvements.
# v2.3.9:
#   Packet capture reliability improvements.
# v2.3.8:
#   Fix SkillFX silently falling back to CPU/PIL in onedir packaged build.
#   Root cause: XiaoACTUI.spec 从未将 ``shaders/`` 目录加入 datas 清单,
#   所以打包后 ``shaders/skillfx.frag`` 不存在. 首个调用 SkillFX 的
#   渲染线程调 ``get_skillfx_pipeline`` → ``_load_fragment`` 抛
#   FileNotFoundError → ``_tls.failed = True`` (永久标记) → 后续所有
#   compose_frame 都走 PIL fallback. 开发环境下 __file__/项目根下存在
#   shaders/ 所以看不出问题 — 仅冻结后才现.
#   Fix:
#     1) XiaoACTUI.spec 加 ('shaders', 'shaders') 进 datas.
#     2) skillfx_pipeline._resolve_shader_path 增加 PyInstaller 感知 —
#        依次检查 HERE/, sys._MEIPASS/, exe 同级、exe/_internal/.
#     3) get_skillfx_pipeline 单独捕获 FileNotFoundError, 打印出期望
#        路径, 下次丢包能从 stdout 直接看出是资源问题还是 GL 问题.
# v2.3.7:
#   Continuation of v2.3.6: BossHP 反复刷/最后消失 (重连路径误识).
#   v2.3.6 关住了跨 addr 服务器切换路径, 但同服重连路径仍然接受
#   `_try_identify` (含松散 c3SB) 或 `_looks_like_frame_start`
#   (4 字节 BE 头 ∈ [6, 999999]) 作为重连签名. 后者误中率约
#   0.023%/包, 繁忙连接上每秒就能误触发, 每次都重置 _next_seq=-1
#   并调用 _on_server_change → 清掉 BossHP 目标. 修复:
#     1) 重连路径改为仅接受 _identify_strict (FrameDown 嵌套 c3SB
#        或 LoginReturn 0x62), 丢弃松散 c3SB 和 帧头启发式判定.
#     2) 增加 3 秒冷却窗口 — 真重连是单次事件, N 秒内重复触发
#        一律视为误识, 避免任何残留误识路径造成刷屏循环.
# v2.3.6:
#   Fix BossHP overlay rapidly flickering / popping then disappearing.
#   Root cause: the cross-addr server-switch detector at packet_capture
#   line 350 reused the LOOSE _try_identify (which returns True for any
#   payload containing the 4-byte literal 'c3SB'). Any non-game TCP
#   stream from the client (chat, social, voice, CDN) whose payload
#   happened to contain those bytes hijacked _server_addr -> fired
#   _on_scene_change -> wiped _bb_last_target_uuid -> BossHP hidden.
#   The next real game packet then had addr != _server_addr again ->
#   flipped back -> ping-pong, eventually stuck on a non-game socket
#   ('过一会不出来了'). Now _try_identify is split into _identify_strict
#   (FrameDown[type=6] nested c3SB or LoginReturn[0x62/type=3]) and
#   _identify_loose (c3SB literal). Server switch path requires strict;
#   initial identification still uses loose (no anchor exists yet);
#   same-server reconnect keeps loose since v2.3.4's _seq_anomalous
#   gate already rules out mid-stream segments.
#   Also: surface SkillFX compose path on first frame (GPU vs CPU/PIL
#   fallback) so '是不是返回CPU了' can be verified from stdout.
# v2.3.5:
#   Fix updater modal hard-crashing the app on rapid clicks (especially
#   in onedir packaged mode). The 立即更新/重启应用/稍后/跳过 buttons in
#   the menu webview updater banner had no debounce: a fast double-click
#   could fire multiple concurrent pywebview JS-bridge calls into the
#   EdgeWebView2 COM apartment while the first call was still importing
#   sao_updater (cold import in onedir takes 200-500ms), occasionally
#   crashing the WebView2 process. Added a hard JS-side busy-lock with
#   pointer-events:none + disabled flags + 1.5s safety timeout; lock is
#   released either by the next state push from Python or the timeout.
# v2.3.4:
#   Fix BossHP overlay randomly disappearing mid-fight. The same-server
#   reconnect detector accepted any out-of-order TCP packet whose payload
#   contained the 4-byte 'c3SB' literal (common in ZSTD'd game data /
#   names / buff IDs) as a 'reconnect', triggering scene-change cleanup
#   that hides BossHP and resets _bb_last_target_uuid. Now require the
#   strong seq-anomaly signal (>1MB both directions = guaranteed new
#   ISN) for ALL reconnect paths; mid-stream reorder packets within the
#   TCP window can never falsely trigger again.
# v2.3.3:
#   Fix GPU SAO popup menu hard crash (PyEval_RestoreThread NULL tstate
#   fast-fail) on the second click. Tk's Tcl mainloop on Windows runs an
#   implicit PeekMessage(NULL,...)+DispatchMessage pump that captured
#   GLFW window messages and dispatched them to GLFW's WndProc, firing
#   our mouse callback in a re-entrant context where touching any Tk API
#   (root.after_idle) corrupts Tcl interpreter state mid-dispatch and
#   crashes the next mainloop checkpoint. Cb now only enqueues hits to
#   a deque; a polled drainer on a top-level Tk after() callback runs
#   the actual handlers safely.
#   Fix infinite same-server-reconnect loop that locked DPS/HP at zero.
#   Replay window after reconnect now filters out pre-reconnect packets
#   (old TCP ISN seqs) which previously polluted _next_seq and made the
#   next live packet trigger another reconnect. Replay only packets
#   within ±1MB of the new ISN.
# v2.3.2:
#   Fix Gil compound deadlock when the GPU render thread tries to acquire the GIL,
#   while the main thread is waiting for the render thread to join during shutdown.
#   Fix packet reconnection logic that could cause the DPS and HP won't update in 
#   same dungeon.
# v2.3.0: 
#   GPU-accelerated rendering pipeline for all ULW overlays, replacing the old 
#   PIL-based CPU rendering + DirectX upload path. 
#   This should significantly reduce CPU usage and eliminate stutter on slower machines, 
#   especially for the more complex BossHP overlay.
# v2.2.16:
#   Combat-CPU + SkillFX framerate. Two changes:
#   1. CPU-affinity pinning of render lanes is now opt-in via
#      SAO_RENDER_AFFINITY=1 (default OFF). On hybrid CPUs (12th-gen+ /
#      14900HX P-core+E-core) the always-on pin parked SkillFX on a
#      2.5 GHz E-core and capped its compose at ~30 fps; letting Windows
#      scheduler migrate it to a P-core under turbo restores 60 fps.
#   2. Scheduler combat-load tier: while SkillFX (or any heavy panel)
#      is active, idle entity panels throttle from ≈10 Hz to ≈6 Hz so
#      the burst animation and the menu open animation get the spare
#      CPU/render-lane bandwidth. Animating panels still tick every
#      frame.
#   Note: the floating menu button tearing during fisheye is structural
#   to Tk widgets on a chroma-key transparentcolor Toplevel (DWM does
#   not vsync those composites). A real fix requires moving the menu
#   buttons into the ULW HUD as PIL sprites — deferred (would lose Tk
#   focus / IME / native click).
# v2.2.15:
#   Fix HP/DPS clock + NErVGear pulse + ELAPSED counter freezing on idle.
#   v2.2.14's per-tick `_idle_committed` short-circuit assumed every panel
#   stops drawing once tweens settle, but HP renders a system clock and
#   id-pulse continuously and DPS renders an elapsed counter every second.
#   The scheduler's idle downsampling (~10–20 Hz when `_is_animating()` is
#   False) keeps the CPU savings; only BossHP — which truly is static at
#   full HP — keeps the per-tick gate.
# v2.2.14:
#   Idle CPU reduction (target webview parity ~2-3% on i9-14900HX).
#   - Fix BossHP._is_animating() (was hard-coded `return True`, forcing 60 Hz
#     compose+commit on a steady boss bar at full HP — biggest single drain).
#   - Add per-panel idle short-circuit in HP / DPS / BossHP _tick(): once a
#     steady frame is committed and nothing is animating, skip compose+submit
#     until state changes again. Combat / fades / tweens unaffected.
#   - Scheduler: lower idle-downsample threshold 70% → 30% of frame budget AND
#     unconditionally throttle non-animating panels to ~10–20 Hz regardless of
#     CPU headroom. Animating panels keep full 60 Hz.
#   No visual effects removed.
# v2.2.13:
#   Add HP pannel and BossHP to GPU-accelerated.
# v2.2.12:
#   Fully GPU-accelerated SAO Menu HUD via per-pixel-alpha layered window.
#   Eliminates fullscreen chroma-key recomposites and tearing on the
#   floating menu. Off-thread compose on the heavy render lane keeps the
#   Tk main thread free of HUD draw work.
# v2.2.11:
#   Fully GPU-accelerated rendering pipeline for ULW overlays, replacing the old PIL-based CPU 
#   rendering + DirectX upload path. 
#   This should significantly reduce CPU usage and eliminate stutter on slower machines, 
#   especially for the more complex BossHP overlay.
# v2.2.10:
#   Fix pannel rendering not respecting the render FPS target, causing stutter on slower machines. 
#   Create sub-pixel paste / bar-width helpers for ULW overlays.
# v2.2.9:
#   Profiled compose_frame on a worker thread 
#   — it averaged 33 ms steady, 80+ ms during ENTER, with cold-start spikes to 200 ms. 
#   That maps directly to the user's "only 3 frames" symptom on slower machines.
# v2.2.8:
#   使用显示器刷新率而非固定 60 Hz 作为调度器默认频率, 让高刷显示器的动画更流畅。
# v2.2.7:
#   Fix GL Cache caused upside-down rendering in skillfx.
# v2.2.6:
#   Fix GPU Cache caused upside-down rendering in menu pannels.
# v2.2.5:
#   GPU Cache masks/overlay GPU rendering.
# v2.2.4:
#   修正阴影残留。
# v2.2.3:
#   [entity HP 右侧外观修正] 把 HP 条右侧壳层改回接近 webview 的结构:
#   xt_right 不再整块实心铺满，而是左半实体、右半渐隐到底层 cover；
#   同时恢复 number_xt 独立数值底板，避免右侧视觉发闷、发厚。
#   [HP / BossHP 底板统一] 把 webview / entity 的 ID plate、HP cover、
#   BossHP cover 底板统一到同一套冷灰白层级；普通 HP 去掉残留灰绿色，
#   BossHP 下调纯白度并拉开 cover / box 层次，避免整片白成一体。
# v2.2.2:
#   1) [菜单白板统一] 把 webview / entity 主菜单的圆形按钮、左侧信息板、
#      子菜单卡片，以及 commander / autokey / boss raid 编辑器的灰绿底板
#      统一收敛到 sao_alert 那套冷白 + 轻灰层次，降低纯白刺眼感，并把
#      子菜单 hover 从整块金色改成更克制的浅金 / 冷青过渡。
#   2) [entity HP fade 修复] 恢复 HP 组件隐藏时两侧 XT 外框壳层的
#      fadeout。此前 group fade 只覆盖名义 48px HP box，number_xt 壳层
#      底部超出 box_rect，隐藏时会留下外框残影；现在改为覆盖整个 XT shell.
# v2.2.1:
#   修复 entity 模式下 HP/BossHP/DPS overlay 隐藏后阴影残留:
#     _apply_panel_style() 在设置面板里用 SetClassLongW(CS_DROPSHADOW)
#     修改了整个 Tk 进程的窗口类, 导致同进程所有 Toplevel (包括 ULW
#     overlay) 都被 DWM 加上系统阴影矩形. ULW bitmap 淡出到透明后窗口
#     尚未 destroy, DWM 阴影仍可见. 修复: HpOverlay / BossHpOverlay /
#     DpsOverlay.show() 创建窗口后立即调用
#     DwmSetWindowAttribute(DWMWA_NCRENDERING_POLICY, DWMNCRP_DISABLED)
#     让 DWM 对这三个 overlay 窗口不渲染非客户区 (含阴影).
# v2.2.0:
#   1) [继续修 Hide & Seek] webview 下持续 alert 之前会被普通 identity 通知
#      或 9s auto-dismiss 计时器误关 → "过一会就消失". 现在 hide_seek
#      alert 用 alert_kind='hide_seek' 标记, _hide_identity_alert_window
#      在引擎仍 active 时直接拒绝关闭, _sync_identity_alert 也不再用
#      identity 推送覆盖 hide_seek alert.
#   2) [全面 UI 重设计] 把所有面板的 "灰绿" 配色 (rgb(207,208,197) /
#      rgb(60,62,50) / rgb(188,190,178) ...) 全部换成 SAO Alert 的
#      "纯白 + 略灰" 扁平高科技配色 (rgba(255,255,255,X) / rgb(100,99,100) /
#      rgb(140,135,138)). 透明度 (alpha) 一律保留原面板设置, 没动.
#      影响:
#        - web/menu.html, dps.html, boss_hp.html, hp.html, commander.html,
#          autokey_editor.html, raid_editor.html
#        - sao_gui_dps.py, sao_gui_bosshp.py, sao_gui_hp.py (entity ULW 面板)
#      DPS/BossHP 面板新增青色切角外框 (基于参考图):
#        - DPS: 右上 + 左下 22px 切角, 顶部青色高亮 + 底部 cyan→amber 渐变线
#        - BossHP: 八边形切角 (四角各切 14px), 主条 + 附属单位统一风格
#      其他面板暂保留圆角, 后续轮次按反馈微调.
# v2.1.20:
#   修复"自动躲猫猫"两个回归 (检测算法本身一行未动):
#     1) [entity / webview 共同] HideSeekEngine._assets_dir 之前用
#        os.path.dirname(__file__) 拼接 'assets', 在 PyInstaller onedir
#        打包 (runtime/ 子目录) 下永远落到 runtime/assets/ — 此目录在
#        build_release.bat 把 assets/ 提升到 exe 顶层后并不存在, 导致
#        5 个 template (1.png ~ 5.png) 全部 cv2.imread 失败 → 引擎线程
#        正常运行但 _match_template 永远没结果, 表现为 "启动了不会有效果".
#        改为优先 config.BASE_DIR/assets, 再 fallback 模块同级 assets,
#        兼容源码 / onedir / 旧 onefile 三种布局.
#     2) [webview] JS 桥 toggle_hide_seek 之前会再 spawn 一个 daemon
#        thread 去跑 _toggle_hide_seek, 而 pywebview 的 JS callback 本身
#        就在 worker thread; 嵌套两层非主线程后, _show_identity_alert_window
#        内部的 alert_win.show() / pythonnet form.Invoke 与 evaluate_js
#        会在两个不同的非 GUI 线程并发触达 WebView2 → 部分机器上 native
#        crash, 表现为 "启动一下会自己闪退". 改为直接同步调用, engine
#        自己的后台线程不变.
# v2.1.19:
#   同 v2.1.18, 版本号补丁升级.
# v2.1.18:
#   1) [核心] 修复切换场景服务器后 DPS / boss 血条 / 全量同步全部失效的根因:
#      a) packet_parser.reset_scene 之前保留了 _current_uuid (旧场景的 entity
#         UUID), 但游戏在新场景里给玩家分配的是新 UUID, 导致后续 SCDeltaInfo 里
#         attacker_uuid != _current_uuid → attacker_is_self 永远 False →
#         DPS tracker 把自己的伤害全部当成"别人的", 自己的条不出, boss bar
#         target 也永远不会被采纳. 现在 reset_scene 会清零 _current_uuid,
#         由下一个 SyncToMeDeltaInfo 自然重新填充;
#      b) packet_capture 之前在 server-change / 同服重连的瞬间会把切换前后
#         几个 TCP 段直接丢弃 (旧 addr 的被短路过滤, 新 addr 的在 _try_identify
#         成功之前也被过滤), 这正好把关键的 SyncContainerData / SyncToMeDelta
#         首包丢掉, 导致 "切场景/重新上线触发不了 full sync". 现在维护一个
#         24-pkt 环形缓冲, 任何 server-change / reconnect / 首次识别成功后
#         都会按 seq 升序回放属于该 addr 的缓存包.
#   2) 修复 v2.1.17 webview→entity 持久化仍失效的根因: SettingsManager 在
#      sao_webview 内同时存在两个独立实例 (self.settings 与 _cfg_settings_ref),
#      各自持有不同的内存快照. v2.1.17 的预存逻辑先用 self.settings 写入
#      ui_mode='entity' 后, 紧接着 _persist_cached_identity_state 又通过
#      _cfg_settings_ref.save() 把 stale 的 ui_mode='webview' 覆写回磁盘.
#      现在统一通过 _cfg_settings_ref 写 ui_mode + game_cache, 一次性 save();
#   3) _persist_cached_identity_state 改为优先读取 GameState 上的实时字段
#      (gs.player_name / profession_name / level_base / hp_* / stamina_*),
#      实例变量仅作兜底. 之前实例变量在菜单未打开/recognition 未跑完时是
#      stale 的, 导致即使 webview 内 GameState 已经收到 SyncContainerData,
#      切到 entity 时仍然只能写出空名/0 级;
#   4) entity (sao_gui) 必须先 subscribe(_on_game_state_update) 再 load_cache,
#      否则 GameState.load_cache 内部对订阅者的初始通知会被丢弃,
#      entity 面板启动时无法显示 webview 切过来时持久化的角色名/等级/HP.
# v2.1.17:
#   1) 修复 webview 模式下 sao_alert 弹窗"跳两次"的视觉故障:
#      _show_identity_alert_window 出于冷启动 WebView2 竞态考虑会立即 +
#      350ms 各 push 一次, 但 alert.html 的 showAlert 每次都会重新触发
#      show 动画. 现在 JS 端基于 (title, body) 签名去重, 同一条 alert
#      仅播放一次入场动画;
#   2) 修复 boss raid 告警声音播放两次的问题: BossRaidEngine 通过
#      on_sound("boss_alert") 已经播了一次 Popup.SAO.Alert.mp3, 而
#      _show_identity_alert_window 默认还会再播 'alert' (同一个文件).
#      现在 _on_boss_alert_with_linkage 调用时显式 play_sound=False;
#   3) 修复 onedir 冷启动时 BossHP / DPS 面板不出现的问题: 在
#      _on_webview_started 内追加 4s/10s/16s 的延迟兜底, 若 boss_hp_win
#      仍未可见就重新 show 并补做 click-through 设置, 同时 DPS 重新
#      套用穿透样式;
#   4) 修复 webview→entity 热切换时角色信息丢失 + 退出后菜单模式没写回
#      的问题: _transition_with_animation 在销毁 webview 之前先把
#      ui_mode 同步到磁盘 (切换到 entity 时立刻写 'entity'), 同时调用
#      _persist_cached_identity_state(save_now=True) 把 player_name /
#      level / profession / fight_point 立刻持久化, entity 启动后能直接
#      读取到, 即使后续 entity __init__ 因任何原因没保存 ui_mode 也能
#      在下次启动正确进入 entity 菜单.
# v2.1.16:
#   1) 多核渲染优化: overlay_render_worker.py 提高高核心系统的渲染通道数
#      (8 核以上系统从 4 通道提升至最多 6 通道), 多面板 (DPS+BossHP+Burst+
#      menu) 可真正并行 compose 而非排队;
#   2) Windows 线程亲和: 渲染通道线程 + CPU 任务池线程通过
#      SetThreadAffinityMask 固定到 cores 2.., 减少 context-switch 抖动,
#      改善 L1/L2 cache 命中, 让 Tk 主线程独占核心 0/1;
#   3) 调度器自适应限速: overlay_scheduler.py 在 avg_frame_ms ≥ 13ms 时
#      把空闲面板的 tick 频率从 20 Hz 降到 10 Hz (IDLE_EVERY_N_OVERLOADED=6),
#      保护动画中的面板 60 Hz 预算;
#   4) 调度器新增可选 visibility_fn (向下兼容): 隐藏面板可直接被跳过,
#      避免 winfo 检查与 GIL 抢占;
#   5) 进程优先级: main.py 启动时把进程提升到 ABOVE_NORMAL,
#      防止重战斗时被后台程序抢占时间片导致掉帧;
#   6) dev_publish 工具: 本地发布改为可选 (CLI --no-local-publish + GUI 复选框),
#      并修复 GUI 全量包构建未刷新 release 布局导致 SHA256 与上版相同的问题.
# v2.1.13:
#   1) 抓包层回退至 v1.3.1 基底并补齐三处关键 TCP 重组缺陷：
#      a) 识别首包不喂入 TCP 流 → SyncContainerData / SyncNearEntities 丢失；
#      b) 无 TCP 重传段过滤 → 已消费 seq 重入缓存, 加速 300 条溢出；
#      c) 无缺段跳跃 → pcap 丢失一段后 _next_seq 卡住, 所有后续段堆满缓存
#         触发溢出, DPS 伤害事件与全量角色同步数据全部丢失;
#   2) 新增 GAP_SKIP_SEC=2.0 缺段超时跳跃 (参考 C# SRDPS ForceResyncTo),
#      缓存有段但 2 秒未消费时自动跳过间隙恢复后续数据；
#   3) _extract_frames 新增帧对齐修复扫描, gap-skip 后自动重定位帧边界；
#   4) 诊断行新增 gap_skip= / overflow= 计数便于排查 pcap 丢包；
#   5) 本版以 full-package + force_update + minimum_version=2.1.13 推送。
# v2.1.11:
#   1) 抓包层回退到 v1.3.1 的源地址识别策略: 移除 _infer_server_endpoint
#      方向推断 (私网/临时端口/小端口 启发式), 改为始终用包的源地址
#      (src_ip:sport) 标识游戏服务器. 修复 VPN/加速器等双私网环境下方向
#      推断失败→ _server_addr 被设为客户端地址→所有下行包被丢弃的问题;
#   2) 保留 v2.1.8+ 的候选帧缓冲回放、空闲重识别、game-frame 回退识别;
#   3) 本版以 full-package + force_update + minimum_version=2.1.11 推送。
# v2.1.10:
#   1) TCP 重组层修复重传段缓存泄漏：seq 已消费的段不再入 cache，
#      杜绝 "TCP cache overflow (301), reset" 导致关键同步包丢失；
#   2) Alert 弹框从 4× _push (即时+0.12s+0.32s+0.72s) 缩减为 2×
#      (即时+0.35s)，修复弹框重复显示三次的视觉问题；
#   3) 本版以 full-package + force_update + minimum_version=2.1.10 推送。
# v2.1.9:
#   1) parser 在 self UID 未确认前缓存并回放早到的 0x16 / 0x2E 自身同步包，
#      修复 EnterGame 较晚时角色名 / 基础等级 / 赛季等级 / 技能 CD 与 slot
#      长期缺失或显示 unknown；
#   2) 本版以 full-package + force_update + minimum_version=2.1.9 推送，
#      强制所有旧版本升级。
# v2.1.8:
#   1) 抓包层在识别游戏服务器前先缓冲并回放候选下行首包，修复首个
#      0x15/0x16 身份同步被丢弃后角色名 / 基础等级 / 赛季等级长期为空；
#   2) 本版以 full-package + force_update + minimum_version=2.1.8 推送，
#      强制所有旧版本升级。
# v2.1.7:
#   1) SyncContainerData 在 pb2 缺失 / 解析失败时回退 mini 解码，恢复
#      UID / 角色名 / 等级 / 职业等关键身份字段；
#   2) webview HUD 改为跟随游戏窗口所在显示器的 DPI / 几何，修复高 DPI /
#      多显示器下 STA / HP 区域漂移；
#   3) _set_dpi_aware 统一复用早期 PerMonitorV2 提升逻辑；
#   4) 本版以 full-package + force_update + minimum_version=2.1.7 推送，
#      强制所有旧版本升级。
# v2.1.6:
#   1) 识别线程设置 per-thread PerMonitorV2 DPI，修复 onedir/webview 下
#      GetClientRect 返回逻辑像素导致 STA 裁剪坐标偏移、始终 OFFLINE；
#   2) update.exe.new 替换增加 5 次重试 + 0.5s 间隔，处理目标被瞬时锁定
#      的 PermissionError，并在失败后清理 .promoting 残留文件；
#   3) PacketParser 启动时从 player_cache.json 预填充角色名/等级/职业，
#      修复中途启动（未经过登录/换图）时名字 UID 等级长期为空的问题；
#   4) _set_dpi_aware 回退从 SystemDpiAware(1) 改为 PerMonitorDpiAware(2)；
#   5) 本版以 full-package + force_update + minimum_version=2.1.6 推送，
#      强制所有旧版本升级。
# v2.1.2-m: 修复 sao_alert 同条 alert 4s 内重复触发只续展不重弹;
#           webview _maybe_show_update_popup 同步 sao_gui 的 downloading
#           静音 + alert 可见时跳过非 error 提示;
#           本版以 full-package + force_update + minimum_version=2.1.2-l
#           推送, 强制清理积压问题。
# v2.1.2-h: main.py bootstrap 把 EXE-dir 加入 sys.path → 修复 onedir 下
#           `from proto import star_resonance_pb2` ImportError (proto/ 被
#           build_release.bat 提升出 runtime/, 旧 sys.path 找不到);
#           同时 main.py 最早调用 promote_runtime_update_exe() 解决新
#           update.exe 不替换的问题; spec 显式 hiddenimport 抓包链路。
# v2.1.2-f: 彻底去掉 entity 识别循环的 _recognition_active 闸门 + 修复模块化布局下资源路径 (skill_names.json / fonts) + dev_publish 自动重建 update.exe 并注入增量包
# v2.1.2-e: entity 识别循环外层 recognition_ok/packet_active 总闸门完全去除，BurstReady / HP overlay / commander / identity 仅依赖自身数据检查
# v2.1.2-d: DPS/Boss HP 推送完全脱离 recognition gate (_push_packet_overlays); update.exe 无边框 + 圆角 + 60FPS 动画
# v2.1.2-c: packet_active 在识别到服务器后立即置 True，修复 DPS/Boss HP 不弹出
# v2.1.2-b: STA 识别只依赖 vision；entity 更新弹窗中文不再为方框；多个 alert 不再重叠
# v2.1.2-a: 同步当前源码整理并重新发布后缀版本增量包，延续 2.1.2 更新链路
# v2.1.2: entity SAO menu 常驻 60Hz HUD 调度; child-menu 刷新去重与状态合帧; updater 版本后缀比较修复
# v2.1.1-a: entity SAO menu 常驻 60Hz HUD 调度; child-menu 刷新去重与状态合帧
# v2.1.1: webview 更新提示不再被身份提示循环瞬时关闭; entity 更新面板中文字体修正; STA offline 状态同步修复
# v2.1.0: 远程更新链路、独立 update.exe、模块化 onedir 布局、发布工具、entity/webview 更新提示修正
# v2.0.1: entity 面板 webview 对齐、Overlay 异步渲染、Burst Ready 平滑度与透明线修复
# v2.0.0: entity/webview 双 UI、SAO 菜单与 HUD 新版打包/发布整理
# v1.3.1: 躲猫猫引擎：失败回退检测所有前置步骤; 线程崩溃自动恢复(resume); alert 持久显示修复
# v1.2.26: Commander 面板; 菜单滚动; 退出流程修复
# v1.2.25: 移除 level_adjust 模块依赖
# v1.2.24: 副本重开死亡单位重置; 升级时赛季等级优先级修复; 移除 level_adjust 模块
# v1.2.23: 深眠心相仪等级解析 (field 102); full CharSerialize dump on login; level_adjust override module

BASE_CLIENT_WIDTH = 1920.0
BASE_CLIENT_HEIGHT = 1080.0

VISUAL_RECT_SPECS: Dict[str, Dict[str, int]] = {
    "stamina_bar_visual": {"right": 1214, "bottom": 1050, "width": 250, "height": 10},
    "skill_slot_1": {"right": 720, "bottom": 1003, "width": 52, "height": 85},
    "skill_slot_2": {"right": 767, "bottom": 1002, "width": 47, "height": 83},
    "skill_slot_3": {"right": 816, "bottom": 1003, "width": 49, "height": 85},
    "skill_slot_4": {"right": 864, "bottom": 1003, "width": 49, "height": 90},
    "skill_slot_5": {"right": 911, "bottom": 1002, "width": 45, "height": 87},
    "skill_slot_6": {"right": 960, "bottom": 1003, "width": 49, "height": 89},
    "skill_slot_7": {"right": 1032, "bottom": 1009, "width": 72, "height": 119},
    "skill_slot_8": {"right": 1104, "bottom": 1012, "width": 73, "height": 124},
    "skill_slot_9": {"right": 1177, "bottom": 1007, "width": 74, "height": 119},
}

# Packet / watched slot numbers now match the on-screen boxes directly.
SKILL_SLOT_VISUAL_INDEX = {
    1: 1,
    2: 2,
    3: 3,
    4: 4,
    5: 5,
    6: 6,
    7: 7,
    8: 8,
    9: 9,
}


def get_visual_rect_spec(name: str) -> Dict[str, int]:
    return dict(VISUAL_RECT_SPECS.get(name, {}))


def get_skill_slot_visual_index(slot_index: int) -> int:
    try:
        slot_index = int(slot_index or 0)
    except Exception:
        return 0
    return int(SKILL_SLOT_VISUAL_INDEX.get(slot_index, slot_index))


def _spec_to_base_box(spec: Dict[str, int]) -> Tuple[float, float, float, float]:
    right = float(spec["right"])
    bottom = float(spec["bottom"])
    width = float(spec["width"])
    height = float(spec["height"])
    return (right - width, bottom - height, right, bottom)


def _union_base_boxes(spec_names: List[str]) -> Dict[str, float]:
    boxes = [_spec_to_base_box(VISUAL_RECT_SPECS[name]) for name in spec_names if name in VISUAL_RECT_SPECS]
    if not boxes:
        return {"x": 0.0, "y": 0.0, "w": 0.0, "h": 0.0}
    left = min(box[0] for box in boxes)
    top = min(box[1] for box in boxes)
    right = max(box[2] for box in boxes)
    bottom = max(box[3] for box in boxes)
    return {
        "x": left / BASE_CLIENT_WIDTH,
        "y": top / BASE_CLIENT_HEIGHT,
        "w": (right - left) / BASE_CLIENT_WIDTH,
        "h": (bottom - top) / BASE_CLIENT_HEIGHT,
    }


_SKILL_SLOT_NAMES = [f"skill_slot_{idx}" for idx in range(1, 10)]
_SKILL_BAR_ROI = _union_base_boxes(_SKILL_SLOT_NAMES)

# Skill-slot positions are pure functions of the game-client geometry, so we
# compute once per (client_rect / client_w x client_h) and reuse on every UI /
# vision tick. Bounded to a handful of entries because client geometry only
# changes when the game window is moved or resized.
_SKILL_SLOT_BBOX_CACHE: Dict[Tuple[int, int, int, int], List[Dict[str, Any]]] = {}
_SKILL_SLOT_CLIENT_CACHE: Dict[Tuple[int, int], List[Dict[str, Any]]] = {}

DEFAULT_ROI = {
    "identity": {"x": 0.010, "y": 0.910, "w": 0.200, "h": 0.060},
    "level": {"x": 0.010, "y": 0.925, "w": 0.100, "h": 0.040},
    "name": {"x": 0.085, "y": 0.930, "w": 0.120, "h": 0.030},
    "hp_bar": {"x": 0.330, "y": 0.932, "w": 0.340, "h": 0.036},
    "hp_text": {"x": 0.380, "y": 0.940, "w": 0.240, "h": 0.028},
    "stamina_bar": {"x": 0.330, "y": 0.957, "w": 0.340, "h": 0.036},
    "stamina_text": {"x": 0.530, "y": 0.968, "w": 0.130, "h": 0.018},
    "player_id": {"x": 0.230, "y": 0.968, "w": 0.100, "h": 0.020},
    "skill_bar": dict(_SKILL_BAR_ROI),
}

BAR_COLORS = {
    "hp": {"h_min": 45, "h_max": 160, "s_min": 25, "s_max": 255, "v_min": 60, "v_max": 255},
    "stamina": {"h_min": 8, "h_max": 50, "s_min": 50, "s_max": 255, "v_min": 80, "v_max": 255},
    "skill_cooldown": {"v_max_dark": 80, "s_max_gray": 40},
}

DATA_SOURCE_COMPONENTS = ("hp", "level", "stamina", "skills", "identity")

DEFAULT_DATA_SOURCE_MAP = {
    "hp": "packet",
    "level": "packet",
    "stamina": "vision",
    "skills": "packet",
    "identity": "packet",
}


def normalize_source_mode(mode: Any, default: str = "packet") -> str:
    text = str(mode or "").strip().lower()
    if text in ("ocr", "vision", "screen", "screen_vision"):
        return "vision"
    if text in ("packet", "network", "network_capture"):
        return "packet"
    return default


def normalize_source_map(raw_map: Any, legacy_mode: Any = None) -> dict:
    legacy = normalize_source_mode(legacy_mode, "packet")
    normalized = {key: legacy for key in DATA_SOURCE_COMPONENTS}
    normalized.update(DEFAULT_DATA_SOURCE_MAP)
    if isinstance(raw_map, dict):
        for key, value in raw_map.items():
            if key == "stamina":
                normalized[key] = "vision"
            elif key == "skills":
                normalized[key] = "packet"
            elif key in normalized:
                normalized[key] = normalize_source_mode(value, normalized[key])
    normalized["stamina"] = "vision"
    normalized["skills"] = "packet"
    return normalized


def anchored_rect_spec_to_pixels(
    spec: Dict[str, int], client_rect: Tuple[int, int, int, int]
) -> Optional[Tuple[int, int, int, int]]:
    if not spec or not client_rect:
        return None
    left, top, right, bottom = client_rect
    client_w = max(1, int(right - left))
    client_h = max(1, int(bottom - top))
    x2 = left + int(round(client_w * (float(spec["right"]) / BASE_CLIENT_WIDTH)))
    y2 = top + int(round(client_h * (float(spec["bottom"]) / BASE_CLIENT_HEIGHT)))
    width = max(1, int(round(client_w * (float(spec["width"]) / BASE_CLIENT_WIDTH))))
    height = max(1, int(round(client_h * (float(spec["height"]) / BASE_CLIENT_HEIGHT))))
    x1 = x2 - width
    y1 = y2 - height
    return (x1, y1, x2, y2)


def anchored_rect_spec_to_client_rect(
    spec: Dict[str, int], client_w: int, client_h: int
) -> Optional[Dict[str, int]]:
    if not spec or client_w <= 0 or client_h <= 0:
        return None
    x2 = int(round(client_w * (float(spec["right"]) / BASE_CLIENT_WIDTH)))
    y2 = int(round(client_h * (float(spec["bottom"]) / BASE_CLIENT_HEIGHT)))
    width = max(1, int(round(client_w * (float(spec["width"]) / BASE_CLIENT_WIDTH))))
    height = max(1, int(round(client_h * (float(spec["height"]) / BASE_CLIENT_HEIGHT))))
    return {"x": x2 - width, "y": y2 - height, "w": width, "h": height}


def get_visual_rect_bbox(name: str, client_rect: Tuple[int, int, int, int]):
    return anchored_rect_spec_to_pixels(VISUAL_RECT_SPECS.get(name, {}), client_rect)


def get_visual_rect_client_rect(name: str, client_w: int, client_h: int):
    return anchored_rect_spec_to_client_rect(VISUAL_RECT_SPECS.get(name, {}), client_w, client_h)


def get_skill_slot_rects(client_rect: Tuple[int, int, int, int]) -> List[Dict[str, Any]]:
    if not client_rect:
        return []
    cached = _SKILL_SLOT_BBOX_CACHE.get(tuple(client_rect))
    if cached is None:
        cached = _build_skill_slot_rects(client_rect)
        _SKILL_SLOT_BBOX_CACHE[tuple(client_rect)] = cached
    return [dict(item) for item in cached]


def _build_skill_slot_rects(client_rect: Tuple[int, int, int, int]) -> List[Dict[str, Any]]:
    rects: List[Dict[str, Any]] = []
    for idx in range(1, 10):
        visual_idx = get_skill_slot_visual_index(idx)
        name = f"skill_slot_{visual_idx}"
        bbox = get_visual_rect_bbox(name, client_rect)
        if not bbox:
            continue
        rects.append({
            "index": idx,
            "visual_index": visual_idx,
            "bbox": bbox,
            "spec": get_visual_rect_spec(name),
        })
    return rects


def get_skill_slot_client_rects(client_w: int, client_h: int) -> List[Dict[str, Any]]:
    cached = _SKILL_SLOT_CLIENT_CACHE.get((client_w, client_h))
    if cached is None:
        cached = _build_skill_slot_client_rects(client_w, client_h)
        _SKILL_SLOT_CLIENT_CACHE[(client_w, client_h)] = cached
    return [dict(item) for item in cached]


def _build_skill_slot_client_rects(client_w: int, client_h: int) -> List[Dict[str, Any]]:
    rects: List[Dict[str, Any]] = []
    for idx in range(1, 10):
        visual_idx = get_skill_slot_visual_index(idx)
        name = f"skill_slot_{visual_idx}"
        rect = get_visual_rect_client_rect(name, client_w, client_h)
        if not rect:
            continue
        rects.append({
            "index": idx,
            "visual_index": visual_idx,
            "rect": rect,
            "spec": get_visual_rect_spec(name),
        })
    return rects


def get_skill_bar_roi() -> Dict[str, float]:
    return dict(_SKILL_BAR_ROI)


DEFAULT_HOTKEYS = {
    "toggle_recognition": "F5",
    "toggle_auto_script": "F6",
    "boss_raid_start": "F7",
    "boss_raid_next_phase": "F8",
    "toggle_topmost": "F9",
    "hide_panels": "F10",
    "toggle_hide_seek": "F11",
}


def _get_config_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(__file__)


CONFIG_FILE = os.path.join(_get_config_dir(), "settings.json")

GAME_WINDOW_KEYWORDS = ["Star", "星痕共鸣"]
GAME_PROCESS_NAMES = ["star.exe"]

CAPTURE_FPS = 5
CAPTURE_FPS_FAST = 10


class SettingsManager:
    _LEGACY_KEYS = ("last_file", "speed", "transpose", "chord_mode")

    def __init__(self, path: Optional[str] = None):
        self._path = path or os.path.join(BASE_DIR, "settings.json")
        self._data: dict = {}
        self._load()

    def _load(self):
        # Clean up stale temp files from interrupted atomic saves
        try:
            dir_name = os.path.dirname(self._path) or os.getcwd()
            for f in os.listdir(dir_name):
                if f.endswith(".tmp.json") and f.startswith("tmp"):
                    try:
                        os.remove(os.path.join(dir_name, f))
                    except Exception:
                        pass
        except Exception:
            pass
        try:
            if os.path.exists(self._path):
                with open(self._path, "r", encoding="utf-8") as handle:
                    self._data = json.load(handle)
        except Exception:
            self._data = {}

    def get(self, key: str, default: Any = None) -> Any:
        return self._data.get(key, default)

    def set(self, key: str, value: Any):
        self._data[key] = value

    def save(self):
        try:
            for legacy_key in self._LEGACY_KEYS:
                self._data.pop(legacy_key, None)
            # Atomic write to improve reliability on exit/crash (80% failure rate fixed)
            dir_name = os.path.dirname(self._path) or os.getcwd()
            with tempfile.NamedTemporaryFile(
                mode="w", dir=dir_name, delete=False, encoding="utf-8", suffix=".tmp.json"
            ) as tmp:
                json.dump(self._data, tmp, indent=2, ensure_ascii=False)
                tmp.flush()
                os.fsync(tmp.fileno())
                tmp_path = tmp.name
            os.replace(tmp_path, self._path)
        except Exception as e:
            print(f"[Settings] Save failed: {e} (path={self._path})")
            # Clean up orphaned temp file if os.replace failed
            try:
                if tmp_path and os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except Exception:
                pass
            # fallback to direct write
            try:
                with open(self._path, "w", encoding="utf-8") as handle:
                    json.dump(self._data, handle, indent=2, ensure_ascii=False)
                    handle.flush()
                    os.fsync(handle.fileno())
            except Exception:
                pass

    def get_data_source_map(self) -> dict:
        raw_map = self._data.get("data_source_map", {})
        legacy_mode = self._data.get("data_source", "packet")
        normalized = normalize_source_map(raw_map, legacy_mode)
        self._data["data_source_map"] = dict(normalized)
        self._data["data_source"] = "mixed"
        return dict(normalized)

    def get_component_source(self, component: str, default: Optional[str] = None) -> str:
        fallback = normalize_source_mode(default, DEFAULT_DATA_SOURCE_MAP.get(component, "packet"))
        return self.get_data_source_map().get(component, fallback)

    def set_component_source(self, component: str, mode: str):
        if component not in DATA_SOURCE_COMPONENTS:
            return
        source_map = self.get_data_source_map()
        if component == "stamina":
            source_map[component] = "vision"
        elif component == "skills":
            source_map[component] = "packet"
        else:
            source_map[component] = normalize_source_mode(mode, source_map.get(component, "packet"))
        self._data["data_source_map"] = source_map
        self._data["data_source"] = "mixed"

    def set_all_component_sources(self, mode: str):
        normalized = normalize_source_mode(mode, "packet")
        source_map = {
            key: ("vision" if key == "stamina" else ("packet" if key == "skills" else normalized))
            for key in DATA_SOURCE_COMPONENTS
        }
        self._data["data_source_map"] = source_map
        self._data["data_source"] = "mixed"

    def get_roi(self, name: str) -> dict:
        custom = self._data.get("roi", {}).get(name)
        if custom:
            return custom
        if name == "skill_bar":
            return get_skill_bar_roi()
        return dict(DEFAULT_ROI.get(name, {}))

    def set_roi(self, name: str, roi: dict):
        if "roi" not in self._data:
            self._data["roi"] = {}
        self._data["roi"][name] = roi
