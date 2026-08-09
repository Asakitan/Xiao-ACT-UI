if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

# ─── Shared gap-closure test support ─────────────────────────────────
# STATIC library so every gap test binary (core/engine/net) links the same
# object code once.  Nothing here ships in production DLLs.
add_library(sao_core_gap_support_lib STATIC
    core_gap_support.cpp
)
target_include_directories(sao_core_gap_support_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(sao_core_gap_support_lib PUBLIC
    sao::core
)
target_compile_features(sao_core_gap_support_lib PUBLIC cxx_std_20)

add_executable(sao_platform_core_tests
    test_core.cpp
    test_logging_concurrency.cpp
    test_settings.cpp
)

target_link_libraries(sao_platform_core_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_core_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
add_test(NAME sao_platform_core_tests COMMAND sao_platform_core_tests)

# ── platform/core RPM/WPM coverage ───────────────────────────────────
# Self-attach tests that exercise process open, read, batch, module +
# region enumeration.  Isolated target so a Windows-only issue in the
# smoke test doesn't red the process-read slice.
add_executable(sao_platform_core_process_read_tests
    test_process_read.cpp
)

target_link_libraries(sao_platform_core_process_read_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_core_process_read_tests PRIVATE cxx_std_20)

add_test(NAME sao_platform_core_process_read_tests COMMAND sao_platform_core_process_read_tests)

# ── engine type detection + IL2CPP probe ─────────────────────────────
# Two isolated targets so a compile break in one side (e.g. il2cpp
# probe internals) doesn't block the detector smoke.
add_executable(sao_platform_core_engine_detector_tests
    test_engine_detector.cpp
)
target_link_libraries(sao_platform_core_engine_detector_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_core_engine_detector_tests PRIVATE cxx_std_20)
add_test(NAME sao_platform_core_engine_detector_tests COMMAND sao_platform_core_engine_detector_tests)

add_executable(sao_platform_core_il2cpp_probe_tests
    test_il2cpp_probe.cpp
)
target_link_libraries(sao_platform_core_il2cpp_probe_tests PRIVATE
    sao::core
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_core_il2cpp_probe_tests PRIVATE cxx_std_20)
# One case SKIPs when the test host somehow ships GameAssembly.dll; the
# other two run against synthetic memory.  Catch2 SKIP() exits 4.
add_test(NAME sao_platform_core_il2cpp_probe_tests COMMAND sao_platform_core_il2cpp_probe_tests)
set_tests_properties(sao_platform_core_il2cpp_probe_tests PROPERTIES
    SKIP_RETURN_CODE 4)

# ── settings fixture parity (10 pinned fixtures) ─────────────────────
#
# Drives sao_core_settings_dump_compact / _dump_pretty /
# _normalize_panel_themes / _merge_hotkeys / _strip_legacy_dump through
# every JSON fixture at docs/fixtures/settings/ and asserts byte-for-byte
# parity vs the Python oracle (python/config.py).
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_settings_fixture_parity.cpp")
    find_package(nlohmann_json 3 CONFIG REQUIRED)
    add_executable(sao_platform_core_settings_fixture_parity_tests
        test_settings_fixture_parity.cpp
    )
    target_link_libraries(sao_platform_core_settings_fixture_parity_tests
        PRIVATE
            sao::core
            nlohmann_json::nlohmann_json
            Catch2::Catch2WithMain
    )
    target_compile_features(sao_platform_core_settings_fixture_parity_tests
        PRIVATE cxx_std_20)
    target_compile_definitions(sao_platform_core_settings_fixture_parity_tests
        PRIVATE SAO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/docs/fixtures/settings")
    add_test(NAME sao_platform_core_settings_fixture_parity_tests
             COMMAND sao_platform_core_settings_fixture_parity_tests)
    set_tests_properties(sao_platform_core_settings_fixture_parity_tests PROPERTIES
        LABELS "settings;fixture_parity")
endif()

# ── gap closure tests for platform/core ──────────────────────────────
# Exercises the crypto/window capability gates and the
# thread/event/path/string/time real cross-platform implementations that
# used to sit behind NOT_IMPLEMENTED stubs.  Hermetic — no driver, no
# real Npcap, no zstd stream, no external process spawn.
add_executable(sao_platform_core_gap_tests
    test_gap_core.cpp
)
target_link_libraries(sao_platform_core_gap_tests PRIVATE
    sao::core
    sao_core_gap_support_lib
    Catch2::Catch2WithMain
)
target_compile_features(sao_platform_core_gap_tests PRIVATE cxx_std_20)
add_test(NAME sao_platform_core_gap_tests COMMAND sao_platform_core_gap_tests)
set_tests_properties(sao_platform_core_gap_tests PROPERTIES
    LABELS "core;gap_closure")
