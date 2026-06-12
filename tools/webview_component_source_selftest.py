# -*- coding: utf-8 -*-
"""Regression checks for WebView component source settings propagation."""

from __future__ import annotations

import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from config import SettingsManager  # noqa: E402
from sao_webview import SAOWebAPI  # noqa: E402


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


def main() -> int:
    settings = SettingsManager(str(ROOT / "temp" / "webview_component_source_selftest.json"))
    settings.set("data_source_map", {"level": "network", "stamina": "packet", "skills": "vision"})
    settings.save()
    owner = _Owner(settings)
    api = SAOWebAPI(owner)

    result = json.loads(api.set_component_source("level", "screen"))
    if not result.get("ok"):
        raise AssertionError("valid component source update should succeed")
    if result.get("component") != "level" or result.get("mode") != "vision":
        raise AssertionError(f"component source update was not normalized: {result}")
    saved = settings.get_data_source_map()
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
