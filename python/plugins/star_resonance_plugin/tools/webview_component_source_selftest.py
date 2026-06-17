# -*- coding: utf-8 -*-
"""Regression checks for WebView component source settings propagation."""

from __future__ import annotations

import json
import sys
from pathlib import Path


import _bootstrap  # noqa: F401


ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from config import SettingsManager  # noqa: E402
from act_platform.runtime import register_webview_extension  # noqa: E402
from sao_webview import SAOWebAPI  # noqa: E402


class _ComponentSourceExtension:
    def __init__(self, owner: "_Owner") -> None:
        self.owner = owner

    def set_component_source(self, component, mode):
        component_name = str(component or "").strip().lower()
        if component_name not in {"hp", "level", "stamina", "skills"}:
            self.owner._sync_menu_settings()
            return json.dumps({"ok": False, "error": "bad_component", "component": component_name}, ensure_ascii=False)
        mode_name = str(mode or "packet").strip().lower()
        if mode_name == "screen":
            mode_name = "vision"
        if mode_name not in {"packet", "memory", "vision"}:
            mode_name = "packet"
        if component_name == "stamina":
            mode_name = "vision"
        elif component_name == "skills":
            mode_name = "packet"
        ref = self.owner._cfg_settings_ref
        if hasattr(ref, "set_component_source") and hasattr(ref, "get_data_source_map"):
            ref.set_component_source(component_name, mode_name)
            source_map = ref.get_data_source_map()
        else:
            source_map = dict(ref.get("data_source_map", {}) or {})
            source_map[component_name] = mode_name
            source_map["stamina"] = "vision"
            source_map["skills"] = "packet"
            ref.set("data_source_map", source_map)
            ref.set("data_source", "mixed")
        ref.save()
        self.owner._reconfigure_data_engines(restart_packet=False)
        self.owner._sync_menu_settings()
        return json.dumps({"ok": True, "component": component_name, "mode": mode_name,
                           "data_source_map": dict(source_map)}, ensure_ascii=False)

    def set_dps_fade_timeout(self, seconds):
        try:
            val = max(0, min(120, int(float(seconds))))
        except Exception:
            val = 0
        self.owner._set_setting("dps_fade_timeout_s", val)
        self.owner._sync_menu_settings()
        return json.dumps({"ok": True, "timeout": val}, ensure_ascii=False)


def _source_map(settings: SettingsManager):
    if hasattr(settings, "get_data_source_map"):
        return settings.get_data_source_map()
    return dict(settings.get("data_source_map", {}) or {})


class _Owner:
    def __init__(self, settings: SettingsManager) -> None:
        self._cfg_settings_ref = settings
        self.settings = settings
        self.sync_count = 0
        self.reconfigure_calls = []

    def _sync_menu_settings(self) -> None:
        self.sync_count += 1

    def _reconfigure_data_engines(self, restart_packet: bool = True) -> None:
        self.reconfigure_calls.append(restart_packet)

    def _set_setting(self, key: str, value) -> None:
        self._cfg_settings_ref.set(key, value)
        self._cfg_settings_ref.save()

    def _get_setting(self, key: str, default=None):
        return self._cfg_settings_ref.get(key, default)


def main() -> int:
    settings = SettingsManager(str(ROOT / "temp" / "webview_component_source_selftest.json"))
    settings.set("data_source_map", {"level": "network", "stamina": "packet", "skills": "vision"})
    settings.save()
    owner = _Owner(settings)
    api = SAOWebAPI(owner)
    register_webview_extension(owner, _ComponentSourceExtension(owner))

    result = json.loads(api.set_component_source("level", "screen"))
    if not result.get("ok"):
        raise AssertionError("valid component source update should succeed")
    if result.get("component") != "level" or result.get("mode") != "vision":
        raise AssertionError(f"component source update was not normalized: {result}")
    saved = _source_map(settings)
    if saved["level"] != "vision":
        raise AssertionError("component source was not persisted")
    if saved["stamina"] != "vision" or saved["skills"] != "packet":
        raise AssertionError("forced component source defaults were not preserved")
    if owner.reconfigure_calls != [False]:
        raise AssertionError(f"component update should refresh vision without packet restart: {owner.reconfigure_calls}")
    if owner.sync_count < 1:
        raise AssertionError("component update should sync menu settings")

    bad = json.loads(api.set_component_source("boss", "packet"))
    if bad.get("ok") is not False or bad.get("error") != "bad_component":
        raise AssertionError(f"invalid component should be rejected: {bad}")

    timeout = json.loads(api.set_dps_fade_timeout("999.7"))
    if timeout.get("timeout") != 120 or settings.get("dps_fade_timeout_s") != 120:
        raise AssertionError(f"DPS fade timeout should clamp high values: {timeout}")
    timeout = json.loads(api.set_dps_fade_timeout("not-a-number"))
    if timeout.get("timeout") != 0 or settings.get("dps_fade_timeout_s") != 0:
        raise AssertionError(f"DPS fade timeout should recover bad values: {timeout}")

    try:
        Path(settings._path).unlink(missing_ok=True)
    except Exception:
        pass
    print("OK webview component source propagation")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"FAIL webview component source selftest: {exc}", file=sys.stderr)
        raise SystemExit(1)
