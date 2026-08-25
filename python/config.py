# -*- coding: utf-8 -*-
# Shared configuration and settings helpers for SAO Auto.

import filecmp
import os
import shutil
import sys
import tempfile
import threading
from typing import Any, Dict, Optional

_is_frozen = getattr(sys, "frozen", False)
if not _is_frozen:
    _is_frozen = not os.path.isfile(os.path.abspath(__file__))
if _is_frozen:
    sys.frozen = True
if _is_frozen:
    _exe_dir = os.path.dirname(os.path.abspath(sys.executable))
    _meipass = getattr(sys, '_MEIPASS', None)
    if _meipass:
        BASE_DIR = os.path.dirname(_meipass)
        BUNDLE_DIR = _meipass
    elif os.path.basename(_exe_dir).lower() == 'runtime':
        BASE_DIR = os.path.dirname(_exe_dir)
        BUNDLE_DIR = _exe_dir
    else:
        BASE_DIR = _exe_dir
        BUNDLE_DIR = _exe_dir
    _pydll = os.path.join(_exe_dir, 'xactrt311.dll')
    if os.path.isfile(_pydll):
        os.environ.setdefault('PYTHONNET_PYDLL', _pydll)
else:
    BUNDLE_DIR = os.path.dirname(os.path.abspath(__file__))
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))

try:
    if BASE_DIR and BASE_DIR not in sys.path:
        sys.path.insert(0, BASE_DIR)
    _rt_dir = os.path.join(BASE_DIR, 'runtime')
    if os.path.isdir(_rt_dir):
        if _rt_dir not in sys.path:
            sys.path.insert(0, _rt_dir)
        if hasattr(os, 'add_dll_directory'):
            os.add_dll_directory(_rt_dir)
except Exception:
    pass

# Dev layout: plugin Cython accelerators are built in-place under
# plugins/<id>/cython/ but several hot UI modules import selected
# accelerators as top-level modules (for example _sao_cy_pixels).
# Add those accelerator directories immediately after BASE_DIR so the
# in-tree GPU helpers win over stale installed copies while BASE_DIR
# remains the highest-priority source root.
try:
    _plugin_roots = []
    for _base in (BASE_DIR, BUNDLE_DIR):
        _plugins_dir = os.path.join(_base, 'plugins')
        if _plugins_dir not in _plugin_roots and os.path.isdir(_plugins_dir):
            _plugin_roots.append(_plugins_dir)
    _insert_at = (sys.path.index(BASE_DIR) + 1) if BASE_DIR in sys.path else 0
    for _plugins_dir in _plugin_roots:
        for _plugin_name in sorted(os.listdir(_plugins_dir)):
            _cython_dir = os.path.join(_plugins_dir, _plugin_name, 'cython')
            if not os.path.isdir(_cython_dir):
                continue
            try:
                sys.path.remove(_cython_dir)
            except ValueError:
                pass
            sys.path.insert(_insert_at, _cython_dir)
            _insert_at += 1
except Exception:
    pass


def _files_are_identical(left: str, right: str) -> bool:
    # Return True only when two files are byte-for-byte identical.
    try:
        return filecmp.cmp(left, right, shallow=False)
    except Exception:
        return False


