# `platform/ui`

## Measured labels, keyboard children and plugin status

Typed Label and time-label wrapping, ellipsis, alignment and line height now use
real font metrics, including trailing-space advance. Default size is16; normal/bold
measurement and painting share a Body role. Explicit tracking uses measured glyph
advances and a common baseline; fallback geometry is only for unavailable DirectWrite.

Entity keyboard navigation has root/child focus, skips disabled rows and scrolls
past the eight visible child slots. Enter/Space use the shared pointer action path;
Left/Escape return to roots before full close. Publication changes and cancellation
retire old child focus. Production routing respects focused widgets/panels, guides,
modifiers and global Home/Insert; no motion timing changed.

UI ABI1.20 consumes the first reserved byte of the56-byte plugin-tab descriptor for
Unknown0/Active1/Disabled2. Remaining reserved bytes stay zero and all prefix offsets
and strides are preserved. Names remain unchanged; separate status text and a real
two-level plugin heading express state/ownership. Disabled tabs remain selectable,
while action admission stays with the publisher's existing gates.

Final Debug and the explicit offline readability probe passed54checks/5actions,
ABI65556; font16/28 samples measure174x22/304x38 and mixed tracking uses27px height.
After two interrupted independent attempts, the user explicitly requested parent
review instead. That review completed without a new blocking finding, followed by
a fresh full Debug build and54-check/plugin17/menu18/dark/handoff reruns, all exit0.
Hardened acceptance passed22PEs/76files/17inputs and refreshed ship; the32,426,064-byte
bundle has identical build/ship hashes. Evidence uses `readability-reviewed*`.
Full-product focus, all-DPI, host matrices, complex grapheme clusters and frame-cost
measurement remain separate checks; no independent-review verdict is claimed.

## Latin typography and long labels

The private DirectWrite layout applies 112% font size and 0.025em tracking to
Latin ranges only. CJK and other non-Latin sizes remain unchanged. Measurement,
software glyph masks, GPU text and Entity text share the same range formatting;
no font binaries or public ABI changed. Four packaged web stylesheets use a
Latin-only `size-adjust:112%`; their CJK face and explicit editor line height remain.
Plugin tab titles/rows and table text reuse the existing measured UTF-8 codepoint
ellipsis. Table alignment now uses the same width instead of a glyph-count estimate.
The measured-label follow-up above supersedes the older typed Label fixed-grid sizing.

Final Debug and offline plugin17/menu18-event sequences passed; seven initial
font exports and five final exports exited0. Menu before/after inspection found no
new text clipping or overlap. Workbench browser checks at1280x820,430x820,760x480
found no page-horizontal overflow; native backend calls remain offline in that check.
Independent final review passed after restoring the missing runtime-memory UI typography
section and synchronizing review status; no source fix was needed. Saved logs and selected
images were inspected; Debug exit and browser results remain parent-reported evidence.
The parent subsequently passed Hardened acceptance:22PEs/76files/17inputs,
32,415,312-byte bundle with matching build/ship hashes and four exact CSS copies.
All-DPI, complex grapheme clusters, real backend integration and measured frame-cost
checks remain separate.

## SYSTEM-to-menu transition

A successful release of a parked bootstrap hold consumes the next positive tick
as a resume frame and stops using the advanced audio clock for scene time. This
preserves the exit tail across synchronous launcher UI bring-up; repeated release
does not restart it. Early release, failed bootstrap and outro retain their paths.
Only a successful armed release with the final telemetry stage at100% enables
the default SYSTEM camera flyby. Its frame/text/ring/rail share age-squared depth,
perspective enlargement and a lens-side pass, with constant vertical center and
material opacity. Loading1/6 through6/6 remains unchanged; even100% waits for release.
The existing geometric backdrop aperture and natural-completion menu entrance remain.
Ordinary non-held intro, custom geometry, pillars and audio resources are unchanged;
reduced motion retains its250ms post-hold fade.
Offline `--intro-handoff` checks each loading stage and100% before release, repeated
release and a3s delayed first tick. Final Debug,11GPU keyframes,120-frame video and
reduced-motion samples pass. Camera-flyby review passed without findings; Hardened
acceptance passed22PE/76files with matching build/ship bundle hashes. Full production
bootstrap and all-DPI behavior remain separate checks; exact evidence is in session-78.

## Shared-texture source contract

UI ABI 1.19 adds size-versioned source/state structures, explicit legacy/NT handle
kinds, configurable keyed-mutex keys, bounded timeouts, and owner-thread attachment,
state and capability queries. Invalid replacement preserves the committed source;
zero/null attachment clears it. NT handles must already be valid in this process;
the consumer retains its own duplicate. Legacy shared handles are never closed.

