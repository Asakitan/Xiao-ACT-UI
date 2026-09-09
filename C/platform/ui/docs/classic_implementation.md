# Classic SAO implementation and acceptance

## Current reconstruction status
The previous generic-card/AI-only Preview result was rejected as a complete visual rebuild.
The native GPU rendering foundations below are implemented, but full visual and functional
parity with the complete Python editor and the supplied SAO Utils references remains open.
The frozen `python/web/ai_editor_app.html` now supplies the complete hosted Workbench
structure through the native CompositionController path. Its editor, explorer, terminal,
extension and settings method families still require the remaining native adapters;
hosting the complete frontend alone is not backend parity.

## Design reference
- Primary: https://www.bilibili.com/video/BV1iv4y1U7xA/
- Supplemental: https://www.bilibili.com/video/BV1844y1374J/
- Reference catalogs only: https://github.com/SAO-UI/sao-assets and https://github.com/vm-sao/sao-icons
- White/gray translucent surfaces, restrained orange focus, round menu actions, perspective hierarchy. Workbench controls stay readable and functional rather than copying decorative menu geometry into every field.

## Implemented contracts
- Classic light is the new default. An explicit persisted dark choice remains dark. Palette: #F8F8F8 / #646364 / #BCC4CA / #F3AF12.
- Display/Chinese body/monospace roles share DirectWrite measurement and paint configuration. The existing SAO UI font is embedded privately, with no system font installation. Resource provenance is in `classic_asset_sources.md` and the asset manifests; original repository font/audio rights remain unverified.
- Public sound cue 0–10 and Link Start config sizes remain unchanged. Cue 11–14, sound sessions, event prioritization, custom PCM WAV playback and dialog password options are append-only.
- Hosted guide audio uses the native XAudio2 mixer; its local mute is an additional restriction. A separately opened browser uses bounded HTML audio. Global volume keeps the existing min(requested, global) ABI semantics.
- TextField uses a compositor-owned Win32 EDIT proxy for selection, clipboard and IME, with native SAO rendering. Real-time values retain `text` and `value`; selection-only changes repaint without redispatching a business action.
- Compositor layer buttons use the shared `1 = down`, `0 = up` contract. A capture-loss or cancel edge clears panel presses, slider/resize/scrollbar drags and close arming without dispatching an action; the next complete click starts from a neutral state.
- JSON `dock`, `clip`, and `scroll` are optional. Old documents retain the legacy whole-panel scrolling path. Viewports preserve offsets by stable ID; text focus and edits survive stable body refresh.
- Slider uses a real typed control, updates while dragging, and emits one final action on release. Dropdown uses the existing native popup, selection callback, keyboard navigation, and clipped z-last painting.
- Panels, Entity/NerveGear and Link Start text record immutable drawing commands. Direct2D 1.1 draws those primitives and DirectWrite text directly into compositor-owned BGRA8 D3D11 textures. Normal native pages no longer rasterize/upload complete CPU frames. Explicit panel rasterization and local image/custom-canvas content retain their separate CPU paths.
- The one DirectComposition target now owns explicit `BELOW_NATIVE` and `ABOVE_NATIVE` external visual slots around the flattened native swapchain. Slot targets are generation-scoped across device loss, participate in host clipping and exact chorded L/R/M/X raw input without entering native snapshots/effects, and remain on the compositor owner thread.
- AI main prefers an in-process WebView2 CompositionController bound to the full-client `ABOVE_NATIVE` slot and fixed `https://sao-workbench.local/ai_editor_app.html` origin. It issues a fresh challenge after each completed navigation, accepts only challenge-bound `sao.workbench` hello/request envelopes, supplies a light-default bootstrap config plus `win_close`, returns structured errors for unavailable adapters, and keeps the existing native panel as startup/runtime fallback.
- GPU Link Start uses 300 instanced 3D cylinders, depth, canonical camera/Bezier curves, warm/cool palettes, motion trails and half-resolution bloom. Precompiled HLSL is built with Windows SDK FXC.
- One monotonic production clock coordinates the opening and sound phases. The default preserves the reference's 0.72-second prelude plus 10-second scene; explicit custom timelines preserve their wall-clock total duration. Intro skip, completion and failure converge on one completion edge. GPU white field fades with premultiplied alpha into the actual underlying UI.

