# `platform/ui`

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
   reliable mechanism.  The host's RGN is never NULL.  (See the
   `SetWindowRgn 裸调` handle-leak note.)
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

## Anti-screencap integration (this batch — Agent a)

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
  override, geometry persist hook.
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

## Phase plan

- **Phase 1 (this skeleton)** — headers + stub cpps.
- **Phase 5** — overlay_host + compositor + dcomp_bridge (the critical
  path); then d3d11_device.
- **Phase 6** — d2d_widgets + theme + panel + popup + menu + dialog
  + fisheye + animator + nervegear.  (Everything Agent d owns.)
- **Phase 7** — input + z_order (they depend on the host being live).

## Integration notes for Agent 6

`sao_ui.dll` links against:
- `d3d11.lib` `dxgi.lib` `dcomp.lib` `d2d1.lib` `dwrite.lib`
- `dwmapi.lib` `user32.lib` `gdi32.lib`
- `opengl32.lib` (WGL context for the interop path)
- `winmm.lib` (`timeBeginPeriod(1)` for the scheduler)
- Optional: `sao::security::anti_screencap` (capture-mode toggle)

All Windows-stock system libraries — no vendored dependencies needed
in the platform tier.

## Wave 1 Agent a deliverables (this batch — overlay + anti-screencap 1:1)

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

## Wave 1 Agent e deliverables (this batch — game-agnostic widget kit)

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

## Wave 1 Agent d deliverables (this batch)

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
