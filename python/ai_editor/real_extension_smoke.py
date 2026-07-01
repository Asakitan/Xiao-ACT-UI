"""阶段1.4 real VSIX smoke farm.

AI_IDE_VSCODE_PARITY_LONG_TERM_PLAN.md asks for a recorded, reproducible
compatibility sweep against real installed extensions (not just synthetic
fixtures). This scans whatever VS Code extensions happen to be installed on
the current machine under ``~/.vscode/extensions`` - each one is a real,
published VSIX that real users depend on - and records, per extension, which
manifest-level contribution points this project's ``ExtensionScanner`` /
``ExtensionDescription`` can see.

Scope: this is a **manifest-level** compatibility check (can we parse the
package.json and see its contribution points), not a live-activation check.
Actually activating arbitrary third-party extension code (some proprietary,
e.g. ms-dotnettools) from an automated test run is out of scope and would be
unsafe/slow/non-reproducible. Live activation + rendering verification is a
separate, larger follow-up (see the long-term plan's 阶段1.4 "是否渲染/是否
有 console error/是否可点击" criteria).

Tolerant of running on a machine with no VS Code installed at all (CI, a
fresh dev machine) - reports zero extensions rather than failing.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List

from ai_editor.extension_host import ExtensionDescription, ExtensionScanner

RESULTS_PATH = Path(__file__).resolve().parent / "real_extension_smoke_results.json"

# Contribution keys this smoke test explicitly counts, matching the
# contribution-point categories AI_IDE_VSCODE_PARITY_LONG_TERM_PLAN.md's
# 阶段1.4 asks a real VSIX sweep to cover.
_COUNTED_CONTRIBUTIONS = (
    "commands", "languages", "grammars", "configuration", "views",
    "viewsContainers", "menus", "keybindings", "themes", "iconThemes",
    "snippets", "debuggers", "taskDefinitions", "customEditors",
    "notebooks", "chatParticipants", "languageModelTools",
    "authentication", "walkthroughs", "problemMatchers", "jsonValidation",
)


def _real_extensions_dir() -> str:
    return os.path.join(os.path.expanduser("~"), ".vscode", "extensions")


def _contribution_count(contributes: Dict[str, Any], key: str) -> int:
    value = contributes.get(key)
    if isinstance(value, list):
        return len(value)
    if isinstance(value, dict):
        return 1
    return 0


def _describe_extension(ext: ExtensionDescription) -> Dict[str, Any]:
    contributes = ext.contributes or {}
    counts = {
        key: _contribution_count(contributes, key)
        for key in _COUNTED_CONTRIBUTIONS
    }
    return {
        "id": ext.id,
        "publisher": ext.publisher,
        "name": ext.name,
        "displayName": ext.display_name,
        "version": ext.version,
        "extensionPath": ext.extension_path,
        "hasMain": bool(ext.main),
        "hasBrowser": bool(ext.browser),
        "activationEventCount": len(ext.activation_events),
        "activationEvents": ext.activation_events[:8],
        "contributesKeys": sorted(contributes.keys()),
        "contributionCounts": counts,
        "totalContributedItems": sum(counts.values()),
    }


def scan_real_extensions(directory: str = "") -> List[Dict[str, Any]]:
    directory = directory or _real_extensions_dir()
    descriptions = ExtensionScanner.scan_directory(directory)
    return [_describe_extension(ext) for ext in descriptions]


class _LiveActivationGui:
    """Minimal AIEditorAPI host stub, matching the _SettingsGui/_FakeGui
    pattern selftest.py uses elsewhere - no real settings/plugin backend
    needed just to install a manifest and read back a theme file."""
    settings = None
    _ai_engine_actions: Dict[str, Any] = {}
    _plugin_manager = None
    _packet_bridge = None
    _mem_bridge = None
    _start_time = 0.0


def theme_only_candidates(reports: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Extensions safe to actually install and read for real: no JS entry
    point at all (no main, no browser) and their only contribution is
    color/icon themes - pure declarative JSON, nothing to execute."""
    return [
        r for r in reports
        if not r["hasMain"] and not r["hasBrowser"]
        and r["contributesKeys"]
        and set(r["contributesKeys"]) <= {"themes", "iconThemes"}
        and r["contributionCounts"].get("themes", 0) > 0
    ]


