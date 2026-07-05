# -*- coding: utf-8 -*-
# Focused self-test for updater popup/manual-check edge cases.

from __future__ import annotations

import os
import sys
import time
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import gui_modules.sao_gui_status_updater_mixin as updater_mixin
from gui_modules.sao_gui_status_updater_mixin import SAOPlayerGUIStatusUpdaterMixin
import updater.sao_updater as sao_updater


class _FakeRoot:
    def __init__(self) -> None:
        self.jobs = []

    def after(self, delay_ms, callback):
        self.jobs.append((delay_ms, callback))
        return f"after-{len(self.jobs)}"

    def run_next(self):
        _delay, callback = self.jobs.pop(0)
        callback()

    def run_all(self):
        while self.jobs:
            self.run_next()


class _FakeOverlay:
    def __init__(self, alive: bool = True) -> None:
        self.alive = alive

    def winfo_exists(self) -> bool:
        return self.alive


class _FakeMenu:
    def __init__(self) -> None:
        self.visible = True
        self._closing = False
        self._overlay = _FakeOverlay(alive=True)
        self._hud_overlay = None
        self.close_calls = 0

    def close(self) -> None:
        self.close_calls += 1
        self._closing = True


class _PopupOwner(SAOPlayerGUIStatusUpdaterMixin):
    def __init__(self) -> None:
        self._destroyed = False
        self._update_panel = None
        self._last_update_popup_key = ""
        self.alerts = []

    def _get_update_view(self, snapshot=None):
        snapshot = snapshot or SimpleNamespace()
        return {
            "state": getattr(snapshot, "state", "idle"),
            "latest_version": getattr(snapshot, "latest_version", ""),
            "force_required": getattr(snapshot, "force_required", False),
            "progress": getattr(snapshot, "progress", 0.0),
        }

    def _show_entity_alert(self, title, message="", display_time=5.0):
        self.alerts.append((title, message, display_time))


class _FlowOwner(SAOPlayerGUIStatusUpdaterMixin):
    def __init__(self) -> None:
        self._destroyed = False
        self.root = _FakeRoot()
        self._sao_menu = _FakeMenu()
        self.runs = 0

    def _run_update_check_interactive(self) -> None:
        self.runs += 1


class _ManualOwner(SAOPlayerGUIStatusUpdaterMixin):
    def __init__(self) -> None:
        self._destroyed = False
        self.root = _FakeRoot()
        self._float = object()
        self._sao_menu = SimpleNamespace(visible=False, _overlay=None, _hud_overlay=None)
        self._fisheye_ov = None
        self._sao_menu_close_pending = False
        self._manual_update_check_inflight = False
        self.available_prompts = []

    def _prompt_update_available(self, snapshot) -> None:
        self.available_prompts.append(snapshot)

    def _any_panel_open(self) -> bool:
        return False


class _FakeManager:
    def __init__(self, initial_state="idle") -> None:
        self.listeners = []
        self.check_calls = 0
        self._snapshot = SimpleNamespace(
            state=initial_state,
            latest_version="",
            progress=0.0,
            error="",
        )

    def snapshot(self):
        return self._snapshot

    def add_listener(self, cb) -> None:
        self.listeners.append(cb)

    def remove_listener(self, cb) -> None:
        if cb in self.listeners:
            self.listeners.remove(cb)

    def check_async(self) -> None:
        self.check_calls += 1

    def emit(self, state, **kwargs) -> None:
        snap = SimpleNamespace(state=state, latest_version="", progress=0.0, error="", **kwargs)
        for cb in list(self.listeners):
            cb(snap)


class _DialogRecorder:
    calls = []

    @classmethod
    def showinfo(cls, parent, title, message, on_ok=None):
        cls.calls.append(("showinfo", title, message))
        return SimpleNamespace()

    @classmethod
    def ask(cls, parent, title, message, on_ok=None, on_cancel=None):
        cls.calls.append(("ask", title, message))
        return SimpleNamespace()


def test_error_popup_is_suppressed() -> None:
    owner = _PopupOwner()
    snapshot = SimpleNamespace(
        state="error",
        latest_version="4.0.12",
        force_required=False,
        progress=0.0,
        skipped_version="",
        error="无法连接更新服务",
    )
    assert owner._build_update_popup_payload(snapshot) is None
    owner._maybe_show_update_popup(snapshot)
    assert owner.alerts == []


def test_available_popup_still_surfaces() -> None:
    owner = _PopupOwner()
    snapshot = SimpleNamespace(
        state="available",
        latest_version="4.0.12",
        force_required=False,
        progress=0.0,
        skipped_version="",
    )
    owner._maybe_show_update_popup(snapshot)
    assert len(owner.alerts) == 1
    assert owner.alerts[0][0] == "SYSTEM UPDATE"


def test_update_popup_tolerates_bad_display_time() -> None:
    owner = _PopupOwner()
    owner._build_update_popup_payload = lambda _snapshot=None: {
        "key": "available:bad-time",
        "title": "SYSTEM UPDATE",
        "message": "bad display time",
        "display_time": "bad",
    }

    owner._maybe_show_update_popup(SimpleNamespace())

    assert owner.alerts == [("SYSTEM UPDATE", "bad display time", 5.0)]