The color path copies straight-alpha RGBA8/BGRA8 UNORM into a private GPU texture
and premultiplies in a shader before existing composition/effects. MMF remains an
independent alpha/hit-test and fallback source. Without MMF, irregular interactive
layers read back alpha only; color does not take a readback/upload route. A successful
copy activates the generation; timeout retains the last frame, abandonment requires
reattachment, and device loss retires imported/derived resources before recovery.
Device-less compositors report no GPU interop. Before the native timer-owner revision,
Debug builds and Hardened acceptance passed. The GPU probe passed 566 checks for
three sharing variants on two devices,
12 replacements and zero pixel mismatches, including timeout/recovery, rejected
replacement, clearing and retirement. Hardened acceptance passed 22 PEs/76 files;
the 17-event tab preview passed again. These are pre-timer baseline results, not
validation of the current launcher owner pump. Cross-process NT, cross-adapter, device-loss/
abandonment injection, MMF/alpha fault cases and all-DPI behavior remain untested.

The cutegirl pipe reader now publishes to a per-connection inbox consumed by Tick;
Stop clears the inbox and GPU handshake state. Tick itself previously ran through
the core timer worker, so that move alone did not establish compositor ownership.
The launcher owner bind/pump/unbind path passed independent review and final builds.
`sao_dir_probe --timer-owner-probe` passed 94 checks and seven native callbacks with
zero timer/worker/context residue, including headless layer replacement and input
reentry rejection; it does not exercise GPU or the full product. Timer callbacks may
retire their layers, while Input/Render reentry protections remain. Final GPU566,
navigation2446, six-host128-level and 17-event regressions all passed again, followed
by Hardened22PE/76-file acceptance. Exact artifact identity is in session-77.

Deep menus stay outside the UI's flat Entity ABI: hosts publish a current page,
with queued Back/open requests committed through navigation candidates. There is
no fixed depth cap, but limits remain 4096 nodes, 1 MiB text, 1024 root data rows
and 1023 child data rows plus Back. CPython/pymini materialize callable submenus
lazily along the selected path; other hosts currently execute subbuilders on refresh.

## Persistent plugin tabs and legacy drawing (earlier session-77 baseline)

UI ABI 1.18 adds a compositor-owned plugin tab rail with draggable title, scrolling,
UTF-8/custom semantic icons and a refreshable ancillary action column. Item data is
copied synchronously; selection follows stable plugin IDs. It remains visible when
the Entity menu closes, while the launcher suppresses it during intro/outro/offline.
Capture cancellation, viewport clamps and action-failure isolation are explicit.

Panel canvas nodes now submit real bounded draw operations; RGBA frames decode to
premultiplied BGRA bitmaps. Slider nodes honor their declared lo/hi ranges. SDK
overlays reuse the same parser and paint context without panel chrome, and failed
replacements retain the previous surface. Debug GPU preview completed 17 interaction
events, 67 canvas operations, a 16-byte exact RGBA comparison and real overlay set.
Post-review Debug rebuild and the same probes passed; Hardened acceptance passed
with 22 PEs and 76 exact files, and the authenticated shipped bundle was refreshed.
Full product/DPI/external-engine acceptance remains separate from this preview.

## Link Start flight and shutdown (current)

Default blue pillars now finish their full age-squared flight 300ms before the
bootstrap hold; that remaining window settles history continuously instead of
clearing visible trails on the cutoff frame. The one-shot flight, SYSTEM hold,
audio duration, custom timelines and public ABI remain unchanged.
Normal Link End keeps its 900ms scene, followed by a separate 650ms CRT shutdown:
vertical collapse to a horizontal line, horizontal collapse to a dot, then power-off.
Completion occurs at 1550ms, not at the end of the initial scene. Reduced motion
retains the 180ms fade. The shared GPU constants remain 112 bytes.
RelWithDebInfo compiler/linker and all four shaders passed; current GPU, independent
review and package evidence are tracked in the session-70 final follow-up.

## Link Start flight and loading hold (2026-09-21 historical baseline)

Blue pillars make one scene-clock flight from `p3_start` to `p4_hold_end`, then
remain off-screen. Only the central SYSTEM plate waits for bootstrap completion;
its loading ring keeps rotating. The early wall-clock-driven loop and private
`FrameState::motion_seconds` field are removed. Release timing, reduced motion,
custom timelines and the public ABI are unchanged. See the session-70 follow-up
for verification; earlier looping-flight evidence does not cover this revision.
Independent review found that the first cutoff frame could retain old trails and
then be cached indefinitely by `same_frame` during the hold. The default intro
now uses zero history weight from the blue-flight cutoff onward; custom timelines
and outro retain their existing history behavior. Post-repair RelWithDebInfo acceptance
passed (22 PEs/76 files), and GPU exports at 7500/8700/9701/9750ms succeeded.
The inspected 8700ms frame shows blue pillars passing the SYSTEM plate; the 9701ms
frame shows the plate without pillars. Build/ship bundle hashes match. Independent
review passed after the history fix; long bootstrap-hold live validation has not run.

