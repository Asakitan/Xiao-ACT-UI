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

Current Link Start prioritizes the user's first-generation SAO direction and the local
white-space/cylinder scene excerpts; the frozen Python renderer is a timing/structure
reference, not the current palette authority. Supplemental references include the Python
fisheye/menu shaders, MakeAGif `DZ4lIe` and Tenor `24757565` scene excerpts.
The Python file credits Cad-noob/SAO-UI; this is reference-guided reconstruction, not a claim
of frame-exact parity with an official Season 1 master. Reference locations:
```
https://github.com/Cad-noob/SAO-UI
https://makeagif.com/gif/sword-art-online-link-start-DZ4lIe
https://tenor.com/view/link-start-sao-gif-24757565
```
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
- The current first-generation-inspired direction replaces the previous dark neon field with luminous white space, solid colored cylinders, pearl UI and blue/white second flight. Depth lighting, shallow bevels, restrained highlights and distance fog remain; it is not a return to flat ribbons or a claim of frame-exact official footage.
- Each flight emits 720 finite columns using a 48-sided/four-band shared mesh (386 vertices, 2304 uint16 indices). Camera travel is 18000 times quintic smoothstep; normalized speed rises and falls symmetrically. Birth distance is `(18000 - spawnDepth - baseLength - 8) * i / 719`; geometry starts at zero size at the exact center and expands continuously over 320 travel units at far depth. Once born, it is not removed by density/near-alpha/group-fade gates: ordinary frustum clipping follows the complete body/tail leaving view. Physical length plus a bounded speed-driven extension produces straight trails; one 8.33ms exposure remains.
- Distant columns retain a birth-scaled 0.85px minimum projected radius rather than disappearing below raster coverage. Stable material brightness, broader highlights and white depth fog replace shimmer. At phase end speed is zero and every tail, including the bevel, is behind the near plane with margin; zero-duration phases emit nothing. These are source-level lifecycle properties, not an all-resolution visual-parity claim.
- Explicit custom timelines start colored flight at their prelude, while the default prelude-relative clock starts at zero. A positive custom prelude therefore begins at progress zero rather than mid-flight; `prelude == p1_end` emits no colored columns. These boundary cases were source-reviewed, not run as an automated or live custom-timeline matrix.
- Seven single-sample RGBA16F targets are retained. A separate MSAA scene target and matching D32 depth prefer 4 samples, 2 above 2560x1440, then 1 if format/depth/resolve support is absent. The radial pass samples the scene seven times rather than fifteen; bloom extracts highlights rather than the white field. History retention is 0.06..0.22 with frame-interval decay and current-neighborhood clamping. Discontinuity/seed/reduced/blue-boundary resets, edge resolve and single sRGB conversion remain.
- The five beats are sequential sensory indicators with LINK START, colored flight, pearl welcome UI, blue flight and a pale Connected confirmation. Quintic easing, overlaps and reading holds are retained. UI samples at 8/16ms have lower weights and appear only during motion; full-layer text shadows are disabled. The first background is opaque; zero custom prelude adds no startup card. Reduced motion remains 450ms without motion samples; public enums and bootstrap contracts are unchanged.
- The shared procedural fisheye service renders an 85%-resolution GPU field and then applies lens distortion, blur and chromatic offsets. It includes frost noise, five rays, three data-rain layers, four drifting segmented gauge rings, three scanning beams, grid cells, six motes, scanlines and vignette. One layer is reused across 500ms show/400ms hide transitions; reduced motion freezes procedural time and resolves opacity immediately. The existing live-image and CPU inspection paths remain separate. `sao_ui_fisheye_backdrop_advance` adds deterministic owner-thread stepping (0..1000ms) without changing existing structure sizes.
- Native menu keeps 70px slots, 54..70px circles and two-neighbor focus. Orbit arcs, selection trails/press pulses and a status marker strip accompany translucent menu surfaces. Root hover is 200ms in/out; child entry is 240ms with 28ms row stagger; the bounded existing 450ms root popup remains. Existing hit regions, callbacks, scrolling and high-contrast action colors are preserved.
- Native pointer and keyboard skip affordances are removed. Both intro layers retain full rectangular input coverage; Launcher and production Preview consume intro keys without dismissal. Public programmatic dismissal, window shutdown and error completion remain unchanged. Late ticks do not replay expired cues. Preview parent-window input still uses the compositor and screen-space wheel coordinates.
- Guide intro owns focus and makes existing background nodes inert through its exit, restores their prior state, and supports short keyboard/complete-pointer skip. The native completion fragment and existing session preference still suppress duplicate introductions.
- Zero-duration P1/P3 phases emit no particles; positive phases budget complete column travel before their boundary without changing public completion time. Guide pointer leave and lost capture still clear only the matching armed pointer.
- Default sound is one source voice with all three buffers queued before Start: LINK_START (44116 frames), NERVEGEAR (171990), ALO_WELCOME (246974), all PCM16/stereo/44100Hz. Only the last buffer carries END_OF_STREAM. SamplesPlayed drives visual time and the atomic OnStreamEnd callback confirms completion; callbacks do not take the playback mutex and voices are destroyed outside it. Four bounded sequence slots are pruned on the worker's 50ms wake-up.
- No additional Welcome plays after completion. The experimental handoff cue, Preview tail delay and fourth muxed cue were removed at the user's request; intro and Preview audio behavior return to the existing three-cue contract.
- Default boundaries: voice/first flight1.000363s; Online/title4.900363s; blue6.655363s; title end7.150363s (card exit through7.330363s); Connected entry10.100680s; audio end10.500680s; hold end11.400680s; completion12.150680s. The interface frame starts preparing0.20s before Online, but title text never appears early. Silent/interrupted/750ms stalled fallback, explicit custom timing/natural audio tails, cancellation/error/destruction/replay and reduced-motion suppression remain.
- Bootstrap/hold integration was added concurrently in session-31 and is outside this visual/audio slice's changes and verification; the separate section below describes that work. Offline frame export runs without backend activation or telemetry.