def attempt_theme_live_activation(reports: List[Dict[str, Any]]) -> Dict[str, Any]:
    """阶段1.4 live-activation depth, scoped to what's safe to run
    unattended: real theme-only extensions (see theme_only_candidates) are
    actually installed via the real install_extension_dir path and their
    real theme JSON is read back through get_editor_theme() - the same
    method the Settings UI uses to apply a color theme. No code from any
    extension actually executes; this is manifest parsing + JSON asset
    reads, which is why it's safe to do beyond the manifest-only sweep in
    run_real_extension_smoke().
    """
    from ai_editor.app import AIEditorAPI

    candidates = theme_only_candidates(reports)
    results: List[Dict[str, Any]] = []
    if candidates:
        api = AIEditorAPI(_LiveActivationGui())
        for report in candidates:
            entry: Dict[str, Any] = {"id": report["id"], "extensionPath": report["extensionPath"]}
            install = api.install_extension_dir(report["extensionPath"])
            entry["installed"] = bool(install.get("ok"))
            entry["installError"] = install.get("error", "")
            if entry["installed"]:
                themes = api.list_editor_themes()
                theme_rows = [
                    t for t in themes.get("themes", [])
                    if t.get("extension_id") == report["id"]
                ]
                entry["themesDiscovered"] = len(theme_rows)
                readable = 0
                for row in theme_rows:
                    theme_id = row.get("id", "")
                    detail = api.get_editor_theme(theme_id) if theme_id else {}
                    if detail.get("ok") and isinstance(detail.get("colors"), dict) and detail["colors"]:
                        readable += 1
                entry["themesReadable"] = readable
            else:
                entry["themesDiscovered"] = 0
                entry["themesReadable"] = 0
            results.append(entry)
    return {
        "candidateCount": len(candidates),
        "results": results,
        "allInstalled": all(r["installed"] for r in results) if results else True,
        "allThemesReadable": all(
            r["themesDiscovered"] > 0 and r["themesReadable"] == r["themesDiscovered"]
            for r in results) if results else True,
    }


def run_real_extension_smoke(
        directory: str = "", write_results: bool = True,
        include_live_activation: bool = False) -> Dict[str, Any]:
    directory = directory or _real_extensions_dir()
    available = os.path.isdir(directory)
    reports = scan_real_extensions(directory) if available else []
    summary = {
        "directory": directory,
        "directoryAvailable": available,
        "extensionCount": len(reports),
        "extensionsWithContributions": sum(
            1 for r in reports if r["totalContributedItems"] > 0),
        "extensionsWithMain": sum(1 for r in reports if r["hasMain"]),
        "extensions": reports,
    }
    if include_live_activation:
        summary["liveActivation"] = attempt_theme_live_activation(reports)
    if write_results:
        try:
            RESULTS_PATH.write_text(
                json.dumps(summary, indent=2, ensure_ascii=False),
                encoding="utf-8")
        except OSError:
            pass
    return summary


def main() -> int:
    summary = run_real_extension_smoke(include_live_activation=True)
    if not summary["directoryAvailable"]:
        print(f"No VS Code extensions directory found at {summary['directory']!r} "
              "(nothing to smoke-test on this machine).")
        return 0
    print(f"Real VSIX smoke: {summary['extensionCount']} extension(s) found under "
          f"{summary['directory']}")
    for report in summary["extensions"]:
        print(f"  - {report['id']} v{report['version']} "
              f"({report['totalContributedItems']} contributed items, "
              f"{report['activationEventCount']} activation events, "
              f"main={'js' if report['hasMain'] else 'none'})")
    live = summary.get("liveActivation") or {}
    if live.get("candidateCount"):
        print(f"Live activation (theme-only, no JS to execute): "
              f"{live['candidateCount']} candidate(s)")
        for entry in live["results"]:
            print(f"  - {entry['id']}: installed={entry['installed']} "
                  f"themes discovered={entry['themesDiscovered']} "
                  f"readable={entry['themesReadable']}")
    print(f"Results written to {RESULTS_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