## Geometric menu transitions (2026-09-21)

SAOmenu entrance/exit now combines an 88px bounded slide with staggered geometric
assembly/retraction and cyan/gold scan edges. Material opacity stays constant; input
rectangles follow the same clip as the visible controls. Root reversal and child
join/move/split remain intact. Procedural and LIVE backgrounds share a 500/400ms
horizontal-seam/beveled segmented aperture at fixed 0.93 material opacity. LIVE keeps
its last frame through exit and uses a separate texture/pass without double warping;
capture uses host pixels plus desktop origin, without a second DPI scale. Constants
remain 80 bytes. Review passed after clip/input/DPI fixes; Release build and ship
acceptance passed (22 PEs/76 files). Final GPU export is 540 frames/9s with all 18 events,
including full close/reopen, and Settings round-trip. LIVE/DPI/device recovery remain
separate live gates; see `docs/classic_implementation.md` and session-69.

## SaoMenu motion revision (2026-09-19)

Entity consumes private resolved child anchor/extension/split values. Root reversals
preserve current progress; rapid child selection redirects the current trajectory.
NerveGear feedback is continuous; opaque neutral cards retain thin material edges,
icon micro-motion and short hover glints. A value-only compositor-keyed scene shares
the owner clock, 1000ms theme progress and host-normalized 48-column front between
foreground and background; a feathered quiet region reduces detail behind the menu.
The private GPU constant buffer is 80 bytes; public ABI is unchanged.

Synchronous menu actions scope generic-panel origins: 220/160ms reveal/close with
at most 20px translation, immediate logical hide/input withdrawal, owner-thread ticks
and stable-ID/token-gated return focus. Deferred/WebView surfaces retain their original
presentation. Reduced motion/high contrast bypass connected movement and settle theme
changes. Preview `--menu-motion` is an offline-only frame-export interaction timeline.
Initial Debug UI/Preview build and 420-frame export passed before final refinements.
Static review fixes preserve child-page IDs across reversal/reorder, root rollback
trajectories, navigation-token invalidation, theme-frame geometry/time, high-contrast
short-circuiting, same-thread panel retirement and SDK-owned opacity multiplication.
Preview rejects explicit backend options and routes Settings titlebar close through
compositor input. Final Debug rebuild and revised 420-frame/7s export both passed;
all 15 events completed, Settings titlebar close withdrew logical visibility and returned
navigation to front. Final light/dark captures were inspected and runtime memory synchronized.
The full DPI/accessibility/retirement fault matrix remains source-reviewed, not runtime-proven.
Older timing/layout descriptions below retain their original historical context.

The native overlay stack — 1:1 port of Python `sao_auto/python/render/`
and `sao_auto/python/ui_gpu/`.  One HWND, virtual layer compositor,
Direct2D widget kit, GPU pop-up, panel + layout engine, input router,
z-order state machine, off-thread render worker, and integration point
with `security/anti_screencap/` for capture-mode toggling.

## Corresponding Python source

