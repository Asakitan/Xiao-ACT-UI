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
DEFAULT_UPDATE_HOST = "http://47.82.157.220:9330"
UPDATE_CHANNEL = "stable"
UPDATE_TARGET = "windows-x64"

WINDOW_TITLE = "SAO Auto - Game HUD"
WINDOW_SIZE = "900x980"
APP_VERSION = "2.4.31"
APP_VERSION_LABEL = f"v{APP_VERSION}"
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
# v2.3.0: GPU SDF shader pipeline for SkillFX (ring + beam + glow as a
# single fragment-shader pass). Replaces the old PIL/numpy compose that
# cost 60-90 ms per frame on the render worker; the GPU path runs the
# whole layer set in ~2-5 ms on the integrated GPU. Caption sprites
# still PIL (cached statically). Falls back to the CPU path on any
# pipeline failure. Set `SAO_SKILLFX_GPU=0` to force the legacy CPU
# path for diagnostics.
USE_GPU_SKILLFX = True
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
