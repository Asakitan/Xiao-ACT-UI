# Classic SAO implementation and acceptance

## Three readability recommendations — September 22, 2026

Typed Label now shares actual advance/height/baseline between wrapping, ellipsis,
alignment and rendering, including custom tracking and bold. Child keyboard focus
supports disabled skipping, scrolling beyond8rows, real Enter/Space actions and
Left/Escape return-before-close; publication/cancel invalidate stale focus. Plugin
status is separate from identity using UI ABI1.20's reserved-byte state without
changing the56-byte stride. The visible heading expresses plugin ownership only.

Final Debug launcher/Preview and the54-check/5-action offline probe passed with
ABI65556. Label size16/28 measures174x22/304x38; mixed normal/tracked rows both use
27px height after baseline alignment. Actual labels and menu/tab snapshots are
under `.sao/ui-preview/readability-final*`; no visible overlap or clipping was found
in the inspected samples. Existing plugin17/menu18-event, dark and handoff exports
also passed. Following two interrupted independent attempts, the user explicitly
requested parent review. Parent review completed without a new blocking finding;
full Debug and54-check/plugin17/menu18/dark/handoff reruns passed using the current
disk revision (`readability-reviewed*`). Hardened acceptance passed22PE/76files/17inputs;
ship now contains the32,426,064-byte bundle with matching build/ship hashes.
Full-product focus/all-DPI, complex grapheme clusters and performance remain unverified.

## Readability follow-up — September 22, 2026

Latin glyphs use112% sizing and0.025em native tracking, applied before DirectWrite
measurement and drawing in all three native text backends. CJK size, font files,
existing motion and public ABI remain unchanged. Plugin tab text and table text now
reuse measured codepoint-safe ellipsis; table alignment uses actual advance widths.
Workbench/embedded tools/guide use112% for the Latin font face only; the workbench
welcome heading no longer uses negative letter spacing.

Debug rebuilt after the long-text changes; plugin17-event, menu18-event and natural
handoff probes passed. Seven initial font captures and five final captures exited0;
the root-menu before/after comparison found no new clipping/overlap. Workbench font
loading reports Latin112%/CJK100%, and three browser viewports have no page overflow.
Final review passed after a documentation-only fix to the missing runtime-memory
UI typography section; no source changes or build/product reruns were made. Saved
logs and selected images were checked; Debug exit and browser results remain
parent-reported. Subsequent parent Hardened acceptance passed22PE/76files/17inputs;
the32,415,312-byte bundle and four CSS assets match the ship copies at that earlier
font-only checkpoint. Label metrics, child keyboard and plugin status are now
implemented above; static backdrop scheduling remains a profiling opportunity.

## Geometric entrance and exit — September 21, 2026

Entity retains the 450/300ms reversible state machine but uses at most 88px of slide
combined with staggered full-strength scan assembly/retraction. Header, root rows,
footer and children are geometrically clipped; cyan/gold cut edges and small moving
fragments mark the front. Root and close-button hit rectangles share the reveal geometry.
Reduced-motion/high-contrast behavior and public ABI are preserved. The offline
`--menu-motion` timeline now has 18 events. Escape closes the menu at 6400ms;
Home at 7000/7600/8400ms opens/closes/opens it, covering complete exit and reentry.
The background uses a 500/400ms horizontal seam and segmented beveled aperture;
opacity stays 0.93 inside its geometric coverage. LIVE waits for a first frame and
retains the final frame through exit; a separate texture/pass samples the already-warped
frame. Capture extents are host pixels, not rescaled DIPs. Constants remain 80 bytes.

Independent Review passed after zero-area/empty-state clips, reversal tails, hit bounds
and duplicate capture-DPI scaling were repaired. Release build and release acceptance
passed, with 22 PEs/76 exact files and a 31,066,192-byte runtime bundle. Final shipped-input
GPU preview exported 540 frames/9 seconds, 18 events, Settings round-trip and both themes;
the contact sheet was inspected. Open background pixels retain alpha 237; completed
close samples retain only the NerveGear control, with no background material pixels.
Video: `sao_auto/.sao/ui-preview/geometric-20260921/menu-geometric.mp4`, SHA256
`4bc1b10239b36f68b5ffaf533ac69df55347116a8b62be53bc7e5b160f819108`.
An earlier hostless run was all-transparent because no D3D device is created without
a host; it is excluded from visual evidence. User-approved normal product closure
released the preview single-instance lock. LIVE capture, device loss and all-DPI
accessibility remain separate gates. Older fade/timing descriptions below are historical.

## SaoMenu motion — September 19, 2026

Root 450/300ms opening/closing preserves positions on reversal. First child opening
is 480ms; a full switch uses 120ms join, 180ms move and 180ms split, shortened from
partial states. Latest selection retargets immediately; moving rows remain noninteractive
until settled. Entity consumes resolved anchor/extension/split rather than deriving a
second trajectory. Continuous NerveGear/icon feedback, thin card edges, readable secondary
labels and short hover glints preserve the neutral SAO style.

Private compositor-keyed numeric scene values coordinate the foreground and backdrop
1000ms theme transition, 48-column front and menu-local quiet region. The shader buffer
is 80 bytes, without public ABI changes or another blur pass. Generic panels opened by
synchronous menu actions use bounded 220/160ms connected motion; logical hide and input
withdrawal remain immediate. Owner ticks run outside Entity's mutex. Fresh scope tokens
and stable root IDs gate focus return; async/WebView surfaces do not inherit stale origins.