def _is_main_app_host() -> bool:
    # v2.1.2-k: 只有当宿主进程是 XiaoACTUI 主程序时, 才允许动 update.exe.
    #
    # update.exe 自己也会 import config (它被 PyInstaller 一起打包),
    # 如果在 update.exe 进程里跑 promote 逻辑, 会 rename/replace 自己,
    # 导致 update.exe 启动后 "凭空消失"。
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
    # Finalize update.exe.new at process bootstrap without touching old update.exe otherwise.
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
    # v2.1.2-h: bootstrap 把 runtime/update.exe 提升到顶层.
    #
    # 与 sao_updater.promote_runtime_update_exe 等价, 但放在 config 里
    # 保证最早被调用 (大多数模块都 import config). 解决用户反馈的
    # "升级后 update.exe 没替换" — 之前依赖 sao_updater 的延迟 import
    # 路径, 在 webview/atexit 没触发时就跑不到。
    #
    # v2.1.2-k: 仅在主程序 (XiaoACTUI) 进程里运行, 防止 update.exe
    # 自己 promote 自己导致被删除。
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
    # v2.1.2-j: 扫描 BASE_DIR 下所有 *.new 文件并 finalize.
    #
    # 场景:
    # - 旧 update.exe 用 os.replace 覆盖 SAOUI.ttf 失败 → 我们的新 update_apply
    # 把它 stage 到 SAOUI.ttf.new。
    # - 旧 update.exe 处理 update.exe 自身时, 失败时 fallback 留下 update.exe.new
    # (之前由 MoveFileEx DELAY_UNTIL_REBOOT 排队, 但用户要求重启前完成)。
    # 主程序 XiaoACTUI 启动到这里时, 之前持锁的进程已完全退出, 可以直接 rename。
    # 返回 finalize 成功的文件数。
    #
    # v2.1.2-k: 仅在主程序 (XiaoACTUI) 进程里运行 — update.exe 自己 import
    # config 时若 finalize update.exe.new 会删除自己。
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
    # v2.1.2-n: 清理 schedule_apply_on_exit 留下的 ``<name>.old-<ts>`` 文件.
    #
    # 主程序在退出前 rename 字体/DLL 让老 update.exe 能直接 os.replace,
    # 本进程持有的 GDI/loader handle 在主进程退出后释放, 重新启动时
    # 这些 .old-<ts> 文件已经无人持有, 可以安全删除避免堆积。
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
    # v2.1.2-n: 清理 BASE_DIR 下残留的 _swap_update_*.cmd.
    #
    # update.exe 自己被覆盖时, _schedule_self_replace 会 spawn 一个 cmd 脚本,
    # 脚本末尾 `del /f /q "%~f0"` 应自删, 但偶尔 cmd.exe 没释放句柄就退出
    # (用户截图能看到 _swap_update_<ts>.cmd 残留)。主程序启动时, 旧 update.exe
    # 及其 spawn 的 cmd 都已彻底退出, 直接清掉。
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

def get_main_executable() -> str:
    # Return the path to the main application EXE.
    #
    # Nuitka sets sys.executable to python.exe, not the compiled binary.
    exe = sys.executable
    if _is_frozen and os.path.basename(exe).lower() == 'python.exe':
        candidate = os.path.join(os.path.dirname(exe), 'XiaoACTUI.exe')
        if os.path.isfile(candidate):
            return candidate
    return exe


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
    # 返回资源路径: 优先 BASE_DIR (顶层模块化文件夹), 不存在则回退 BUNDLE_DIR.
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

# 远程更新服务地址 (可被 settings.json 中 update_host 覆盖). 留空表示禁用更新检查.
DEFAULT_UPDATE_HOST = "http://x2.sjcmc.cn:15018"
DEFAULT_LICENSE_SERVER = "https://x2.sjcmc.cn:15522"
UPDATE_CHANNEL = "stable"
UPDATE_TARGET = "windows-x64"

WINDOW_TITLE = "SAO Auto - Game HUD"
WINDOW_SIZE = "900x980"
APP_VERSION = "5.2.15"
APP_VERSION_LABEL = f"v{APP_VERSION}"
# 完整版本历史见 CHANGELOG.md。

# GPU 渲染开关: GLFW/ModernGL 后端可用时默认全开 (LinkStart 开场动画依赖默认创建 GPU 窗口),
# 不要用环境变量门控; 后端不可用或窗口创建失败时自动回退到 ULW/CPU 路径。
USE_GPU_MENU_HUD = True
USE_GPU_OVERLAY = True
USE_UNIFIED_OVERLAY = True

DEFAULT_PANEL_THEMES: Dict[str, str] = {"act": "dark"}


def normalize_panel_theme(theme: Any, default: str = "dark") -> str:
    fallback = "light" if str(default or "").strip().lower() == "light" else "dark"
    return "light" if str(theme or "").strip().lower() == "light" else fallback


def normalize_panel_themes(raw: Any) -> dict:
    themes = dict(DEFAULT_PANEL_THEMES)
    if isinstance(raw, dict):
        for key, value in raw.items():
            name = str(key or "").strip().lower()
            themes[name] = normalize_panel_theme(value, themes.get(name, "dark"))
    return themes


DEFAULT_SETTINGS: Dict[str, Any] = {
    "panel_themes": dict(DEFAULT_PANEL_THEMES),
}


FISHEYE_SOURCE_DESKTOP = "desktop"
FISHEYE_SOURCE_PREFIX_IMAGE = "image:"
FISHEYE_SOURCE_PREFIX_COLOR = "color:"
DEFAULT_FISHEYE_BACKGROUND_SOURCE = FISHEYE_SOURCE_DESKTOP

DEFAULT_HOTKEYS = {
    "toggle_recognition": "F5",
    "toggle_topmost": "F9",
    "hide_panels": "F10",
    "show_plugins": "F11",
    "toggle_float_button": "INSERT",
    "toggle_sao_menu": "HOME",
}

