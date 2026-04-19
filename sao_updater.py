# -*- coding: utf-8 -*-
"""SAO Auto - 远程更新客户端

功能:
- 拉取远程 manifest (HTTPS/HTTP, JSON)
- 比较版本号决定是否需要更新, 是否强制更新
- 下载 zip 包到 staging 目录, SHA256 校验
- 写入 update_state.json, 由外部 update_apply.py 在主进程退出后应用

manifest schema (JSON):
{
  "version": "2.1.0",
  "minimum_version": "2.0.1",   # 客户端 < 该版本时强制升级
  "force_update": false,
  "package_type": "runtime-delta" | "full-package",
  "target": "windows-x64",
  "channel": "stable",
  "download_url": "https://.../release.zip",
  "sha256": "<hex>",
  "size": 12345,
  "notes": "...",
  "published_at": "2026-04-19T12:00:00Z"
}

runtime-delta zip 内容: 按 runtime/ 下的相对路径布局, 例如
    runtime/sao_gui.py
  web/menu.html
  assets/sounds/ding.wav

full-package zip 内容: 顶层包含 XiaoACTUI.exe 等完整客户端文件.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, asdict, field
from typing import Any, Callable, Dict, Optional, Tuple

try:
    from config import (
        APP_VERSION,
        BASE_DIR,
        DEFAULT_UPDATE_HOST,
        RUNTIME_DIR,
        RUNTIME_STAGING_DIR,
        UPDATE_CHANNEL,
        UPDATE_STATE_FILE,
        UPDATE_TARGET,
    )
except Exception:  # pragma: no cover - 仅在最早 bootstrap 失败时
    APP_VERSION = "0.0.0"
    BASE_DIR = os.path.dirname(os.path.abspath(__file__))
    DEFAULT_UPDATE_HOST = ""
    RUNTIME_DIR = os.path.join(BASE_DIR, "runtime")
    RUNTIME_STAGING_DIR = os.path.join(RUNTIME_DIR, "staging")
    UPDATE_CHANNEL = "stable"
    UPDATE_STATE_FILE = os.path.join(RUNTIME_DIR, "update_state.json")
    UPDATE_TARGET = "windows-x64"


HTTP_TIMEOUT = 8.0
DOWNLOAD_TIMEOUT = 60.0
USER_AGENT = f"SAOAuto-Updater/{APP_VERSION}"

# 状态码
STATE_IDLE = "idle"
STATE_CHECKING = "checking"
STATE_AVAILABLE = "available"
STATE_DOWNLOADING = "downloading"
STATE_READY = "ready"          # 已下载, 待重启应用
STATE_UP_TO_DATE = "up_to_date"
STATE_ERROR = "error"


@dataclass
class UpdateManifest:
    version: str = ""
    minimum_version: str = ""
    force_update: bool = False
    package_type: str = "runtime-delta"  # 或 full-package
    target: str = UPDATE_TARGET
    channel: str = UPDATE_CHANNEL
    download_url: str = ""
    sha256: str = ""
    size: int = 0
    notes: str = ""
    published_at: str = ""

    @classmethod
    def from_json(cls, data: Dict[str, Any]) -> "UpdateManifest":
        m = cls()
        for f in (
            "version", "minimum_version", "package_type", "target", "channel",
            "download_url", "sha256", "notes", "published_at",
        ):
            v = data.get(f)
            if isinstance(v, str):
                setattr(m, f, v)
        m.force_update = bool(data.get("force_update", False))
        try:
            m.size = int(data.get("size") or 0)
        except Exception:
            m.size = 0
        return m


@dataclass
class UpdateStatus:
    state: str = STATE_IDLE
    current_version: str = APP_VERSION
    latest_version: str = ""
    force_required: bool = False
    package_type: str = ""
    notes: str = ""
    published_at: str = ""
    download_url: str = ""
    sha256: str = ""
    size: int = 0
    progress: float = 0.0
    error: str = ""
    skipped_version: str = ""
    last_checked: float = 0.0

    def to_json(self) -> Dict[str, Any]:
        return asdict(self)


def _parse_version(v: str) -> tuple:
    parts = []
    for chunk in (v or "").strip().lstrip("vV").split("."):
        try:
            parts.append(int(chunk))
        except Exception:
            parts.append(0)
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:4])


def compare_versions(a: str, b: str) -> int:
    pa, pb = _parse_version(a), _parse_version(b)
    if pa < pb:
        return -1
    if pa > pb:
        return 1
    return 0


def _ensure_dirs():
    for d in (RUNTIME_DIR, RUNTIME_STAGING_DIR):
        try:
            os.makedirs(d, exist_ok=True)
        except Exception:
            pass


def _http_get_json(url: str, timeout: float = HTTP_TIMEOUT) -> Tuple[int, Optional[Dict[str, Any]], str]:
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            status = int(getattr(resp, "status", 200) or 200)
            data = resp.read()
            if not data:
                return status, None, ""
            return status, json.loads(data.decode("utf-8")), ""
    except urllib.error.HTTPError as e:
        body = b""
        try:
            body = e.read()
        except Exception:
            body = b""
        payload: Optional[Dict[str, Any]] = None
        if body:
            try:
                parsed = json.loads(body.decode("utf-8"))
                payload = parsed if isinstance(parsed, dict) else None
            except Exception:
                payload = None
        return int(getattr(e, "code", 0) or 0), payload, f"更新服务返回 HTTP {getattr(e, 'code', 'error')}"
    except urllib.error.URLError as e:
        reason = getattr(e, "reason", None)
        return 0, None, f"无法连接更新服务: {reason or e}"
    except TimeoutError:
        return 0, None, "连接更新服务超时"
    except (ValueError, json.JSONDecodeError) as e:
        return 0, None, f"更新服务响应无效: {e}"
    except Exception as e:
        return 0, None, f"更新服务异常: {e}"


def _http_download(
    url: str,
    dst_path: str,
    expected_sha256: str = "",
    timeout: float = DOWNLOAD_TIMEOUT,
    progress_cb: Optional[Callable[[float], None]] = None,
) -> Optional[str]:
    """下载到 dst_path. 返回错误字符串, None 表示成功."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    sha = hashlib.sha256()
    tmp_path = dst_path + ".part"
    try:
        try:
            os.makedirs(os.path.dirname(dst_path) or ".", exist_ok=True)
        except Exception:
            pass
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            total = 0
            try:
                content_length = int(resp.headers.get("Content-Length") or 0)
            except Exception:
                content_length = 0
            with open(tmp_path, "wb") as out:
                while True:
                    chunk = resp.read(64 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
                    sha.update(chunk)
                    total += len(chunk)
                    if progress_cb and content_length > 0:
                        try:
                            progress_cb(min(1.0, total / content_length))
                        except Exception:
                            pass
        digest = sha.hexdigest()
        if expected_sha256 and digest.lower() != expected_sha256.lower():
            try:
                os.remove(tmp_path)
            except Exception:
                pass
            return f"SHA256 校验失败 (expected={expected_sha256[:12]}..., got={digest[:12]}...)"
        try:
            if os.path.exists(dst_path):
                os.remove(dst_path)
        except Exception:
            pass
        os.replace(tmp_path, dst_path)
        return None
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
        try:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)
        except Exception:
            pass
        return f"下载失败: {e}"
    except Exception as e:
        try:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)
        except Exception:
            pass
        return f"下载异常: {e}"


