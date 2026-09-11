# Classic SAO implementation and acceptance

## Current reconstruction status
The previous generic-card/AI-only Preview result was rejected as a complete visual rebuild.
The native GPU rendering foundations below are implemented, but full visual and functional
parity with the complete Python editor and the supplied SAO Utils references remains open.
The legacy `python/web/ai_editor_app.html` supplies the hosted Workbench structure through
the native CompositionController path. The native copy adds visual and keyboard refinements;
the Python source remains unchanged. `workbench_native_adapter.cpp` implements ordinary
method families, but their complete interactive backend acceptance remains open.

Session-30 updates native paint, Launcher feedback/catalog filtering, editor/embedded-page
CSS and guide navigation. Initial full Debug and Preview builds succeeded. A real settings
screen capture exposed truncated footer labels, now shortened in source; rebuilding the
correction initially encountered an out-of-scope C2607 assertion. The subsequent v4.15
status records that dependency blocker as resolved. Browser checks cover guide
1280/390 and offline editor 1280/430 widths, modal focus return and single-fire Enter/Space
on the editor notification control. This is not complete UI/API acceptance.

## Design reference

The editor workbench was directly restyled after the user rejected its decorative result:
neutral white/charcoal IDE surfaces, compact rectangular navigation, restrained warm accent,
aligned reading/composer columns and stacked narrow panes. Native menu and embedded-panel
styling remain separate. Code overlay geometry and transparency are no longer overridden
by the workbench stylesheet; DOM, JS and backend contracts did not change.
- Primary: https://www.bilibili.com/video/BV1iv4y1U7xA/
- Supplemental: https://www.bilibili.com/video/BV1844y1374J/
- Reference catalogs only: https://github.com/SAO-UI/sao-assets and https://github.com/vm-sao/sao-icons
- Ordinary native chrome uses a circular launcher, legible floating cards, thin neutral rules, clipped technical corner details, and amber selection without blue decorative gradients.
- Workbench controls keep conventional readable geometry; success, warning, error, HP, shield, and element colors remain semantic rather than becoming chrome accents.