Preview `--offline --page root --menu-motion --frame-out PATH --frame-ms N` exports
interaction points, including quick retargets, theme reversal and Settings round-trip;
streamed export uses `--frame-out - --frame-count N`; explicit backend options are rejected.
Initial Debug UI/Preview build and 420-frame export passed before final refinements.
Static review repaired child-page identity across reversal/reorder, independent root
opening during selection, root-trajectory rollback, navigation scope invalidation,
theme-frame geometry/time cache inputs, high-contrast short-circuiting, same-thread
panel retirement and SDK-opacity multiplication. The revised timeline uses compositor
mouse routing and Settings titlebar close with logical-visibility checks.
Final Debug UI/Preview rebuild passed. Revised compositor-input export/encoder both exit0:
15 events,420frames,7.000s,1264x822; Settings titlebar logical close and navigation return
confirmed in the log. Final light/dark captures were inspected; the quiet region attenuates
detail without a bright plate. Runtime memory is synchronized. Video:
`sao_auto/.sao/ui-preview/menu-polished-20260919.mp4` (workspace-relative), SHA256
`d00e300ce3342baa46dd26116c5edf5ad2d0f6e9337b42fdcd360033f8bbeaef`.
Full DPI/accessibility/retirement fault injection remains source-reviewed only.
Historical timings below are superseded by this section for the modified menu paths.

## Process monitoring and Preview shutdown continuation

The version-1 panel specification accepts a noninteractive `sparkline` leaf with
at most 120 finite `values` and a finite numeric span. Creation, responsive rebuild,
updates and rollback use typed chart operations; pointer interaction bypasses the
generic widget state APIs, and the painter resolves the current theme. Legacy
unversioned chart nodes are rejected rather than silently normalized away.
The process page publishes CPU/memory curves from up to 60 real samples, plus CPU,
working set, threads, architecture and system summary values, with twelve rows per
page. Missing/first samples and interrupted intervals are not replaced by zeros.
Preview exits outside message dispatch, cancels queued refresh once, suppresses
late input and preserves quit codes. Debug/RelWithDebInfo compile; the six-case
Debug window probe passed, including populated charts, resize, synchronous close
and nonzero quit. The reported original hang was not reproduced. The packaging
statements below are the pre-continuation ship snapshot; no ship was refreshed.

Current packaging authority (September 13): RelWithDebInfo and Hardened fresh
acceptance each contains 22 directly loadable PEs and 76 exact manifest files plus
the authenticated runtime bundle. The refreshed Debug ship has the same 22-PE/
76-file surface. All three current trees have zero plaintext first-party DLLs,
opaque helper DLLs, transaction locks, and LocalAppData generations after probes.
The logical `sao_platform_ui` target and its ABI are unchanged, but its physical
leaf is `ed0fa2d377a29333.dll` inside `runtime/ff22701a59858ebf`; no plaintext
first-party UI DLL is shipped. Complete Debug and both release acceptance targets
pass. All 38-PE/91-file and named-UI-DLL timestamps below are explicitly
pre-bootstrap UI evidence, not the current artifact inventory.

The current Debug, RelWithDebInfo, and Hardened build-tree DLLs export UI ABI 1.17
(`65553`) and AI Editor ABI 1.2 (`65538`); each ship carries those inputs only in
the authenticated bundle. UI minor 16 append-only adds tracked DC-mutation
submit/wait/destroy while retaining the minor-15 Link End surface and all prior
structure sizes. UI minor 17 append-only adds `sao_ui_panel_find_widget`
(spec-node id → live widget handle) plus the `canvas` spec leaf wired to the
scriptable canvas; the leaf is non-interactive like `sparkline` (no hit-test,
no interaction-state migration, no `set_enabled`). Tracked submissions use
unique internal sequence keys and bypass ordinary coalescing so each wait
observes its own executor result.

The developer `sao_ui_preview` target now participates in the default build.
This prevents a stale preview executable from retaining descriptive pre-rename
DLL imports after the first-party outputs move to opaque leaves. The repaired
Debug executable imports the current opaque AI/license/SDK/UI DLL names and a
real launch reached input-idle with a nonzero top-level window handle.

## Current reconstruction status

Panel movement/resize now compensates current versus initial panel origin before applying the
pointer displacement. Only geometry modes1/2 use the host frame; widget modes3/4/5 remain local.
An old-DLL native routing probe reproduced4wrong positions in8stationary events. Debug and
shipped Release each pass203geometry observations through production compositor mouse dispatch:
stationary/forward/reverse/negative positions, release/cancel and five edge/corner resizes.
Source Review passes; ABI and existing size constraints are unchanged. This is a windowless
native input/geometry inspection, not desktop frame-time or multi-DPI evidence. That inspection's
pre-bootstrap Debug/Release build and resource hashes pass; the current encrypted Release/Hardened
surface and opaque UI leaf are recorded above. Earlier module times below describe their original
stages, not the current package.

September 12 delivered layering: the root DComp rebuild appends ascending
BELOW_NATIVE/native/ABOVE_NATIVE visuals with `AddVisual(child, FALSE, nullptr)`;
attach, rebuild, rollback and recovery share that path. Existing `LayerLess` already orders
band/z/creation; native CPU/GPU painting traverses forward and input traverses backward.
Private `src/layer_order_internal.h` connects Entity to compositor-owned navigation state;
public ABI, signatures and layouts remain unchanged. Menu/NerveGear retain raised z
1500000000/1500000001 and rest at -10/-9, above the known fisheye at -50.
Only false-to-true visibility or a changed z on a visible, non-clickthrough, non-navigation
native layer at z>=0 yields both navigation layers under one compositor lock.
Home raises the pair: visible-behind stays open, visible-front closes, closed opens in front.
Repeated same visibility/z, rendering and ticks do not reclaim foreground. Guide remains modal
ABOVE_NATIVE with background mouse/hotkeys blocked; native Home never crosses external bands.