def _save_state(status: UpdateStatus):
    _ensure_dirs()
    try:
        tmp = UPDATE_STATE_FILE + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(status.to_json(), f, ensure_ascii=False, indent=2)
        os.replace(tmp, UPDATE_STATE_FILE)
    except Exception:
        pass


def _load_state() -> Dict[str, Any]:
    try:
        if os.path.exists(UPDATE_STATE_FILE):
            with open(UPDATE_STATE_FILE, "r", encoding="utf-8") as f:
                return json.load(f) or {}
    except Exception:
        pass
    return {}


def get_skipped_version() -> str:
    return str(_load_state().get("skipped_version") or "")


def skip_version(version: str):
    state = _load_state()
    state["skipped_version"] = version
    _ensure_dirs()
    try:
        with open(UPDATE_STATE_FILE, "w", encoding="utf-8") as f:
            json.dump(state, f, ensure_ascii=False, indent=2)
    except Exception:
        pass


class UpdateManager:
    """线程安全的更新管理器, 由 entity / webview 共用."""

    def __init__(self, host: Optional[str] = None, channel: str = UPDATE_CHANNEL):
        self.host = (host or DEFAULT_UPDATE_HOST or "").rstrip("/")
        self.channel = channel
        self.status = UpdateStatus()
        self.manifest: Optional[UpdateManifest] = None
        self._lock = threading.RLock()
        self._listeners: list = []
        self._worker: Optional[threading.Thread] = None
        try:
            saved = _load_state()
            if saved.get("skipped_version"):
                self.status.skipped_version = str(saved.get("skipped_version"))
        except Exception:
            pass

    # ---- 监听 ----
    def add_listener(self, cb: Callable[[UpdateStatus], None]):
        with self._lock:
            if cb not in self._listeners:
                self._listeners.append(cb)

    def remove_listener(self, cb: Callable[[UpdateStatus], None]):
        with self._lock:
            try:
                self._listeners.remove(cb)
            except ValueError:
                pass

    def _notify(self):
        with self._lock:
            snapshot = UpdateStatus(**self.status.to_json())
            listeners = list(self._listeners)
        for cb in listeners:
            try:
                cb(snapshot)
            except Exception:
                pass

    def _set_state(self, **kwargs):
        with self._lock:
            for k, v in kwargs.items():
                if hasattr(self.status, k):
                    setattr(self.status, k, v)
            _save_state(self.status)
        self._notify()

    # ---- API ----
    def snapshot(self) -> UpdateStatus:
        with self._lock:
            return UpdateStatus(**self.status.to_json())

    def is_busy(self) -> bool:
        return bool(self._worker and self._worker.is_alive())

    def check_async(self):
        if self.is_busy():
            return
        self._worker = threading.Thread(target=self._do_check, name="sao-updater-check", daemon=True)
        self._worker.start()

    def download_async(self):
        if self.is_busy():
            return
        self._worker = threading.Thread(target=self._do_download, name="sao-updater-dl", daemon=True)
        self._worker.start()

    def skip_current(self):
        with self._lock:
            v = self.manifest.version if self.manifest else self.status.latest_version
        if v:
            skip_version(v)
            self._set_state(skipped_version=v)

    # ---- 内部 ----
    def _do_check(self):
        if not self.host:
            self._set_state(state=STATE_ERROR, error="未配置 update_host")
            return
        self._set_state(state=STATE_CHECKING, error="", progress=0.0)
        url = f"{self.host}/api/update/latest?channel={self.channel}&target={UPDATE_TARGET}&current={APP_VERSION}"
        status_code, data, fetch_error = _http_get_json(url)
        if status_code in (204, 404):
            with self._lock:
                self.manifest = None
            self._set_state(
                state=STATE_UP_TO_DATE,
                latest_version=APP_VERSION,
                force_required=False,
                package_type="",
                notes="",
                published_at="",
                download_url="",
                sha256="",
                size=0,
                error="",
                last_checked=time.time(),
            )
            return
        if data and data.get("available") is False:
            with self._lock:
                self.manifest = None
            self._set_state(
                state=STATE_UP_TO_DATE,
                latest_version=str(data.get("version") or APP_VERSION),
                force_required=False,
                package_type="",
                notes="",
                published_at="",
                download_url="",
                sha256="",
                size=0,
                error="",
                last_checked=time.time(),
            )
            return
        if not data:
            self._set_state(
                state=STATE_ERROR,
                error=fetch_error or "无法连接更新服务",
                last_checked=time.time(),
            )
            return
        manifest = UpdateManifest.from_json(data)
        with self._lock:
            self.manifest = manifest
        if not manifest.version:
            self._set_state(state=STATE_ERROR, error="manifest 缺少 version", last_checked=time.time())
            return
        # minimum_version 强制
        force_required = bool(manifest.force_update)
        if manifest.minimum_version and compare_versions(APP_VERSION, manifest.minimum_version) < 0:
            force_required = True
        cmp = compare_versions(APP_VERSION, manifest.version)
        if cmp >= 0:
            self._set_state(
                state=STATE_UP_TO_DATE,
                latest_version=manifest.version,
                force_required=False,
                package_type=manifest.package_type,
                notes=manifest.notes,
                published_at=manifest.published_at,
                download_url=manifest.download_url,
                sha256=manifest.sha256,
                size=manifest.size,
                error="",
                last_checked=time.time(),
            )
            return
        self._set_state(
            state=STATE_AVAILABLE,
            latest_version=manifest.version,
            force_required=force_required,
            package_type=manifest.package_type,
            notes=manifest.notes,
            published_at=manifest.published_at,
            download_url=manifest.download_url,
            sha256=manifest.sha256,
            size=manifest.size,
            error="",
            last_checked=time.time(),
        )

    def _do_download(self):
        with self._lock:
            manifest = self.manifest
        if not manifest or not manifest.download_url:
            self._set_state(state=STATE_ERROR, error="无可用更新包")
            return
        _ensure_dirs()
        ext = ".zip"
        url_lower = manifest.download_url.lower()
        for cand in (".zip", ".tar.gz", ".7z"):
            if url_lower.endswith(cand):
                ext = cand
                break
        pkg_name = f"update-{manifest.version}-{manifest.package_type}{ext}"
        dst = os.path.join(RUNTIME_STAGING_DIR, pkg_name)
        self._set_state(state=STATE_DOWNLOADING, progress=0.0, error="")

        def _progress(p: float):
            self._set_state(progress=p)

        err = _http_download(manifest.download_url, dst, manifest.sha256, progress_cb=_progress)
        if err:
            self._set_state(state=STATE_ERROR, error=err, progress=0.0)
            return
        # 写入 staging 元数据, 供 update_apply.py 使用
        meta = {
            "version": manifest.version,
            "package_type": manifest.package_type,
            "target": manifest.target,
            "package_path": dst,
            "sha256": manifest.sha256,
            "size": manifest.size,
            "force_update": bool(manifest.force_update),
            "current_version_at_stage": APP_VERSION,
            "staged_at": time.time(),
            "exe_path": sys.executable if getattr(sys, "frozen", False) else "",
        }
        try:
            meta_path = os.path.join(RUNTIME_STAGING_DIR, "pending.json")
            with open(meta_path, "w", encoding="utf-8") as f:
                json.dump(meta, f, ensure_ascii=False, indent=2)
        except Exception as e:
            self._set_state(state=STATE_ERROR, error=f"写入 staging 失败: {e}")
            return
        self._set_state(state=STATE_READY, progress=1.0, error="")


