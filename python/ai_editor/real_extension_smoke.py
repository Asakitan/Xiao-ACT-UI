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


def run_real_extension_smoke(directory: str = "", write_results: bool = True) -> Dict[str, Any]:
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
    if write_results:
        try:
            RESULTS_PATH.write_text(
                json.dumps(summary, indent=2, ensure_ascii=False),
                encoding="utf-8")
        except OSError:
            pass
    return summary


def main() -> int:
    summary = run_real_extension_smoke()
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
    print(f"Results written to {RESULTS_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