SDK registration now assigns final class z and alpha before initial visibility. This closes the
temporary-z=0 bottom-HUD finding; focused follow-up Review returned pass with no additional edit.
Negative-z HUD and clickthrough decorations remain excluded. Runtime/SDK panels, popups, dialogs
and FilePicker use the inspected shared visibility/z paths; no public ABI or new sorting key.

Debug UI compilation and a native overlapping-coordinate probe pass: new popup wins, Home makes
the same coordinate route to Entity, reopening the popup wins again after service ticks, and Home
front-close/reopen remains functional. Guide HTML_READY compositor=1 and graceful exit0 pass.
Desktop captures did not contain the target UI; they were deleted and excluded from visual proof.
Full Docs pixels, multi-DPI and individual popup-type live coverage remain open.
The pre-bootstrap RelWithDebInfo acceptance and guide/font hashes remain valid UI evidence, but
their 38PE/91-file inventory and named DLL times are historical. Current RelWithDebInfo/Hardened
package acceptance is recorded above. Backend/production startup and full-DPI coverage remain
outside this UI slice.

### Previous UserMenu and guide review (pre-layering baseline)

Final incremental review, September 12: no additional source defect found in user_menu.cpp/.h
or the guide stylesheet; documentation corrections yield `passed-after-fix`, with no source
modification or rebuild required by this review. The previous guide modal review and its built
fixes are retained, not reopened. Earlier dated pending-build/review/no-release and dual-HTML
statements below describe their original stages, not a new AI-main run or current release state.

The actual UserMenu runtime panel uses `set_visible(false)` for repeated entry, forwards its
titlebar close event to `menuAction("close")`, and includes a separate Close Menu button.
Default size and positioning use400x440; failed initialization and destruction remove both
handlers, and visibility telemetry is Debug-only. The parent's native input probe reports
USER_MENU_X_CLOSE_OK, USER_MENU_BUTTON_CLOSE_OK, USER_MENU_REPEAT_ENTRY_CLOSE_OK and
USER_MENU_CLOSE_PROBE_OK backend=offline exit=0. Each real state query returns visible=0;
the panel is at70,0 with400x440 bounds. This is UserMenu evidence, not a Settings surrogate.

Guide CSS restores SC1, particles, glow, scanlines and physical ripples: body isolation,
cursor/ripple z6000/5500, glow z91 with18%/10% stops and scan opacity0.7. Exact-source headless
Edge inspection at1280x820 reports document.hidden=false, visible SC1, glow on, particles
opacity1, scan display:block/opacity0.7 and one ripple domain with domain/lighting opacity1,
normal blend, energy4.1788 and6mapFrames. The parent visually inspected guide-effects-browser.png.
HTML close and detail-Escape then guide-Escape pass through a recorded mock native bridge;
390px close hit/no overflow and reduced-motion suppression pass. The narrow probe initially
sampled an exiting detail barrier; waiting until hidden corrected only the probe. Native WebView
CDP37829 refused connection: no native effects screenshot is claimed, and the integrated browser
with document.hidden=true is excluded from effects-live evidence. Fault injection, multi-DPI
and native Tab wrap remain source-reviewed/open.

Historical parent verification covers Debug SaoAuto/Preview compilation, nine pages
(root/settings/hotkeys/plugins/workshop/process/license/user/files) with font-ready and exit0,
and native guide HTML_READY compositor=1/exit0. The latest combined export result contains only
guide readiness, not AI-main completion. RelWithDebInfo acceptance passed38PE/91exact files/
ship diagnostics absent; three binaries match bin/ship,18shipped guide files and8fonts match
source. Extra source audio copies are not all shipped. Release SaoAuto is2614784bytes at
2026-09-12T12:15:40Z; UI DLL is15621120bytes at12:15:28Z; full executable hash is in session-32.
Hardened was unchanged in that historical UI slice; it has since been refreshed by session-33.
These are explicit temporary UI/browser inspection probes, not unit/CTest
tests or new repository tests. This review only reads source and edits existing documentation;
no builds, tests, processes, production/backend startup, deployment or driver operations were run.

### Earlier UI stages (historical evidence)

The September12 click audit covered Entity/compositor coordinates, native panel/popups, HTML
composition and owner dispatch. Two confirmed fixes landed: Entity uses physical client pixels
without a second96/DPI conversion, and dropdown hit testing clears stale hover before checking
separator/disabled/outside rows. Existing keyboard/lease contracts remain; source review is
pass-with-notes. Debug nine-page/HTML/menu/36-frame/intro checks pass at96DPI; non96DPI and invalid
dropdown-row behavior are source-verified, not newly runtime-probed.
RelWithDebInfo Release is refreshed in build/windows-release/ship: acceptance exit0,38PEs,
91files, diagnostics absent, three main bin/ship hashes and four web/eight font hashes match.
Hardened remains at its previous baseline; no server deployment or production startup was run.
The no-refresh notes in the preview history below refer only to those earlier captures.

Scoped review findings were applied and read back by the parent: valid publication retains
child transitions and closing rows, unchanged children are skipped, and keyboard activation/close
synchronizes identity and input flags. The10:12Z UI rebuild and existing offline matrix pass.
Publication-during-motion and keyboard/publication combinations remain source-traced only;
older dated pending-review notes below are historical, not the current corrective status.