## Source coverage and remaining acceptance
| Surface | Source change | Visual/runtime acceptance |
| --- | --- | --- |
| Entity root/child menu | GPU fisheye backdrop, segmented arcs/pulses, translucent surfaces, shared label paint/hit geometry, contained child rows and reference hover/child timing | Fullscreen backdrop and all five readable menu rows verified in native export; full DPI/data/interaction matrix remains open |
| Launcher settings | Fixed header/footer, independent category/content scroll, checkbox, volume slider, theme dropdown, 15-cue audition | Production binding compiled; full backend session pending |
| Hotkeys / plugins / license / user menu | Classic tokens, fixed status and scrollable content, shorter labels | Production binding compiled; real data/empty/error/manual gates pending |
| Workshop | Catalog connectivity is separate from owner/worker lifetime; stale catalog actions are disabled when the latest validated list is disconnected | Detached production Preview observed `目录未连接`, structured list failure and normal retry/empty layout; connected backend still pending |
| Process selector | Full process snapshot with 32-row materialization pages; core enumeration remains available without RT I/O while attach is explicitly disabled | Production Preview observed 1–32 of 357, Page 1/12, `Attach off / 未连接`, disabled attach actions and clean close |
| AI settings | Scope rail, actual text search, independent scroll, bottom save area, checkbox/dropdown fields | Native offline renderer inspected at 1264×820; initial Dock/resize/TextField dispatch defects found and corrected; input frame visually rechecked; full editing/scroll acceptance remains open |
| AI main / Control Center | Full Workbench asset hosted by CompositionController; native panel retained as fallback | Debug production Preview created the AIWorkbench WebView2 process tree and completed hello/ready; adapter slices and full visual/input/backend acceptance remain open |
| Embedded AI pages | Classic light/default and neutral-dark CSS | Source review; hosted WebView runtime pending |
| User guide | Classic CSS, shorter interactions, native sound bridge and real WAV copies | Browser inspected at 1280×900 and 390×844, no horizontal overflow or broken images |
| Link Start | Center-born 720-column flights, 48-sided mesh, complete exit instead of visibility toggles, slow-fast-slow travel, stable highlights and unchanged three-cue audio | Review's custom-prelude start defect fixed by parent and read back; four shaders /WX and Debug UI/Preview rebuilt;22stills and780-frame/60fps/13s movie regenerated and decoded, audio=0/status=0 with natural exit. Three180-frame render/readback runs approximately1.03s, not presentationFPS. Custom boundaries are source-reviewed; appearance/DPI/device/acoustic acceptance remains open |

## Manual gates (not claimed as passed)
- Five-beat code review passed after parent repaired short-hold opacity: entry ends by the hold boundary, overall entrance ends by its absolute timestamp, old samples stop after entry, active hold forces opacity1, and parking rounds upward to a representable millisecond. Zero/100ms/submillisecond/early holds were statically reviewed, not claimed as a live runtime matrix; default timing remains unchanged. The previous review/persistence blocker is resolved.
- Current full preview is `.sao/ui-preview/linkstart-center-flight.mp4` (SHA256 `541f207f92592d56f4b0f980a27d367b06e271405aa4a68176da22fd7609677b`); first-generation/flow/five-stage/upgraded/reference movies are historical. Current Debug UI SHA256 is `6a52f9a490b1c4ea1efe1118c52cd1d2a42c84c88c08e281929738c12f33a63a`.
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
- Preview owns the shared fisheye backdrop, resized to its client rectangle and shown behind a visible root/child menu; hide and retryable teardown use the same service. Other standalone Preview pages do not force it on. `--intro-audition` enables offline audio at volume 55 without global hotkeys, suppresses the initial menu-open cue, logs phases and closes on natural completion.
- `--offline --intro --frame-ms 0 --frame-out - --frame-count 780` streams13s of concatenated BMP from one renderer at deterministic60fps (16/17ms steps); max1800frames and endpoint<=60000ms. Tick stops after intro completion while subsequent menu frames remain exportable; single-file BMP stays unchanged. Exported60fps is not a measured live rendering-FPS claim.
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

The 11:37 UTC pale reconstruction was visually rejected despite successful compilation.
The subsequent actual-GL rework built Debug UI/Preview at12:39:01/12:39:07 UTC and exported
15 still times through11700ms plus menu. One native renderer then generated720 consecutive
frames (12s/60fps); the frozen Python class generated644frames using its original GL shaders,
without launching its application/backend or modifying legacy files. The audio run logged
first_flight1016/text4907/post-title6907/connected10063/complete11641ms, always audio0/status0.
Full source review passed with no defects; final low-emission/edge shader polish is undergoing
incremental verification. Both MP4 previews live under.sao/ui-preview; renderer-frame exports
do not measure speaker acoustics or live frame-time performance. Release/Hardened unchanged;
no new audio resource or Markdown file, no production startup execution.

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
