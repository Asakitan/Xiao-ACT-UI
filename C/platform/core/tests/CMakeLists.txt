find_package(Catch2 3 CONFIG REQUIRED)

# ─── Wave 17c / Agent c — shared gap-closure test support ─────────────
# STATIC library so every gap test binary (core/engine/net) links the same
# object code once.  Nothing here ships in production DLLs — the target
# name is prefixed with wave17c_ so the wave-impl audit picks it up as a
# test-only artefact.
add_library(wave17c_support_lib STATIC
    wave17c_support.cpp
)
target_include_directories(wave17c_support_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(wave17c_support_lib PUBLIC
    sao::core
)
target_compile_features(wave17c_support_lib PUBLIC cxx_std_20)

add_executable(sao_platform_core_tests
    test_core.cpp
    test_logging_concurrency.cpp
    test_settings_wave5.cpp
)

target_link_libraries(sao_platform_core_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_core_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
catch_discover_tests(sao_platform_core_tests)

# ── Wave 4 / Agent a — G1.9 platform/core RPM/WPM coverage ──────────
# Self-attach tests that exercise process open, read, batch, module +
# region enumeration.  Isolated target so a Windows-only issue in the
# smoke test doesn't red the wave4 slice.
add_executable(sao_platform_core_wave4_tests
    test_process_read_wave4.cpp
)

target_link_libraries(sao_platform_core_wave4_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_core_wave4_tests PRIVATE cxx_std_20)

catch_discover_tests(sao_platform_core_wave4_tests)

# ── Wave 6 / Agent b — Phase 4 engine type detection + IL2CPP probe ─
# Two isolated targets so a compile break in one side (e.g. il2cpp
# probe internals) doesn't block the detector smoke.
add_executable(sao_platform_core_engine_detector_wave6_tests
    test_engine_detector_wave6.cpp
)
target_link_libraries(sao_platform_core_engine_detector_wave6_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_core_engine_detector_wave6_tests PRIVATE cxx_std_20)
catch_discover_tests(sao_platform_core_engine_detector_wave6_tests)

add_executable(sao_platform_core_il2cpp_probe_wave6_tests
    test_il2cpp_probe_wave6.cpp
)
target_link_libraries(sao_platform_core_il2cpp_probe_wave6_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_core_il2cpp_probe_wave6_tests PRIVATE cxx_std_20)
# One case SKIPs when the test host somehow ships GameAssembly.dll; the
# other two run against synthetic memory.  Catch2 SKIP() exits 4.
catch_discover_tests(sao_platform_core_il2cpp_probe_wave6_tests
    PROPERTIES SKIP_RETURN_CODE 4)

# ── Wave 9 / Agent e — settings fixture parity (10 pinned fixtures) ──
#
# Drives sao_core_settings_dump_compact / _dump_pretty /
# _normalize_panel_themes / _merge_hotkeys / _strip_legacy_dump (new APIs
# added this wave) through every JSON fixture at docs/fixtures/settings/
# and asserts byte-for-byte parity vs the Python oracle (python/config.py).
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_settings_fixture_parity_wave9.cpp")
    find_package(nlohmann_json 3 CONFIG REQUIRED)
    add_executable(sao_platform_core_settings_fixture_parity_wave9_tests
        test_settings_fixture_parity_wave9.cpp
    )
    target_link_libraries(sao_platform_core_settings_fixture_parity_wave9_tests
        PRIVATE
            sao::core
            nlohmann_json::nlohmann_json
            Catch2::Catch2WithMain
    )
    target_compile_features(sao_platform_core_settings_fixture_parity_wave9_tests
        PRIVATE cxx_std_20)
    target_compile_definitions(sao_platform_core_settings_fixture_parity_wave9_tests
        PRIVATE SAO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/docs/fixtures/settings")
    catch_discover_tests(sao_platform_core_settings_fixture_parity_wave9_tests
        PROPERTIES LABELS "settings;fixture_parity_wave9;wave9")
endif()

# ── Wave 17c / Agent c — gap closure tests for platform/core ─────────
# Exercises the crypto/window capability gates and the
# thread/event/path/string/time real cross-platform implementations that
# used to sit behind NOT_IMPLEMENTED stubs.  Hermetic — no driver, no
# real Npcap, no zstd stream, no external process spawn.
add_executable(sao_platform_core_gap_wave17c_tests
    test_gap_wave17c_core.cpp
)
target_link_libraries(sao_platform_core_gap_wave17c_tests PRIVATE
    sao::core
    wave17c_support_lib
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_core_gap_wave17c_tests PRIVATE cxx_std_20)
catch_discover_tests(sao_platform_core_gap_wave17c_tests
    PROPERTIES LABELS "core;wave17c;gap_closure")