| C++ header       | Python source                                        | Key concern |
| ---------------- | ---------------------------------------------------- | ----------- |
| `overlay_host.h` | `render/overlay_host.py` (1124 lines)                | single HWND (hRender + hControl decoy) |
| `compositor.h`   | `render/overlay_compositor.py` (3460 lines) + `overlay_adapter.py` | virtual layers |
| `dcomp_bridge.h` | `render/dcomp_bridge.py` (1268 lines)                | DirectComposition + DXGI + WGL interop |
| `d3d11_device.h` | (implicit) parts of `render/gpu_renderer.py`         | shared D3D device |
| `scheduler.h`    | `render/overlay_scheduler.py` (395 lines)            | 60-240Hz frame pacer, timeBeginPeriod(1) |
| `render_worker.h`| `render/overlay_render_worker.py` (758 lines)        | off-thread compose lanes |
| `capture_sync.h` | `render/render_capture_sync.py`                      | PrintWindow / recognition sync |
| `subpixel.h`     | `render/overlay_subpixel.py` (120 lines)             | fractional-offset compose |
| `dc_mutation.h`  | `render/dc_mutation_coordinator.py` (399 lines)      | kernel-side tagWND write coordination |
| `dxgi_dup.h`     | `render/dxgi_duplication.py` (580 lines)             | WDA-aware desktop capture |
| `gpu_capture.h`  | `render/gpu_capture.py` (578 lines)                  | Windows.Graphics.Capture (async) |
| `gpu_overlay_window.h` | `render/gpu_overlay_window.py` (748 lines)     | legacy compat shim |
| `adapter.h`      | `render/overlay_adapter.py` (317 lines)              | legacy compat shim |
| `d2d_widgets.h`  | `sao_gui_*`, `sao_panel_components.py`, `gui_modules/*` | Direct2D widget kit |
| `theme.h`        | `sao_theme/colors.py` + `sao_theme/theme_manager.py` | **flat-token static color table (3 themes)** |
| `menu.h`         | `gui_modules/sao_gui_menu_hud.py` + `sao_theme/menu_bar.py` + `ui_gpu/popup.py` | **SAO ring/strip menu (GPU compose)** |
| `nervegear.h`    | `gui_modules/sao_gui_nervegear_button.py` + `sao_theme/link_start.py` | **NerveGear button + Link Start intro** |
| `popup.h`        | `ui_gpu/popup.py` (1507 lines, authoritative)        | **GPU transient popup (right-click, context, dropdown)** |
| `dialog.h`       | `sao_theme/dialogs.py`                               | **Alert/Info/Warning/Error/Ask/Input modal** |
| `fisheye.h`      | `gui_modules/sao_gui_fisheye_mixin.py` + `sao_theme/menu_bar.py` fisheye | **hover magnification math** |
| `animator.h`     | `sao_theme/animator.py` + `sao_theme/utils.py`       | **curve library + 60Hz scheduler** |
| `panel.h`        | `sao_panel_ui.py`, `sao_gui/panel.py`                | plugin panel container + SDK descriptor path |
| `input.h`        | `sao_gui_hotkey.py`, `render/overlay_host.py` (input proxy) | legacy input router |
| `z_order.h`      | `render/overlay_host.py` (`_enforce_z_order`)        | TOPMOST state machine |
| **`widget_kit.h`** (umbrella) + `widget_text.h` / `widget_input.h` / `widget_container.h` / `widget_data.h` / `widget_table.h` / `widget_chart.h` | `gui_modules/sao_panel_components.py`, `sao_theme/hp_bar.py`, `sao_theme/child_bar.py`, `sao_theme/widgets.py` | **game-agnostic widget collection (40+ types) — text / input / container / data / table / chart** |
| **`panel_sdk.h`** | `plugin_manager.register_panel()` + `settings.panel_themes[<panel>]` | **modern descriptor path: SaoPanelDescriptor + body handle + batched mutations** |
| **`panel_layout.h`** | Tk `pack`/`place`/`grid` + web CSS grid/flex duals | **measure/arrange/hit-test engine, 6 layout modes** |
| **`render_hook.h`** (UI-side) | `act_platform/render_hooks.py` + `render/overlay_compositor.py` per-tick callbacks | **frame-clock hook points (before/after compositor+present, per-layer paint)** |
| **`input_router.h`** (deep) | `sao_gui_hotkey.py` (subset matcher + most-specific-wins) + `sao_panel_components.attach_tooltip/_bind_hover` + `render/overlay_host.py` (focus shield) | **typed event routing, focus stack, modal barriers, hotkey binding** |
| **`sao_ui_scriptable_canvas.h`** | `act_platform/ui_spec.py::_normalize_canvas`, `sao_plugin_ui_render.py` (Tk dual) + `web/plugin_layer.js` (Web dual), `plugins/candy_demo/candy_render.py`, `plugins/midi_piano_plugin/*` | **scriptable canvas ops for Lua/Emma/AS/C# — line/rect/oval/polygon/text/bitmap + state stack** |

## Non-negotiable design constraints

Every one of these comes from a memory note about a real regression in
the Python overlay.

1. **One HWND.**  `overlay_host` creates exactly one Win32 window.
   Every visible plugin/pet/mmf/panel is a *virtual layer* inside the
   compositor.  This is the "unified DWM overlay compositor" work.
2. **SetWindowRgn owns click-through.**  It's the only cross-process-
  reliable mechanism. Creation does not pre-install an empty region; before the
  first successful transaction there is no installed region, and `overlay_host`
  records that null preimage explicitly,
  installs the compositor region on success, and restores null on rollback.
  Once Link Start has presented, the live host region must be nonempty. (See
  the `SetWindowRgn 裸调` handle-leak note.)
3. **`z_order` is the sole `SetWindowPos` authority.**  No other module
   changes topmost state directly.
4. **Layer name reuse fails loudly.**  Reuse used to leak GL FBOs in the
   Python compositor.
5. **DisplayAffinity is opt-in.**  We only set it in streaming mode
   because some games' anti-cheat trips on it.  See "Anti-screencap
   integration" below.
6. **Theme tables are `constexpr` static.**  `theme.h` exposes the
   colour table as a compile-time-constant `SaoUiColorTable`; every
   token lookup is a single array load.  Any runtime mutation goes
   through `sao_ui_theme_register_panel_override`.
7. **Popup / menu layout constants stay compile-time.**  Memory note
   [连续反馈不对要停止调参数] documents the class of bug where
   `content_w/h` is cached but `left/top` is read live — the child bar
   slide-in de-syncs the HUD backdrop.  `menu.h` and `popup.h` both
   expose the layout consts via `_layout_constants()` so consumers
   pin them explicitly per frame.
