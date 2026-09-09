# Classic SAO implementation and acceptance

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
- JSON `dock`, `clip`, and `scroll` are optional. Old documents retain the legacy whole-panel scrolling path. Viewports preserve offsets by stable ID; text focus and edits survive stable body refresh.
- Slider uses a real typed control, updates while dragging, and emits one final action on release. Dropdown uses the existing native popup, selection callback, keyboard navigation, and clipped z-last painting.
- GPU Link Start uses D3D11 instanced perspective streaks, particles/background shading and half-resolution bloom. Its cropped CPU text layer is transparent; the full-screen background never uses the CPU raster/readback loop. Precompiled HLSL is built with Windows SDK FXC.
- One monotonic production clock coordinates 10-second visual/sound phases. Intro skip, completion and failure converge on one completion edge. GPU white field fades with premultiplied alpha into the actual underlying UI.

## Source coverage and remaining acceptance
| Surface | Source change | Visual/runtime acceptance |
| --- | --- | --- |
| Entity root/child menu | Classic circular states, geometric icons, perspective/info-panel styling | Source review; full production interaction/blur still pending |
| Launcher settings | Fixed header/footer, independent category/content scroll, checkbox, volume slider, theme dropdown, 15-cue audition | Production binding compiled; full backend session pending |
| Hotkeys / plugins / workshop / process selector / license / user menu | Classic tokens, fixed status and scrollable content, shorter labels | Production binding compiled; real data/empty/error/manual gates pending |
| AI settings | Scope rail, actual text search, independent scroll, bottom save area, checkbox/dropdown fields | Native offline renderer inspected at 1264×820; initial Dock/resize/TextField dispatch defects found and corrected; input frame visually rechecked; full editing/scroll acceptance remains open |
| AI main / Control Center | Fixed composer, actual multiline edit, separate control scroll/fixed actions | Native offline main/composer visually inspected; real backend/chat session pending |
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
- Native GPU layers are composited directly in z order. CPU layers upload by revision.
- Existing effect-heavy CPU layers below the first GPU layer are a cached prefix; animated prefix content still incurs its CPU compositing work.
- CPU backdrop/shadow effects above a native GPU layer currently report an explicit unsupported combination; they are not silently dropped.
- Legacy externally shared texture sources still use their old staging conversion path. This was not generalized into the new native GPU path.
- GPU callbacks run under the compositor render-thread contract and must not re-enter compositor APIs.

## Build / preview (no automated tests)
Production compilation targets: `sao_platform_ui sao_plugin_ai_editor SaoAuto` in `C/build/windows-debug`, Debug.
Developer preview: target `sao_ui_preview`; uses the real overlay host + DComp compositor, not a screenshot display loop.
- Default: real neighboring `SaoAiEditor.exe` with native named-pipe handshake and current workspace.
- `--workspace PATH --backend EXE`: explicit real process/workspace selection.
- `--offline`: explicitly disconnected UI; never reports a mock backend as connected.
- `--main`: AI main panel; default is AI settings.
- `--intro`: run native GPU intro over the panel.
Close an active AI run with its Stop action before closing the host. Busy retirement retains the UI and dependencies instead of freeing borrowed backend handles.

Validation follows the repository's no-test policy: production compiler/linker, artifact metadata inspection, source path review and explicitly scoped manual UI inspection only.
