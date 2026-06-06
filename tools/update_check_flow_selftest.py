# -*- coding: utf-8 -*-
"""Focused self-test for updater popup/manual-check edge cases."""

from __future__ import annotations

import os
import sys
from types import SimpleNamespace

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from gui_modules.sao_gui_status_updater_mixin import SAOPlayerGUIStatusUpdaterMixin


class _FakeRoot:
    def __init__(self) -> None:
        self.jobs = []

    def after(self, delay_ms, callback):
        self.jobs.append((delay_ms, callback))
        return f"after-{len(self.jobs)}"

    def run_next(self):
        _delay, callback = self.jobs.pop(0)
        callback()


class _FakeMenu:
    def __init__(self) -> None:
        self.visible = True
        self._closing = False
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


def test_manual_check_waits_for_menu_close() -> None:
    owner = _FlowOwner()
    owner._check_for_updates_interactive()
    assert owner._sao_menu.close_calls == 1
    assert owner.runs == 0
    assert owner.root.jobs

    owner._sao_menu.visible = False
    owner.root.run_next()
    assert owner.runs == 1


def main() -> int:
    test_error_popup_is_suppressed()
    test_available_popup_still_surfaces()
    test_manual_check_waits_for_menu_close()
    print("update_check_flow_selftest: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
