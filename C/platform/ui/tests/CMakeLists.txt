# Catch2 is provisioned by the top-level CMakeLists via FetchContent's
# FIND_PACKAGE_ARGS integration when tests are enabled, so a plain
# find_package succeeds here whether Catch2 came from vcpkg, a system
# package, or the FetchContent fallback.
find_package(Catch2 3 CONFIG REQUIRED)

add_executable(sao_platform_ui_tests
    test_ui.cpp
    # ── Wave 2 slice tests ─────────────────────────────────────
    test_overlay_host_wave2.cpp
    test_compositor_wave2.cpp
    # ── Wave 3 slice tests ─────────────────────────────────────
    test_dcomp_bridge_wave3.cpp
    test_dxgi_dup_wave3.cpp
    test_render_spine_wave15.cpp
)

target_link_libraries(sao_platform_ui_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

# Wave 2 tests touch user32 (GetWindowLongPtrW, SendMessageW, IsWindow)
# directly for verification.  On the DLL build posture we still need
# to link the import lib into the test executable.
# Wave 3 tests additionally probe D3D11 / DXGI / DirectComposition
# through the C++ ABI (create+destroy path), so link the same import
# libs the shared library links -- test executable talks D3D11/DXGI
# indirectly via sao::ui, but static linker still needs the CRT lib
# reference here for symbol resolution against the platform SDK when
# a debug MSVC toolchain does not otherwise pull them in.
if (WIN32)
    target_link_libraries(sao_platform_ui_tests PRIVATE
        user32 gdi32
        d3d11 dxgi dcomp
    )
endif()

target_compile_definitions(sao_platform_ui_tests PRIVATE
    SAO_UI_USING_DLL
    SAO_UI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_features(sao_platform_ui_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
catch_discover_tests(sao_platform_ui_tests)

# ── Wave 2 / Agent d — G3.1 flat-token theme tests ──────────────────
# Isolated target so it stays green even if the shared overlay/comp
# tests need Windows-only setup that the flat-token tests don't.
add_executable(sao_platform_ui_theme_wave2_tests
    test_theme_wave2.cpp
)

target_link_libraries(sao_platform_ui_theme_wave2_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_theme_wave2_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_theme_wave2_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_theme_wave2_tests)

# ── Wave 3 / Agent d — G3.2 SAO menu + G3.3 NerveGear tests ────────
# Pure state-machine + geometry — no Windows-only surface, no compositor,
# no D3D11/DXGI.  Kept in a dedicated target so a Windows-only regression
# on the shared overlay/compositor suite doesn't red the wave3 slice.
add_executable(sao_platform_ui_wave3_tests
    test_menu_wave3.cpp
    test_nervegear_wave3.cpp
)

target_link_libraries(sao_platform_ui_wave3_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_wave3_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave3_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave3_tests)

# ── Wave 3 / Agent e — G3.4 popup nav + G3.7 animator + G3.9 layout ──
# Pure logic / math / tree walks — no Windows-only surface, no D3D11,
# no compositor.  Kept in a dedicated target so Windows-specific runs
# of the shared overlay/compositor suite don't red the wave3e slice.
add_executable(sao_platform_ui_wave3e_tests
    test_popup_wave3.cpp
    test_animator_wave3.cpp
    test_panel_layout_wave3.cpp
)

target_link_libraries(sao_platform_ui_wave3e_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_wave3e_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave3e_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave3e_tests)

# ── Wave 4 / Agent a — G1.5 scheduler 60fps + render_worker thread pool ──
# Pure timing / thread-pool state — no Windows-only D3D11/DXGI/DComp.
# Kept in its own target so a Windows-only regression in the shared
# overlay/compositor suite doesn't red the wave4 slice.
add_executable(sao_platform_ui_wave4_tests
    test_scheduler_wave4.cpp
    test_render_worker_wave4.cpp
)

target_link_libraries(sao_platform_ui_wave4_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_wave4_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave4_tests PRIVATE cxx_std_20)

if (WIN32)
    target_link_libraries(sao_platform_ui_wave4_tests PRIVATE
        winmm user32 gdi32
    )
endif()

catch_discover_tests(sao_platform_ui_wave4_tests)

# ── Wave 4 / Agent c — G3.5 dialog + G3.6 fisheye first slices ─────
# Pure state machine + math — no Windows-only surface, no compositor,
# no D3D11.  Isolated target so Windows-only breakage on the shared
# overlay/compositor suite doesn't red the wave4c slice.  Also kept
# separate from wave4a (scheduler/render_worker) so agent boundaries
# stay explicit.
add_executable(sao_platform_ui_wave4c_tests
    test_dialog_wave4.cpp
    test_fisheye_wave4.cpp
)

target_link_libraries(sao_platform_ui_wave4c_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_wave4c_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave4c_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave4c_tests)

# ── Wave 4 / Agent e — G3.10 panel SDK + G3.11 input router +
# G3.12 scriptable canvas first slices ─────────────────────────
# Pure registry / matcher / op-append logic — no Windows-only
# surface, no D3D11, no compositor.  Isolated target keeps the
# wave4e slice green when Windows-specific runs regress the
# shared overlay/compositor suite.
add_executable(sao_platform_ui_wave4e_tests
    test_panel_sdk_wave4.cpp
    test_input_router_wave4.cpp
    test_scriptable_canvas_wave4.cpp
    test_native_raster.cpp
)

target_link_libraries(sao_platform_ui_wave4e_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_wave4e_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave4e_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave4e_tests)

# ── Wave 4 / Agent d — G3.8 widget_kit first 5 widgets + 3 time labels ─
# Pure state-machine + geometry — no Windows-only surface, no
# compositor, no D3D11.  Isolated target so Windows-specific runs
# of the shared overlay/compositor suite don't red the wave4d slice.
# The time-labels test additionally proves the "3类严格分离防误用
# 编译期机制" contract via static_asserts on the strict-typed
# wave4 formatter signatures — mixing int64/int32/uint64 across
# widgets is a compile diagnostic, not a runtime bug.
add_executable(sao_platform_ui_widget_wave4_tests
    test_widget_text_wave4.cpp
    test_widget_input_wave4.cpp
    test_widget_data_wave4.cpp
    test_widget_chart_wave4.cpp
    test_widget_table_wave4.cpp
    test_widget_time_labels_wave4.cpp
    test_ui_completion_slice.cpp
    test_widget_extension_abi.cpp
)

target_link_libraries(sao_platform_ui_widget_wave4_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_ui_widget_wave4_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_widget_wave4_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_widget_wave4_tests)

# ── Wave 6 / Agent d — Phase 5 automation SDK batch 2 ───────────
# auto_key (SendInput hold ledger) + alerts (SAPI TTS + banner
# queue + winmm sound) + input_arbitration (per-VK policy + plugin
# reservation + observed-source conflict detection).  Game-agnostic
# — no hard-coded VKs, no game-specific mappings.
#
# Isolated target: SAPI COM init happens lazily on first speak() so
# the enum_voices test can run on CI images without a SAPI voice.
# winmm is already linked into sao::ui; sapi is added at the shared
# lib link line.  The tests link the shared lib and pull in symbols
# via the DLL import surface.
add_executable(sao_platform_ui_wave6_tests
    test_auto_key_wave6.cpp
    test_alerts_wave6.cpp
    test_input_arb_wave6.cpp
)

target_link_libraries(sao_platform_ui_wave6_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_wave6_tests PRIVATE
        user32 winmm
    )
endif()

target_compile_definitions(sao_platform_ui_wave6_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave6_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave6_tests)

# ── Wave 6 / Agent e — Phase 6 overlay live parity first slice ──────
# render_capture_sync + overlay_subpixel + dc_mutation_coordinator +
# overlay_adapter.  Pure logic / state-machine / pixel-kernel tests —
# no Windows-only surface, no D3D11, no compositor.  Isolated target
# so Windows-only regressions on the shared overlay/compositor suite
# don't red the wave6e slice.
#
# The subpixel test reads a W5b overlay fixture JSON via
# `docs/fixtures/overlay/present_tick_normal_dirty_flag.json` for
# parity — the test walks up a few candidate roots so it works from
# any CTest working-directory setting.
add_executable(sao_platform_ui_wave6e_tests
    test_capture_sync_wave6.cpp
    test_subpixel_wave6.cpp
    test_dc_mutation_wave6.cpp
    test_adapter_wave6.cpp
)

target_link_libraries(sao_platform_ui_wave6e_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_wave6e_tests PRIVATE user32)
endif()

target_compile_definitions(sao_platform_ui_wave6e_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave6e_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave6e_tests)

# ── Wave 7 / Agent a — Phase 6 overlay live parity depth ────────────
# dxgi_dup dirty/move/cursor/staging + gpu_capture BGRA readback +
# overlay_host WM_ handler completion (SIZE/MOVE/ACTIVATE/WPOS/DPI/
# DISPLAY).  Windows-only surface — no external D3D11 fixtures required
# because gpu_capture tests boot their own device with WARP fallback,
# and dxgi_dup tests SKIP when the environment can't duplicate.
#
# One fixture-parity test reads `docs/fixtures/overlay/
# topmost_race_deterministic.json`; the raise_count field there is
# the compositor's "how many z-order bumps happened" oracle, which
# our WM_WINDOWPOSCHANGING repair counter must match exactly.
add_executable(sao_platform_ui_wave7a_tests
    test_dxgi_dup_wave7.cpp
    test_gpu_capture_wave7.cpp
    test_overlay_host_wave7.cpp
)

target_link_libraries(sao_platform_ui_wave7a_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_wave7a_tests PRIVATE
        user32 gdi32
        d3d11 dxgi
    )
endif()

target_compile_definitions(sao_platform_ui_wave7a_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_wave7a_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_wave7a_tests)

# ── Overlay host production chain ──────────────────────────────────
# One real render HWND plus owner/control helpers, SetWindowRgn temporal
# input updates, single-authority z order, resize/message-loop behavior,
# and teardown.  This is intentionally a normal production test target,
# not a fixture-only or test-helper target.
add_executable(sao_platform_ui_overlay_host_chain_tests
    test_overlay_host_main_chain.cpp
)

target_link_libraries(sao_platform_ui_overlay_host_chain_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_overlay_host_chain_tests PRIVATE user32 gdi32)
endif()

target_compile_definitions(sao_platform_ui_overlay_host_chain_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_overlay_host_chain_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_overlay_host_chain_tests)

add_executable(sao_platform_ui_entity_shell_tests
    test_entity_shell.cpp
)

target_link_libraries(sao_platform_ui_entity_shell_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_entity_shell_tests PRIVATE user32 gdi32)
endif()

target_compile_definitions(sao_platform_ui_entity_shell_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_entity_shell_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_entity_shell_tests
    TEST_PREFIX "ui.entity_shell."
    PROPERTIES LABELS "ui;entity_shell;production_slice"
)

add_executable(sao_platform_ui_entity_roots_focused_tests
    test_entity_roots_focused.cpp
)

target_link_libraries(sao_platform_ui_entity_roots_focused_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_entity_roots_focused_tests PRIVATE user32 gdi32)
endif()

target_compile_definitions(sao_platform_ui_entity_roots_focused_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_entity_roots_focused_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_entity_roots_focused_tests
    TEST_PREFIX "ui.entity_roots.focused."
    PROPERTIES LABELS "ui;entity_shell;entity_roots;w21"
)

if (WIN32)
    add_executable(sao_platform_ui_parity_entity_w19_tests
        test_ui_parity_entity_w19.cpp
    )

    target_link_libraries(sao_platform_ui_parity_entity_w19_tests PRIVATE
        sao::ui
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
        bcrypt
    )

    target_compile_definitions(sao_platform_ui_parity_entity_w19_tests PRIVATE
        SAO_UI_USING_DLL
        SAO_UI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        SAO_UI_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/entity"
    )
    target_compile_features(sao_platform_ui_parity_entity_w19_tests PRIVATE cxx_std_20)

    catch_discover_tests(sao_platform_ui_parity_entity_w19_tests
        TEST_PREFIX "ui.w19.entity."
        PROPERTIES LABELS "ui;w19;entity;parity"
    )

    add_executable(sao_platform_ui_entity_menu_hud_w20_tests
        test_entity_menu_hud_w20.cpp
    )

    target_link_libraries(sao_platform_ui_entity_menu_hud_w20_tests PRIVATE
        sao::ui
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
        bcrypt
    )

    target_compile_definitions(sao_platform_ui_entity_menu_hud_w20_tests PRIVATE
        SAO_UI_USING_DLL
        SAO_UI_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
        SAO_UI_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../assets/entity"
    )
    target_compile_features(sao_platform_ui_entity_menu_hud_w20_tests PRIVATE cxx_std_20)

    catch_discover_tests(sao_platform_ui_entity_menu_hud_w20_tests
        TEST_PREFIX "ui.w20.entity_menu_hud."
        PROPERTIES LABELS "ui;w20;entity;resource_integrity;production_composition"
    )
endif()

# Overlay-region transaction tests compile the host directly so failure
# injection stays on the private Win32 seam and outside the production DLL ABI.
if (WIN32)
    add_executable(sao_platform_ui_overlay_region_tests
        test_overlay_host_region_transaction.cpp
        ../src/overlay_host.cpp
        ../src/dc_mutation.cpp
    )

    target_include_directories(sao_platform_ui_overlay_region_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/platform/core/include
    )

    target_link_libraries(sao_platform_ui_overlay_region_tests PRIVATE
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
        user32 gdi32 dwmapi
    )

    target_compile_definitions(sao_platform_ui_overlay_region_tests PRIVATE
        SAO_UI_OVERLAY_HOST_TESTING
    )
    target_compile_features(sao_platform_ui_overlay_region_tests PRIVATE cxx_std_20)

    catch_discover_tests(sao_platform_ui_overlay_region_tests
        TEST_PREFIX "ui.overlay_region."
        PROPERTIES LABELS "ui;overlay_host;region;transaction"
    )
endif()

# Low-level hook lifetime tests compile input.cpp directly so the private
# Win32 seam remains outside the public header and production DLL ABI.
add_executable(sao_platform_ui_input_hook_lifecycle_tests
    test_input_hook_lifecycle.cpp
    ../src/input.cpp
)

target_include_directories(sao_platform_ui_input_hook_lifecycle_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/platform/core/include
)

target_link_libraries(sao_platform_ui_input_hook_lifecycle_tests PRIVATE
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_input_hook_lifecycle_tests PRIVATE user32)
endif()

target_compile_definitions(sao_platform_ui_input_hook_lifecycle_tests PRIVATE
    SAO_UI_INPUT_TESTING
)
target_compile_features(sao_platform_ui_input_hook_lifecycle_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_input_hook_lifecycle_tests
    TEST_PREFIX "ui.input_hook_lifecycle."
    PROPERTIES LABELS "ui;input_hook;lifecycle"
)

# Production state completion: focused API snapshots and composed primitive
# chains for z-order, alerts, input routing, overlay host, GPU overlay,
# D3D11, DirectComposition, and DXGI duplication.
add_executable(sao_platform_ui_production_state_tests
    test_ui_production_state_completion.cpp
)

target_link_libraries(sao_platform_ui_production_state_tests PRIVATE
    sao::ui
    Catch2::Catch2WithMain
)

if (WIN32)
    target_link_libraries(sao_platform_ui_production_state_tests PRIVATE
        user32 gdi32 d3d11 dxgi dcomp
    )
endif()

target_compile_definitions(sao_platform_ui_production_state_tests PRIVATE SAO_UI_USING_DLL)
target_compile_features(sao_platform_ui_production_state_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_ui_production_state_tests
    TEST_PREFIX "ui.production_state."
    PROPERTIES LABELS "ui;production_state"
)

# ── Wave 8 / Agent c — Phase 6+7 fixture parity deep dive ───────────
# Byte-for-byte parity of the state transitions frozen at
# docs/fixtures/overlay/*.json and docs/fixtures/ui_spec/*.json.  The
# overlay side ports the Python ``gen_overlay.CompositorState`` 1:1 in
# C++ and checks each of the six scenarios; the ui_spec side drives the
# newly-implemented ``sao_engine_ui_spec_normalize`` (previously a stub)
# through the eight declarative-UI cases.  Pure state / normalization
# work -- no Windows-only surface, no D3D11, no compositor.  Isolated
# target so a Windows-only regression on the shared overlay/compositor
# suite doesn't red the wave8c slice.
# Wave 8c 目前处于并发 land 状态: overlay 侧已 land, ui_spec 侧 cpp 未到位.
# 用 file(EXISTS) 判断可用源, 保持 configure 通过, 让并发 agent 后续补齐后
# 自动包含.  这不改任何已 land 的 wave8c 逻辑.
set(_sao_wave8c_sources)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_overlay_fixture_parity_wave8.cpp")
    list(APPEND _sao_wave8c_sources test_overlay_fixture_parity_wave8.cpp)
endif()
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ui_spec_fixture_parity_wave8.cpp")
    list(APPEND _sao_wave8c_sources test_ui_spec_fixture_parity_wave8.cpp)
endif()
if (_sao_wave8c_sources)
    add_executable(sao_platform_ui_wave8c_tests ${_sao_wave8c_sources})

    target_link_libraries(sao_platform_ui_wave8c_tests PRIVATE
        sao::ui
        sao::engine
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
    )

    target_compile_definitions(sao_platform_ui_wave8c_tests PRIVATE SAO_UI_USING_DLL)
    target_compile_features(sao_platform_ui_wave8c_tests PRIVATE cxx_std_20)

    catch_discover_tests(sao_platform_ui_wave8c_tests)
endif()
unset(_sao_wave8c_sources)

    # ── Wave 15 — independent Python-authority UI parity suites ──────────
    # These suites exercise the production compositor and UI APIs directly.  The
    # generated BGRA/JSON fixtures stay external test data and are located through
    # one shared source-tree fixture directory definition.
    set(_sao_wave15_fixture_dir "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

    add_executable(sao_platform_ui_parity_menu_dialog_wave15_tests
        test_ui_parity_menu_dialog_wave15.cpp
    )

    target_link_libraries(sao_platform_ui_parity_menu_dialog_wave15_tests PRIVATE
        sao::ui
        Catch2::Catch2WithMain
    )

    if (WIN32)
        target_link_libraries(sao_platform_ui_parity_menu_dialog_wave15_tests PRIVATE
            user32
            gdi32
        )
    endif()

    target_compile_definitions(sao_platform_ui_parity_menu_dialog_wave15_tests PRIVATE
        SAO_UI_USING_DLL
        SAO_UI_TEST_FIXTURE_DIR="${_sao_wave15_fixture_dir}"
    )
    target_compile_features(sao_platform_ui_parity_menu_dialog_wave15_tests PRIVATE cxx_std_20)
    catch_discover_tests(sao_platform_ui_parity_menu_dialog_wave15_tests
        TEST_PREFIX "ui.wave15.menu_dialog."
        PROPERTIES LABELS "wave15"
    )

    add_executable(sao_platform_ui_parity_combat_wave15_tests
        test_ui_parity_combat_wave15.cpp
    )

    target_link_libraries(sao_platform_ui_parity_combat_wave15_tests PRIVATE
        sao::ui
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
    )

    if (WIN32)
        target_link_libraries(sao_platform_ui_parity_combat_wave15_tests PRIVATE bcrypt)
    endif()

    target_compile_definitions(sao_platform_ui_parity_combat_wave15_tests PRIVATE
        SAO_UI_USING_DLL
        SAO_UI_TEST_FIXTURE_DIR="${_sao_wave15_fixture_dir}"
    )
    target_compile_features(sao_platform_ui_parity_combat_wave15_tests PRIVATE cxx_std_20)
    catch_discover_tests(sao_platform_ui_parity_combat_wave15_tests
        TEST_PREFIX "ui.wave15.combat."
        PROPERTIES LABELS "wave15"
    )

    add_executable(sao_platform_ui_parity_effects_wave15_tests
        test_ui_parity_effects_wave15.cpp
    )

    target_link_libraries(sao_platform_ui_parity_effects_wave15_tests PRIVATE
        sao::ui
        nlohmann_json::nlohmann_json
        Catch2::Catch2WithMain
    )

    target_compile_definitions(sao_platform_ui_parity_effects_wave15_tests PRIVATE
        SAO_UI_USING_DLL
        SAO_UI_TEST_FIXTURE_DIR="${_sao_wave15_fixture_dir}"
    )
    target_compile_features(sao_platform_ui_parity_effects_wave15_tests PRIVATE cxx_std_20)
    catch_discover_tests(sao_platform_ui_parity_effects_wave15_tests
        TEST_PREFIX "ui.wave15.effects."
        PROPERTIES LABELS "wave15"
    )

    unset(_sao_wave15_fixture_dir)

# ── Wave 17 / Agent a — platform/ui NOT_IMPLEMENTED classification ────
# The independent STATIC support library (sao_wave17a_ui_support_lib —
# the `_impl` suffix is banned by the top-level wave-guard, so we use
# `_support_lib`) hosts the classification matrix, the hermetic mocks
# (SendInputLedger, ShellNotifyLedger, DcompLateBindMock), and the
# taxonomy predicates.  It is never linked into the production sao::ui
# shared library.
#
# The test binary is fully hermetic: it does not open a real HWND, does
# not call SendInput to the OS, does not call ShellNotifyIcon, does not
# start a WGC session, does not call DXGI Duplication against the live
# desktop.  Every real API call in the assertions is short-circuited
# through argument validation before the capability probe fires.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_ui_gap_wave17a.cpp"
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/wave17a_ui_gap_support.cpp")
    add_library(sao_wave17a_ui_support_lib STATIC
        wave17a_ui_gap_support.cpp
    )
    target_include_directories(sao_wave17a_ui_support_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(sao_wave17a_ui_support_lib PUBLIC
        sao::ui
    )
    target_compile_definitions(sao_wave17a_ui_support_lib PUBLIC
        SAO_UI_USING_DLL
    )
    target_compile_features(sao_wave17a_ui_support_lib PUBLIC cxx_std_20)

    add_executable(test_ui_gap_wave17a
        test_ui_gap_wave17a.cpp
    )
    target_include_directories(test_ui_gap_wave17a PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_ui_gap_wave17a PRIVATE
        sao_wave17a_ui_support_lib
        sao::ui
        Catch2::Catch2WithMain
    )
    target_compile_definitions(test_ui_gap_wave17a PRIVATE
        SAO_UI_USING_DLL
    )
    target_compile_features(test_ui_gap_wave17a PRIVATE cxx_std_20)

    # NOTE — Catch2 v3.5.4's `catch_discover_tests` flattens the LABELS
    # list into space-separated tokens on the generated
    # set_tests_properties call, which makes `ctest -L wave17a` miss
    # every case.  Register the focused cases directly with add_test so
    # LABELS is a properly quoted list (same workaround as wave16c).
    set(_wave17a_cases
        wave17a_taxonomy_codes_are_distinct
        wave17a_matrix_totals_are_pinned
        wave17a_matrix_covers_every_expected_cpp_file
        wave17a_alerts_speak_missing_voice_is_capability_gated
        wave17a_alerts_sound_stop_gate_semantics
        wave17a_auto_key_send_key_arg_validation
        wave17a_auto_key_get_key_state_arg_and_gate
        wave17a_dcomp_bridge_create_null_out_is_arg_error
        wave17a_dcomp_bridge_create_null_config_capability_gated
        wave17a_dxgi_dup_create_null_out_is_arg_error
        wave17a_dxgi_dup_release_frame_handle_invalid
        wave17a_gpu_capture_supported_is_bool
        wave17a_gpu_capture_create_null_out_arg_error
        wave17a_gpu_capture_bgra_from_texture_null_texture
        wave17a_gpu_capture_ensure_session_handle_invalid
        wave17a_input_install_ll_hooks_handle_invalid
        wave17a_input_uninstall_ll_hooks_handle_invalid
        wave17a_legacy_wgl_make_current_stays_not_implemented
        wave17a_legacy_wgl_release_current_stays_not_implemented
        wave17a_legacy_wgl_swap_buffers_stays_not_implemented
        wave17a_legacy_dcomp_gl_interop_stays_not_implemented
        wave17a_send_input_ledger_records_press_release
        wave17a_shell_notify_ledger_counts_all_ops
        wave17a_dcomp_late_bind_mock_classification
        wave17a_matrix_class_a_rows_reference_real_files
        wave17a_matrix_class_c_rows_reference_real_files
    )
    foreach(_case IN LISTS _wave17a_cases)
        add_test(NAME "wave17a.${_case}"
                 COMMAND test_ui_gap_wave17a "${_case}")
        set_tests_properties("wave17a.${_case}" PROPERTIES
            LABELS "ui;wave17;wave17a")
    endforeach()
endif()
