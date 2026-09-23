# Workbench asset source and native bridge contract

## Frozen source record

- Canonical frozen source: `E:\VC\SAO-UI\sao_auto\python\web\ai_editor_app.html`
- Source SHA-256: `FE9CD236F9B8C519F1382FDD260DF1273C2EEF50452752368A7B25EF4875DC8D`
- Source logical line count: `59517`
- Copied workbench page: `ai_editor_app.html`
- Copy delta: native transport, visual/accessibility refinements, originating-tab Save As handling, native tools tab, real extension inventory/tree operations, RTIO-backed Memory sidebar, global Latin/CJK typography and common extension-iframe font defaults. Existing IDs, commands and RPC names are retained. The legacy Python source remains unchanged; this native copy is no longer byte-identical to its original inline bundle.

## New local resources and licensing provenance

| Resource | Origin | License/provenance |
| --- | --- | --- |
| `ai_editor_app.html` | Frozen in-repository source listed above | Repository source; see `E:\VC\SAO-UI\sao_auto\LICENSE` for repository licensing. |
| `classic-theme.css` | Neutral light/charcoal IDE presentation with restrained warm accents | Authored locally; uses two bundled range-limited font faces, including form controls and technical values. |
| `fonts/SAOUI.ttf` / `fonts/ZhuZiAYuanJWD.ttf` | Existing project font assets, copied unchanged | Identity and provenance are recorded in `C/platform/ui/assets/fonts/SOURCE.md`; no font download or system installation occurs. |
| `native-bridge.js` | New local WebView2 transport/compatibility facade | Authored for this workbench asset; no third-party code or network dependency. |
| `SOURCE.md` | New local provenance/contract record | Authored for this workbench asset. |

## WebView2 transport contract

The editor presentation override replaces the earlier decorative treatment rather than
stacking another theme on top. The global font rule applies to code and ordinary controls;
the code layers retain their shared padding and transparent overlays. It aligns conversation and composer widths, wraps existing controls
inside narrow panes, and stacks editor/chat below 760px without changing their actions.
The original inline fallback tokens remain; this external stylesheet is the normal visual
authority. Earlier browser inspection covered light/dark, desktop split panes, 430px stacked panes,
settings and matching editor overlay coordinates. Backend calls remained offline; appearance
inspection does not establish backend parity or user approval of the design.

Current typography uses SAOUI for Latin/numbers and ZhuZiAYuanJWD for Chinese. Both faces
loaded in local-browser inspection; all 326 editor form controls resolve to SAO Product.
The common iframe theme provides the same faces and VS Code font variables without replacing
extension CSP or icon declarations. Only the font directory is mapped at sao-fonts.local with
CORS access; hosted iframe loading, unsupported glyphs and proportional terminal alignment
remain runtime checks, not results inferred from the zero-width browser viewport.

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
- terminal/tasks/debug: ConPTY shell terminals and extension pseudoterminals; task provider
	fetch/resolve/execution/cancellation and lifecycle events; debug configuration/descriptor/tracker
	providers plus executable, server, named-pipe and inline DAP transports;
- chat/providers: controls, conversation/history, provider status/model list, send/cancel/stream events, and workflow operations;
- extensions/MCP: only the ordinary extension and MCP management calls implemented by the native runtime;
- window controls and embedded webview panel lifecycle: `win_minimize`, `win_maximize`, `win_close`, file dialogs, and `webview_*` calls when the host owns those surfaces.

Every unimplemented method must reply with `ok:false` and a meaningful error code/message. The copied UI contains a larger historical API surface; its presence is not evidence of an implemented native endpoint. The Memory sidebar is an explicit exception to the previous offline-only rule: the composition host installs challenge-bound `memviewer_status`, `memviewer_regions`, `memviewer_read`, and `memviewer_read_value` methods before `pywebviewready`, and the adapter implements all four through its private read-only provider. No proxy pointer or native handle crosses the JSON boundary.

The native host is implemented by `src/workbench_composition_host.cpp`. It uses
`ICoreWebView2Environment3::CreateCoreWebView2CompositionController`, attaches to
the platform compositor's generation-scoped `BELOW_NATIVE` visual slot, and maps
this directory to the fixed document
`https://sao-workbench.local/ai_editor_app.html`. Top-level navigation and web
messages are accepted only from that exact document (an optional fragment is
ignored), and every other navigation is cancelled. Slot geometry, DPI, raw
mouse input, focus, device-generation rebind, and teardown remain on the one
compositor/`hRender` owner thread.

`src/workbench_native_adapter.cpp` implements the native dispatch for configuration,
file/editor services, chat/history/providers, agents/workflows, terminal/tasks/debug,
diagnostics, extensions and embedded webview requests. The composition host drains
its document-bound replies and events. These are implemented adapters, not a
bootstrap-only host; backend-dependent methods still report explicit failures when
the backend is absent. Unknown methods receive a structured `SAO_METHOD_UNAVAILABLE`
reply. Extension execution is owned by `extension_process_runtime.cpp`: process trees
are job-contained, cwd stays within the workspace, ConPTY and DAP traffic use bounded
queues/frames, and extension generation retirement closes all sessions. A local real
extension fixture has completed terminal input/output/dispose, TaskProvider fetch plus
ShellExecution lifecycle, and DAP initialize/launch/evaluate/disconnect in one host
generation with clean deactivation. Arbitrary marketplace adapters, long-running
background problem matchers and cross-machine shell matrices remain separate coverage.

