# -*- coding: utf-8 -*-
"""SAO Auto - 外部更新应用器

由 sao_updater.schedule_apply_on_exit() 在主进程退出前 detach 启动。
读取 BASE_DIR/staging/pending.json，等待主进程退出后：
  - runtime-delta: 解压模块更新到 BASE_DIR/
  - full-package: 替换完整客户端文件

本版本为可视化 SAO 风格 updater：
  - 显示接管、备份、替换、重启四个阶段
  - 带进度条与状态动画
  - full-package 下会对 update.exe 自身使用延迟自替换，避免运行中覆盖失败
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from queue import Empty, Queue
from typing import Callable, Dict, List, Optional, Tuple

try:
    import tkinter as tk
except Exception:  # pragma: no cover - 回退到无界面模式
    tk = None


POLL_INTERVAL = 0.5
WAIT_TIMEOUT = 60.0

UI_BG = "#0a0e14"
UI_CARD = "#111820"
UI_BORDER = "#1a3a4e"
UI_HEADER = "#1a2030"
UI_TEXT = "#e8f4f8"
UI_SUBTEXT = "#84a6b8"
UI_DIM = "#4f6b78"
UI_ACCENT = "#68e4ff"
UI_GOLD = "#f3af12"
UI_SUCCESS = "#9ad334"
UI_ERROR = "#ff707a"
UI_TRACK = "#142335"


def _log(msg: str, base: str):
    try:
        path = os.path.join(base, "update_apply.log")
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}\n")
    except Exception:
        pass


def _emit(progress_cb: Optional[Callable[[Dict[str, object]], None]], **data):
    if progress_cb is None:
        return
    try:
        progress_cb(data)
    except Exception:
        pass


def _wait_for_exit(pid: int, timeout: float = WAIT_TIMEOUT) -> bool:
    """等待主进程退出。仅 Windows: 通过 OpenProcess 检查。"""
    if pid <= 0:
        return True
    deadline = time.time() + timeout
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        kernel32 = ctypes.windll.kernel32
        STILL_ACTIVE = 259
        while time.time() < deadline:
            handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not handle:
                return True
            exit_code = wintypes.DWORD()
            ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code))
            kernel32.CloseHandle(handle)
            if not ok:
                return True
            if exit_code.value != STILL_ACTIVE:
                return True
            time.sleep(POLL_INTERVAL)
        return False

    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            return True
        time.sleep(POLL_INTERVAL)
    return False


def _normalize_rel(rel_path: str) -> str:
    rel = str(rel_path or "").replace("\\", "/").lstrip("/")
    if not rel:
        raise ValueError("空路径")
    parts = rel.split("/")
    if ".." in parts or os.path.isabs(rel):
        raise ValueError(f"非法路径: {rel}")
    return rel


def _collect_entries(zf: zipfile.ZipFile, base: str, allow_top_level_exe: bool) -> List[Dict[str, object]]:
    entries: List[Dict[str, object]] = []
    for info in zf.infolist():
        if info.is_dir():
            continue
        rel = _normalize_rel(info.filename)
        if not allow_top_level_exe and rel.lower().endswith(".exe") and "/" not in rel:
            raise ValueError(f"runtime-delta 不允许覆盖启动器 exe: {rel}")
        dst = os.path.join(base, rel)
        entries.append({
            "info": info,
            "rel": rel,
            "dst": dst,
            "exists": os.path.exists(dst),
        })
    return entries


def _get_self_rel(base: str) -> str:
    try:
        current = sys.executable if getattr(sys, "frozen", False) else os.path.abspath(__file__)
        return os.path.relpath(current, base).replace("\\", "/").lower()
    except Exception:
        current = sys.executable if getattr(sys, "frozen", False) else __file__
        return os.path.basename(current).replace("\\", "/").lower()


def _apply_zip_package(
    zip_path: str,
    base: str,
    version: str,
    package_type: str,
    progress_cb: Optional[Callable[[Dict[str, object]], None]] = None,
) -> Tuple[List[str], List[Tuple[str, str]], List[str], Optional[Tuple[str, str]]]:
    """应用 zip 更新包，返回 (已应用列表, 备份列表, 新建文件列表, 延迟自替换文件)。"""
    allow_top_level_exe = package_type == "full-package"
    backup_root = os.path.join(base, "backup", version)
    if allow_top_level_exe:
        backup_root = os.path.join(backup_root, "__full__")
    applied: List[str] = []
    backed_up: List[Tuple[str, str]] = []
    created_files: List[str] = []
    staged_self_update: Optional[Tuple[str, str]] = None
    self_rel = _get_self_rel(base)

    with zipfile.ZipFile(zip_path, "r") as zf:
        entries = _collect_entries(zf, base, allow_top_level_exe)
        existing_entries = [entry for entry in entries if bool(entry.get("exists"))]
        total_backups = len(existing_entries)
        if total_backups:
            _emit(
                progress_cb,
                phase="backup",
                step=1,
                headline="正在备份当前模块",
                detail="准备写入安全副本…",
                progress=0.0,
                indeterminate=False,
            )
            for index, entry in enumerate(existing_entries, 1):
                rel = str(entry["rel"])
                dst = str(entry["dst"])
                backup_path = os.path.join(backup_root, rel)
                os.makedirs(os.path.dirname(backup_path) or backup_root, exist_ok=True)
                shutil.copy2(dst, backup_path)
                backed_up.append((rel, backup_path))
                _emit(
                    progress_cb,
                    phase="backup",
                    step=1,
                    headline="正在备份当前模块",
                    detail=f"备份 {rel}",
                    progress=float(index) / float(total_backups),
                    indeterminate=False,
                )

        total_files = len(entries)
        if total_files:
            _emit(
                progress_cb,
                phase="apply",
                step=2,
                headline="正在写入新版本文件",
                detail="准备替换目标文件…",
                progress=0.0,
                indeterminate=False,
            )

        for index, entry in enumerate(entries, 1):
            info = entry["info"]
            rel = str(entry["rel"])
            dst = str(entry["dst"])
            os.makedirs(os.path.dirname(dst) or base, exist_ok=True)
            if allow_top_level_exe and rel.lower() == self_rel:
                staged_path = dst + ".new"
                try:
                    if os.path.exists(staged_path):
                        os.remove(staged_path)
                except Exception:
                    pass
                with zf.open(info, "r") as src, open(staged_path, "wb") as out:
                    shutil.copyfileobj(src, out)
                staged_self_update = (staged_path, dst)
                applied.append(rel)
                detail = f"暂存 {rel}，退出后自动切换"
            else:
                tmp_dst = dst + ".tmp-update"
                try:
                    if os.path.exists(tmp_dst):
                        os.remove(tmp_dst)
                except Exception:
                    pass
                try:
                    with zf.open(info, "r") as src, open(tmp_dst, "wb") as out:
                        shutil.copyfileobj(src, out)
                    os.replace(tmp_dst, dst)
                finally:
                    try:
                        if os.path.exists(tmp_dst):
                            os.remove(tmp_dst)
                    except Exception:
                        pass
                if not bool(entry["exists"]):
                    created_files.append(dst)
                applied.append(rel)
                detail = f"写入 {rel}"
            _emit(
                progress_cb,
                phase="apply",
                step=2,
                headline="正在写入新版本文件",
                detail=detail,
                progress=float(index) / float(total_files),
                indeterminate=False,
            )

    return applied, backed_up, created_files, staged_self_update


def _rollback(
    base: str,
    backed_up: List[Tuple[str, str]],
    created_files: Optional[List[str]] = None,
    progress_cb: Optional[Callable[[Dict[str, object]], None]] = None,
):
    created_files = list(created_files or [])
    total = len(backed_up) + len(created_files)
    if total:
        _emit(
            progress_cb,
            phase="rollback",
            step=2,
            headline="正在回滚已备份文件",
            detail="恢复旧版本中…",
            progress=0.0,
            indeterminate=False,
        )
    completed = 0
    for dst in sorted(created_files, key=len, reverse=True):
        try:
            if os.path.exists(dst):
                os.remove(dst)
        except Exception:
            pass
        completed += 1
        rel = os.path.relpath(dst, base).replace("\\", "/")
        _emit(
            progress_cb,
            phase="rollback",
            step=2,
            headline="正在回滚已备份文件",
            detail=f"移除 {rel}",
            progress=float(completed) / float(total),
            indeterminate=False,
        )
    for rel, backup_path in backed_up:
        try:
            dst = os.path.join(base, rel)
            os.makedirs(os.path.dirname(dst) or base, exist_ok=True)
            shutil.copy2(backup_path, dst)
        except Exception:
            pass
        completed += 1
        _emit(
            progress_cb,
            phase="rollback",
            step=2,
            headline="正在回滚已备份文件",
            detail=f"恢复 {rel}",
            progress=float(completed) / float(total),
            indeterminate=False,
        )


def _schedule_self_replace(staged_path: str, live_path: str, base: str) -> bool:
    if os.name != "nt" or not staged_path or not live_path:
        return False
    script_path = os.path.join(base, f"_swap_update_{int(time.time() * 1000)}.cmd")
    try:
        with open(script_path, "w", encoding="utf-8") as f:
            f.write("@echo off\r\n")
            f.write("ping 127.0.0.1 -n 4 > nul\r\n")
            f.write(f'copy /y "{staged_path}" "{live_path}" > nul\r\n')
            f.write(f'if exist "{staged_path}" del /f /q "{staged_path}" > nul 2>nul\r\n')
            f.write('del /f /q "%~f0" > nul 2>nul\r\n')
        creationflags = 0
        if os.name == "nt":
            DETACHED_PROCESS = 0x00000008
            CREATE_NEW_PROCESS_GROUP = 0x00000200
            creationflags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        subprocess.Popen(
            ["cmd.exe", "/c", script_path],
            cwd=base,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            close_fds=True,
            creationflags=creationflags,
        )
        return True
    except Exception:
        return False


def _cleanup_staging(package_path: str, pending_path: str):
    try:
        os.remove(package_path)
    except Exception:
        pass
    try:
        os.remove(pending_path)
    except Exception:
        pass


def _resolve_restart_target(base: str, exe_path: str) -> Optional[str]:
    if exe_path and os.path.exists(exe_path):
        return exe_path
    candidate = os.path.join(base, "XiaoACTUI.exe")
    if os.path.exists(candidate):
        return candidate
    return None


def _restart_target(target: str, base: str) -> bool:
    if not target:
        return False
    creationflags = 0
    if os.name == "nt":
        DETACHED_PROCESS = 0x00000008
        CREATE_NEW_PROCESS_GROUP = 0x00000200
        creationflags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    subprocess.Popen(
        [target],
        cwd=base,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        close_fds=True,
        creationflags=creationflags,
    )
    return True


def run_apply_flow(
    pid: int,
    base: str,
    meta: Dict[str, object],
    progress_cb: Optional[Callable[[Dict[str, object]], None]] = None,
) -> int:
    version = str(meta.get("version") or "")
    package_type = str(meta.get("package_type") or "runtime-delta")
    package_path = str(meta.get("package_path") or "")
    exe_path = str(meta.get("exe_path") or "")
    pending_path = os.path.join(base, "staging", "pending.json")

    if not package_path or not os.path.exists(package_path):
        detail = f"更新包不存在: {package_path or 'unknown'}"
        _log(detail, base)
        _emit(
            progress_cb,
            phase="error",
            step=0,
            headline="更新包缺失",
            detail=detail,
            progress=0.0,
            indeterminate=False,
            status="error",
            can_close=True,
        )
        return 1

    _log(f"waiting for pid={pid} to exit", base)
    _emit(
        progress_cb,
        phase="wait",
        step=0,
        headline="等待主程序退出",
        detail="更新器已接管流程，正在等待 XiaoACTUI 释放文件句柄…",
        progress=None,
        indeterminate=True,
        meta=f"v{version or '?'} · {package_type}",
    )
    if not _wait_for_exit(pid):
        detail = "等待主程序退出超时，请确认 XiaoACTUI 已关闭后重试。"
        _log(detail, base)
        _emit(
            progress_cb,
            phase="error",
            step=0,
            headline="无法接管更新",
            detail=detail,
            progress=0.0,
            indeterminate=False,
            status="error",
            can_close=True,
        )
        return 2

    time.sleep(0.8)
    backed_up: List[Tuple[str, str]] = []
    created_files: List[str] = []
    staged_self_update: Optional[Tuple[str, str]] = None
    try:
        applied, backed_up, created_files, staged_self_update = _apply_zip_package(
            zip_path=package_path,
            base=base,
            version=version or "unknown",
            package_type=package_type,
            progress_cb=progress_cb,
        )
        _log(f"applied {len(applied)} files for {package_type} v{version}", base)
    except Exception as e:
        _log(f"apply failed: {e}; rolling back", base)
        _rollback(base, backed_up, created_files, progress_cb=progress_cb)
        if staged_self_update:
            try:
                os.remove(staged_self_update[0])
            except Exception:
                pass
        _emit(
            progress_cb,
            phase="error",
            step=2,
            headline="更新失败",
            detail=str(e),
            progress=0.0,
            indeterminate=False,
            status="error",
            can_close=True,
        )
        return 3

    _cleanup_staging(package_path, pending_path)

    if staged_self_update:
        if _schedule_self_replace(staged_self_update[0], staged_self_update[1], base):
            _log("scheduled delayed self-replace for update.exe", base)
        else:
            _log("failed to schedule delayed self-replace for update.exe", base)

    _emit(
        progress_cb,
        phase="restart",
        step=3,
        headline="正在启动新版本",
        detail="模块替换完成，准备重新启动客户端…",
        progress=None,
        indeterminate=True,
        status="active",
    )

    target = _resolve_restart_target(base, exe_path)
    launched = False
    if target:
        try:
            _log(f"restarting {target}", base)
            launched = _restart_target(target, base)
        except Exception as e:
            _log(f"restart failed: {e}", base)

    if launched:
        _emit(
            progress_cb,
            phase="done",
            step=3,
            headline="更新完成",
            detail=f"已切换到 v{version or '?'}，客户端正在重新启动。",
            progress=1.0,
            indeterminate=False,
            status="success",
            can_close=True,
            auto_close=True,
        )
    else:
        _emit(
            progress_cb,
            phase="done",
            step=3,
            headline="更新完成",
            detail="文件已替换完成，请手动启动 XiaoACTUI.exe。",
            progress=1.0,
            indeterminate=False,
            status="success",
            can_close=True,
            auto_close=False,
        )
    return 0


class UpdateApplyWindow:
    WIDTH = 580
    HEIGHT = 320
    STEPS = ("接管", "备份", "替换", "重启")

    def __init__(self, pid: int, base: str, meta: Dict[str, object]):
        if tk is None:
            raise RuntimeError("tkinter unavailable")
        self.pid = pid
        self.base = base
        self.meta = meta
        self.version = str(meta.get("version") or "")
        self.package_type = str(meta.get("package_type") or "runtime-delta")
        self._queue: Queue = Queue()
        self._result_code = 0
        self._allow_close = False
        self._running = True
        self._pulse_t0 = time.time()
        self._phase = "wait"
        self._step = 0
        self._target_progress = 0.0
        self._display_progress = 0.0
        self._progress_indeterminate = True
        self._status_text = "HANDOFF"
        self._status_color = UI_ACCENT
        self._close_after_at: Optional[float] = None

        self.root = tk.Tk()
        self.root.withdraw()
        self.root.title("SAO Auto Updater")
        self.root.configure(bg=UI_BG)
        self.root.resizable(False, False)
        self.root.attributes("-topmost", True)
        try:
            self.root.attributes("-alpha", 0.0)
            self._alpha_supported = True
        except Exception:
            self._alpha_supported = False
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self._apply_icon()
        self._build_ui()
        self._center_window()
        self.root.deiconify()
        self.root.after(20, self._poll_events)
        self.root.after(33, self._animate)
        self.root.after(80, self._start_worker)

    def _apply_icon(self):
        icon_path = os.path.join(self.base, "icon.ico")
        if not os.path.exists(icon_path):
            return
        try:
            self.root.iconbitmap(default=icon_path)
            self.root.iconbitmap(icon_path)
        except Exception:
            pass

    def _center_window(self):
        self.root.update_idletasks()
        sw = self.root.winfo_screenwidth()
        sh = self.root.winfo_screenheight()
        x = max(0, (sw - self.WIDTH) // 2)
        y = max(0, (sh - self.HEIGHT) // 2)
        self.root.geometry(f"{self.WIDTH}x{self.HEIGHT}+{x}+{y}")

    def _build_ui(self):
        border = tk.Frame(self.root, bg=UI_BORDER, padx=1, pady=1)
        border.pack(fill=tk.BOTH, expand=True)

        card = tk.Frame(border, bg=UI_CARD)
        card.pack(fill=tk.BOTH, expand=True)

        header = tk.Frame(card, bg=UI_HEADER, height=40)
        header.pack(fill=tk.X)
        header.pack_propagate(False)
        tk.Frame(header, bg=UI_ACCENT, width=4).pack(side=tk.LEFT, fill=tk.Y, padx=(8, 0), pady=8)
        tk.Label(
            header,
            text="SYSTEM UPDATER",
            bg=UI_HEADER,
            fg=UI_TEXT,
            font=("Segoe UI Semibold", 11),
        ).pack(side=tk.LEFT, padx=10)
        tk.Label(
            header,
            text="SAO://TRANSFER",
            bg=UI_HEADER,
            fg=UI_DIM,
            font=("Consolas", 8),
        ).pack(side=tk.RIGHT, padx=12)

        body = tk.Frame(card, bg=UI_CARD)
        body.pack(fill=tk.BOTH, expand=True, padx=18, pady=(16, 14))

        top_row = tk.Frame(body, bg=UI_CARD)
        top_row.pack(fill=tk.X)
        self._tag_label = tk.Label(
            top_row,
            text="SYSTEM UPDATE",
            bg=UI_CARD,
            fg=UI_GOLD,
            font=("Consolas", 9),
        )
        self._tag_label.pack(side=tk.LEFT)
        self._status_label = tk.Label(
            top_row,
            text="HANDOFF",
            bg=UI_HEADER,
            fg=UI_ACCENT,
            font=("Consolas", 9, "bold"),
            padx=10,
            pady=4,
        )
        self._status_label.pack(side=tk.RIGHT)

        self._headline_label = tk.Label(
            body,
            text="等待主程序退出",
            bg=UI_CARD,
            fg=UI_TEXT,
            anchor="w",
            justify=tk.LEFT,
            font=("Microsoft YaHei UI", 18, "bold"),
        )
        self._headline_label.pack(fill=tk.X, pady=(12, 6))

        self._detail_label = tk.Label(
            body,
            text="更新器已接管流程，准备释放文件句柄。",
            bg=UI_CARD,
            fg=UI_SUBTEXT,
            anchor="w",
            justify=tk.LEFT,
            wraplength=self.WIDTH - 70,
            font=("Microsoft YaHei UI", 10),
        )
        self._detail_label.pack(fill=tk.X)

        self._meta_label = tk.Label(
            body,
            text=f"v{self.version or '?'} · {self.package_type}",
            bg=UI_CARD,
            fg=UI_DIM,
            anchor="w",
            justify=tk.LEFT,
            font=("Consolas", 9),
        )
        self._meta_label.pack(fill=tk.X, pady=(8, 12))

        self._steps_canvas = tk.Canvas(body, width=530, height=56, bg=UI_CARD, highlightthickness=0, bd=0)
        self._steps_canvas.pack(fill=tk.X)

        self._progress_canvas = tk.Canvas(body, width=530, height=28, bg=UI_CARD, highlightthickness=0, bd=0)
        self._progress_canvas.pack(fill=tk.X, pady=(14, 8))

        self._footer_canvas = tk.Canvas(body, width=530, height=44, bg=UI_CARD, highlightthickness=0, bd=0)
        self._footer_canvas.pack(fill=tk.X, side=tk.BOTTOM)

        bottom_row = tk.Frame(body, bg=UI_CARD)
        bottom_row.pack(fill=tk.X, side=tk.BOTTOM, pady=(10, 0))
        tk.Label(
            bottom_row,
            text="SYS:UPDATER",
            bg=UI_CARD,
            fg=UI_DIM,
            font=("Consolas", 8),
        ).pack(side=tk.LEFT)
        self._close_button = tk.Label(
            bottom_row,
            text="关闭",
            bg=UI_CARD,
            fg=UI_SUBTEXT,
            font=("Microsoft YaHei UI", 9, "bold"),
            cursor="hand2",
            padx=8,
            pady=2,
        )
        self._close_button.bind("<Button-1>", lambda _e: self._on_close())
        self._close_button.pack(side=tk.RIGHT)
        self._close_button.pack_forget()

    def _start_worker(self):
        worker = threading.Thread(target=self._worker_main, name="sao-update-apply-ui", daemon=True)
        worker.start()

    def _worker_main(self):
        code = run_apply_flow(self.pid, self.base, self.meta, progress_cb=self._queue.put)
        self._result_code = code

    def _on_close(self):
        if not self._allow_close:
            return
        self._running = False
        try:
            self.root.destroy()
        except Exception:
            pass

    def _poll_events(self):
        dirty = False
        while True:
            try:
                event = self._queue.get_nowait()
            except Empty:
                break
            self._apply_event(event)
            dirty = True
        if dirty:
            self._draw_status()
        if self._running and self.root.winfo_exists():
            self.root.after(24, self._poll_events)

    def _apply_event(self, event: Dict[str, object]):
        phase = str(event.get("phase") or self._phase)
        self._phase = phase
        try:
            self._step = max(0, min(len(self.STEPS) - 1, int(event.get("step", self._step))))
        except Exception:
            pass
        headline = event.get("headline")
        if headline is not None:
            self._headline_label.configure(text=str(headline))
        detail = event.get("detail")
        if detail is not None:
            self._detail_label.configure(text=str(detail))
        meta = event.get("meta")
        if meta is not None:
            self._meta_label.configure(text=str(meta))
        if "progress" in event:
            progress = event.get("progress")
            if progress is None:
                self._progress_indeterminate = True
            else:
                try:
                    self._target_progress = max(0.0, min(1.0, float(progress)))
                except Exception:
                    self._target_progress = 0.0
                self._progress_indeterminate = bool(event.get("indeterminate", False))
        elif "indeterminate" in event:
            self._progress_indeterminate = bool(event.get("indeterminate", False))

        status = str(event.get("status") or "")
        badge = str(event.get("badge") or "")
        if badge:
            self._status_text = badge
        elif status == "error" or phase == "error":
            self._status_text = "ERROR"
        elif status == "success" or phase == "done":
            self._status_text = "COMPLETE"
        elif phase == "wait":
            self._status_text = "HANDOFF"
        elif phase == "backup":
            self._status_text = "BACKUP"
        elif phase == "apply":
            self._status_text = "TRANSFER"
        elif phase == "restart":
            self._status_text = "RESTART"
        elif phase == "rollback":
            self._status_text = "ROLLBACK"

        if status == "error" or phase == "error":
            self._status_color = UI_ERROR
        elif status == "success" or phase == "done":
            self._status_color = UI_SUCCESS
        elif phase == "backup":
            self._status_color = UI_GOLD
        else:
            self._status_color = UI_ACCENT
        self._status_label.configure(text=self._status_text, fg=self._status_color)

        if event.get("can_close"):
            self._allow_close = True
            if not self._close_button.winfo_manager():
                self._close_button.pack(side=tk.RIGHT)
        if event.get("auto_close"):
            self._close_after_at = time.time() + 1.3

    def _draw_status(self):
        self._draw_steps(time.time())
        self._draw_progress(time.time())
        self._draw_footer(time.time())

    def _draw_steps(self, now: float):
        cv = self._steps_canvas
        cv.delete("all")
        width = max(1, cv.winfo_width() or 530)
        base_y = 18
        left = 34
        right = width - 34
        span = right - left
        pulse = 0.5 + 0.5 * math.sin((now - self._pulse_t0) * 4.2)
        for index, title in enumerate(self.STEPS):
            cx = left + (span * index / max(1, len(self.STEPS) - 1))
            if index < self._step:
                color = UI_GOLD
                radius = 8
                text_fg = UI_TEXT
            elif index == self._step:
                color = self._status_color
                radius = 8 + pulse * 2.6
                text_fg = UI_TEXT
            else:
                color = UI_DIM
                radius = 7
                text_fg = UI_SUBTEXT
            if index < len(self.STEPS) - 1:
                nx = left + (span * (index + 1) / max(1, len(self.STEPS) - 1))
                line_color = UI_GOLD if index < self._step else UI_BORDER
                cv.create_line(cx + 12, base_y, nx - 12, base_y, fill=line_color, width=2)
            cv.create_oval(cx - radius, base_y - radius, cx + radius, base_y + radius, fill=UI_TRACK, outline=color, width=2)
            if index <= self._step:
                inner = max(3.5, radius - 3.2)
                fill = color if index < self._step else color
                cv.create_oval(cx - inner, base_y - inner, cx + inner, base_y + inner, fill=fill, outline="")
            cv.create_text(cx, 42, text=title, fill=text_fg, font=("Microsoft YaHei UI", 9, "bold"))

    def _draw_progress(self, now: float):
        cv = self._progress_canvas
        cv.delete("all")
        width = max(1, cv.winfo_width() or 530)
        x0 = 2
        x1 = width - 2
        y0 = 4
        y1 = 24
        cv.create_rectangle(x0, y0, x1, y1, fill=UI_TRACK, outline=UI_BORDER, width=1)
        self._display_progress += (self._target_progress - self._display_progress) * 0.18
        if abs(self._target_progress - self._display_progress) < 0.002:
            self._display_progress = self._target_progress

        if self._progress_indeterminate:
            block_w = max(72, int((x1 - x0) * 0.24))
            travel = (x1 - x0) + block_w
            t = (now - self._pulse_t0) * 1.8
            lead = int((t - math.floor(t)) * travel) - block_w
            fill_left = max(x0 + 1, lead)
            fill_right = min(x1 - 1, lead + block_w)
            if fill_right > fill_left:
                cv.create_rectangle(fill_left, y0 + 1, fill_right, y1 - 1, fill=self._status_color, outline="")
            cv.create_text((x0 + x1) // 2, (y0 + y1) // 2, text="处理中", fill=UI_TEXT, font=("Segoe UI", 9, "bold"))
            return

        fill_w = int((x1 - x0 - 2) * self._display_progress)
        if fill_w > 0:
            cv.create_rectangle(x0 + 1, y0 + 1, x0 + 1 + fill_w, y1 - 1, fill=self._status_color, outline="")
            scan_x = x0 + 1 + int(fill_w * (0.18 + 0.82 * (0.5 + 0.5 * math.sin((now - self._pulse_t0) * 4.5))))
            cv.create_line(scan_x, y0 + 1, scan_x, y1 - 1, fill=UI_TEXT, width=1)
        pct = int(round(self._display_progress * 100.0))
        cv.create_text((x0 + x1) // 2, (y0 + y1) // 2, text=f"{pct}%", fill=UI_TEXT, font=("Segoe UI", 9, "bold"))

    def _draw_footer(self, now: float):
        cv = self._footer_canvas
        cv.delete("all")
        width = max(1, cv.winfo_width() or 530)
        height = max(1, cv.winfo_height() or 44)
        cv.create_line(0, 8, width, 8, fill=UI_BORDER, width=1)
        drift = 18 * math.sin((now - self._pulse_t0) * 0.8)
        cv.create_line(24 + drift, 24, 170 + drift, 24, fill=UI_ACCENT, width=2)
        cv.create_line(width - 180 - drift, 24, width - 34 - drift, 24, fill=UI_GOLD, width=2)
        cv.create_text(8, height - 8, text="STATUS:DEPLOY", anchor="w", fill=UI_DIM, font=("Consolas", 8))
        cv.create_text(width - 8, height - 8, text="SAO://MODULE-SYNC", anchor="e", fill=UI_DIM, font=("Consolas", 8))

    def _animate(self):
        if not self._running:
            return
        if self._alpha_supported:
            try:
                current = float(self.root.attributes("-alpha"))
                self.root.attributes("-alpha", min(1.0, current + 0.10))
            except Exception:
                self._alpha_supported = False
        self._draw_status()
        if self._close_after_at and time.time() >= self._close_after_at:
            self._on_close()
            return
        if self.root.winfo_exists():
            self.root.after(33, self._animate)

    def run(self) -> int:
        self.root.mainloop()
        return self._result_code


def _load_pending_meta(base: str) -> Tuple[str, Optional[Dict[str, object]]]:
    pending_path = os.path.join(base, "staging", "pending.json")
    if not os.path.exists(pending_path):
        _log("no pending.json; exit", base)
        return pending_path, None
    try:
        with open(pending_path, "r", encoding="utf-8") as f:
            return pending_path, json.load(f) or {}
    except Exception as e:
        _log(f"read pending.json failed: {e}", base)
        return pending_path, None


def main() -> int:
    pid = 0
    if len(sys.argv) >= 2:
        try:
            pid = int(sys.argv[1])
        except Exception:
            pid = 0

    if getattr(sys, "frozen", False):
        base = os.path.dirname(sys.executable)
    else:
        base = os.path.dirname(os.path.abspath(__file__))

    _pending_path, meta = _load_pending_meta(base)
    if not meta:
        return 0

    if tk is None:
        return run_apply_flow(pid, base, meta, progress_cb=None)

    try:
        window = UpdateApplyWindow(pid, base, meta)
        return window.run()
    except Exception as e:
        _log(f"ui startup failed: {e}; fallback to headless apply", base)
        return run_apply_flow(pid, base, meta, progress_cb=None)


if __name__ == "__main__":
    sys.exit(main())