8. **Fisheye hit boxes grow with the sprite.**  `sao_ui_fisheye_hit_test`
   walks each button's *current* size, not the slot.  The Python
   version does the same via `SAOCircleButton._size` (float).  Fixing
   the SAO "why is my click landing wrong" bug for good.
9. **Animator is scheduler-driven.**  `sao_ui_animator_tick` is called
   by the shared 60Hz overlay pump — never spawn per-animation threads.
   Mirrors `sao_theme/animator.py` using `after()` on the main Tk thread.
10. **NerveGear disc has its own two-tone palette.**  `nervegear.h` does
    NOT reuse `SaoUiThemeId` for the button — the disc uses a hand-tuned
    cyan/gold palette (`_DARK` / `_LIGHT` in the Python source) that
    doesn't map onto full-app themes.

## Anti-screencap integration

Reference implementation is `render/overlay_host.py::set_capture_mode`
which applies `WDA_EXCLUDEFROMCAPTURE` (or `WDA_MONITOR` fallback)
SYMMETRICALLY to both `hRender` and `hControl` with rollback on partial
failure.  The C++ port routes this through
`security/anti_screencap/window_affinity.h`:

- `sao_ui_overlay_host_set_capture_mode(handle, bool exclude)` in
  `overlay_host.h` — public API called from license / streaming policy.
- Delegates to
  `sao_security_anti_screencap_apply_to_overlay(pair, exclude)` in
  `security/anti_screencap/window_affinity.h` — implements the symmetric
  apply + rollback logic.
- Fallback selection (`WDA_EXCLUDEFROMCAPTURE` on Win10 20H1+ /
  `WDA_MONITOR` on older builds) is auto-detected via
  `sao_security_anti_screencap_supports_exclude_from_capture()`.

Do NOT call `SetWindowDisplayAffinity` directly from anywhere else in
`platform/ui/`.  The symmetric application + rollback is the ONLY
correct pattern per the Python handoff (asymmetric state exposes the
control decoy while the render side is excluded).

## Public headers

- `abi.h` — export macro + `sao_ui_abi_version()`.
- `overlay_host.h` — the single HWND.
- `compositor.h` — virtual layers + z-order + RGN accumulation.
- `dcomp_bridge.h` — DirectComposition / DXGI plumbing.
- `d3d11_device.h` — shared ID3D11Device + immediate context.
- `d2d_widgets.h` — Direct2D widget kit.
- **`theme.h`** — static color table (dark/light/glass) + metrics
  + panel overrides + change listener broadcast.
- **`menu.h`** — SAO main menu (vertical strip / ring / cascade).
  Wraps `popup.h` internally for the GPU compose lane.
- **`nervegear.h`** — 72×72 floating NerveGear disc, Link Start
  intro state machine, alpha-silhouette hit shape.
- **`popup.h`** — GPU-rendered transient menus (right-click, context,
  dropdown).  Sub-menus supported.  Sole production menu path.
- **`dialog.h`** — modal Alert/Confirm/Info/Input.
- **`fisheye.h`** — hover magnification math, ring/column layout
  helpers, hit-test against grown sprite bounds.
- **`animator.h`** — 10 curve types (linear/ease_in/ease_out/
  ease_in_out/ease_out_back_lite/ease_out_cubic/spring/bounce/step/
  cubic_bezier), 60Hz scheduler with dedup keys and completion
  callbacks.
- `panel.h` — plugin panel container.  Two register paths: legacy
  `SaoPanelConfig` + ui_spec JSON *and* new `SaoPanelDescriptor` +
  layout-tree body (SDK-facing).  Batched body mutations, theme
  override, geometry persist hook.  Minor 17 adds
  `sao_ui_panel_find_widget` (spec-node id → live widget handle)
  plus the `canvas` spec leaf wired to `sao_ui_scriptable_canvas.h`;
  the leaf is non-interactive like `sparkline`.
- `input.h` — legacy input router + hotkey manager + cursor override
  + focus shield (arm-once WM_MOUSEACTIVATE → MA_NOACTIVATE).
- `z_order.h` — TOPMOST / follow-game state machine.  Sole authority;
  see the a90adbf/c208715/6e1eb20 leak-pattern guard.
- **`scheduler.h`** — display-synced frame pacer (60-240 Hz), auto-
  detect via GetDeviceCaps(VREFRESH), timeBeginPeriod(1) engagement,
  wall-time pressure floor with combat/menu explicit signals.
- **`render_worker.h`** — off-thread frame composition; lane-affine
  workers for standalone GL contexts; ULW commit path.
- **`capture_sync.h`** — reference-counted capture section
  (begin/end/wait_until_idle) so recognition PrintWindow doesn't
  starve overlay commits.
- **`subpixel.h`** — fractional-offset alpha composite + progressive
  bar-width fade for smooth tweens under 1 px/frame animation.
- **`dc_mutation.h`** — kernel-side tagWND write coordinator with
  invalidation barrier + stale-failure identity check (HWND reuse).