Initial expansion and submenu switching are distinct. Switching joins old rows into one full
mother bar for180ms, moves it to the new root with210ms quintic easing, then unfolds over270ms.
The bar never exits or regenerates. New selections during opening queue behind the active motion,
with the latest target winning; private snapshots preserve the source and queued state on rollback.
Expanded rows align with the selected root, clamping upward only to keep the visible column
inside local y40..418. Paint and input share the same column calculation; the connector follows.
The09:45Z build and full offline matrix pass, including shared width/source anchor/latest target,
Appearance top263/bottom354 and User top327/bottom418. Scoped review findings are corrected as above.

NerveGear uses a60px circular monochrome/graphite face, a complete arched headband with solid
visor and three menu dots instead of tiny labels. Hover scales the icon to1.04 with a0.5px lift;
press scales it to0.94 with a1px depression and restrained face tint. Disabled/high-contrast
palettes and separate linking/linked line indicators remain. Its72px canvas, hit geometry,
drag/toggle logic and state machine are unchanged. The visual borrows clear-icon, spacing and
press-state principles from Material FAB/Menu, Windows Buttons and Apple HIG Buttons, without
adopting their platform-specific sizing or changing this menu's behavior. Debug UI/Preview
compilation passed; no preview, image export or independent review was run, as requested.
Reference pages: `https://m3.material.io/components/floating-action-button/overview`,
`https://m3.material.io/components/fab-menu/overview`,
`https://learn.microsoft.com/en-us/windows/apps/design/controls/buttons`,
`https://developer.apple.com/design/human-interface-guidelines/buttons`.
Root navigation
cards are224x44 with8px corners, name/root-ID hierarchy, an independent ordinal chip and child
chevron. Icons/cards follow by6px/18px; opening uses18ms per-item delay, closing reverses at12ms.
The main menu slides with450ms cubic deceleration and300ms quadratic acceleration, not alpha fades.
An initial root expansion grows for90ms, generates one parent bar for150ms, holds60ms, then splits into
secondary rows for180ms. Secondary hit regions activate only after completion; reduced motion
skips the animated stages. Root/child motion uses opaque content and shared clipped hit geometry.
Owner-tick paint is coalesced; visibility input flags update immediately. A scoped suppression
flag prevents self-input callbacks from reentering a locked visual commit. Wheel/home/lifecycle
paths remain. Host-backed GPU composition uses the host dimensions, not moving-layer bounds;
headless composition keeps its previous extent rule. Menu shadow remains, backdrop blur is removed.
The09:15Z Debug/export/font/HTML/interaction/intro-order matrix passes, including black-white
switching and grow/generate/split/ready markers.36 native frames hold1264x821 with monotonically
moving opaque header pixels1256→842. This was not final desktop or full-DPI acceptance; the
then-pending combined review and later scoped corrections are recorded above. Earlier click timing
comparisons remain historical message timings.

Startup hides both Entity menu and NerveGear until Link Start completes, then opens the menu once.
Production preserves the guide's completion event and restores UI on render/device termination,
not offline/teardown/bootstrap failure. Preview opens directly without an intro and retains the
audition auto-exit. Default-timeline offline samples0/1800/10000ms contain no menu-open event;
11200ms contains exactly one natural-completion/menu-open event.
Root buttons now use layered metallic rims, inset faces, directional highlights and short status
arcs. Numbered cards and child icon wells share the material; faces are opaque, root pitch,
callbacks and fonts are unchanged. High contrast omits material overlays; bevel highlights are
static. The current screenshot shows no overlapping text or ring-clipped icons.

Menu actions toggle existing management panels; guide and AI panels expose visibility queries,
and a second AI launch request can suppress a pending show. Root and child menu headers have a
close button with matching paint/input/public hit geometry. Reopening during the close animation
clears stale child state. NerveGear uses a headset device tile, a DPI-scaled threshold and client-pixel drag
clamped to the host; release after drag does not open the menu. Position is session-local.

Only the Link Start GPU scene and fisheye backdrop layers use opacity0.93; the fisheye shader
retains its fade but no extra0.95 multiplier. Production and preview hide the shared fisheye while
the intro is active, avoiding nearly opaque stacking. Intro layers stay above menu utility layers;
foreground status/text/control/panel alpha and global compositor opacity are unchanged.
The09:15Z Debug build, nine exports, eight font hashes and both hosted HTML ready/exit checks pass.
The offline96-DPI probe passes menu/settings toggles, black-white, drag, root/child close and cancel.
Native BGRA exports report left-side background alpha237 for intro and fisheye. At1.8s the intro
contains no menu/opaque UI pixels; the root page has73243 opaque pixels. These are compositor
snapshots, not desktop captures. Full DPI, AI launch races, live fisheye input and subjective
acceptance remain open; this stage did not record an independent review.

The global typography rule is SAOUI.ttf for Latin/numbers and ZhuZiAYuanJWD.ttf for Chinese.
Both native DirectWrite paths share one private embedded collection, font mapping and measurement
format across Display/Body/Monospace roles. HTML editor, guide and built-in panels ship matching
files and range-limited font faces; a font-only virtual host supplies opaque embedded frames.
Shared neutral surfaces, soft badges, measured vertical text alignment and navigation states
replace the earlier flat chrome. Settings and Process receive new page organization; the menu
uses selected-state material instead of continuously rotating decoration. Other management pages
inherit shared styling, not a separate per-page layout rewrite in this continuation.
Debug build and nine native exports passed with both fonts ready; eight staged font hashes match.
Browser editor/guide font faces loaded, and hosted HTML readiness/normal exit passed. Process was
captured loading; populated rows, terminal proportional-cell alignment, missing glyphs, full DPI
and subjective acceptance remain open. Focused final review passed with notes after the parent
fixed form-control font inheritance and tiny navigation-marker bounds. The final 05:29Z Debug
rebuild and all nine exports passed again; all 326 editor form controls resolve to SAO Product.
Common extension-frame defaults are supplied without replacing extension CSP or icon fonts.

