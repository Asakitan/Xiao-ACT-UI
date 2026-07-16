# SAO Auto — launcher/tests
#
# Small unit-tests for argv parsing, single-instance mutex, and
# working-dir resolution.  Anything that needs a real UI stays in the
# top-level tests/integration/ tree.

find_package(Catch2 3 CONFIG QUIET)
if (NOT Catch2_FOUND)
    message(STATUS "launcher/tests: Catch2 not found, tests skipped")
    return()
endif()

# Wave 8 phase 12 test cpps 由并发 agent 后续 land;
# 用 file(EXISTS) 判断可用源, 保持 configure 通过.
set(_sao_launcher_test_sources
    test_launcher_smoke.cpp
    test_args.cpp
    test_single_instance.cpp
    test_working_dir.cpp
    test_init_pipeline_wave5.cpp
)
# Wave 10 / Agent a — Phase 12 rollout tests join the same discovery scheme
# so an operator building a slim launcher can drop them without CMake edits.
foreach (_maybe
    test_dual_run_phase12.cpp
    test_cutover_diff_phase12.cpp
    test_rollout_wave10.cpp
    test_rollout_admin_wave10.cpp)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_maybe}")
        list(APPEND _sao_launcher_test_sources ${_maybe})
    endif()
endforeach()
add_executable(sao_launcher_tests ${_sao_launcher_test_sources})

target_include_directories(sao_launcher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# We link the launcher's source files directly rather than the SaoAuto exe
# target so tests can call helpers without going through wWinMain.
target_sources(sao_launcher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/args.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/single_instance.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/working_dir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/crash_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/app.cpp
    # Wave 5 / Phase 1 — headless init pipeline + shutdown so the
    # wave5 tests can drive the full sequence with fail-closed stubs.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/init_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shutdown.cpp
    # Wave 8 / Phase 12 — dual-run driver.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/dual_run.cpp
    # Wave 10 / Agent a — Phase 12 rollout manager + dispatcher.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rollout_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rollout_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/provider_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/launcher_lifecycle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/user_menu.cpp
)

target_compile_definitions(sao_launcher_tests PRIVATE
    SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER=1)

# Phase 12 tests need the tools/cutover_diff exe path baked in so
# ``test_cutover_diff_phase12`` can spawn and diff two runs.  When the
# tools tree isn't built (SAO_BUILD_TOOLS=OFF), fall back to a name-only
# lookup — the cutover_diff test will WARN + skip its subprocess assertions.
target_compile_definitions(sao_launcher_tests
    PRIVATE
        SAO_PYTHON_ROOT_HINT="${CMAKE_CURRENT_SOURCE_DIR}/../../../python"
        SAO_CPP_ROOT_HINT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
if (TARGET cutover_diff)
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_CUTOVER_DIFF_EXE_DIR="$<TARGET_FILE_DIR:cutover_diff>"
            SAO_CUTOVER_DIFF_EXE_NAME="$<TARGET_FILE_NAME:cutover_diff>"
    )
    add_dependencies(sao_launcher_tests cutover_diff)
else()
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_CUTOVER_DIFF_EXE_DIR=""
            SAO_CUTOVER_DIFF_EXE_NAME=""
    )
endif()

# Wave 10 / Agent a — sao_rollout_admin CLI target lookup.
#
# tools/ is add_subdirectory'd AFTER launcher/ in the top-level CMakeLists,
# so at THIS point in configure `TARGET sao_rollout_admin` is not yet
# defined.  Use a generator expression with $<TARGET_EXISTS:...> so the
# path resolves at build time when tools have already been visited.
target_compile_definitions(sao_launcher_tests
    PRIVATE
        SAO_ROLLOUT_ADMIN_EXE_DIR="$<IF:$<TARGET_EXISTS:sao_rollout_admin>,$<TARGET_FILE_DIR:sao_rollout_admin>,>"
        SAO_ROLLOUT_ADMIN_EXE_NAME="$<IF:$<TARGET_EXISTS:sao_rollout_admin>,$<TARGET_FILE_NAME:sao_rollout_admin>,>"
)

# Defer the add_dependencies() to the very end of top-level configure so
# tools/ has time to declare sao_rollout_admin.  Without this the tool
# won't be built when the user runs `--target sao_launcher_tests` in
# isolation and the 4 admin cases will silently skip.
cmake_language(DEFER
    DIRECTORY ${CMAKE_SOURCE_DIR}
    CALL _sao_launcher_tests_wire_rollout_admin_dep)

function(_sao_launcher_tests_wire_rollout_admin_dep)
    if (TARGET sao_rollout_admin AND TARGET sao_launcher_tests)
        add_dependencies(sao_launcher_tests sao_rollout_admin)
    endif()
endfunction()

target_link_libraries(sao_launcher_tests
    PRIVATE
        Catch2::Catch2WithMain
        user32
        shell32
        advapi32
        shlwapi
        dbghelp
        version
        bcrypt          # HMAC-SHA-256 for rollout bucketing (Wave 10)
        nlohmann_json::nlohmann_json
)
if (TARGET sao::core)
    target_link_libraries(sao_launcher_tests PRIVATE sao::core)
    target_compile_definitions(sao_launcher_tests PRIVATE
        SAO_LAUNCHER_CORE_LOG_PROVIDER=1)
endif()

# Wave 10 / Agent a — rollout_manager.cpp guards its telemetry_client
# include behind SAO_LAUNCHER_HAS_TELEMETRY_CLIENT.  Tests set the
# telemetry hook to observe events, so we still enable the include path
# when the target is present so tests exercise the real client too.
if (TARGET sao::server::freetier::telemetry_client)
    target_link_libraries(sao_launcher_tests
        PRIVATE sao::server::freetier::telemetry_client)
    target_compile_definitions(sao_launcher_tests
        PRIVATE SAO_LAUNCHER_HAS_TELEMETRY_CLIENT=1)
endif()

include(Catch)
catch_discover_tests(sao_launcher_tests
    TEST_PREFIX "launcher."
    PROPERTIES LABELS "launcher"
)
