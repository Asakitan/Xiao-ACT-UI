"""Headless probe for real VS Code extensions in the AI Editor runtime.

Usage:
    python -m ai_editor.real_extension_probe
    python -m ai_editor.real_extension_probe --extension-dir C:\path\to\ext
"""

from __future__ import annotations

import argparse
import gzip
import html as html_lib
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


def _safe_json(data: Any) -> Any:
    try:
        return json.loads(json.dumps(data, ensure_ascii=False, default=str))
    except Exception:
        return str(data)


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
        ("gzip-empty", gzip.compress(raw_empty)),
        ("gzip-named-root", gzip.compress(raw_named)),
        ("raw-empty", raw_empty),
        ("raw-named-root", raw_named),
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


def _wait_for_render_matching(
        rendered: Dict[str, Dict[str, Any]], marker: str,
        timeout: float = 10.0) -> Optional[Dict[str, Any]]:
    deadline = time.time() + timeout
    while time.time() < deadline:
        for payload in rendered.values():
            if marker in str(payload.get("html", "")):
                return payload
        time.sleep(0.05)
    for payload in rendered.values():
        if marker in str(payload.get("html", "")):
            return payload
    return None


def _matching_render_payloads(
        rendered: Dict[str, Dict[str, Any]], marker: str) -> List[Dict[str, Any]]:
    return [
        dict(payload)
        for payload in rendered.values()
        if marker in str(payload.get("html", ""))
    ]


def _latest_render_matching(
        rendered: Dict[str, Dict[str, Any]], marker: str) -> Optional[Dict[str, Any]]:
    matches = _matching_render_payloads(rendered, marker)
    return matches[-1] if matches else None


def _find_event_payload(
        events: List[Dict[str, Any]], event: str, view_id: str) -> Dict[str, Any]:
    normalized = str(view_id or "")
    for item in reversed(events):
        if item.get("event") != event:
            continue
        data = item.get("data", {})
        if isinstance(data, dict) and str(data.get("view_id", "")) == normalized:
            return dict(data)
    return {}


def _write_json(path: str, data: Dict[str, Any]) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=2)
        fh.write("\n")