Session-32 makes the original HTML the editor surface, with its former native tools exposed
inside an HTML tab; the native main now displays only loading/error/retry. The guide uses an
owner-thread CompositionController on the same compositor, and the tray menu and asynchronous
file picker are native compositor panels. Launcher establishes STA before the first font-cache
initialization. Shared native headings use measured text ellipsis and clearer spacing; management
toolbars/status areas remain visible while content scrolls. Shared rounded controls now paint a
real outline instead of filling a transparent interior with the border color; native text wraps
within its allocated height and badges use constrained intrinsic width. Review findings were
applied by the parent. Final Debug compilation and staged-asset hashes passed; eight native pages
exported and exited with code 0, and both guide/editor reported HTML_READY compositor=1 and exited
normally. Full interaction, DPI/IME, backend and subjective visual acceptance remain open; legacy
synchronous dialog compatibility paths are not claimed to be fully migrated.

The September 12 continuation connects top-document script alert/confirm/prompt to an owned
compositor dialog and WebView2 deferral, cancelled on navigation/hide/failure/close. Handle-based
dialogs are explicitly advanced on the owner thread; only convenience one-shots auto-tick.
Runtime information/warning/error events use the existing HTML toast and notification history.
Legacy shim notifications require a registered host and otherwise return a structured error.
Launcher restart confirmation is asynchronous, defaults to Cancel, and invokes the unchanged
executor only after explicit approval is consumed by the owner tick. CLI help/version writes
console or redirected output; shutdown drains at most 64 queued messages per iteration.
Debug compilation, the eight-page exports and guide/editor readiness plus normal exit passed
again. New confirmation/input behavior is source-reviewed only; independent review and interactive
result delivery remain open.
Dedicated Kernel Map selection and legacy synchronous/developer windows remain unchanged.

