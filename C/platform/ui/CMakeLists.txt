# platform/ui — the native overlay + widget stack.
#
# SHARED because plugin renderers link against it via the SDK.  Depends
# on core + engine + D3D11/DComp/DXGI + D2D + DirectWrite.  Legacy
# WGL compatibility entry points remain explicit capability gates.
#
# 1:1 with Python `sao_auto/python/render/` + `sao_auto/python/ui_gpu/`
# (see include/sao/ui/*.h header banners for each mapping).

add_library(sao_platform_ui SHARED
    src/abi.cpp

    # ── Overlay window / compositor / z-order ─────
    src/overlay_host.cpp
    src/compositor.cpp
    src/dcomp_bridge.cpp
    src/d3d11_device.cpp
    src/z_order.cpp
    src/input.cpp

    # ── Presentation subsystem ────────────────────
    src/scheduler.cpp          # overlay_scheduler.py
    src/render_worker.cpp      # overlay_render_worker.py
    src/capture_sync.cpp       # render_capture_sync.py
    src/subpixel.cpp           # overlay_subpixel.py
    src/dc_mutation.cpp        # dc_mutation_coordinator.py

    # ── DXGI / GPU capture ────────────────────────
    src/dxgi_dup.cpp           # dxgi_duplication.py
    src/gpu_capture.cpp        # gpu_capture.py

    # ── Legacy compat shims ───────────────────────
    src/gpu_overlay_window.cpp # gpu_overlay_window.py
    src/adapter.cpp            # overlay_adapter.py
    src/legacy_webview_stub.cpp

    # ── Widgets / theme / panels ──────────────────
    src/d2d_widgets.cpp
    src/d2d_effects.cpp
    src/particle_system.cpp
    src/hp_bar_widget.cpp
    src/shader_pipeline.cpp    # per-layer shader-pass pipeline (14.5.93)
    src/widget_text_render_win.cpp # DirectWrite true-font text backend (14.7.103)
    src/theme.cpp
    src/popup.cpp              # ui_gpu/popup.py
    src/panel.cpp
    src/panel_layout.cpp

    # ── SAO menu, NerveGear, and Entity root composition ─────
    src/menu.cpp               # gui_modules/sao_gui_menu_hud.py + menu_mixin.py
    src/nervegear.cpp          # gui_modules/sao_gui_nervegear_button.py + link_start.py
    src/linkstart_intro.cpp
    src/entity_shell.cpp       # production Entity root composition

    # ── Popup navigation, animation, and panel layout ─────────
    src/animator.cpp           # sao_theme/animator.py + utils easing

    # ── Dialog state machine + fisheye focus math ─────────────
    src/dialog.cpp             # sao_theme/dialogs.py state machine
    src/fisheye.cpp             # sao_theme/menu_bar.py fisheye math
    src/fisheye_backdrop.cpp   # shared procedural/live fisheye backdrop

    # ── Panel SDK + input router + scriptable canvas ──────────────
    src/panel_sdk.cpp                # plugin_manager.register_panel
    src/input_router.cpp             # sao_gui_hotkey.py subset matcher
    src/sao_ui_scriptable_canvas.cpp # ui_spec.py CANVAS_OPS producer

    # ── Widget kit + time-display labels ──────────────────────────
    src/widget_text.cpp        # Label + Clock/Rel/Dur time labels
    src/widget_input.cpp       # Button (action_button parity)
    src/widget_data.cpp        # ProgressBar (flat/hp_ramp/hp_trail/segments)
    src/widget_chart.cpp       # TimeSeriesChart (ring buffer + axis)
    src/widget_table.cpp       # Table (columns/sort/filter/virtual scroll)

    # ── Phase 14.6 cross-surface UX infrastructure ───────────────
    src/widget_toast.cpp       # Toast notification overlay (stack/dismiss/fade)
    src/focus_manager.cpp      # Keyboard focus ring + Tab traversal
    src/widget_badge.cpp       # Animated badge widget (pulse via animator)
    src/widget_filter_row.cpp  # Panel search-box + filter-chips row

    # ── Automation capabilities ───────────────────────────────
    src/auto_key.cpp           # game-agnostic SendInput / hold ledger
    src/alerts.cpp             # SAPI TTS + banner queue + winmm sound
    src/sound.cpp              # embedded Python-parity UI sound catalog
    src/input_arbitration.cpp  # per-VK policy + reservation + conflict

    # ── Streaming-mode flow control ────────────────────────────
    # overlay_compositor.py streaming_mode: generation + accepting +
    # worker set + mode/threads locks + 2-phase teardown.  Orthogonal
    # to capture_sync.cpp — that module owns render/capture
    # contention; this one owns streaming-mode worker lifecycle.
    src/streaming_flow.cpp

    # ── Phase 13 (Python parity closure): new dialog widgets ──
    src/dialog_leaderboard.cpp # SAOLeaderboardDialog port
    src/dialog_file_picker.cpp # SAOFilePicker port (native IFileDialog)
    src/dialog_native_comdlg32.cpp # comdlg32 GetOpenFileNameW fallback
    # Phase 14: DWM chrome tweaks (dark caption / rounded / shadow).
    src/window_effects.cpp
    # Phase 14: perf_probe integration overlay (dev-only).
    src/level_up_effect_overlay.cpp
)
add_library(sao::ui ALIAS sao_platform_ui)

target_compile_definitions(sao_platform_ui PRIVATE SAO_UI_BUILDING_DLL)

target_include_directories(sao_platform_ui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(sao_platform_ui PUBLIC cxx_std_20)

target_link_libraries(sao_platform_ui
    PUBLIC sao::core sao::engine
    PRIVATE nlohmann_json::nlohmann_json
)

if (WIN32)
    target_sources(sao_platform_ui PRIVATE
        src/entity_authority_frames.rc
        src/sound_assets.rc
        src/entity_text_renderer_win.cpp
    )

    target_link_libraries(sao_platform_ui
        PRIVATE
            d3d11
            dxgi
            dxguid
            dcomp
            d2d1
            dwrite
            windowscodecs
            ole32
            user32
            gdi32
            dwmapi
            winmm
            xaudio2
            # SAPI TTS (sapi.lib) uses IIDs / CLSIDs
            # via the system SDK.  Every modern Windows SDK ships the
            # link import lib as `sapi.lib`.  No runtime dependency
            # beyond the SAPI COM inproc server that the OS provides
            # by default.
            sapi
            windowsapp
    )
endif()

# The security subtree is configured after platform/, so it owns the optional
# UI -> anti_screencap edge once its target exists.  Keeping this declaration
# out of the early platform pass makes the link independent of configuration
# order while builds without the security target remain valid.