# ── 快捷键组合解析 (三套监听器共用: SAOHotkeyManager / sao_webview / automation) ──
# F 键虚拟键码 (Windows VK)。
HOTKEY_FKEY_VK = {
    "F1": 112, "F2": 113, "F3": 114, "F4": 115,
    "F5": 116, "F6": 117, "F7": 118, "F8": 119,
    "F9": 120, "F10": 121, "F11": 122, "F12": 123,
    "INSERT": 0x2D, "DELETE": 0x2E,
    "HOME": 0x24, "END": 0x23,
    "PAGEUP": 0x21, "PAGEDOWN": 0x22,
}
# 字母/数字主键 (插件快捷键, 如 CTRL+SHIFT+F)。裸键会被日常打字误触,
# parse_hotkey 要求这类主键至少搭配一个修饰键。
HOTKEY_CHAR_VK = {chr(c): c for c in range(0x41, 0x5B)}  # A-Z (VK==ord)
HOTKEY_CHAR_VK.update({chr(c): c for c in range(0x30, 0x3A)})  # 0-9
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
    # 解析快捷键定义 → ``{'vk': int, 'mods': frozenset[str]}`` 或 None。
    #
    # 接受 ``"F5"`` / ``"CTRL+F5"`` / ``"Ctrl+Alt+F12"`` 字符串,
    # ``{'vk': N[, 'mods': [...]]}`` 自定义 VK dict, 以及插件映射的
    # ``{'key': 'CTRL+F8'}`` 形式。字符串主键限 ``HOTKEY_FKEY_VK`` 中的命名键
    # (如 F1-F12 / HOME 等) 或 ``HOTKEY_CHAR_VK`` 的字母/数字 (必须搭配
    # 修饰键; dict 的 vk 不限); 解析失败返回 None, 该绑定不触发。
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
    char_main = False
    for part in spec.upper().split("+"):
        part = _HOTKEY_MOD_ALIASES.get(part.strip(), part.strip())
        if part in HOTKEY_MODIFIER_VKS:
            mods.add(part)
        elif part in HOTKEY_FKEY_VK and vk is None:
            vk = HOTKEY_FKEY_VK[part]
        elif part in HOTKEY_CHAR_VK and vk is None:
            vk = HOTKEY_CHAR_VK[part]
            char_main = True
        else:
            return None
    if vk is None:
        return None
    if char_main and not mods:
        return None
    return {"vk": vk, "mods": frozenset(mods)}


_HOTKEY_VK_TO_FKEY = {v: k for k, v in HOTKEY_FKEY_VK.items()}
_HOTKEY_VK_TO_FKEY.update({v: k for k, v in HOTKEY_CHAR_VK.items()})

# GetAsyncKeyState 句柄: 修饰键实测状态的权威来源。pynput 的 WH_KEYBOARD_LL
# 钩子在安全桌面 (UAC/Win+L) 和独占输入游戏下会丢 key-up, 残留在 pressed
# 集合里的脏修饰键会永久卡死匹配 — 所以匹配时优先实测, 集合推断只作回退。
try:
    import ctypes as _ctypes_hotkey
    _HOTKEY_GAKS = _ctypes_hotkey.windll.user32.GetAsyncKeyState
except Exception:
    _HOTKEY_GAKS = None


def normalize_hotkey(spec):
    # 规范化拼写 → ``'CTRL+ALT+F5'`` (修饰键固定 CTRL,ALT,SHIFT 序)。
    #
    # 'control + f8' / 'MENU+F5' 等别名拼写都收敛到唯一形式, 占用表和
    # 冲突拒绝才能按字符串比较。主键不是已支持的命名键或解析失败返回 None。
    parsed = parse_hotkey(spec)
    if not parsed:
        return None
    name = _HOTKEY_VK_TO_FKEY.get(parsed["vk"])
    if not name:
        return None
    if name in HOTKEY_CHAR_VK and not parsed["mods"]:
        # 裸字母/数字 parse_hotkey 拒收 — 规范形式必须能 parse 回去
        return None
    mods = [m for m in ("CTRL", "ALT", "SHIFT") if m in parsed["mods"]]
    return "+".join(mods + [name])


def hotkey_mods_down(pressed_vks=frozenset()):
    # 当前按住的修饰键集合 (如 ``{'CTRL'}``)。
    #
    # 优先 GetAsyncKeyState 实测; 不可用 (非 Windows / ctypes 失败) 时
    # 回退从 ``pressed_vks`` 推断。
    if _HOTKEY_GAKS is not None:
        try:
            return {m for m, vks in HOTKEY_MODIFIER_VKS.items()
                    if any(_HOTKEY_GAKS(v) & 0x8000 for v in vks)}
        except Exception:
            pass
    return {m for m, vks in HOTKEY_MODIFIER_VKS.items()
            if any(v in pressed_vks for v in vks)}


