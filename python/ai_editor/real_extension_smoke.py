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


def _platform_extension_dirs() -> List[str]:
    """The platform's OWN extension install dirs (not the global VS Code
    set). Mirrors AIEditorAPI._extension_scan_dirs' platform entries."""
    dirs: List[str] = []
    try:
        from ai_editor.scopes import _base_dir
        base = _base_dir()
    except Exception:
        base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for candidate in (
            os.path.join(base, "ai_editor_extensions"),
            os.path.join(os.path.expanduser("~"), ".sao", "extensions")):
        if os.path.isdir(candidate):
            dirs.append(candidate)
    return dirs


def attempt_js_live_activation(
        directory: str = "",
        per_extension_timeout: float = 20.0,
        extension_ids: Any = None) -> Dict[str, Any]:
    """OPT-IN offline live activation of JS extensions installed under the
    PLATFORM (ai_editor_extensions / ~/.sao/extensions) - NOT the global
    ~/.vscode/extensions set (those are arbitrary published extensions that
    expect the full VS Code API and mostly fail against this host's shim;
    the in-app ``AIEditorAPI.verify_installed_extension_activation`` is the
    primary, live-host home of this check).

    Pass ``directory`` to point at a specific extension folder; the default
    sweeps the platform's own install dirs. Stays OUT of the automated
    selftest suite: it executes real extension code, which is machine-
    dependent and may do real work (telemetry, file scans). Isolation
    applied:

    - the Node host gets an EMPTY temp workspace root, so extensions cannot
      see or touch the real workspace;
    - a temp storage root, so globalState/workspaceState writes land in a
      throwaway directory;
    - a per-extension activation timeout, so one hanging extension cannot
      stall the sweep;
    - errors are recorded per extension, never raised.

    Run via ``python -m ai_editor.real_extension_smoke --live-js`` (the user
    explicitly opting in to run their own installed extensions' code - the
    same code VS Code itself runs on their machine every day).
    """
    import shutil
    import tempfile
    import time

    from ai_editor.extension_host import CommandService, ExtensionScanner, NodeExtensionHost
    from ai_editor.node_runtime import get_node_path

    scan_dirs = [directory] if directory else _platform_extension_dirs()
    summary: Dict[str, Any] = {
        "directory": scan_dirs[0] if scan_dirs else "",
        "scanDirs": scan_dirs,
        "nodeAvailable": False,
        "attempted": 0,
        "activated": 0,
        "failed": 0,
        "results": [],
    }
    node_path = get_node_path()
    if not node_path:
        summary["error"] = "Node.js not found - cannot run JS extensions"
        return summary
    summary["nodeAvailable"] = True
    if not scan_dirs:
        summary["error"] = "No platform extension directories exist yet"
        return summary

    descriptions = [
        ext
        for scan_dir in scan_dirs
        for ext in ExtensionScanner.scan_directory(scan_dir)
        if ext.main]
    wanted = {str(x) for x in (extension_ids or []) if str(x)}
    if wanted:
        descriptions = [ext for ext in descriptions if ext.id in wanted]
    if not descriptions:
        summary["error"] = "No JS (main-entry) extensions found to activate"
        return summary

    script_path = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "node_ext_host.js")
    if not os.path.isfile(script_path):
        summary["error"] = f"node_ext_host.js not found at {script_path!r}"
        return summary

    storage_tmp = tempfile.mkdtemp(prefix="sao_js_live_storage_")
    workspace_tmp = tempfile.mkdtemp(prefix="sao_js_live_workspace_")
    errors_by_ext: Dict[str, str] = {}
    host = NodeExtensionHost(
        node_path=node_path,
        script_path=script_path,
        ui_bridge=None,
        storage_root=storage_tmp,
        workspace_root=workspace_tmp,
    )
    host.on_error(lambda ext_id, message: errors_by_ext.setdefault(
        str(ext_id), str(message)))
    # Real extensions commonly call vscode.commands.executeCommand/
    # registerCommand synchronously during activation (e.g. to check for
    # another extension's command, or self-register commands) - without a
    # command service the Node side reports "Command service is not
    # available" and activation fails, even though the gap is this harness
    # never wiring one up (the real in-platform host always has one).
    host.set_command_service(CommandService())
    try:
        if not host.start():
            summary["error"] = "Node extension host failed to start"
            return summary
        host.register_extensions(descriptions)
        for ext in descriptions:
            entry: Dict[str, Any] = {
                "id": ext.id,
                "version": ext.version,
                "activationEvents": ext.activation_events[:8],
            }
            summary["attempted"] += 1
            started = time.monotonic()
            sent = host.activate(
                ext.extension_path, ext.id, host.extension_manifest(ext))
            if not sent:
                entry["activated"] = False
                entry["error"] = "activate message could not be sent"
            else:
                activated = host.wait_for_activation(
                    [ext.id], timeout=per_extension_timeout)
                entry["activated"] = bool(activated)
                if not activated:
                    entry["error"] = errors_by_ext.get(
                        ext.id, f"not activated within {per_extension_timeout}s")
            entry["durationMs"] = int((time.monotonic() - started) * 1000)
            if entry["activated"]:
                summary["activated"] += 1
            else:
                summary["failed"] += 1
            summary["results"].append(entry)
        for ext in descriptions:
            try:
                host.deactivate(ext.id)
            except Exception:
                pass
    finally:
        try:
            host.stop()
        except Exception:
            pass
        for tmp in (storage_tmp, workspace_tmp):
            shutil.rmtree(tmp, ignore_errors=True)
    return summary


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


def main(argv: Any = None) -> int:
    import sys
    args = list(sys.argv[1:] if argv is None else argv)
    live_js = "--live-js" in args
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
    if live_js:
        print("Live JS activation (--live-js: activating PLATFORM-installed "
              "extensions' real code in an isolated temp workspace; the global "
              "~/.vscode set is intentionally NOT touched)...")
        js_summary = attempt_js_live_activation()
        summary["jsLiveActivation"] = js_summary
        if js_summary.get("error"):
            print(f"  {js_summary['error']}")
        for entry in js_summary.get("results", []):
            state = "activated" if entry.get("activated") else (
                "FAILED: " + str(entry.get("error", "")))
            print(f"  - {entry['id']} v{entry['version']}: {state} "
                  f"({entry['durationMs']}ms)")
        print(f"  JS live activation: {js_summary.get('activated', 0)}/"
              f"{js_summary.get('attempted', 0)} activated")
        try:
            RESULTS_PATH.write_text(
                json.dumps(summary, indent=2, ensure_ascii=False),
                encoding="utf-8")
        except OSError:
            pass
    print(f"Results written to {RESULTS_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
