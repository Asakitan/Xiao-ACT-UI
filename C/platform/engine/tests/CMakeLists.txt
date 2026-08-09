if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_executable(sao_platform_engine_tests
    test_engine.cpp
    test_event_bus_priority.cpp
    test_runtime_state_render.cpp
    # automation SDK primitives.
    test_state_machine.cpp
    test_history_ring.cpp
    test_trigger_engine.cpp
)

target_link_libraries(sao_platform_engine_tests PRIVATE
    sao::engine
    Catch2::Catch2WithMain
)

target_compile_definitions(sao_platform_engine_tests PRIVATE SAO_ENGINE_USING_DLL)
target_compile_features(sao_platform_engine_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
add_test(NAME sao_platform_engine_tests COMMAND sao_platform_engine_tests)

# ── event_bus fixture parity (8 pinned scenarios) ────────────────────
#
# Drives sao_engine_event_bus_subscribe_priority / _publish_priority /
# _unsubscribe through every JSON fixture at docs/fixtures/event_bus/
# and asserts subscriber_logs + published + retained + recent_topics +
# callback_failures parity vs the Python oracle
# (act_platform/event_bus.py).  Adds a small harness for wildcard "*"
# fan-out, ephemeral-topic set + retained counter, since the priority ABI
# doesn't carry those out of the box.
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_event_bus_fixture_parity.cpp")
    find_package(nlohmann_json 3 CONFIG REQUIRED)
    add_executable(sao_platform_engine_event_bus_fixture_parity_tests
        test_event_bus_fixture_parity.cpp
    )
    target_link_libraries(sao_platform_engine_event_bus_fixture_parity_tests
        PRIVATE
            sao::engine
            nlohmann_json::nlohmann_json
            Catch2::Catch2WithMain
    )
    target_compile_definitions(sao_platform_engine_event_bus_fixture_parity_tests
        PRIVATE
            SAO_ENGINE_USING_DLL
            SAO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/docs/fixtures/event_bus")
    target_compile_features(sao_platform_engine_event_bus_fixture_parity_tests
        PRIVATE cxx_std_20)
    add_test(NAME sao_platform_engine_event_bus_fixture_parity_tests
             COMMAND sao_platform_engine_event_bus_fixture_parity_tests)
    set_tests_properties(sao_platform_engine_event_bus_fixture_parity_tests PROPERTIES
        LABELS "event_bus;fixture_parity")
endif()

# ── gap closure tests for platform/engine ────────────────────────────
# Asserts the render_hook capability gate plus EventBus source wiring and
# executable production behavior.  Dead-source counts are inventory only.
# The shared support library lives under platform/core/tests so we don't
# duplicate object code across the three gap binaries.
add_executable(sao_platform_engine_gap_tests
    test_gap_engine.cpp
)
target_link_libraries(sao_platform_engine_gap_tests PRIVATE
    sao::engine
    sao_core_gap_support_lib
    Catch2::Catch2WithMain
)
target_compile_definitions(sao_platform_engine_gap_tests PRIVATE
    SAO_ENGINE_USING_DLL
    SAO_ENGINE_MODULE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(sao_platform_engine_gap_tests PRIVATE cxx_std_20)
add_test(NAME sao_platform_engine_gap_tests COMMAND sao_platform_engine_gap_tests)
set_tests_properties(sao_platform_engine_gap_tests PROPERTIES
    LABELS "engine;gap_closure")