def hotkey_matches(parsed, pressed_vks, mods_down=None):
    # 子集匹配: 主键按下 + 要求的修饰键全按住; 多余的修饰键不挡触发。
    #
    # 多余修饰键不挡是刻意的: 躲避自动化会注入 SHIFT, 急停键 (纯 F12)
    # 必须在 Shift 被按住时照样触发。'F5' 与 'CTRL+F5' 的互斥共存由调度
    # 方负责 — 同主键多个候选命中时用 ``select_hotkey_match`` 取修饰键
    # 最多的 (最特异优先), Ctrl+F5 命中组合而不是裸键。
    if not parsed or parsed["vk"] not in pressed_vks:
        return False
    if mods_down is None:
        mods_down = hotkey_mods_down(pressed_vks)
    return parsed["mods"] <= mods_down


def select_hotkey_match(candidates, pressed_vks, mods_down=None):
    # 从 ``[(parsed, payload), ...]`` 里选出命中的最特异绑定的 payload。
    #
    # 并列特异度取先出现的 — 调用方把内置绑定排在插件绑定前面即保持
    # 内置优先的既有语义。无命中返回 None。
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
    return BASE_DIR


CONFIG_FILE = os.path.join(_get_config_dir(), "settings.json")

import settings_crypto


class SettingsManager:
    _LEGACY_KEYS = ("last_file", "speed", "transpose", "chord_mode")

    def __init__(self, path: Optional[str] = None):
        self._path = path or os.path.join(BASE_DIR, "settings.json")
        self._data: dict = {}
        self._load_error: str = ""
        self._lock = threading.Lock()
        self._load()

    def _backup_corrupt_file(self, exc: Exception) -> None:
        try:
            if not os.path.exists(self._path):
                return
            backup = f"{self._path}.corrupt"
            if os.path.exists(backup):
                backup = f"{self._path}.corrupt.{os.getpid()}"
            shutil.copy2(self._path, backup)
            print(f"[Settings] Invalid settings JSON backed up to {backup}: {exc}")
        except Exception as backup_exc:
            print(f"[Settings] Invalid settings JSON; backup failed: {backup_exc} ({exc})")

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
                with open(self._path, "rb") as handle:
                    raw = handle.read()
                self._data = settings_crypto.decode_settings(raw)
                if not isinstance(self._data, dict):
                    raise ValueError("settings root must be an object")
                self._load_error = ""
                if settings_crypto.is_legacy_plaintext(raw):
                    self.save()
        except Exception as exc:
            self._load_error = str(exc)
            self._backup_corrupt_file(exc)
            self._data = {}

    def get_load_error(self) -> str:
        # Non-empty if settings.json failed to parse at load time and was
        # reset to defaults (a .corrupt backup was written alongside it).
        return self._load_error

    def get(self, key: str, default: Any = None) -> Any:
        if key == "panel_themes":
            if key in self._data:
                return normalize_panel_themes(self._data.get(key))
            return normalize_panel_themes(default if default is not None else DEFAULT_SETTINGS.get(key))
        if key in self._data:
            return self._data.get(key)
        if default is not None:
            return default
        return DEFAULT_SETTINGS.get(key, default)

    def set(self, key: str, value: Any):
        if key == "panel_themes":
            value = normalize_panel_themes(value)
        with self._lock:
            self._data[key] = value

    def save(self):
        with self._lock:
            for legacy_key in self._LEGACY_KEYS:
                self._data.pop(legacy_key, None)
            blob = settings_crypto.encode_settings(self._data)
        tmp_path = ""
        try:
            dir_name = os.path.dirname(self._path) or os.getcwd()
            os.makedirs(dir_name, exist_ok=True)
            with tempfile.NamedTemporaryFile(
                mode="wb", dir=dir_name, delete=False, suffix=".tmp.json"
            ) as tmp:
                tmp.write(blob)
                tmp.flush()
                os.fsync(tmp.fileno())
                tmp_path = tmp.name
            os.replace(tmp_path, self._path)
            self._load_error = ""
        except Exception as e:
            print(f"[Settings] Save failed: {e} (path={self._path}); previous settings kept")
            try:
                if tmp_path and os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except Exception:
                pass

    def get_roi(self, name: str) -> dict:
        return self._data.get("roi", {}).get(name, {})