The previous generic-card/AI-only Preview result was rejected as a complete visual rebuild.
The native GPU rendering foundations below are implemented, but full visual and functional
parity with the complete Python editor and the supplied SAO Utils references remains open.
The legacy `python/web/ai_editor_app.html` supplies the original editor structure through
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
- All text roles share DirectWrite measurement/paint and the two private embedded fonts; HTML uses the same asset bytes. Resource provenance is in `assets/fonts/SOURCE.md` and the existing asset manifests; no system font installation occurs.
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
- AI main uses an in-process WebView2 CompositionController in the `BELOW_NATIVE` band at the fixed `https://sao-workbench.local/ai_editor_app.html` origin. Navigation challenges still bind requests and events. Native panels, menus and file selection can render above HTML; editor window commands no longer manipulate the platform HWND. The old main body is only a loading/error/retry view, not a native workbench fallback.
- The full-page guide uses `ABOVE_NATIVE`, matching visual and mouse precedence. A full-client native input barrier at z=1700000000 remains active during loading/error/recovery, with the closable status panel above it; hide retires input and queued retry. Host keys and global shortcuts cannot act on underlying UI while the guide owns the screen. Both the HTML header and its intro expose close controls; nested detail/intro Escape takes precedence over closing the guide.
- Source review fixes defer guide retry until async creation and callbacks drain, preserving the error barrier and resize service meanwhile. Tab traversal wraps within WebView; presentation failure withdraws its input/visual slot, with permanent failure exposing Retry and transient failure retried by the owner tick. Bounds must succeed before dimensions are cached, and visibility transitions participate in callback lifetime protection. These fixes were read back and covered by the completed Debug and pre-bootstrap RelWithDebInfo build plus `HTML_READY compositor=1` evidence above; fault injection, multi-DPI, and native Tab-wrap acceptance remain open.
- UserMenu owns a runtime panel created with `sao_ui_panel_create`; repeated-click hide uses `sao_ui_panel_set_visible(false)`, not the distinct SDK registered-panel hide API. Existing Home/Entity navigation remains separate.
- The current first-generation-inspired direction replaces the previous dark neon field with luminous white space, solid colored cylinders, pearl UI and blue/white second flight. Depth lighting, shallow bevels, restrained highlights and distance fog remain; it is not a return to flat ribbons or a claim of frame-exact official footage.
- Each flight emits 384 finite columns using a 48-sided/four-band mesh (386 vertices, 2304 uint16 indices). The CPU publishes a linear phase clock; each column has its own birth time (`0.45 * birthFraction`) and 0.55-phase lifetime. Radius, radial position and depth travel use age squared, starting at the exact center with zero velocity and accelerating outward without a late slowdown or a fast fixed-radius emergence. Length grows by at most 16%; per-column travel includes the stretched tail and bevel exit margin. Geometry exits through ordinary frustum clipping rather than alpha/density gates.
- Stable material brightness and white depth fog remain. Zero-duration custom flight intervals emit nothing. Endpoint geometry is source-reviewed; it is not an all-resolution visual-parity claim.
- Explicit custom timelines start colored flight at their prelude, while the default prelude-relative clock starts at zero. A positive custom prelude therefore begins at progress zero rather than mid-flight; `prelude == p1_end` emits no colored columns. These boundary cases were source-reviewed, not run as an automated or live custom-timeline matrix.
- Scene alpha stores column coverage, not final opacity. A bounded forward-coverage probe applies medium body blur with a maximum0.36×speed mask inside columns while protecting leading areas without forward coverage. The probe extends at least1.5 vertical-resolution pixels and rejects out-of-viewport coordinates. Four positive-radial taps keep rearward trails; historyScale=1 prevents forward reprojection. The7.5ms/0.10 exposure, original shutter radius, bloom0.19/0.07, MSAA, final opacity/sRGB and112-byte constants remain unchanged.
- Sensory discs use pearl-white/silver glass matching the information panels, directionally lit bevels, inset silver tracks,24 fine ticks, moving glints and a subtle confirmation ripple; green is restricted to small status accents. The centered label type scale was doubled from0.17 to0.34 of the disc radius (clamps18/80). The white plate is now a snug translucent band behind the glyphs: width `max(textWidth*1.14+6, 0.5*radius)` capped at1.5 radius, height1.22*textHeight, plate alpha80%, shadow16% and top highlight80%; disc radius, rings, ticks, colors and motion are unchanged. Maximum active radius is38% of viewport height capped by28% width; dock radius38 and spacing86 enlarge both states. The0.30-phase motion, curved return and16ms staggered offscreen exits remain. Audio/onsets/10.500680s total and Connected layout are unchanged; global fonts remain outside this slice.
- The 1000ms vertical background reveal keeps 48 silver/cyan columns, dark downward and light upward. Each column maps global progress through its own start (0..0.24), finish (0.68..1) and acceleration exponent (1.10..2.30), so the actual color boundary advances in staggered streams rather than one shared front. Small time-noise offsets taper to zero at local endpoints. Long, faint filaments coexist with short packets; packet lengths vary per cell, spacing per column, and tail lengths span 0.035..0.36 before a 0.85..1.15 noise scale. Packet travel integrates a sinusoidal speed modulation: base speed 8..26 phase units/s with bounded ±30% variation, replacing elapsed-time multiplied by a changing speed. Existing snapping, reversal progress, 32-byte constants, 85% field resolution, visibility timing, opacity and live-image paths remain. Debug UI shader regeneration and relink returned 0; no preview, tests or repack were run.
- Lens compositing eases each coordinate toward an inset mapping in the outer 20% of its axis instead of allowing outward distortion to clamp into repeated edge texels. The inset includes both outer radial blur taps, RGB separation and half a source texel obtained from the field texture dimensions. The central region retains its original UVs; no field-target expansion, edge-cover overlay or menu modification is used. Debug UI compilation returned 0; the edge appearance has not been previewed and remains for user acceptance.
- Light/Dark action IDs take precedence over icon tokens in native root/child paint and software child paint, so old Home/Lock tokens no longer override Sun/Moon. Preview appearance rows and default children explicitly use `sao:sun`/`sao:moon`; existing SVG sources, stroke geometry and manifest hashes remain unchanged. Both light and dark selected root buttons omit the black drop shadow/recess/lower arc and white outer highlight; continuous selection-colored rims and restrained face highlights replace the mismatched ring. Unselected and high-contrast rendering remain unchanged. Debug UI compilation succeeded (DLL timestamp 2026-09-13 05:56:56Z); Preview source was synchronized but not rebuilt or run, and its previously reported settings-page declaration failure was not rechecked. No image export or release repack was performed.
- Entity foreground retains its 1000ms reveal and 12..160 horizontal strips across buttons, bars, icons, text and NerveGear. Per-row start/finish windows (0..0.24 / 0.68..1) and stable 1.10..2.30 exponents replace time-varying exponents: the main cut advances monotonically with progress rather than sliding backward when noise changes. Independently sized notches, main chips, echoes and 0..8px flecks keep evolving through 480ms random interpolation. All cut edges are integer-aligned, chips are bounded and each echo is followed by its own gap, keeping both palette masks complementary and fragments inside the endpoint margins. Clock sharing, transparent gaps, input, snapping, actual CLOSED gate, fonts, sound and public interfaces remain unchanged. Debug UI compilation returned 0; no preview, tests, export or release repack were run.
- Native menu keeps 70px slots, 54..70px circles and two-neighbor focus. Orbit arcs, selection trails/press pulses and a status marker strip accompany translucent menu surfaces. Root hover is 200ms in/out; child entry is 240ms with 28ms row stagger; the bounded existing 450ms root popup remains. Existing hit regions, callbacks, scrolling and high-contrast action colors are preserved.
- Native pointer and keyboard skip affordances are removed. Both intro layers retain full rectangular input coverage; Launcher and production Preview consume intro keys without dismissal. Public programmatic dismissal, window shutdown and error completion remain unchanged. Late ticks do not replay expired cues. Preview parent-window input still uses the compositor and screen-space wheel coordinates.
- Guide intro owns focus and makes existing background nodes inert through its exit, restores their prior state, and supports short keyboard/complete-pointer skip. The native completion fragment and existing session preference still suppress duplicate introductions.
- Zero-duration P1/P3 phases emit no particles; positive phases budget complete column travel before their boundary without changing public completion time. Guide pointer leave and lost capture still clear only the matching armed pointer.
- Default sound uses one source voice and three PCM16/stereo/44100Hz buffers: LINK_START (44116 frames), private NERVEGEAR/SENSORY premix (171990), ALO_WELCOME (246974). Resource616 supplies the premix only to default Link Start; public NerveGear611 and original assets remain unchanged. The premix is original audio at unity gain plus the sensory clip at 0.8 gain, starting101430frames into the flight buffer; no original ducking, pitch shift or speed change. Only the last buffer carries END_OF_STREAM; SamplesPlayed and existing group/worker cleanup remain authoritative.
- Sensory asset provenance: user-provided `sword_art_online_sao_link_start_short_custom_transition.webm`, audio3.45..7.00s; `atempo=2.0,atempo=1.109375` compresses only this new effect while preserving pitch. `loudnorm=I=-20:TP=-6:LRA=7`, resampling to44100Hz, padding/trimming to70560frames and12ms/40ms fades produce `Transition.SAO.Sensory.wav`. Overlay at2300ms with `volume=0.8` and `amix=duration=first:normalize=0` produces `Startup.SAO.NerveGear.Sensory.wav`; final SHA256=`84c09aeee1a9aafee09e3f5de64a015994d2c9853276bda626b71532f522860c`. The standalone clip is source material, not another playback buffer.
- No additional Welcome plays after completion. The experimental handoff cue, Preview tail delay and fourth muxed cue were removed at the user's request; intro and Preview audio behavior return to the existing three-cue contract.
- Default absolute boundaries: visual prelude/first flight0.800290s, first PCM end1.000363s, colored flight end3.420363s, sensory sound/left-right zoom3.300363..4.900363s within the first stage, right-hand dock/green/exit crossfade through5.300363s. Welcome4.900363..7.150363s (card through7.375363s), blue6.475363s, Connected8.300680s, hold9.700680s, audio/completion10.500680s remain unchanged. Calibration is default-only; custom timings and450ms reduced motion are preserved.
- Bootstrap/hold integration was added concurrently in session-31 and is outside this visual/audio slice's changes and verification; the separate section below describes that work. Offline frame export runs without backend activation or telemetry.