- **`dxgi_dup.h`** — DXGI Desktop Duplication with WDA-aware
  substitute-frame handling; access-lost auto-recovery.
- **`gpu_capture.h`** — Windows.Graphics.Capture-backed async HWND
  grabber (WGC, Win10 1903+).  Legacy `PrintWindow` fallback.
- **`gpu_overlay_window.h`** — legacy `GpuOverlayWindow` compat shim
  (delegates to compositor); process-wide WGL serialize lock.
- **`adapter.h`** — legacy `CompositorOverlayWindow` /
  `CompositorBgraPresenter` compat shims.
- **`widget_kit.h`** — umbrella pulling in the five widget family
  headers.  Version metadata + shared event subscription + size hint
  query used by the layout engine.
- **`widget_text.h`** — label / rich text / editable field; separate
  clock / relative time / duration labels each locked to one input
  quantity (memory [ACT时间显示三类分开]).
- **`widget_input.h`** — button (with `active`/`set_active` +
  `SaoUiButtonColors` overrides), icon button, dropdown button
  (memory [ACT全面板UX批次38bafcf]), checkbox / radio group, slider.
- **`widget_container.h`** — rounded panel widget, scroll view
  (auto-hide thumb, kinetic flick), tab view (per-tab close),
  CSS-grid.
- **`widget_data.h`** — progress bar (flat / HP ramp / HP trail /
  segmented) matching hp_bar.py + BossHP trail, circular gauge,
  status badge, tooltip attach, more-indicator.
- **`widget_table.h`** — table (columns spec / batched row upsert /
  sort / filter / row highlight incl. MEM priority badge from memory
  [DPS Tk三条造行路径白名单]) + tree view (indent + connector lines,
  expand/collapse, multi-select).  Split from widget_data.h so both
  fit under the per-header line budget.
- **`widget_chart.h`** — time series chart with axis meta that
  declares fmt intent (clock/dur/rel — no guessing), bar chart with
  overlay segments (mem_priority), line chart with threshold rules.
- **`panel_layout.h`** — vertical / horizontal / grid / absolute /
  flex / dock modes.  Two-phase measure/arrange with per-node dirty
  flags + `sao_ui_layout_take_dirty_rects()` for the compositor.
  Hit test returns hit_widget + ancestor path.
- **`render_hook.h`** — 8 hook points around the compositor + present
  clock.  Per-layer / per-panel / per-widget filter.  Declarative
  overlay attach for the common "widget on top of frame" pattern.
- **`input_router.h`** — layered API: raw Win32 feed → typed event
  → route to hit widget.  Focus stack + modal barrier.  Hotkey
  subset matcher with `enforce_ctrl_prefix` guardrail for plugin
  hotkeys.  Mouse capture.
- **`sao_ui_scriptable_canvas.h`** — begin_draw / submit_ops /
  end_draw session.  17 op kinds (12 draws + 5 state).  Bitmap
  registry.  Fluent helpers (draw_line / rect / rounded_rect /
  polygon / text / bitmap).  Pointer callback for scripts that don't
  wire the router directly.

## Implementation map

- **Foundation surface** — headers + baseline implementations.
- **Overlay composition** — overlay_host + compositor + dcomp_bridge (the critical
  path); then d3d11_device.
- **Widget and theme layer** — d2d_widgets + theme + panel + popup + menu + dialog
  + fisheye + animator + nervegear.
- **Input and z-order** — input + z_order (they depend on the host being live).

## Build integration notes

`sao_ui.dll` links against:
- `d3d11.lib` `dxgi.lib` `dcomp.lib` `d2d1.lib` `dwrite.lib`
- `dwmapi.lib` `user32.lib` `gdi32.lib`
- `opengl32.lib` (WGL context for the interop path)
- `winmm.lib` (`timeBeginPeriod(1)` for the scheduler)
- Optional: `sao::security::anti_screencap` (capture-mode toggle)

All Windows-stock system libraries — no vendored dependencies needed
in the platform tier.

## Overlay + anti-screencap deliverables

Existing headers deepened (10):
- `overlay_host.h` — 220 lines: full config struct, dual HWND semantics
  (hRender + hControl decoy), capture-mode integration, focus-shield
  contract, three TOPMOST leak-pattern warning, WGL context accessors,
  msg pump + msg-wait, hit/mouse callbacks.
- `compositor.h` — 260 lines: SaoCompositorConfig, SaoLayerConfig,
  three content sources (bgra/mmf/shared texture), atomic snapshot
  contract, layer name reuse → ALREADY_EXISTS, temporal-union RGN
  padding, layer input proxy enable.
- `dcomp_bridge.h` — 175 lines: SaoDcompBridgeConfig, DEVICE_LOST
  handling contract, WGL_NV_DX_interop2 register/lock, keyed mutex
  0x100, device_removed check, borrowed COM accessors.
