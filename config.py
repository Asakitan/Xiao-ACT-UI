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

# onedir 下 sys.path 只含 runtime/, 但 build_release.bat 把
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
APP_VERSION = "4.5.24"
APP_VERSION_LABEL = f"v{APP_VERSION}"
# 完整版本历史见 CHANGELOG.md。

# GPU 渲染开关: GLFW/ModernGL 后端可用时默认全开 (LinkStart 开场动画依赖默认创建 GPU 窗口),
# 不要用环境变量门控; 后端不可用或窗口创建失败时自动回退到 ULW/CPU 路径。
USE_GPU_MENU_HUD = True
USE_GPU_OVERLAY = True
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
USE_GPU_SKILLFX = True

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
    "show_plugins": "F11",
    "toggle_auto_dodge": "F12",
}

# ── 快捷键组合解析 (三套监听器共用: SAOHotkeyManager / sao_webview / automation) ──
# F 键虚拟键码 (Windows VK)。
HOTKEY_FKEY_VK = {
    "F1": 112, "F2": 113, "F3": 114, "F4": 115,
    "F5": 116, "F6": 117, "F7": 118, "F8": 119,
    "F9": 120, "F10": 121, "F11": 122, "F12": 123,
}
# 修饰键 VK 组: pynput 上报左右具体码 (162/163 等), GetAsyncKeyState
# 轮询路径用通用码 (17/18/16), 两路都要认。
HOTKEY_MODIFIER_VKS = {
    "CTRL": (17, 162, 163),
    "ALT": (18, 164, 165),
    "SHIFT": (16, 160, 161),
}
HOTKEY_MOD_ALL_VKS = frozenset(
    v for vks in HOTKEY_MODIFIER_VKS.values() for v in vks)
_HOTKEY_MOD_ALIASES = {"CONTROL": "CTRL", "MENU": "ALT"}


def parse_hotkey(spec):
    """解析快捷键定义 → ``{'vk': int, 'mods': frozenset[str]}`` 或 None。

    接受 ``"F5"`` / ``"CTRL+F5"`` / ``"Ctrl+Alt+F12"`` 字符串,
    ``{'vk': N[, 'mods': [...]]}`` 自定义 VK dict, 以及插件映射的
    ``{'key': 'CTRL+F8'}`` 形式。字符串主键限 F1-F12 (dict 的 vk 不限);
    解析失败返回 None, 该绑定不触发。
    """
    if isinstance(spec, dict):
        raw_vk = spec.get("vk")
        if raw_vk:
            try:
                vk = int(raw_vk)
            except (TypeError, ValueError):
                return None
            mods = set()
            for m in (spec.get("mods") or ()):
                m = _HOTKEY_MOD_ALIASES.get(str(m).upper(), str(m).upper())
                if m not in HOTKEY_MODIFIER_VKS:
                    return None
                mods.add(m)
            return {"vk": vk, "mods": frozenset(mods)}
        spec = spec.get("key") or spec.get("name") or ""
    if not isinstance(spec, str) or not spec.strip():
        return None
    mods = set()
    vk = None
    for part in spec.upper().split("+"):
        part = _HOTKEY_MOD_ALIASES.get(part.strip(), part.strip())
        if part in HOTKEY_MODIFIER_VKS:
            mods.add(part)
        elif part in HOTKEY_FKEY_VK and vk is None:
            vk = HOTKEY_FKEY_VK[part]
        else:
            return None
    if vk is None:
        return None
    return {"vk": vk, "mods": frozenset(mods)}


_HOTKEY_VK_TO_FKEY = {v: k for k, v in HOTKEY_FKEY_VK.items()}

# GetAsyncKeyState 句柄: 修饰键实测状态的权威来源。pynput 的 WH_KEYBOARD_LL
# 钩子在安全桌面 (UAC/Win+L) 和独占输入游戏下会丢 key-up, 残留在 pressed
# 集合里的脏修饰键会永久卡死匹配 — 所以匹配时优先实测, 集合推断只作回退。
try:
    import ctypes as _ctypes_hotkey
    _HOTKEY_GAKS = _ctypes_hotkey.windll.user32.GetAsyncKeyState
except Exception:
    _HOTKEY_GAKS = None


def normalize_hotkey(spec):
    """规范化拼写 → ``'CTRL+ALT+F5'`` (修饰键固定 CTRL,ALT,SHIFT 序)。

    'control + f8' / 'MENU+F5' 等别名拼写都收敛到唯一形式, 占用表和
    冲突拒绝才能按字符串比较。主键不是 F1-F12 或解析失败返回 None。
    """
    parsed = parse_hotkey(spec)
    if not parsed:
        return None
    name = _HOTKEY_VK_TO_FKEY.get(parsed["vk"])
    if not name:
        return None
    mods = [m for m in ("CTRL", "ALT", "SHIFT") if m in parsed["mods"]]
    return "+".join(mods + [name])


def hotkey_mods_down(pressed_vks=frozenset()):
    """当前按住的修饰键集合 (如 ``{'CTRL'}``)。

    优先 GetAsyncKeyState 实测; 不可用 (非 Windows / ctypes 失败) 时
    回退从 ``pressed_vks`` 推断。
    """
    if _HOTKEY_GAKS is not None:
        try:
            return {m for m, vks in HOTKEY_MODIFIER_VKS.items()
                    if any(_HOTKEY_GAKS(v) & 0x8000 for v in vks)}
        except Exception:
            pass
    return {m for m, vks in HOTKEY_MODIFIER_VKS.items()
            if any(v in pressed_vks for v in vks)}


def hotkey_matches(parsed, pressed_vks, mods_down=None):
    """子集匹配: 主键按下 + 要求的修饰键全按住; 多余的修饰键不挡触发。

    多余修饰键不挡是刻意的: 躲避自动化会注入 SHIFT, 急停键 (纯 F12)
    必须在 Shift 被按住时照样触发。'F5' 与 'CTRL+F5' 的互斥共存由调度
    方负责 — 同主键多个候选命中时用 ``select_hotkey_match`` 取修饰键
    最多的 (最特异优先), Ctrl+F5 命中组合而不是裸键。
    """
    if not parsed or parsed["vk"] not in pressed_vks:
        return False
    if mods_down is None:
        mods_down = hotkey_mods_down(pressed_vks)
    return parsed["mods"] <= mods_down


def select_hotkey_match(candidates, pressed_vks, mods_down=None):
    """从 ``[(parsed, payload), ...]`` 里选出命中的最特异绑定的 payload。

    并列特异度取先出现的 — 调用方把内置绑定排在插件绑定前面即保持
    内置优先的既有语义。无命中返回 None。
    """
    if mods_down is None:
        mods_down = hotkey_mods_down(pressed_vks)
    best = None
    best_n = -1
    for parsed, payload in candidates:
        if not hotkey_matches(parsed, pressed_vks, mods_down=mods_down):
            continue
        n = len(parsed["mods"])
        if n > best_n:
            best, best_n = payload, n
    return best


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
        tmp_path = ""
        try:
            for legacy_key in self._LEGACY_KEYS:
                self._data.pop(legacy_key, None)
            # Atomic write to improve reliability on exit/crash (80% failure rate fixed)
            dir_name = os.path.dirname(self._path) or os.getcwd()
            os.makedirs(dir_name, exist_ok=True)
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
                dir_name = os.path.dirname(self._path) or os.getcwd()
                os.makedirs(dir_name, exist_ok=True)
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