## Editor and extension wiring (session 79)

Language providers now connect the native registry to Node callbacks with
generation ownership. Diagnostics and document/configuration/folder events have
concrete consumers; direct Node notifications coexist with the existing drain
for other clients, rather than destructively polling that drain twice.
Workbench completion, hover, definition and formatting carry unsaved content and
reject stale results. Missing providers remain explicit capability failures.
Versionless workbench snapshots use a separate bounded cache; they do not replace
authoritative `workspace.openTextDocument` buffers or establish full shared editor
document-lifecycle parity. Native extapi remains single-bound-runtime, not an
isolated multi-runtime service. Failed activation retires generation-owned state.

Opened files retain workspace/absolute identity. Saves serialize per originating
tab and compare expected content before atomic publication. Production and
standalone native byte APIs share a 1 MiB binary helper with workspace containment,
byte validation and identity checks; text APIs retain UTF-8 validation.
Post-publication verification failure does not guarantee rollback.

Settings/hotkeys/workshop/plugins/process/license requests run on the compositor
owner through the existing launcher SDK binding; memory uses its existing panel
provider. Worker entry for those seven names rejects owner-thread violations
without SDK invocation or child-IPC fallback. This adds no second panel owner.
The original language-service evidence remains tracked in session 79. Session 82 adds
the terminal/task/debug lifecycle described above without changing the public AI Editor
C ABI; Node-to-native methods and process/DAP handles stay private.

## Extension inventory and tree ownership

The extension host validates a canonical extension root, `package.json`, and `main`
entry before registration. A pinned manifest read is limited to 1 MiB and rejects
duplicate JSON keys, invalid UTF-8, NUL/control data, non-finite values, duplicate
extension IDs, and duplicate commands. Inventory responses distinguish unavailable,
error, and valid-empty states and retain owner path, owner ID, and contribution index.

The Node shim implements `registerTreeDataProvider` and `createTreeView`. Providers,
tree handles, and disposables belong to one extension generation. Every operation is
bound to generation, tree version, and item handle; stale handles fail rather than
mutating a newer tree. Provider callbacks execute outside host locks with bounded
timeouts, at most 500 children, depth 64, 64 KiB label data, and a 512 KiB response.
Action, expansion, checkbox, selection, drop, and visibility callbacks use the same
ownership checks. Retirement and quarantine invalidate the complete generation.

The workbench serializes mutations per tree and uses request sequencing. It commits
the result only when the current response is schema-valid and reports `applied:true`;
an absent provider, callback failure, or stale response never becomes a successful
empty repaint.

## Memory viewer contract

The Process Selector performs a provider drain before attach and revalidates PID and
process start time after attach. It then publishes a non-wrapping generation through
the AI Editor owner. Refresh disappearance, detach, panel teardown, and target change
clear the binding; a pending binding is replayed after panel recreation.

`MemoryViewerProvider` borrows the RTIO proxy and fences PID, start time, and generation
before and after every operation. Reads are exact and limited to 64 KiB. Region lists
are capped at 4,096 entries and the serialized response is capped below 4 MiB. A
region snapshot performs one explicit attach rather than reattaching per region.
Addresses and other 64-bit values are JSON strings; bytes are lowercase hexadecimal.
Detached, stale, exited, unreadable, and budget failures are structured errors.

The append-only public surface is AI Editor ABI 1.2:

- `sao_ai_editor_main_panel_bind_memory_target`
- `sao_ai_editor_main_panel_clear_memory_target`

The major version and every pre-existing structure layout and export remain unchanged.

## Native SDK dumper tools

`sdkDumper.source` performs a bounded `client.dll` `ClientClass → RecvTable →
RecvProp` traversal and writes `Source-netvars.dt`. `sdkDumper.unreal` validates the
target PE and signatures, resolves RIP-relative anchors, and bounds GUObjectArray,
FNamePool, FUObjectItem, UObject, and class traversal before writing `Unreal-dump.h`.
Both tools share a 64 MiB read budget, a deadline, checked module/pointer arithmetic,
cycle/visit limits, and a workspace-pinned random `CREATE_NEW` sibling followed by
atomic publication. They are registered as mutating native tools, require ask/plan
confirmation, and never accept an arbitrary output directory from the model.

## Historical execution evidence (before session 79)

Debug `sao_platform_ui`, the opaque AI Editor DLL target, and the production launcher
were built in the complete Debug tree. RelWithDebInfo and Hardened release acceptance
also compiled and packaged the same workbench and native adapter. `extension_host_shim.js`
and external `native-bridge.js` pass `node --check`; all 25 non-empty inline scripts across
the page's 26 total script tags parse. Real
extension packages/TreeDataProviders and live RTIO memory reads remain integration-host
gates. Automated tests remain disabled by repository policy.