- `z_order.h` — 155 lines: policy enum + docs of HWND_TOP vs
  HWND_NOTOPMOST trap, poll intervals, leak-pattern guard, kernel
  exstyle hide mask.
- `input.h` — 175 lines: shield_arm_once with chain-of-death
  documentation, hotkey list, per-layer cursor, LL hook install.
- `d3d11_device.h` — 100 lines: config, adapter/factory/feature-level
  accessors, TDR recreate with on_lost callback.
- `d2d_widgets.h` — 175 lines: 16 widget kinds, float-coord paint,
  paint context lifecycle, theme-token clear.
- `panel.h` — 185 lines: rendering modes (native/tk_mirror/custom),
  event handler, get_state, find_by_id (single-instance pattern).

New headers added (10):
- `scheduler.h` — 170 lines: 60-240Hz auto-detect, pressure floor,
  combat/menu signals, timeBeginPeriod(1) engagement.
- `render_worker.h` — 195 lines: lane pool, submit_compose,
  try_take_frame, ULW commit, premultiply helper.
- `capture_sync.h` — 75 lines: reentrant begin/end,
  wait_until_idle, C++ RAII CaptureSection.
- `subpixel.h` — 80 lines: fractional composite, bar-width fade,
  snap_or_floor with eps.
- `dc_mutation.h` — 130 lines: register/submit/invalidate barrier,
  stale-failure identity check, stats.
- `dxgi_dup.h` — 155 lines: Desktop Duplication with WDA-aware
  frame, access-lost recovery, monitor desc query.
- `gpu_capture.h` — 130 lines: Windows.Graphics.Capture (WGC)
  async grabber, client-inset compensation, supported() probe.
- `gpu_overlay_window.h` — 165 lines: GpuOverlayWindow compat
  shim, WGL serialize lock, delegate to compositor.
- `adapter.h` — 165 lines: CompositorOverlayWindow +
  CompositorBgraPresenter compat shims.

Security integration:
- `security/anti_screencap/window_affinity.h` — 105 lines:
  SetWindowDisplayAffinity wrapper + `sao_security_anti_screencap_
  apply_to_overlay()` symmetric-apply-with-rollback entry point.
  Streaming-mode policy flag.
- `security/anti_screencap/dxgi_dup_deny.h` — 100 lines:
  IDXGIOutputDuplication::AcquireNextFrame hook, foreign-process
  detection, watchdog callback.
- `security/anti_screencap/obs_hook_detect.h` — 105 lines:
  obs-graphics-hook64.dll / ReShade / ShadowPlay / streamer-mode /
  IAT-tamper detection, signature database.
- `security/anti_screencap/gdi_bitblt_hook.h` — 85 lines:
  BitBlt / StretchBlt / PrintWindow / GetWindowDC / GetDIBits
  filter, per-API detailed stats.
- `security/anti_screencap/dwm_thumbnail.h` — 85 lines:
  DwmRegisterThumbnail poll + blank, per-relationship enumeration.
- `security/anti_screencap/secure_desktop.h` — 85 lines:
  Winlogon-style secure-desktop transition for license unlock/MFA.

Historical bug/leak invariants encoded at compile-time (see individual
header banners):

1. SetWindowRgn argtypes overflow (§5 of handoff) — enforced by
   `HRGN` (void*) native type.
2. Layer name reuse leak (§7 of handoff) — enforced by
   ALREADY_EXISTS return from `layer_create`.
3. wglDXLockObjectsNV no-timeout (§9 of handoff) — precondition
   `dcomp_bridge_device_removed()` before every acquire.
4. WM_MOUSEACTIVATE chain-of-death (compositor.py:350-397) —
   enforced by `shield_arm_once` returning ALREADY_EXISTS on retry.
5. Three known TOPMOST leaks (a90adbf/c208715/6e1eb20) —
   `z_order_check_leak_patterns` boot-time check.
6. Real-topmost + proxy-lift storm (§3 of handoff) — enforced by
   NEVER exposing a real-topmost path except SAO_UI_TOPMOST_ALWAYS
   which is explicitly documented as test-only.
7. hRender + hControl SYMMETRIC capture-affinity — enforced by
   `apply_to_overlay` rollback semantics; asymmetric state is a
   real anti-cheat topology-exposure bug.
8. Atomic (bytes, w, h, seq) snapshot for upload_bgra — enforced
   by the SaoLayer... contract in `sao_ui_layer_update_bgra`.
9. Keyed mutex = 0x100 with try/finally — enforced by
   `dcomp_bridge_lock_texture` / `unlock_texture` documented
   pattern.

## Game-agnostic widget-kit deliverables

Eight new headers, one lightly amended (`panel.h` grew two lines to
point at `panel_sdk.h`):

