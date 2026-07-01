# -*- coding: utf-8 -*-
"""Focused selftest for settings persistence safety."""

from __future__ import annotations

import json
import os
import tempfile
import traceback

from config import SettingsManager

_FAILURE_DETAIL_LINE_LIMIT = 80
_FINAL_FAILURE_POINT_HEADING = "FAILED CHECK POINTS (final):"


def _failure_detail_tail(detail: str) -> list[str]:
    lines = str(detail or "").splitlines()
    if len(lines) <= _FAILURE_DETAIL_LINE_LIMIT:
        return lines
    omitted = len(lines) - _FAILURE_DETAIL_LINE_LIMIT
    return [f"... omitted {omitted} earlier detail lines ...", *lines[-_FAILURE_DETAIL_LINE_LIMIT:]]


def _failure_point_reason(exc: BaseException) -> str:
    message = str(exc).strip()
    if message:
        first_line = message.splitlines()[0].strip()
        if first_line:
            return first_line
    return f"{type(exc).__name__} without detail"


def _print_final_failed_check_points(exc: BaseException, detail: str) -> None:
    print()
    print("=" * 50)
    print(_FINAL_FAILURE_POINT_HEADING)
    print(f"  1. {type(exc).__name__}: {_failure_point_reason(exc)}")
    detail_lines = _failure_detail_tail(detail)
    if detail_lines:
        print("     detail:")
        for line in detail_lines:
            print(f"       {line}")


def run_selftest() -> dict:
    with tempfile.TemporaryDirectory(prefix="sao_settings_selftest_") as root:
        path = os.path.join(root, "settings.json")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('{"act_plugin_settings": {"sample_plugin": {"overlay_enabled": ')

        settings = SettingsManager(path)
        assert settings._load_error, "corrupt JSON should record a load error"
        assert settings.get_load_error() == settings._load_error, (
            "get_load_error() must expose the same value as _load_error "
            "(AI Editor's load_config surfaces this to the user via a toast)")
        backups = [name for name in os.listdir(root) if name.startswith("settings.json.corrupt")]
        assert backups, "corrupt JSON should be backed up"

        settings.set("act_plugin_enabled", {})
        settings.set("act_plugin_settings", {})
        settings.save()
        assert settings.get_load_error() == "", (
            "a successful save should clear the load-error flag")
        with open(path, "r", encoding="utf-8") as handle:
            repaired = json.load(handle)
        assert repaired == {"act_plugin_enabled": {}, "act_plugin_settings": {}}, repaired

        clean_path = os.path.join(root, "clean_settings.json")
        with open(clean_path, "w", encoding="utf-8") as handle:
            json.dump({"foo": "bar"}, handle)
        clean_settings = SettingsManager(clean_path)
        assert clean_settings.get_load_error() == "", (
            "loading a well-formed settings file must not report a load error")

    return {
        "ok": True,
        "settings_corrupt_backup": True,
        "settings_save_json": True,
        "settings_load_error_getter": True,
    }


def main() -> int:
    try:
        print(json.dumps(run_selftest(), ensure_ascii=False, indent=2))
        return 0
    except Exception as exc:
        print(json.dumps({
            "ok": False,
            "failure_points_deferred": True,
        }, ensure_ascii=False, indent=2))
        _print_final_failed_check_points(exc, traceback.format_exc().rstrip())
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