def _write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def _write_builtin_smoke_extension(parent_dir: str) -> str:
    """Create a real VS Code-style extension with dynamic UI surfaces."""
    ext_dir = os.path.join(parent_dir, "sao-ai-editor-smoke")
    media_dir = os.path.join(ext_dir, "media")
    os.makedirs(media_dir, exist_ok=True)
    _write_json(os.path.join(ext_dir, "package.json"), {
        "name": "sao-ai-editor-smoke",
        "displayName": "SAO AI Editor Smoke",
        "publisher": "sao",
        "version": "0.0.1",
        "engines": {"vscode": "^1.90.0"},
        "main": "./extension.js",
        "activationEvents": [
            "onCommand:saoProbe.activate",
            "onView:saoProbe.dynamicView",
            "onCustomEditor:saoProbe.customEditor",
        ],
        "contributes": {
            "commands": [{
                "command": "saoProbe.activate",
                "title": "SAO Probe Activate",
            }, {
                "command": "saoProbe.state",
                "title": "SAO Probe State",
            }],
            "viewsContainers": {
                "activitybar": [{
                    "id": "saoProbe",
                    "title": "SAO Probe",
                    "icon": "media/probe.svg",
                }],
            },
            "views": {
                "saoProbe": [{
                    "id": "saoProbe.dynamicView",
                    "name": "Dynamic Probe",
                    "type": "webview",
                }],
            },
            "customEditors": [{
                "viewType": "saoProbe.customEditor",
                "displayName": "SAO Probe Custom Editor",
                "selector": [{"filenamePattern": "*.sao-probe"}],
                "priority": "default",
            }],
        },
    })
    _write_text(os.path.join(media_dir, "probe.svg"), (
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"64\" height=\"64\" "
        "viewBox=\"0 0 64 64\"><rect width=\"64\" height=\"64\" "
        "fill=\"#007acc\"/><circle cx=\"32\" cy=\"32\" r=\"20\" "
        "fill=\"#ffffff\"/></svg>\n"
    ))
    _write_text(os.path.join(ext_dir, "extension.js"), r"""
const vscode = require('vscode');

const state = {
  activated: false,
  webview: { resolved: false, messages: [], contextState: null },
  custom: { resolved: false, messages: [], text: '' },
};

function htmlFor(webview, context, kind, text) {
  const asset = webview.asWebviewUri(
    vscode.Uri.joinPath(context.extensionUri, 'media', 'probe.svg'));
  const commandHref = 'command:saoProbe.activate?'
    + encodeURIComponent(JSON.stringify([{ source: kind }]));
  return `<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${webview.cspSource}; script-src 'unsafe-inline'; style-src 'unsafe-inline'; form-action ${webview.cspSource};">
  <title>SAO ${kind}</title>
</head>
<body data-probe="${kind}">
  <main data-kind="${kind}">
    <img data-probe-asset src="${asset}" alt="probe">
    <form data-probe-form><input name="message" value="${text || kind}"></form>
    <a data-probe-command href="${commandHref}">command</a>
    <script>
      const api = acquireVsCodeApi();
      api.setState({ kind: '${kind}', ready: true });
      api.postMessage({ type: 'dom-ready', kind: '${kind}' });
      window.__saoProbe = { kind: '${kind}', ready: true };
    </script>
  </main>
</body>
</html>`;
}

function activate(context) {
  state.activated = true;

  context.subscriptions.push(vscode.commands.registerCommand(
    'saoProbe.activate',
    (payload) => ({ ok: true, payload: payload || null, state }),
  ));
  context.subscriptions.push(vscode.commands.registerCommand(
    'saoProbe.state',
    () => state,
  ));

  context.subscriptions.push(vscode.window.registerWebviewViewProvider(
    'saoProbe.dynamicView',
    {
      resolveWebviewView(view, webviewContext) {
        state.webview.resolved = true;
        state.webview.contextState = webviewContext && webviewContext.state || null;
        view.title = 'SAO Probe Dynamic';
        view.description = 'Runtime registered webview';
        view.badge = { value: 7, tooltip: 'Probe badge' };
        view.webview.options = {
          enableScripts: true,
          enableForms: true,
          enableCommandUris: ['saoProbe.activate'],
          portMapping: [{ webviewPort: 5173, extensionHostPort: 15173 }],
          retainContextWhenHidden: true,
          localResourceRoots: [vscode.Uri.joinPath(context.extensionUri, 'media')],
        };
        view.webview.onDidReceiveMessage((message) => {
          state.webview.messages.push(message);
        });
        view.webview.html = htmlFor(view.webview, context, 'webview-view', 'dynamic');
      },
    },
    { webviewOptions: { enableScripts: true, retainContextWhenHidden: true } },
  ));

  context.subscriptions.push(vscode.window.registerCustomEditorProvider(
    'saoProbe.customEditor',
    {
      async resolveCustomTextEditor(document, panel) {
        state.custom.resolved = true;
        state.custom.text = document.getText();
        panel.webview.options = {
          enableScripts: true,
          enableForms: true,
          enableCommandUris: true,
          portMapping: [{ webviewPort: 4173, extensionHostPort: 14173 }],
          retainContextWhenHidden: true,
          localResourceRoots: [vscode.Uri.joinPath(context.extensionUri, 'media')],
        };
        panel.webview.onDidReceiveMessage((message) => {
          state.custom.messages.push(message);
        });
        panel.webview.html = htmlFor(
          panel.webview, context, 'custom-editor', document.getText());
      },
    },
    { supportsMultipleEditorsPerDocument: true },
  ));
}

function deactivate() {}

module.exports = { activate, deactivate };
""".lstrip())
    return ext_dir


def _surface_visual_checks(
        prepared_html: str, kind: str, options: Dict[str, Any],
        metadata: Optional[Dict[str, Any]] = None) -> Dict[str, bool]:
    text = str(prepared_html or "")
    opts = options if isinstance(options, dict) else {}
    meta = metadata if isinstance(metadata, dict) else {}
    is_custom = kind == "custom-editor"
    return {
        "hasProbeBody": f'data-probe="{kind}"' in text,
        "hasMainKind": f'data-kind="{kind}"' in text,
        "hasIframeSafeScriptBridge": (
            "acquireVsCodeApi" in text
            and "postMessage" in text
            and "setState" in text),
        "hasFormWhenEnabled": (
            opts.get("enableForms") is True
            and "data-probe-form" in text
            and "<input" in text),
        "hasCommandUriWhenAllowed": (
            "command:saoProbe.activate" in text
            and (
                opts.get("enableCommandUris") is True
                or opts.get("enableCommandUris") == ["saoProbe.activate"])),
        "hasRewrittenLocalResource": (
            "data-probe-asset" in text
            and ".vscode-resource.webview.local" in text),
        "hasResourceEndpointMetadata": (
            "sao-webview-resource-endpoint" in text
            and "sao-webview-resource-map" in text),
        "hasCspSource": "https://*.vscode-resource.webview.local" in text,
        "hasExpectedTitleOrState": (
            "custom editor body" in text if is_custom
            else meta.get("title") == "SAO Probe Dynamic"),
    }