- **`widget_kit.h`** (umbrella) + `widget_text.h` / `widget_input.h` /
  `widget_container.h` / `widget_data.h` / `widget_chart.h` — 40+
  widget types wrapping the Python `sao_panel_components` /
  `sao_theme` widget set.  Every widget takes a spec struct rather
  than a JSON blob so the ABI is stable across languages (Python via
  `sao_sdk_ui.h`, Lua via `sao_ui_scriptable_canvas.h`, etc.).
  Colour override parameters (`fill` / `border` / `fg` / `canvas_bg`
  / `active_*`) match `_RoundedButton` / `rounded_panel` /
  `status_badge` in the Python component library so a plugin theme
  built for Tk transposes 1:1 to Direct2D.
  * Time-display trio (`SaoUiClockLabel` / `SaoUiRelativeTimeLabel` /
    `SaoUiDurationLabel`) are three separate types — each accepts
    exactly one time semantic (epoch / signed delta / duration).  Do
    not mix (memory [ACT时间显示三类分开]).
- **`panel_layout.h`** — measure/arrange engine.  6 layout modes
  (vertical / horizontal / grid / absolute / flex / dock) each with a
  mode-config struct.  Two-phase pipeline + per-node dirty flags +
  dirty-rects snapshot for the compositor to clip repaint.  Hit-test
  returns the hit widget plus its ancestor path.
- **`panel.h`** — kept the existing legacy `SaoPanelConfig` +
  ui_spec path; added the modern `SaoPanelDescriptor` path that
  returns a body handle the plugin fills with widgets via
  `panel_layout.h`.  `SaoUiBodyMutation[]` batching so live updates
  emit one dirty-rects snapshot per batch instead of per touch.
  `set_theme_override` for panel-scoped tokens.  `bring_to_front` /
  `send_to_back` never cross z-class boundaries (delegates to
  `z_order.h`).
- **`render_hook.h`** — UI-side dual to `engine/render_hook.h`.
  8 render-clock hook points (before/after compositor + present, and
  per-layer / per-widget paint).  Declarative overlay attach for the
  "widget on top of every frame" pattern.  Per-plugin timing stats
  so the plugin manager can name-and-shame budget overruns.
- **`input_router.h`** — deepened, complementary to `input.h`.
  Layered API: raw Win32 feed → typed `SaoUiInputEvent` → route to
  the hit-tested widget.  Focus stack (Tab / Shift+Tab traversal +
  modal barriers).  Hover ENTER/LEAVE synthesis.  Hotkey subset
  matcher: F5 fires for both `{F5}` and `{CTRL+F5}` bindings, and
  the most-specific match wins per memory [快捷键架构].  Plugin
  bindings can be forced into CTRL+combo (`enforce_ctrl_prefix`) so
  the main UI's bare F5-F12 stays reserved.
- **`sao_ui_scriptable_canvas.h`** — for Lua / Emma / AngelScript /
  C# plugins.  Canvas op vocabulary matches
  `act_platform/ui_spec.py::_normalize_canvas` so the same op list
  a script emits is renderable by the Python Tk and Web duals for
  parity testing.  Session model: `begin_draw` → `submit_ops` /
  fluent helpers → `end_draw`.  Bitmaps registered by integer id so
  scripts don't shuttle pixel arrays every frame (memory
  [脚本插件canvas UI三坑] — single draggable layer owns pointer input;
  lua54 hoisting caveat is a binding-side concern).

Summary: 8 new headers + 1 lightly amended (`panel.h`).  Line counts:
`widget_text.h` 240, `widget_input.h` 270, `widget_container.h` 217,
`widget_data.h` 184, `widget_table.h` 221, `widget_chart.h` 208,
`widget_kit.h` 171 (umbrella), `panel_layout.h` 336, `panel_sdk.h` 214,
`render_hook.h` 221, `input_router.h` 309,
`sao_ui_scriptable_canvas.h` 303.  Every header ≤ 350 lines.

## Theme and interaction deliverables

Extended headers (7 new / deepened):

- `theme.h` — 75 colour tokens, 15 metric tokens, 3 static tables,
  panel-override map, JSON hot-reload, change listener.
- `menu.h` — 3 opening modes (vertical/ring/cascade), 4 button states,
  7 phase-machine states, 6 event kinds, HUD-bounds query.
- `nervegear.h` — 7 button states, 2 palettes, Link Start timeline
  struct (9 timing fields), 9 event kinds, alpha-silhouette hit query.
- `popup.h` — sub-menu tree, keyboard nav, entry mutation without
  hiding, layout-consts export (12 fields), 6 nav keys.
- `dialog.h` — 5 kinds, 6 button roles, per-button colour override,
  clip-reveal timing controls, 4 convenience one-shots.
- `fisheye.h` — 8-field config, per-button state POD, ring + column
  layout helpers, per-frame grow-aware hit test.
- `animator.h` — 10 curve enums, bezier params struct, per-animation
  dedup keys, scheduler tick + `has_active` gate.