## Reusable native Link End outro

- UI minor 15 appended `sao_ui_linkstart_show_outro(handle)` and completion reason
	`SAO_UI_LINKSTART_COMPLETION_OUTRO = 8`; current ABI1.17 retains both, and config24/GPU Constants112/Instance52 bytes remain unchanged.
- The opaque handle owns the mode. Startup `show` restores startup mode and its existing
	NATURAL completion. Outro entry cancels the current startup playback/group, clears hold,
	telemetry and pending completion, and creates no sound group or playback.
- Normal duration is900ms, reduced motion180ms, advanced only by caller tick deltas with
	endpoint clamping. Enabling reduced motion during outro shortens its deadline to180ms;
	disabling it does not extend that run. Repeated entry while outro is active is a no-op.
- Existing phase queries report CONNECTED with outro progress, then COMPLETE after natural
	completion. Polling delivers OUTRO once; subsequent polls return NONE. Explicit dismissal
	keeps its existing reasons and does not accept OUTRO as a caller-supplied reason.
- Resize, dismiss and destroy retain existing resource ownership. Valid hold/telemetry/release
	calls are no-ops in outro mode, including failed-hold release. Entry/render/resize errors
	return status so the caller can continue shutdown; outro does not advance the startup NerveGear clock.
- The existing GPU renderer and recorded paint layer draw a pearl-white `LINK END` /
	`SYSTEM >> DISCONNECTED` panel, contracting calibration rings and96 restrained pale blue/white
	forward columns. Reduced motion freezes geometry and omits flight. Mode changes invalidate
	temporal history and select cached mode-specific instance colors; no new device, swapchain or asset.
- A shared quintic entrance/exit envelope fades the panel and final GPU output to zero;
	GPU layer alpha remains0.93 and scene alpha remains column coverage. Existing0.36 body blur,
	rearward trails, shaders, startup timing and0.8 sensory sound assets remain unchanged.
- Normal launcher quit now runs the outro before teardown, suppressing menu/guide and queued
	navigation work; failure/session shutdown bypasses it. The1500ms loop deadline is cooperative.
	Completion and cancellation hide the overlay host before background cleanup, avoiding panel flashback.
- Final Debug UI/SaoAuto/Preview and RelWithDebInfo release acceptance completed (38 PE audit).
	Offline `--outro` exported90frames at60fps/1280x832, with completion at900ms; live preview
	reported reason8/failed0 at922ms and exited normally. Conflicting intro/outro options were rejected,
	and the original660-frame startup movie was regenerated and decoded. Empty post-outro frames are
	synthesized only after successful completion; startup menu restoration stays suppressed.
- Feature review fixes were read back and rebuilt; the final host-hide delta review passed.
	Actual production quit, session shutdown, injected failures and reduced-motion runtime remain
	unmeasured. Exit preview `.sao/ui-preview/linkend.mp4` SHA256 is
	`8cb1e0773d591703946cb5a911fd672ef5ba54f8e6dcad4fcda4fb54f8fb3e54`.

## Source coverage and remaining acceptance
| Surface | Source change | Visual/runtime acceptance |
| --- | --- | --- |
| Entity root/child menu | GPU fisheye backdrop, segmented arcs/pulses, translucent surfaces, shared label paint/hit geometry, contained child rows and reference hover/child timing | Fullscreen backdrop and all five readable menu rows verified in native export; full DPI/data/interaction matrix remains open |
| Launcher settings | Fixed header/footer, independent category/content scroll, checkbox, volume slider, theme dropdown, 15-cue audition | Production binding compiled; full backend session pending |
| Hotkeys / plugins / license / user menu | Classic tokens, fixed status and scrollable content, shorter labels | Production binding compiled; real data/empty/error/manual gates pending |
| Workshop | Catalog connectivity is separate from owner/worker lifetime; stale catalog actions are disabled when the latest validated list is disconnected | Detached production Preview observed `目录未连接`, structured list failure and normal retry/empty layout; connected backend still pending |
| Process selector | Full process snapshot with 32-row materialization pages; core enumeration remains available without RT I/O while attach is explicitly disabled | Production Preview observed 1–32 of 357, Page 1/12, `Attach off / 未连接`, disabled attach actions and clean close |
| AI settings | Scope rail, actual text search, independent scroll, bottom save area, checkbox/dropdown fields | Native offline renderer inspected at 1264×820; initial Dock/resize/TextField dispatch defects found and corrected; input frame visually rechecked; full editing/scroll acceptance remains open |
| AI main / Control Center | Full original editor HTML, native tools tab, single-path HTML actions, script dialogs and runtime notifications; native loading/error/retry only | Session-32 Debug build and HTML_READY compositor=1 with normal exit verified; generic picker frame exported; full interaction/backend and new script-dialog result delivery still pending |
| Embedded AI pages | Classic light/default and neutral-dark CSS | Source review; hosted WebView runtime pending |
| User guide | Classic CSS, shorter interactions, native sound bridge and real WAV copies | Browser inspected at 1280×900 and 390×844, no horizontal overflow or broken images |
| Link Start | Medium column-body blur with forward-edge protection and rearward trails; enlarged pearl-white discs and unchanged audio | Debug UI/Preview, four shaders /WX,31 stills,660-frame/60fps/1280x832/11s decode and natural audition10.848s passed; audio=0/status=0. Three180-frame exports1.65/1.63/1.63s completed; not presentationFPS. Full custom/DPI/device/subjective acceptance remains open |

