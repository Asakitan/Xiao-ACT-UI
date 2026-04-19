# -*- coding: utf-8 -*-
"""Shared configuration and settings helpers for SAO Auto."""

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


def _is_main_app_host() -> bool:
    """v2.1.2-k: 只有当宿主进程是 XiaoACTUI 主程序时, 才允许动 update.exe.

    update.exe 自己也会 import config (它被 PyInstaller 一起打包),
    如果在 update.exe 进程里跑 promote 逻辑, 会 rename/replace 自己,
    导致 update.exe 启动后 "凭空消失" (用户反馈)。
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
                try:
                    if os.path.isfile(target):
                        try:
                            if os.path.getsize(target) == os.path.getsize(staged):
                                # already same → drop staged
                                try:
                                    os.remove(staged)
                                except Exception:
                                    pass
                                continue
                        except Exception:
                            pass
                        # try to delete target first
                        try:
                            os.remove(target)
                        except PermissionError:
                            # try rename-aside
                            try:
                                old = target + f".old-{int(__import__('time').time())}"
                                os.rename(target, old)
                            except Exception:
                                continue  # still locked; leave .new for next start
                        except Exception:
                            continue
                    try:
                        os.replace(staged, target)
                        finalized += 1
                        print(f"[config] finalized pending replacement: {target}", flush=True)
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

WINDOW_TITLE = "SAO Auto - Game HUD"
WINDOW_SIZE = "900x980"
APP_VERSION = "2.1.3"
APP_VERSION_LABEL = f"v{APP_VERSION}"
# v2.1.3:
#   1) 修复升级后启动时 update.exe 被回退/删除 — 顶层 update.exe 现在永远
#      被视为权威 (full-package 解压结果), 嵌套 runtime/update.exe 仅作为
#      残留清理掉, 不再做 size 比较 + replace 的危险操作 (config.py +
#      sao_updater.py 两处 promote 同步修复);
#   2) onedir 模式下 STA 识别失败导致 HP 面板被隐藏 — 根因是 PyInstaller
#      bootloader 默认 DPI-unaware, 高 DPI 屏上 PrintWindow 抓帧与
#      GetClientRect 坐标尺度不一致, STA 颜色匹配长期 0 信号 →
#      stamina_offline=True → setSTAOffline(true) → _setHPGroupHidden(true)。
#      修复: (a) 新增 XiaoACTUI.exe.manifest 显式声明 PerMonitorV2 +
#      requireAdministrator; (b) main.py 模块级 _early_dpi_aware() 兜底;
#      (c) sao_webview._should_show_sta_offline 加保护 — 当 vision
#      capture failed 时不下发 OFFLINE 信号, 保留 HP 面板可见;
#   3) 本版以 full-package + force_update + minimum_version=2.1.3 推送,
#      所有现存 2.1.2-x 安装强制升级。
# v2.1.2-m: 修复 sao_alert 同条 alert 4s 内重复触发只续展不重弹;
#           webview _maybe_show_update_popup 同步 sao_gui 的 downloading
#           静音 + alert 可见时跳过非 error 提示;
#           本版以 full-package + force_update + minimum_version=2.1.2-l
#           推送, 强制清理积压问题。

# 远程更新服务地址 (可被 settings.json 中 update_host 覆盖). 留空表示禁用更新检查.
DEFAULT_UPDATE_HOST = "http://47.82.157.220:9330"
UPDATE_CHANNEL = "stable"
UPDATE_TARGET = "windows-x64"

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
                tmp_path = tmp.name
            os.replace(tmp_path, self._path)
        except Exception as e:
            print(f"[Settings] Save failed: {e} (path={self._path})")
            # fallback to direct write
            try:
                with open(self._path, "w", encoding="utf-8") as handle:
                    json.dump(self._data, handle, indent=2, ensure_ascii=False)
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
