# Workbench asset source and native bridge contract

## Frozen source record

- Canonical frozen source: `E:\VC\SAO-UI\sao_auto\python\web\ai_editor_app.html`
- Source SHA-256: `FE9CD236F9B8C519F1382FDD260DF1273C2EEF50452752368A7B25EF4875DC8D`
- Source logical line count: `59517`
- Copied workbench page: `ai_editor_app.html`
- Copy delta: the external `native-bridge.js` script and `classic-theme.css` overlay, matching light/dark fallback CSS tokens, and the appended `sao-ordinary-control-accessibility` script for existing clickable controls. Existing IDs, commands and RPC names are retained. The legacy Python source remains unchanged; this native copy is no longer byte-identical to its original inline bundle.

## New local resources and licensing provenance

| Resource | Origin | License/provenance |
| --- | --- | --- |
| `ai_editor_app.html` | Frozen in-repository source listed above | Repository source; see `E:\VC\SAO-UI\sao_auto\LICENSE` for repository licensing. |
| `classic-theme.css` | Neutral light/charcoal IDE presentation with restrained warm accents | Authored for this workbench asset; no third-party code, font file, image, or network asset is bundled. System font names are only CSS fallbacks. |
| `native-bridge.js` | New local WebView2 transport/compatibility facade | Authored for this workbench asset; no third-party code or network dependency. |
| `SOURCE.md` | New local provenance/contract record | Authored for this workbench asset. |

## WebView2 transport contract

The editor presentation override replaces the earlier decorative treatment rather than
stacking another theme on top. It leaves code-layer font metrics, padding and transparent
overlays to the document, aligns conversation and composer widths, wraps existing controls
inside narrow panes, and stacks editor/chat below 760px without changing their actions.
The original inline fallback tokens remain; this external stylesheet is the normal visual
authority. Browser inspection covered light/dark, desktop split panes, 430px stacked panes,
settings and matching editor overlay coordinates. Backend calls remained offline; appearance
inspection does not establish backend parity or user approval of the design.

After the native host sends a fresh post-navigation challenge, the page echoes it in every
document-bound envelope through `window.chrome.webview.postMessage`:

```json
{"channel":"sao.workbench","kind":"hello","challenge":"CHALLENGE","capabilities":["rpc","events","pywebview-api-compat"]}
{"channel":"sao.workbench","kind":"request","challenge":"CHALLENGE","id":"sao-...","method":"METHOD","args":[...]}
```

The native host must return messages with `CoreWebView2.PostWebMessageAsJson`; the page accepts replies only from `window.chrome.webview.addEventListener("message", ...)`:

```json
{"channel":"sao.workbench","kind":"ready","challenge":"CHALLENGE","connected":true,"capabilities":["rpc","events"]}
{"channel":"sao.workbench","kind":"reply","challenge":"CHALLENGE","id":"sao-...","ok":true,"result":{}}
{"channel":"sao.workbench","kind":"reply","challenge":"CHALLENGE","id":"sao-...","ok":false,"error":{"code":"SAO_METHOD_UNAVAILABLE","message":"..."}}
{"channel":"sao.workbench","kind":"event","challenge":"CHALLENGE","name":"editor_event","payload":{"event":"EVENT_NAME","data":{}}}
```

`id` is a string and must round-trip unchanged. A `ready` message is the only condition that installs the `window.pywebview.api` compatibility facade and dispatches `pywebviewready`. `connected:false` deliberately installs an offline facade whose calls reject with `SAO_NATIVE_UNAVAILABLE`; it is not a successful fallback. Without a WebView2 host, the same offline facade is installed immediately. Requests time out after 30 seconds with `SAO_NATIVE_TIMEOUT`.

Native events become `sao:workbench-event` / `sao:workbench:EVENT_NAME` browser events. They also call the copied page's `_onEditorEvent`; an early event is queued until that legacy callback exists. Window `postMessage` is not a native reply channel.

## Native host adapter contracts and acceptance

The compatibility facade dynamically maps legacy `window.pywebview.api.METHOD(...args)` calls to `request` envelopes. The host needs an explicit allow-list and real result/error envelopes for the ordinary workbench families that it chooses to support:

- configuration/settings: `load_config`, `save_config`, mode/provider/model and editor theme/language/settings calls. When no persisted user theme exists, `load_config` must return `theme:"light"` and `color_theme:""`; a saved `dark` choice must round-trip unchanged;
- workspace/editor: list/open/save/search/tree/decorations, editor selection/options/visible-range reporting, and command palette actions;
- terminal/tasks: task listing/execution/status/cancel plus `execute_command` where a native implementation exists;
- chat/providers: controls, conversation/history, provider status/model list, send/cancel/stream events, and workflow operations;
- extensions/MCP: only the ordinary extension and MCP management calls implemented by the native runtime;
- window controls and embedded webview panel lifecycle: `win_minimize`, `win_maximize`, `win_close`, file dialogs, and `webview_*` calls when the host owns those surfaces.

Every unimplemented method must reply with `ok:false` and a meaningful error code/message. The copied UI contains a larger historical API surface; its presence is not evidence of an implemented native endpoint. This asset adds no driver, memory, or security workflow. The bridge explicitly rejects `memviewer_read`, `memviewer_read_value`, and `memviewer_status`, and the host allow-list must omit those names.

The native host is implemented by `src/workbench_composition_host.cpp`. It uses
`ICoreWebView2Environment3::CreateCoreWebView2CompositionController`, attaches to
the platform compositor's generation-scoped `ABOVE_NATIVE` visual slot, and maps
this directory to the fixed document
`https://sao-workbench.local/ai_editor_app.html`. Top-level navigation and web
messages are accepted only from that exact document (an optional fragment is
ignored), and every other navigation is cancelled. Slot geometry, DPI, raw
mouse input, focus, device-generation rebind, and teardown remain on the one
compositor/`hRender` owner thread.

`src/workbench_native_adapter.cpp` implements the native dispatch for configuration,
file/editor services, chat/history/providers, agents/workflows, terminal/tasks,
diagnostics, extensions and embedded webview requests. The composition host drains
its document-bound replies and events. These are implemented adapters, not a
bootstrap-only host; backend-dependent methods still report explicit failures when
the backend is absent. Unknown methods receive a structured `SAO_METHOD_UNAVAILABLE`
reply. Complete interactive acceptance of these families is still pending; neither
the frontend's presence nor an adapter branch proves a successful live workflow.

## Execution note

Debug `sao_platform_ui`, `sao_plugin_ai_editor`, and the production
`sao_ui_preview --ai-main --offline` path were built. The production Preview
created the `AIWorkbench` WebView2 process tree and completed the
`hello → ready` exchange through this asset's bridge. Automated tests remain
disabled by repository policy.