## Manual gates (not claimed as passed)
- Five-beat code review passed after parent repaired short-hold opacity: entry ends by the hold boundary, overall entrance ends by its absolute timestamp, old samples stop after entry, active hold forces opacity1, and parking rounds upward to a representable millisecond. Zero/100ms/submillisecond/early holds were statically reviewed, not claimed as a live runtime matrix; default timing remains unchanged. The previous review/persistence blocker is resolved.
- Startup regression preview after outro integration `.sao/ui-preview/linkstart-smooth-tunnel.mp4` SHA256=`5db4993aef3d4b4f4b599495f13ad50f7ffd49377345fa9a39a601584da039a0`; Debug UI SHA256=`0bb74464c239bdc6be0c40411c763433da345429d0331631fb0a9ad198168eac` (current encrypted-runtime build renames the module to a hash basename). The660-frame/11s video was regenerated and fully decoded after the label-size change; animation/audio remains10.500680s. Earlier preview hashes are historical.
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
Developer preview: default-build target `sao_ui_preview`; uses the real overlay host + DComp compositor, not a screenshot display loop.
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

## Presentation pacing, region ownership and z-order evidence (session 43)

Refresh pacing. `sao_ui_scheduler` detects the committed display mode via
`EnumDisplaySettingsW(ENUM_CURRENT_SETTINGS)` over every active display device and takes the
fastest rate (clamped 60-240 Hz), falling back to the primary-device mode query and then
`GetDeviceCaps(VREFRESH)`. The tick thread owns no HWND, so `WM_DISPLAYCHANGE` cannot reach it;
it re-probes every ~5 s and re-phases the deadline when the detected rate changes. The wait loop
coarse-sleeps until ~1.5 ms before the deadline and then spins with `yield`, so jitter rides the
spin window instead of a full timer quantum. `sao_ui_scheduler_set_refresh_rate(hz)` pins the
cadence (clamped 1-240, disables re-probe) or releases to auto (`hz <= 0`);
`sao_ui_scheduler_refresh_hz` reports the currently applied rate. A non-zero
`SaoSchedulerConfig::target_hz` counts as a pin and also disables the re-probe.

Single temporal-union owner. `apply_host_input_regions` gained a `kApplyRegionSkipPrevUnion`
flag routed to `sao::ui::overlay_host_detail::set_input_region_ex` (new internal seam in
`overlay_host_internal.h`; the public `sao_ui_overlay_host_set_input_region` keeps the documented
current-U-previous transaction). `sao_ui_compositor_sync_host_rgn` always passes the flag: when
`enable_temporal_union` is on the emitted spans already fold the previous frame, and when it is
off the path is meant to be pixel-exact — in both cases the host must not stack its own stored
`previous_input_rects` union. Legacy router paths (`reset_host_input`, the `legacy_tk` rebuild in
`input.cpp`) keep flag 0 and therefore the host-side union.

Idle-frame skip. `sao_ui_compositor_s` records `presented_bridge` plus `presented_extent_w/h`
(gpu_composition_extent_locked output) after every successful present, and every bridge teardown
clears `presented_bridge` to nullptr under `compositor->mtx` — an allocator-reused address can
never equal nullptr, so a recycled bridge cannot masquerade as presented (ABA-safe). A present is
skipped only when the live bridge still equals `presented_bridge`, no layer has `bgra_dirty`, no
visible non-proxy layer uses `d3d11_render_fn` (those are time-parameterized and must tick), and
the current extent equals the recorded one. The early return preserves the
`callback_failed ? UNKNOWN : OK` tail status. CPU, GPU and post-recovery success paths all record
the bridge/extent; the GPU path only records when a non-empty frame actually reached the bridge.

External-visual clip bounds. `apply_external_visual_clip_bounds` derives each wrapper clip as the
visual rect intersected with the bridge extent in wrapper-local coordinates (degenerate results
collapse to an empty rect) and is shared by `apply_external_visual_config` and
`sao_ui_dcomp_bridge_resize`, which re-derives every external visual's clip after
`ResizeBuffers` under a single `dc_dev->Commit()`, keeping the first failure for the return
status.

Z-order probes. `sao_ui_z_order_stale` now reports stale when the predecessor walk ends without
a predecessor — a host physically spliced out of the sibling chain by hide_z_order, or one
sitting at the chain top, both mean "target z-state unverifiable" rather than healthy, so
`enforce` re-asserts instead of quiet-quitting. The flattened OK from the unlink path no longer
erases evidence: `sao_ui_z_order_last_unlink_state` exposes NONE / NO_PROVIDER / CANCELLED /
SPLICED / FAILED, recorded under the manager lock by every `submit_z_order_unlink` call.