## Implemented contracts
- Classic light remains the default: porcelain `#eeeae4`, card white `#faf9f6`, ink `#232724`, muted `#646a65`, rules `#d6d6ce`, and selection `#d99536`. Persisted dark uses graphite `#202421` / `#2b302c`, pale text `#eef0e8`, and `#edb45b` accent.
- Display/Chinese body/monospace roles share DirectWrite measurement and paint configuration. The existing SAO UI font is embedded privately, with no system font installation. Resource provenance is in `classic_asset_sources.md` and the asset manifests; original repository font/audio rights remain unverified.
- Public sound cue 0–10 and Link Start config sizes remain unchanged. Cue 11–14, sound sessions, event prioritization, custom PCM WAV playback and dialog password options are append-only.
- Hosted guide audio uses the native XAudio2 mixer; its local mute is an additional restriction. A separately opened browser uses bounded HTML audio. Global volume keeps the existing min(requested, global) ABI semantics.
- TextField uses a compositor-owned Win32 EDIT proxy for selection, clipboard and IME, with native SAO rendering. Real-time values retain `text` and `value`; selection-only changes repaint without redispatching a business action.
- Compositor layer buttons use the shared `1 = down`, `0 = up` contract. A capture-loss or cancel edge clears panel presses, slider/resize/scrollbar drags and close arming without dispatching an action; the next complete click starts from a neutral state.
- JSON `dock`, `clip`, and `scroll` are optional. Old documents retain the legacy whole-panel scrolling path. Viewports preserve offsets by stable ID; text focus and edits survive stable body refresh.
- Slider uses a real typed control, updates while dragging, and emits one final action on release. Dropdown uses the existing native popup, selection callback, keyboard navigation, and clipped z-last painting.
- Shared panel paint now distinguishes headers, panels, sections, groups, and cards with primary/secondary type roles, short warm markers, full neutral rules, and clipped corner strokes. Shared generic and typed controls use the same tokenized surfaces, complete focus outlines, rounded checkbox/dropdown/slider treatment, and ink foregrounds on warm active fills.
- Public types, enum values, callbacks and destruction contracts are unchanged. Native root-label paint and input now share an additive 208x38 clickable area; circle slots retain their existing bounds. The production GPU menu uses ellipsized labels, clipped child rows and quieter decoration; legacy CPU resource-inspection paint remains unchanged.
- Panels, Entity/NerveGear and Link Start text record immutable drawing commands. Direct2D 1.1 draws those primitives and DirectWrite text directly into compositor-owned BGRA8 D3D11 textures. Normal native pages no longer rasterize/upload complete CPU frames. Explicit panel rasterization and local image/custom-canvas content retain their separate CPU paths.
- The one DirectComposition target now owns explicit `BELOW_NATIVE` and `ABOVE_NATIVE` external visual slots around the flattened native swapchain. Slot targets are generation-scoped across device loss, participate in host clipping and exact chorded L/R/M/X raw input without entering native snapshots/effects, and remain on the compositor owner thread.
- AI main prefers an in-process WebView2 CompositionController bound to the full-client `ABOVE_NATIVE` slot and fixed `https://sao-workbench.local/ai_editor_app.html` origin. It issues a fresh challenge after each completed navigation, accepts only challenge-bound `sao.workbench` hello/request envelopes, supplies a light-default bootstrap config plus `win_close`, returns structured errors for unavailable adapters, and keeps the existing native panel as startup/runtime fallback.
- GPU Link Start keeps 300 six-vertex columns, divided into 60 foreground shafts, 180 middle-distance columns and 60 fine tracks. A capsule profile, luminous core/local halo and depth attenuation replace flat wide bands. Seeded golden-angle azimuths and stratified depths reduce random clustering. Three fixed shutter samples use weights 0.25/0.35/0.40; palette selection stays on the current frame so P3 is blue even at a zero-gap boundary. Accumulated 12500/16000-unit flights use smooth acceleration, cruise and deceleration, continuing through their fade tails.
- Scene and narrow/wide bloom targets use internal RGBA16F linear light; authored colors are decoded before accumulation. Bloom extraction occurs once with a soft knee, followed by half- and quarter-resolution filtering. Peak-based compression, explicit sRGB output and static sub-code-value dithering preserve color and dark gradients on the existing SDR compositor. This does not change system/display HDR or the swap chain.
- The latest on-disk aperture/light-valve opening is retained, with its background rays/rings subordinated to the columns; flash remains zero and no chromatic-split or white-field pass is added. The full-window measured fly-out remains. Body-role tracked ASCII captions, a larger hold wordmark and a quieter four-segment `LINK SEQUENCE` footer refine hierarchy. Reduced motion retains its 450ms fade and one-shot completion.
- Native pointer and keyboard skip affordances are removed. Both intro layers retain full rectangular input coverage; Launcher and production Preview consume intro keys without dismissal. Public programmatic dismissal, window shutdown and error completion remain unchanged. Late ticks do not replay expired cues. Preview parent-window input still uses the compositor and screen-space wheel coordinates.
- Guide intro owns focus and makes existing background nodes inert through its exit, restores their prior state, and supports short keyboard/complete-pointer skip. The native completion fragment and existing session preference still suppress duplicate introductions.
- Zero-duration P1/P3 phases do not synthesize new particle tails; nonzero phases retain their existing tails and wall-clock completion. Guide pointer leave and lost capture clear only the matching armed pointer. These two read-only review findings were fixed directly and rebuilt in Debug and both release acceptance configurations.
- One monotonic production clock coordinates the opening and sound phases. The default retains the 0.72-second prelude plus 10-second scene; custom timelines retain their wall-clock duration. Entry and exit use matching premultiplied fades for background and typography.
- The embedded Link Start voice has 44116 frames at 44100 Hz (1.000362812s). NerveGear starts on the first non-overdue tick at or after 1001ms, rather than waiting for the title phase. Online/Welcome remains tied to the blue-flight P3 entrance (5.92s by default). These are visual-clock triggers, not a sample-accurate audio playlist.
- Successful natural completion retains the sound group so the 5.600317s Online/Welcome cue can finish its remaining tail. Explicit dismissal also stops this tail when the intro is already inactive; error, offline, teardown, destruction and replay retain immediate group cleanup without a second completion edge. Reduced-motion suppression remains.
- Bootstrap/hold integration was added concurrently in session-31 and is outside this visual/audio slice's changes and verification; the separate section below describes that work. Offline frame export runs without backend activation or telemetry.