def _write_frontend_visual_fixture(
        fixture_dir: str,
        webview_html: str,
        custom_html: str,
        webview_id: str,
        custom_id: str,
        webview_options: Dict[str, Any],
        custom_options: Dict[str, Any],
        webview_metadata: Dict[str, Any]) -> Dict[str, Any]:
    os.makedirs(fixture_dir, exist_ok=True)
    fixture_path = os.path.join(fixture_dir, "visual-webview-smoke.html")
    data_path = os.path.join(fixture_dir, "visual-webview-smoke.json")
    payload = {
        "webviewId": webview_id,
        "customEditorId": custom_id,
        "webviewOptions": _safe_json(webview_options),
        "customEditorOptions": _safe_json(custom_options),
        "webviewMetadata": _safe_json(webview_metadata),
        "checks": {
            "webview": _surface_visual_checks(
                webview_html, "webview-view", webview_options,
                webview_metadata),
            "customEditor": _surface_visual_checks(
                custom_html, "custom-editor", custom_options, {}),
        },
    }
    webview_srcdoc = html_lib.escape(str(webview_html or ""), quote=True)
    custom_srcdoc = html_lib.escape(str(custom_html or ""), quote=True)
    fixture = f"""<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>AI Editor Webview Visual Smoke</title>
  <style>
    body {{ margin:0; background:#1e1e1e; color:#cccccc; font:12px Segoe UI, sans-serif; }}
    header {{ padding:10px 14px; border-bottom:1px solid #3c3c3c; background:#252526; }}
    main {{ display:grid; grid-template-columns:1fr 1fr; gap:10px; padding:10px; }}
    section {{ min-width:0; border:1px solid #3c3c3c; background:#181818; }}
    h2 {{ margin:0; padding:7px 9px; font-size:12px; border-bottom:1px solid #3c3c3c; }}
    iframe {{ width:100%; height:360px; border:0; background:white; }}
    .meta {{ padding:6px 9px; color:#8f8f8f; font-family:Consolas, monospace; white-space:pre-wrap; }}
  </style>
</head>
<body>
  <header>
    <strong>AI Editor Webview Visual Smoke</strong>
    <span> dynamic WebviewView + custom editor iframe surfaces</span>
  </header>
  <main>
    <section data-surface="webview-view" data-view-id="{html_lib.escape(webview_id)}">
      <h2>WebviewView: {html_lib.escape(webview_id)}</h2>
      <iframe title="Dynamic WebviewView" srcdoc="{webview_srcdoc}"></iframe>
      <div class="meta">{html_lib.escape(json.dumps(webview_options, ensure_ascii=False, indent=2, default=str))}</div>
    </section>
    <section data-surface="custom-editor" data-view-id="{html_lib.escape(custom_id)}">
      <h2>Custom Editor: {html_lib.escape(custom_id)}</h2>
      <iframe title="Custom Editor" srcdoc="{custom_srcdoc}"></iframe>
      <div class="meta">{html_lib.escape(json.dumps(custom_options, ensure_ascii=False, indent=2, default=str))}</div>
    </section>
  </main>
</body>
</html>
"""
    _write_text(fixture_path, fixture)
    _write_json(data_path, payload)
    payload["fixturePath"] = fixture_path
    payload["dataPath"] = data_path
    payload["fixtureBytes"] = os.path.getsize(fixture_path)
    payload["dataBytes"] = os.path.getsize(data_path)
    return payload


