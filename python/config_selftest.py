# -*- coding: utf-8 -*-
"""Focused selftest for settings persistence safety."""

from __future__ import annotations

import json
import os
import tempfile

from config import SettingsManager


def run_selftest() -> dict:
    with tempfile.TemporaryDirectory(prefix="sao_settings_selftest_") as root:
        path = os.path.join(root, "settings.json")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write('{"act_plugin_settings": {"script_stickwoman_csharp": {"overlay_enabled": ')

        settings = SettingsManager(path)
        assert settings._load_error, "corrupt JSON should record a load error"
        backups = [name for name in os.listdir(root) if name.startswith("settings.json.corrupt")]
        assert backups, "corrupt JSON should be backed up"

        settings.set("act_plugin_enabled", {})
        settings.set("act_plugin_settings", {})
        settings.save()
        with open(path, "r", encoding="utf-8") as handle:
            repaired = json.load(handle)
        assert repaired == {"act_plugin_enabled": {}, "act_plugin_settings": {}}, repaired

    return {"ok": True, "settings_corrupt_backup": True, "settings_save_json": True}


def main() -> int:
    print(json.dumps(run_selftest(), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