def test_manual_check_waits_for_menu_close() -> None:
    owner = _FlowOwner()
    owner._check_for_updates_interactive()
    assert owner._sao_menu.close_calls == 1
    assert owner.runs == 0
    assert owner.root.jobs

    owner._sao_menu.visible = False
    owner.root.run_next()
    assert owner.runs == 0
    assert owner.root.jobs

    owner._sao_menu._overlay.alive = False
    owner.root.run_next()
    assert owner.runs == 1


def test_latest_manual_check_has_no_checking_dialog() -> None:
    owner = _ManualOwner()
    mgr = _FakeManager()
    original_dialog = updater_mixin.SAODialog
    original_get_manager = sao_updater.get_manager
    _DialogRecorder.calls = []
    try:
        updater_mixin.SAODialog = _DialogRecorder
        sao_updater.get_manager = lambda: mgr
        owner._run_update_check_interactive()
        assert mgr.check_calls == 1
        assert _DialogRecorder.calls == []
        mgr.emit(sao_updater.STATE_UP_TO_DATE)
        owner.root.run_all()
        assert _DialogRecorder.calls == [
            ("showinfo", "更新", f"已是最新版本 ({updater_mixin.APP_VERSION_LABEL})")
        ]
        assert all("正在检查更新" not in call[2] for call in _DialogRecorder.calls)
        assert owner._manual_update_check_inflight is False
    finally:
        updater_mixin.SAODialog = original_dialog
        sao_updater.get_manager = original_get_manager


def test_latest_manual_result_waits_for_menu_overlay_destroy() -> None:
    owner = _ManualOwner()
    owner._manual_update_check_inflight = True
    owner._sao_menu = SimpleNamespace(
        visible=False,
        _overlay=_FakeOverlay(alive=True),
        _hud_overlay=None,
    )
    snapshot = SimpleNamespace(
        state=sao_updater.STATE_UP_TO_DATE,
        latest_version="",
        progress=0.0,
        error="",
    )
    original_dialog = updater_mixin.SAODialog
    _DialogRecorder.calls = []
    try:
        updater_mixin.SAODialog = _DialogRecorder
        owner._show_manual_update_result(snapshot)
        assert _DialogRecorder.calls == []
        assert owner._manual_update_check_inflight is True
        assert owner.root.jobs

        owner._sao_menu._overlay.alive = False
        owner.root.run_next()
        assert _DialogRecorder.calls == [
            ("showinfo", "更新", f"已是最新版本 ({updater_mixin.APP_VERSION_LABEL})")
        ]
        assert owner._manual_update_check_inflight is False
    finally:
        updater_mixin.SAODialog = original_dialog


def test_latest_manual_result_waits_for_motion_blur_to_settle() -> None:
    owner = _ManualOwner()
    owner._manual_update_check_inflight = True
    owner._motion_blur_active_count = 1
    owner._motion_blur_active_until = time.time() + 10.0
    snapshot = SimpleNamespace(
        state=sao_updater.STATE_UP_TO_DATE,
        latest_version="",
        progress=0.0,
        error="",
    )
    original_dialog = updater_mixin.SAODialog
    _DialogRecorder.calls = []
    try:
        updater_mixin.SAODialog = _DialogRecorder
        owner._show_manual_update_result(snapshot)
        assert _DialogRecorder.calls == []
        assert owner._manual_update_check_inflight is True
        assert owner.root.jobs

        owner._motion_blur_active_count = 0
        owner._motion_blur_active_until = time.time() - 1.0
        owner.root.run_next()
        assert _DialogRecorder.calls == [
            ("showinfo", "更新", f"已是最新版本 ({updater_mixin.APP_VERSION_LABEL})")
        ]
        assert owner._manual_update_check_inflight is False
    finally:
        updater_mixin.SAODialog = original_dialog


def test_update_check_retries_transient_fetch_once() -> None:
    calls = []
    original_http_get_json = sao_updater._http_get_json
    original_sleep = sao_updater.time.sleep
    original_delay = sao_updater.CHECK_RETRY_DELAY

    def fake_http_get_json(url):
        calls.append(url)
        if len(calls) == 1:
            return 0, None, "无法连接更新服务: temporary dns failure"
        return 200, {"available": False, "version": sao_updater.APP_VERSION}, ""

    try:
        sao_updater._http_get_json = fake_http_get_json
        sao_updater.time.sleep = lambda _seconds: None
        sao_updater.CHECK_RETRY_DELAY = 0.0
        mgr = sao_updater.UpdateManager(host="http://updates.example")
        mgr._do_check()
        st = mgr.snapshot()
        assert len(calls) == 2
        assert st.state == sao_updater.STATE_UP_TO_DATE
        assert st.error == ""
    finally:
        sao_updater._http_get_json = original_http_get_json
        sao_updater.time.sleep = original_sleep
        sao_updater.CHECK_RETRY_DELAY = original_delay


def main() -> int:
    test_error_popup_is_suppressed()
    test_available_popup_still_surfaces()
    test_update_popup_tolerates_bad_display_time()
    test_manual_check_waits_for_menu_close()
    test_latest_manual_check_has_no_checking_dialog()
    test_latest_manual_result_waits_for_menu_overlay_destroy()
    test_latest_manual_result_waits_for_motion_blur_to_settle()
    test_update_check_retries_transient_fetch_once()
    print("update_check_flow_selftest: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