## Source coverage and remaining acceptance
| Surface | Source change | Visual/runtime acceptance |
| --- | --- | --- |
| Entity root/child menu | Shared label paint/hit geometry, larger measured/ellipsized text, contained child paint, category/range header, quiet circles and corrected hover draw order | Native GPU static export inspected; full DPI/data/interaction matrix remains open |
| Launcher settings | Fixed header/footer, independent category/content scroll, checkbox, volume slider, theme dropdown, 15-cue audition | Production binding compiled; full backend session pending |
| Hotkeys / plugins / license / user menu | Classic tokens, fixed status and scrollable content, shorter labels | Production binding compiled; real data/empty/error/manual gates pending |
| Workshop | Catalog connectivity is separate from owner/worker lifetime; stale catalog actions are disabled when the latest validated list is disconnected | Detached production Preview observed `目录未连接`, structured list failure and normal retry/empty layout; connected backend still pending |
| Process selector | Full process snapshot with 32-row materialization pages; core enumeration remains available without RT I/O while attach is explicitly disabled | Production Preview observed 1–32 of 357, Page 1/12, `Attach off / 未连接`, disabled attach actions and clean close |
| AI settings | Scope rail, actual text search, independent scroll, bottom save area, checkbox/dropdown fields | Native offline renderer inspected at 1264×820; initial Dock/resize/TextField dispatch defects found and corrected; input frame visually rechecked; full editing/scroll acceptance remains open |
| AI main / Control Center | Full Workbench asset hosted by CompositionController; native panel retained as fallback | Debug production Preview created the AIWorkbench WebView2 process tree and completed hello/ready; adapter slices and full visual/input/backend acceptance remain open |
| Embedded AI pages | Classic light/default and neutral-dark CSS | Source review; hosted WebView runtime pending |
| User guide | Classic CSS, shorter interactions, native sound bridge and real WAV copies | Browser inspected at 1280×900 and 390×844, no horizontal overflow or broken images |
| Link Start | Linear HDR light columns, layered bloom, normalized shutter samples, three depth/size groups, cruise camera, refined type/footer, no interactive skip and aligned audio | Fresh Debug UI/Preview, 14 native intro times plus menu and PCM metadata inspected. Corrected central horizon haze; multicolor/blue and title hold/fly-out remain readable and unclipped internally. Independent review pending; continuous motion/audio-device/input/DPI acceptance remains open |

## Manual gates (not claimed as passed)
- 1080p / 1440p / 4K and 100% / 150% / 200% DPI; high contrast, reduced motion, long Chinese labels, dense/empty/loading/error/disabled pages.
- Chinese IME composition/candidate positioning, selection/paste/password, stable refresh, nested scroll, fixed composer and modal focus.
- Rapid click/focus sound deduplication, live master/local mute, custom WAV playback, unskippable intro with normal shutdown and no residual sound, subjective A/B timbre matching.
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
- `--offline --frame-out PATH.bmp --frame-ms N`: advance native UI on its owner thread and export a frame, then close; add `--intro` for Link Start. N is 0..60000 (default 1000). Export skips global hotkey registration and is muted. It handles the snapshot sizing query's BUFFER_TOO_SMALL result and advances Entity in steps of at most 1000ms. External WebView visuals are not included; this developer executable is not shipped.
- Current owner-thread inspection also exercised a real Launcher Settings Audio click, a `WM_CANCELMODE` between down/up with no stuck press or action, a successful click immediately afterward, the detached Workshop error state and the populated Process Selector first page.
Close an active AI run with its Stop action before closing the host. Busy retirement retains the UI and dependencies instead of freeing borrowed backend handles.