def _install_capture_bridge(
        api: AIEditorAPI,
        rendered: Dict[str, Dict[str, Any]],
        events: List[Dict[str, Any]]) -> None:
    """Capture UI bridge payloads without requiring a real desktop window."""
    original_emit = getattr(api, "_emit", None)

    def _capture(event: str, data: Any) -> None:
        payload = data if isinstance(data, dict) else {"value": data}
        events.append({"event": event, "data": payload})
        if event == "render_webview_panel":
            view_id = str(payload.get("view_id", ""))
            if view_id:
                rendered[view_id] = dict(payload)
        if callable(original_emit):
            try:
                original_emit(event, data)
            except Exception:
                pass

    api._emit = _capture  # type: ignore[method-assign]

    bridge = getattr(getattr(api, "_vscode_ns", None), "_ui_bridge", None)
    if bridge is None:
        return

    def render_webview_panel(
            view_id: str, html: str,
            local_resource_roots: Any = None,
            state: Any = None,
            title: str = "",
            options: Any = None) -> None:
        state_to_render = state
        if state_to_render is None:
            state_to_render = api._webview_states.get(str(view_id or ""))
        elif str(view_id or "").strip():
            api.webview_set_state(view_id, state_to_render)
        option_payload = options if isinstance(options, dict) else {}
        nested_options = option_payload.get("webviewOptions")
        if not isinstance(nested_options, dict):
            nested_options = {}
        prepared = api._prepare_extension_webview_html(
            html, local_resource_roots, view_id=view_id)
        _capture("render_webview_panel", {
            "view_id": view_id,
            "html": prepared,
            "state": state_to_render,
            "title": str(title or ""),
            "options": option_payload,
            "local_resource_roots": local_resource_roots,
            "localResourceRoots": local_resource_roots,
            "retainContextWhenHidden": bool(option_payload.get(
                "retainContextWhenHidden",
                nested_options.get("retainContextWhenHidden", False))),
        })

    def update_webview_panel_options(
            view_id: str, options: Any = None,
            local_resource_roots: Any = None,
            view_type: str = "", title: str = "") -> None:
        _capture("update_webview_panel_options", {
            "view_id": str(view_id or ""),
            "view_type": str(view_type or ""),
            "title": str(title or ""),
            "options": options if isinstance(options, dict) else {},
            "local_resource_roots": local_resource_roots,
        })

    def update_webview_view_metadata(
            view_id: str, view_type: str = "",
            metadata: Optional[Dict[str, Any]] = None) -> None:
        _capture("update_webview_view_metadata", {
            "view_id": str(view_id or ""),
            "view_type": str(view_type or ""),
            "metadata": dict(metadata or {}),
        })

    bridge.render_webview_panel = render_webview_panel
    bridge.update_webview_panel_options = update_webview_panel_options
    bridge.update_webview_view_metadata = update_webview_view_metadata
    host = getattr(api, "_node_ext_host", None)
    if host is not None:
        try:
            host._ui_bridge = bridge
        except Exception:
            pass


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


