"""Headless probe for real VS Code extensions in the AI Editor runtime.

Usage:
    python -m ai_editor.real_extension_probe
    python -m ai_editor.real_extension_probe --extension-dir C:\path\to\ext
"""

from __future__ import annotations

import argparse
import gzip
import json
import os
import shutil
import sys
import tempfile
import time
from typing import Any, Dict, List, Optional

_HERE = os.path.dirname(__file__)
_ROOT = os.path.dirname(_HERE)
if _ROOT not in sys.path:
    sys.path.insert(0, _ROOT)

from ai_editor.app import AIEditorAPI


class _ProbeSettings:
    def __init__(self, data: Dict[str, Any]) -> None:
        self.data = dict(data)

    def get(self, key: str, default: Any = None) -> Any:
        return self.data.get(key, default)

    def set(self, key: str, value: Any) -> None:
        self.data[key] = value

    def save(self) -> None:
        return None


class _ProbeGui:
    settings = None
    _ai_engine_actions = {"sample_state": lambda **kw: {"ok": True}}
    _plugin_manager = None
    _packet_bridge = None
    _mem_bridge = None
    _start_time = 0.0

    def __init__(self, data: Dict[str, Any]) -> None:
        self.settings = _ProbeSettings(data)


def _default_extension_dir() -> str:
    candidates = [
        os.path.join(
            os.path.expanduser("~"),
            ".vscode-insiders", "extensions", "misodee.vscode-nbt-0.9.5"),
        os.path.join(
            os.path.expanduser("~"),
            ".vscode", "extensions", "misodee.vscode-nbt-0.9.5"),
    ]
    for candidate in candidates:
        if os.path.isdir(candidate):
            return candidate
    return candidates[0]


def _candidate_nbt_payloads() -> List[tuple[str, bytes]]:
    raw_empty = b"\x0a\x00\x00\x00"
    raw_named = b"\x0a\x00\x04root\x00"
    return [
        ("raw-empty", raw_empty),
        ("raw-named-root", raw_named),
        ("gzip-empty", gzip.compress(raw_empty)),
        ("gzip-named-root", gzip.compress(raw_named)),
    ]


def _wait_for_render(
        rendered: Dict[str, Dict[str, Any]], view_id: str,
        timeout: float = 10.0) -> Optional[Dict[str, Any]]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        payload = rendered.get(view_id)
        if payload and payload.get("html"):
            return payload
        time.sleep(0.05)
    return rendered.get(view_id)


def _wait_for_webview_message(
        events: List[Dict[str, Any]], view_id: str,
        message_type: str, timeout: float = 3.0) -> Optional[Dict[str, Any]]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        for item in events:
            if item.get("event") != "webview_message":
                continue
            payload = item.get("data", {})
            if not isinstance(payload, dict):
                continue
            message = payload.get("message", {})
            if (payload.get("view_id") == view_id
                    and isinstance(message, dict)
                    and message.get("type") == message_type):
                return dict(payload)
        time.sleep(0.05)
    return None