_GLOBAL_MANAGER: Optional[UpdateManager] = None
_GLOBAL_LOCK = threading.Lock()
_APPLY_SCHEDULED = False
_APPLY_SCHEDULE_LOCK = threading.Lock()


def get_manager() -> UpdateManager:
    global _GLOBAL_MANAGER
    with _GLOBAL_LOCK:
        if _GLOBAL_MANAGER is None:
            host = ""
            try:
                from config import SettingsManager
                host = (SettingsManager().get("update_host", "") or "").strip()
            except Exception:
                pass
            _GLOBAL_MANAGER = UpdateManager(host=host or None)
        return _GLOBAL_MANAGER


def has_pending_update() -> bool:
    """检查 staging 中是否存在待应用的更新包."""
    try:
        meta_path = os.path.join(RUNTIME_STAGING_DIR, "pending.json")
        return os.path.exists(meta_path)
    except Exception:
        return False


def schedule_apply_on_exit() -> bool:
    """在主进程退出前调用. 启动外部 helper 来应用 staging 中的更新.

    helper 需要等待主进程退出, 所以这里 spawn 一个 detached 子进程.
    返回 True 表示已经成功调度.
    """
    global _APPLY_SCHEDULED
    if not has_pending_update():
        return False
    with _APPLY_SCHEDULE_LOCK:
        if _APPLY_SCHEDULED:
            return True
        try:
            import subprocess
            helper = _resolve_apply_helper()
            if not helper:
                return False
            # update.exe / update_apply.exe 直接运行；.py 由当前 Python（平台上可能是 frozen exe）运行
            helper_lower = helper.lower()
            if helper_lower.endswith(".exe"):
                args = [helper, str(os.getpid())]
            else:
                args = [sys.executable, helper, str(os.getpid())]
            creationflags = 0
            if os.name == "nt":
                DETACHED_PROCESS = 0x00000008
                CREATE_NEW_PROCESS_GROUP = 0x00000200
                creationflags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
            subprocess.Popen(
                args,
                cwd=BASE_DIR,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                close_fds=True,
                creationflags=creationflags,
            )
            _APPLY_SCHEDULED = True
            return True
        except Exception as e:
            print(f"[Updater] schedule_apply_on_exit failed: {e}")
            return False


def _resolve_apply_helper() -> str:
    """定位 update.exe（优先）或 update_apply.py 脚本。"""
    candidates = []
    try:
        if getattr(sys, "frozen", False):
            exe_dir = os.path.dirname(sys.executable)
            candidates.append(os.path.join(exe_dir, "update.exe"))
            candidates.append(os.path.join(exe_dir, "update_apply.exe"))
            candidates.append(os.path.join(exe_dir, "update_apply.py"))
            candidates.append(os.path.join(exe_dir, "runtime", "update_apply.py"))
            mei = getattr(sys, "_MEIPASS", "")
            if mei:
                candidates.append(os.path.join(mei, "update_apply.py"))
        here = os.path.dirname(os.path.abspath(__file__))
        candidates.append(os.path.join(here, "update_apply.py"))
    except Exception:
        pass
    for c in candidates:
        if c and os.path.exists(c):
            return c
    return ""