Validation follows the repository's no-test policy: production compiler/linker, artifact metadata inspection, source path review and explicitly scoped manual UI inspection only.
The earlier shared-UI slice received independent static source and diff review. Its evidence
includes full Debug and final Preview builds plus RelWithDebInfo/Hardened acceptance, each
with 38 PE / 83 shipped files. The historical capture-semantics assertion is resolved.
Earlier desktop input captures contained other windows and are excluded from acceptance;
the removed F12 experiment has been replaced by explicit offline frame export. This path
successfully exported seven Link Start stages and the menu directly from the native compositor.
Pointer cancellation, full DPI, audio and frame-time acceptance remain open. No automated test
or connected backend was run.

The earlier colored/blue flight correction has a Debug build of UI, Launcher and production
Preview, clean source diagnostics and scoped diff checks. Native frames were exported at
800/1800/2500/3000/4700/5750/6000/6500/7000/7500/8500/10720ms plus the menu; key flight/title
frames were inspected. Saved local images include `.sao/ui-preview/linkstart-colored-flight.png`
and `linkstart-blue-flight.png` under `sao_auto`. No Release/Hardened refresh or startup backend
execution belongs to this correction. Independent review returned `passed-after-fix` after
correcting the zero-gap palette boundary. The final UI DLL rebuilt successfully; all native
frames were regenerated and the five saved key images retained identical hashes. Custom
zero-gap runtime behavior remains source-reviewed rather than live-validated.

The cinematic-lighting/audio slice changes the two Link Start C++ implementations and four
existing shaders, plus a formatting repair for concurrently joined include directives. Debug
UI/Preview artifacts postdate these sources; 14 intro times plus menu were exported muted.
PCM inspection confirms the voice's 44116 frames, the first effect's 3.9s and Online/Welcome's
5.600317s. Native snapshots validate appearance, not actual playback or motion. Independent
review is pending. No new shipped asset or Markdown file; concurrent session-31 additions
were preserved and no driver/engine/Launcher initialization was run for this slice.

## Bootstrap-covered intro (session-31)

The intro can now act as the cover for driver/engine bring-up instead of playing after it.
`sao_ui_linkstart_arm_bootstrap_hold` parks the scene clock on the CONNECTED frame
(`p4_hold_end`, or 200 ms under reduced motion) so the animation cannot reach
`total_duration` while the hold is armed; `sao_ui_linkstart_set_bootstrap` feeds the
32-byte `SaoUiLinkStartBootstrap` record (stage index/count, flags, caption) that replaces the
animation-clock rail with `(stage_index + stage_progress) / stage_count` and renders the caption
line as `<CAPTION> i/N`. `sao_ui_linkstart_release_bootstrap_hold` either resumes the
`p4_hold_end` → `p4_fade_end` tail (clean boot) or completes through the new
`SAO_UI_LINKSTART_COMPLETION_BOOTSTRAP_FAILED` reason. The Launcher arms the hold from
`sao_ui_intro_show` right after the surface stage and drives six stages underneath it:
`DRIVER CHAIN` (worker thread; the owner thread pumps the intro and compositor through
`sao_ui_intro_pump`, because the entity shell is not online yet), `ENGINE SURFACES`,
`CAPTURE SHIELD`, `WINDOW SCRUB`, `ENGINE RUNTIMES` and `PLUGIN ENGINES`. The public 24-byte
config, the 112-byte timeline constants, the reduced-motion path and the ordering of the existing
completion reasons are unchanged.

The capture-shield stage re-applies the dual-HWND affinity through
`sao_ui_overlay_host_set_capture_mode` after every window exists, so the host keeps ownership of the
hRender/hControl pair and its rollback. The window-scrub stage additionally registers hControl and
the suppressed owner with the rt_io window-rect controller and a coordinator registration for
hControl, then submits the rcWindow `(0,0,1,1)` decoy and the `OVERLAY_EXSTYLE_MASK` ExStyle
transaction. Both mutations degrade to `SAO_STATUS_OK` when no physical provider is installed, which
is the same USER32/DWM-only behaviour the host's own scrub path already had.