def run_probe(extension_dir: str, keep_copy: bool = False) -> Dict[str, Any]:
    if not os.path.isdir(extension_dir):
        return {"ok": False, "error": f"Extension directory not found: {extension_dir}"}

    rendered: Dict[str, Dict[str, Any]] = {}
    events: List[Dict[str, Any]] = []
    copied_extension_dir = ""
    tmpdir = tempfile.mkdtemp(prefix="sao_real_ext_probe_")
    try:
        workspace_dir = os.path.join(tmpdir, "workspace")
        os.makedirs(workspace_dir, exist_ok=True)
        copied_extension_dir = os.path.join(tmpdir, os.path.basename(extension_dir))
        shutil.copytree(extension_dir, copied_extension_dir)

        api = AIEditorAPI(_ProbeGui({
            "ai_editor": {"extensions": {"diagnostics_enabled": True}}
        }))
        api._workspace_root = lambda: workspace_dir

        def _capture(event: str, data: Any) -> None:
            payload = data if isinstance(data, dict) else {"value": data}
            events.append({"event": event, "data": payload})
            if event == "render_webview_panel":
                view_id = str(payload.get("view_id", ""))
                if view_id:
                    rendered[view_id] = dict(payload)

        api._emit = _capture  # type: ignore[method-assign]

        previous_cwd = os.getcwd()
        try:
            os.chdir(workspace_dir)
            install_result = api.install_extension_dir(copied_extension_dir)
            if not install_result.get("ok"):
                return {
                    "ok": False,
                    "error": install_result.get("error", "Extension install failed"),
                    "install": install_result,
                }

            diagnostics_enabled = api.set_extension_host_diagnostics(True)

            attempts = []
            success: Optional[Dict[str, Any]] = None
            selected_fixture = ""
            for name, payload in _candidate_nbt_payloads():
                rel_path = f"{name}.nbt"
                full_path = os.path.join(workspace_dir, rel_path)
                with open(full_path, "wb") as fh:
                    fh.write(payload)
                opened = api.open_workspace_file(rel_path)
                attempts.append({
                    "fixture": name,
                    "result": opened,
                })
                if opened.get("runtime_mode") == "extension-custom-editor":
                    success = opened
                    selected_fixture = name
                    break

            diagnostics = api.get_extension_host_diagnostics()
            installed = api.list_installed_extensions()

            if not success:
                return {
                    "ok": False,
                    "error": "No fixture opened through the real custom editor",
                    "install": install_result,
                    "diagnostics_enabled": diagnostics_enabled,
                    "diagnostics": diagnostics,
                    "installed": installed,
                    "attempts": attempts,
                }

            view_id = str(success.get("view_id", ""))
            render_payload = _wait_for_render(rendered, view_id)
            html = ""
            if isinstance(render_payload, dict):
                html = str(render_payload.get("html", ""))

            state = api.custom_editor_state(view_id=view_id)
            ready_result = api.webview_post_message(view_id, {"type": "ready"})
            init_message = _wait_for_webview_message(events, view_id, "init")
            backup_result = api.backup_extension_custom_editor(
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
            )
            save_result = api.save_extension_custom_editor(
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
            )
            revert_result = api.revert_extension_custom_editor(
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
            )

            result = {
                "ok": True,
                "extensionDir": extension_dir,
                "copiedExtensionDir": copied_extension_dir,
                "workspaceDir": workspace_dir,
                "selectedFixture": selected_fixture,
                "install": install_result,
                "opened": success,
                "customEditorState": state,
                "installed": installed,
                "diagnostics_enabled": diagnostics_enabled,
                "diagnostics": diagnostics,
                "readyResult": ready_result,
                "initMessage": init_message,
                "lifecycle": {
                    "backup": backup_result,
                    "save": save_result,
                    "revert": revert_result,
                },
                "attempts": attempts,
                "render": {
                    "viewId": view_id,
                    "seen": bool(render_payload),
                    "htmlLength": len(html),
                    "hasDataUri": "data:" in html,
                    "hasEndpoint": "sao-webview-resource-endpoint" in html,
                    "hasResourceMap": "sao-webview-resource-map" in html,
                    "hasLocalUrl": "https://webview.local/" in html,
                    "hasVsCodeResourceHost": ".vscode-resource.webview.local" in html,
                },
                "capturedEvents": len(events),
            }
            if keep_copy:
                result["keptExtensionCopy"] = copied_extension_dir
            return result
        finally:
            try:
                api._shutdown_node_extension_host()
            except Exception:
                pass
            try:
                api._stop_webview_resource_server()
            except Exception:
                pass
            os.chdir(previous_cwd)
    finally:
        if not keep_copy:
            shutil.rmtree(tmpdir, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--extension-dir",
        default=_default_extension_dir(),
        help="Directory of an already extracted VS Code extension",
    )
    parser.add_argument(
        "--keep-copy",
        action="store_true",
        help="Keep the copied extension directory path in the JSON output",
    )
    args = parser.parse_args()

    result = run_probe(args.extension_dir, keep_copy=args.keep_copy)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