## Source coverage and remaining acceptance
| Surface | Source change | Visual/runtime acceptance |
| --- | --- | --- |
| Entity root/child menu | Direct GPU vector circles, semantic SVG icons, separate label/submenu strips, orange focus | Production-renderer manual inspection in progress; full DPI/data/interaction matrix open |
| Launcher settings | Fixed header/footer, independent category/content scroll, checkbox, volume slider, theme dropdown, 15-cue audition | Production binding compiled; full backend session pending |
| Hotkeys / plugins / license / user menu | Classic tokens, fixed status and scrollable content, shorter labels | Production binding compiled; real data/empty/error/manual gates pending |
| Workshop | Catalog connectivity is separate from owner/worker lifetime; stale catalog actions are disabled when the latest validated list is disconnected | Detached production Preview observed `目录未连接`, structured list failure and normal retry/empty layout; connected backend still pending |
| Process selector | Full process snapshot with 32-row materialization pages; core enumeration remains available without RT I/O while attach is explicitly disabled | Production Preview observed 1–32 of 357, Page 1/12, `Attach off / 未连接`, disabled attach actions and clean close |
| AI settings | Scope rail, actual text search, independent scroll, bottom save area, checkbox/dropdown fields | Native offline renderer inspected at 1264×820; initial Dock/resize/TextField dispatch defects found and corrected; input frame visually rechecked; full editing/scroll acceptance remains open |
| AI main / Control Center | Full Workbench asset hosted by CompositionController; native panel retained as fallback | Debug production Preview created the AIWorkbench WebView2 process tree and completed hello/ready; adapter slices and full visual/input/backend acceptance remain open |
| Embedded AI pages | Classic light/default and neutral-dark CSS | Source review; hosted WebView runtime pending |
| User guide | Classic CSS, shorter interactions, native sound bridge and real WAV copies | Browser inspected at 1280×900 and 390×844, no horizontal overflow or broken images |
| Link Start | GPU compositor path, white transition, SAO display font, one-shot completion and skip | Compiled; real GPU tunnel and SAO-font white-field welcome captured, then returned to actual settings UI; FPS/audio timing not measured |

## Manual gates (not claimed as passed)
- 1080p / 1440p / 4K and 100% / 150% / 200% DPI; high contrast, reduced motion, long Chinese labels, dense/empty/loading/error/disabled pages.
- Chinese IME composition/candidate positioning, selection/paste/password, stable refresh, nested scroll, fixed composer and modal focus.
- Rapid click/focus sound deduplication, live master/local mute, custom WAV playback, intro skip with no residual sound, subjective A/B timbre matching.
- Measured 60 FPS frame times, memory/VRAM, real DXGI device removal/recreation and long-session resource counts.
- Public/custom backend integration through Preview's real headless process. No backend/driver was started just to inspect UI.

## Known compositor scope
- Native and existing CPU-source layers are composited directly in z order. CPU sources upload by content revision; translation/fade changes do not reupload pixels or replay a static panel's draw list.
- Backdrop blur, color matrix and shadow execute on GPU textures at each layer's actual z position. The CPU-prefix path and the unsupported CPU-effect-above-GPU restriction have been removed.
- Draw-list publication holds no live panel/widget/callback. Only the compositor owner thread creates, replays, resizes and releases GPU contexts, including device recovery. Every master draw explicitly restores rasterizer/depth/shader state after Direct2D or a custom GPU renderer.
- Legacy externally shared texture sources still use their old staging conversion path. This was not generalized into the new native GPU path.
- GPU callbacks run under the compositor render-thread contract and must not re-enter compositor APIs.
- External visuals bypass the native master texture, so native backdrop/effect passes and `snapshot_bgra` do not sample WebView pixels. Cross-band blur must be implemented inside the owning external surface or replaced by an explicit non-sampling treatment.

## Build / preview (no automated tests)
Production compilation targets: `sao_platform_ui sao_plugin_ai_editor SaoAuto` in `C/build/windows-debug`, Debug.
Developer preview: target `sao_ui_preview`; uses the real overlay host + DComp compositor, not a screenshot display loop.
- Default: the actual Entity root menu with Launcher Settings, Hotkeys, Plugins, Workshop, Process Selector, License, User Menu and Guide owners. AI main/settings are menu entries, not the whole application.
- `--workspace PATH --backend EXE`: explicit native AI process/workspace selection. No AI backend starts without `--backend`; `--offline` suppresses even that explicit backend option.
- Workshop remains detached, plugin runtime is not started, and Process Selector has no RT I/O provider in this developer shell. These are the production pages' own empty/error states, not connected-backend evidence.
- `--main` / `--ai-main` / `--ai-settings`: start a particular AI surface instead of the root menu.
- `--intro`: run the native GPU intro. `--settings PATH` chooses a preview settings file; its saved Profiles use a sibling `profiles/` directory. Without the option, `.sao/ui-preview/settings.json` and `.sao/ui-preview/profiles/` stay separate from production preferences and profile snapshots.
- Current owner-thread inspection also exercised a real Launcher Settings Audio click, a `WM_CANCELMODE` between down/up with no stuck press or action, a successful click immediately afterward, the detached Workshop error state and the populated Process Selector first page.
Close an active AI run with its Stop action before closing the host. Busy retirement retains the UI and dependencies instead of freeing borrowed backend handles.

Validation follows the repository's no-test policy: production compiler/linker, artifact metadata inspection, source path review and explicitly scoped manual UI inspection only.