def _wait_for_custom_editor_state(
        api: AIEditorAPI, view_id: str, view_type: str, uri: str,
        predicate, timeout: float = 5.0) -> Dict[str, Any]:
    deadline = time.time() + timeout
    last: Dict[str, Any] = {}
    while time.time() < deadline:
        state = api.custom_editor_state(
            view_id=view_id, view_type=view_type, uri=uri)
        if isinstance(state, dict):
            last = state
            try:
                if predicate(state):
                    return state
            except Exception:
                pass
        time.sleep(0.05)
    return last


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
        _install_capture_bridge(api, rendered, events)

        previous_cwd = os.getcwd()
        try:
            os.chdir(workspace_dir)
            install_result = api.install_extension_dir(copied_extension_dir)
            _install_capture_bridge(api, rendered, events)
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
            init_body = (
                init_message.get("message", {}).get("body", {})
                if isinstance(init_message, dict) else {}
            )
            init_content = (
                init_body.get("content", {})
                if isinstance(init_body, dict) else {}
            )
            old_root = (
                dict(init_content.get("root", {}))
                if isinstance(init_content, dict)
                and isinstance(init_content.get("root", {}), dict)
                else {}
            )
            first_root = dict(old_root)
            first_root["__sao_probe"] = {"type": 8, "value": "hello"}
            edit_payload = {
                "type": "edit",
                "body": {
                    "type": "set",
                    "path": [],
                    "old": {"type": 10, "value": old_root},
                    "new": {"type": 10, "value": first_root},
                },
            }
            edit_result = api.webview_post_message(view_id, edit_payload)
            dirty_state = _wait_for_custom_editor_state(
                api,
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
                lambda item: bool(item.get("dirty")),
            )
            opened_path = str(success.get("absolute_path", ""))
            before_bytes = b""
            with open(opened_path, "rb") as fh:
                before_bytes = fh.read()
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
            clean_state = _wait_for_custom_editor_state(
                api,
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
                lambda item: not bool(item.get("dirty")),
            )
            with open(opened_path, "rb") as fh:
                after_save_bytes = fh.read()
            second_root = dict(first_root)
            second_root["__sao_probe"] = {"type": 8, "value": "world"}
            second_edit_result = api.webview_post_message(view_id, {
                "type": "edit",
                "body": {
                    "type": "set",
                    "path": [],
                    "old": {"type": 10, "value": first_root},
                    "new": {"type": 10, "value": second_root},
                },
            })
            dirty_state_after_second_edit = _wait_for_custom_editor_state(
                api,
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
                lambda item: bool(item.get("dirty")),
            )
            revert_result = api.revert_extension_custom_editor(
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
            )
            reverted_state = _wait_for_custom_editor_state(
                api,
                view_id,
                state.get("viewType", ""),
                state.get("uri", ""),
                lambda item: not bool(item.get("dirty")),
            )
            with open(opened_path, "rb") as fh:
                after_revert_bytes = fh.read()

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
                "edit": {
                    "first": edit_result,
                    "dirtyState": dirty_state,
                    "second": second_edit_result,
                    "dirtyStateAfterSecondEdit": dirty_state_after_second_edit,
                },
                "lifecycle": {
                    "backup": backup_result,
                    "save": save_result,
                    "cleanState": clean_state,
                    "revert": revert_result,
                    "revertedState": reverted_state,
                },
                "disk": {
                    "changedAfterSave": before_bytes != after_save_bytes,
                    "restoredAfterRevert": after_save_bytes == after_revert_bytes,
                    "beforeSize": len(before_bytes),
                    "afterSaveSize": len(after_save_bytes),
                    "afterRevertSize": len(after_revert_bytes),
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


def run_builtin_smoke_probe(
        keep_copy: bool = False,
        visual_fixture_dir: str = "") -> Dict[str, Any]:
    """Run a reproducible dynamic WebviewView/custom editor smoke probe."""
    rendered: Dict[str, Dict[str, Any]] = {}
    events: List[Dict[str, Any]] = []
    requested_fixture_dir = (
        os.path.abspath(visual_fixture_dir)
        if str(visual_fixture_dir or "").strip() else "")
    tmpdir = tempfile.mkdtemp(prefix="sao_builtin_ext_probe_")
    try:
        workspace_dir = os.path.join(tmpdir, "workspace")
        ext_parent = os.path.join(tmpdir, "extensions")
        os.makedirs(workspace_dir, exist_ok=True)
        os.makedirs(ext_parent, exist_ok=True)
        extension_dir = _write_builtin_smoke_extension(ext_parent)
        custom_path = os.path.join(workspace_dir, "sample.sao-probe")
        _write_text(custom_path, "custom editor body")

        api = AIEditorAPI(_ProbeGui({
            "ai_editor": {"extensions": {"diagnostics_enabled": True}}
        }))
        api._workspace_root = lambda: workspace_dir
        _install_capture_bridge(api, rendered, events)

        previous_cwd = os.getcwd()
        try:
            os.chdir(workspace_dir)
            install_result = api.install_extension_dir(extension_dir)
            _install_capture_bridge(api, rendered, events)
            if not install_result.get("ok"):
                return {
                    "ok": False,
                    "error": install_result.get("error", "Extension install failed"),
                    "install": install_result,
                }

            diagnostics_enabled = api.set_extension_host_diagnostics(True)
            activate_result = api.execute_command("saoProbe.activate", {
                "source": "builtin-smoke",
            })

            webview_render = _wait_for_render_matching(
                rendered, 'data-probe="webview-view"')
            time.sleep(0.1)
            webview_render = (
                _latest_render_matching(rendered, 'data-probe="webview-view"')
                or webview_render)
            webview_view_id = str(
                webview_render.get("view_id", "")
                if isinstance(webview_render, dict) else "")
            webview_message_result = (
                api.webview_post_message(webview_view_id, {
                    "type": "ping",
                    "target": "webview-view",
                })
                if webview_view_id else {"ok": False})

            custom_result = api.resolve_extension_custom_editor(
                "saoProbe.customEditor", custom_path, "SAO Probe Custom")
            custom_view_id = str(custom_result.get("viewId", ""))
            custom_render = (
                _wait_for_render(rendered, custom_view_id)
                if custom_view_id else None)
            custom_message_result = (
                api.webview_post_message(custom_view_id, {
                    "type": "ping",
                    "target": "custom-editor",
                })
                if custom_view_id else {"ok": False})

            state_deadline = time.time() + 5.0
            command_state: Dict[str, Any] = {}
            while time.time() < state_deadline:
                raw_state = api.execute_command("saoProbe.state")
                command_state = (
                    raw_state.get("result", raw_state)
                    if isinstance(raw_state, dict) else {})
                if (
                        isinstance(command_state, dict)
                        and command_state.get("webview", {}).get("messages")
                        and command_state.get("custom", {}).get("messages")):
                    break
                time.sleep(0.05)
            if not command_state.get("webview", {}).get("messages"):
                for payload in _matching_render_payloads(
                        rendered, 'data-probe="webview-view"'):
                    candidate_id = str(payload.get("view_id", ""))
                    if not candidate_id or candidate_id == webview_view_id:
                        continue
                    webview_message_result = api.webview_post_message(
                        candidate_id, {
                            "type": "ping",
                            "target": "webview-view",
                        })
                    raw_state = api.execute_command("saoProbe.state")
                    command_state = (
                        raw_state.get("result", raw_state)
                        if isinstance(raw_state, dict) else {})
                    if command_state.get("webview", {}).get("messages"):
                        webview_view_id = candidate_id
                        webview_render = payload
                        break

            runtime_surfaces = api.list_extension_runtime_surfaces()
            custom_state = (
                api.custom_editor_state(custom_view_id)
                if custom_view_id else {"ok": False})
            webview_options = _find_event_payload(
                events, "update_webview_panel_options", webview_view_id)
            webview_metadata = _find_event_payload(
                events, "update_webview_view_metadata", webview_view_id)
            custom_options = _find_event_payload(
                events, "update_webview_panel_options", custom_view_id)

            webview_html = str(
                webview_render.get("html", "")
                if isinstance(webview_render, dict) else "")
            custom_html = str(
                custom_render.get("html", "")
                if isinstance(custom_render, dict) else "")
            webview_option_payload = (
                webview_options.get("options", {})
                if isinstance(webview_options, dict) else {})
            webview_metadata_payload = (
                webview_metadata.get("metadata", {})
                if isinstance(webview_metadata, dict) else {})
            custom_option_payload = (
                custom_options.get("options", {})
                if isinstance(custom_options, dict) else {})
            fixture_dir = requested_fixture_dir or os.path.join(
                tmpdir, "visual-fixture")
            visual_fixture = _write_frontend_visual_fixture(
                fixture_dir,
                webview_html,
                custom_html,
                webview_view_id,
                custom_view_id,
                webview_option_payload,
                custom_option_payload,
                webview_metadata_payload)
            visual_webview_checks = visual_fixture.get(
                "checks", {}).get("webview", {})
            visual_custom_checks = visual_fixture.get(
                "checks", {}).get("customEditor", {})

            checks = {
                "extensionInstalled": install_result.get("ok") is True,
                "commandActivation": activate_result.get("ok") is True,
                "dynamicWebviewRendered": bool(webview_view_id)
                and 'data-probe="webview-view"' in webview_html,
                "dynamicWebviewOptions": (
                    webview_option_payload.get("enableScripts") is True
                    and webview_option_payload.get("enableForms") is True
                    and webview_option_payload.get("enableCommandUris")
                    == ["saoProbe.activate"]
                    and webview_option_payload.get("portMapping") == [{
                        "webviewPort": 5173,
                        "extensionHostPort": 15173,
                    }]
                    and webview_option_payload.get(
                        "retainContextWhenHidden") is True),
                "dynamicWebviewMetadata": (
                    webview_metadata_payload.get("title")
                    == "SAO Probe Dynamic"
                    and webview_metadata_payload.get("description")
                    == "Runtime registered webview"
                    and webview_metadata_payload.get("badge", {}).get("value")
                    == 7),
                "dynamicWebviewResourceRewrite": (
                    "sao-webview-resource-endpoint" in webview_html
                    and "sao-webview-resource-map" in webview_html
                    and ".vscode-resource.webview.local" in webview_html),
                "dynamicWebviewMessageRelay": (
                    webview_message_result.get("ok") is True
                    and any(
                        isinstance(item, dict)
                        and item.get("target") == "webview-view"
                        for item in command_state.get(
                            "webview", {}).get("messages", []))),
                "customEditorRendered": (
                    custom_result.get("ok") is True
                    and custom_view_id
                    and 'data-probe="custom-editor"' in custom_html),
                "customEditorOptions": (
                    custom_option_payload.get("enableScripts") is True
                    and custom_option_payload.get("enableForms") is True
                    and custom_option_payload.get("enableCommandUris") is True
                    and custom_option_payload.get("portMapping") == [{
                        "webviewPort": 4173,
                        "extensionHostPort": 14173,
                    }]
                    and custom_option_payload.get(
                        "retainContextWhenHidden") is True),
                "customEditorMessageRelay": (
                    custom_message_result.get("ok") is True
                    and any(
                        isinstance(item, dict)
                        and item.get("target") == "custom-editor"
                        for item in command_state.get(
                            "custom", {}).get("messages", []))),
                "runtimeSurfaceListing": (
                    runtime_surfaces.get("summary", {}).get(
                        "webviewViews", 0) >= 1
                    and any(
                        item.get("id") == "saoProbe.dynamicView"
                        for item in runtime_surfaces.get("webviewViews", []))
                    and any(
                        item.get("viewType") == "saoProbe.customEditor"
                        for item in runtime_surfaces.get(
                            "customEditors", []))),
                "frontendVisualWebviewSurface": bool(visual_webview_checks)
                and all(bool(value) for value in visual_webview_checks.values()),
                "frontendVisualCustomEditorSurface": bool(visual_custom_checks)
                and all(bool(value) for value in visual_custom_checks.values()),
                "frontendVisualFixtureWritten": (
                    bool(visual_fixture.get("fixtureBytes"))
                    and bool(visual_fixture.get("dataBytes"))),
            }
            result = {
                "ok": all(checks.values()),
                "extensionDir": extension_dir,
                "workspaceDir": workspace_dir,
                "install": install_result,
                "diagnostics_enabled": diagnostics_enabled,
                "activate": activate_result,
                "webview": {
                    "viewId": webview_view_id,
                    "rendered": bool(webview_render),
                    "htmlLength": len(webview_html),
                    "options": webview_option_payload,
                    "metadata": webview_metadata_payload,
                    "message": webview_message_result,
                },
                "customEditor": {
                    "viewId": custom_view_id,
                    "rendered": bool(custom_render),
                    "htmlLength": len(custom_html),
                    "open": custom_result,
                    "state": custom_state,
                    "options": custom_option_payload,
                    "message": custom_message_result,
                },
                "runtimeSurfaces": {
                    "summary": runtime_surfaces.get("summary", {}),
                    "webviewViews": runtime_surfaces.get("webviewViews", []),
                    "customEditors": runtime_surfaces.get("customEditors", []),
                },
                "frontendVisual": visual_fixture,
                "commandState": command_state,
                "checks": checks,
                "capturedEvents": len(events),
            }
            failed = [name for name, ok in checks.items() if not ok]
            if failed:
                result["error"] = "Smoke checks failed: " + ", ".join(failed)
            if keep_copy:
                result["keptExtensionDir"] = extension_dir
                result["keptWorkspaceDir"] = workspace_dir
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
        default="",
        help=(
            "Directory of an already extracted VS Code extension. "
            "When omitted, a temporary dynamic webview/custom editor smoke "
            "extension is generated and tested."
        ),
    )
    parser.add_argument(
        "--keep-copy",
        action="store_true",
        help="Keep the copied extension directory path in the JSON output",
    )
    parser.add_argument(
        "--visual-fixture-dir",
        default="",
        help=(
            "Directory where the built-in smoke writes a browser-openable "
            "visual-webview-smoke.html fixture and JSON evidence."
        ),
    )
    args = parser.parse_args()

    if args.extension_dir:
        result = run_probe(args.extension_dir, keep_copy=args.keep_copy)
    else:
        result = run_builtin_smoke_probe(
            keep_copy=args.keep_copy,
            visual_fixture_dir=args.visual_fixture_dir)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
