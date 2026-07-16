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

    # ── Overlay window / compositor / z-order (Phase 5) ─────
    src/overlay_host.cpp
    src/compositor.cpp
    src/dcomp_bridge.cpp
    src/d3d11_device.cpp
    src/z_order.cpp
    src/input.cpp

    # ── Presentation subsystem (Phase 5) ────────────────────
    src/scheduler.cpp          # overlay_scheduler.py
    src/render_worker.cpp      # overlay_render_worker.py
    src/capture_sync.cpp       # render_capture_sync.py
    src/subpixel.cpp           # overlay_subpixel.py
    src/dc_mutation.cpp        # dc_mutation_coordinator.py

    # ── DXGI / GPU capture (Phase 5) ────────────────────────
    src/dxgi_dup.cpp           # dxgi_duplication.py
    src/gpu_capture.cpp        # gpu_capture.py

    # ── Legacy compat shims (Phase 5) ───────────────────────
    src/gpu_overlay_window.cpp # gpu_overlay_window.py
    src/adapter.cpp            # overlay_adapter.py

    # ── Widgets / theme / panels (Phase 6) ──────────────────
    src/d2d_widgets.cpp
    src/theme.cpp
    src/popup.cpp              # ui_gpu/popup.py
    src/panel.cpp
    src/panel_layout.cpp

    # ── Wave 3 first slices (G3.2 SAO menu, G3.3 NerveGear) ─
    src/menu.cpp               # gui_modules/sao_gui_menu_hud.py + menu_mixin.py
    src/nervegear.cpp          # gui_modules/sao_gui_nervegear_button.py + link_start.py
    src/entity_shell.cpp       # production Entity root composition

    # ── Wave 3 (G3.4 popup nav + G3.7 animator + G3.9 layout) ─
    src/animator.cpp           # sao_theme/animator.py + utils easing

    # ── Wave 4 (G3.5 dialog + G3.6 fisheye first slices) ────
    src/dialog.cpp             # sao_theme/dialogs.py state machine
    src/fisheye.cpp             # sao_theme/menu_bar.py fisheye math

    # ── Wave 4 / Agent e (G3.10 panel SDK + G3.11 input router +
    #    G3.12 scriptable canvas) ─────────────────────────────────
    src/panel_sdk.cpp                # plugin_manager.register_panel
    src/input_router.cpp             # sao_gui_hotkey.py subset matcher
    src/sao_ui_scriptable_canvas.cpp # ui_spec.py CANVAS_OPS producer

    # ── Wave 4 / Agent d (G3.8 widget_kit first 5 widgets +
    #    time-display labels) ─────────────────────────────────────
    src/widget_text.cpp        # Label + Clock/Rel/Dur time labels
    src/widget_input.cpp       # Button (action_button parity)
    src/widget_data.cpp        # ProgressBar (flat/hp_ramp/hp_trail/segments)
    src/widget_chart.cpp       # TimeSeriesChart (ring buffer + axis)
    src/widget_table.cpp       # Table (columns/sort/filter/virtual scroll)

    # ── Wave 6 / Agent d (Phase 5 automation SDK batch 2) ─────
    src/auto_key.cpp           # game-agnostic SendInput / hold ledger
    src/alerts.cpp             # SAPI TTS + banner queue + winmm sound
    src/input_arbitration.cpp  # per-VK policy + reservation + conflict

    # ── Wave 13 / Agent b (streaming-mode flow control) ────────
    # overlay_compositor.py streaming_mode: generation + accepting +
    # worker set + mode/threads locks + 2-phase teardown.  Orthogonal
    # to capture_sync.cpp (Wave 5b) — that module owns render/capture
    # contention; this one owns streaming-mode worker lifecycle.
    src/streaming_flow.cpp
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
    )

    target_link_libraries(sao_platform_ui
        PRIVATE
            d3d11
            dxgi
            dcomp
            d2d1
            dwrite
            user32
            gdi32
            dwmapi
            winmm
            # Wave 6 / Agent d — SAPI TTS (sapi.lib) uses IIDs / CLSIDs
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

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
