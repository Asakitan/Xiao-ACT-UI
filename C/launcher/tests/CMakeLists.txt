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

# Optional launcher test sources are discovered with file(EXISTS) so slim
# source sets continue to configure.
set(_sao_launcher_test_sources
    test_launcher_smoke.cpp
    test_args.cpp
    test_single_instance.cpp
    test_stealth_strings.cpp
    test_working_dir.cpp
    test_init_pipeline.cpp
    test_entity_builtin_action_focused.cpp
    test_entity_action_routes_focused.cpp
    test_settings_owner_focused.cpp
    test_tool_launch_focused.cpp
)
# Rollout tests join the same discovery scheme
# so an operator building a slim launcher can drop them without CMake edits.
foreach (_maybe
    test_dual_run.cpp
    test_cutover_diff.cpp
    test_rollout.cpp
    test_rollout_admin.cpp)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_maybe}")
        list(APPEND _sao_launcher_test_sources ${_maybe})
    endif()
endforeach()

set(_sao_launcher_panel_test_libraries)
if (TARGET sao::ui)
    list(APPEND _sao_launcher_test_sources
        test_plugin_manager_panel_focused.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/plugin_manager_panel_internal.cpp)
    list(APPEND _sao_launcher_panel_test_libraries sao::ui)
endif()
if (TARGET sao::core AND TARGET sao::ui AND TARGET sao::rt_io)
    list(APPEND _sao_launcher_test_sources
        test_process_selector_panel_focused.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/process_selector_panel_internal.cpp)
    list(APPEND _sao_launcher_panel_test_libraries
        sao::core
        sao::ui
        sao::rt_io)
endif()
if (TARGET sao::ui AND TARGET sao::server::freetier::workshop_client)
    list(APPEND _sao_launcher_test_sources
        test_workshop_panel_focused.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/workshop_panel_internal.cpp)
    list(APPEND _sao_launcher_panel_test_libraries
        sao::ui
        sao::server::freetier::workshop_client)
endif()
list(REMOVE_DUPLICATES _sao_launcher_panel_test_libraries)
add_executable(sao_launcher_tests ${_sao_launcher_test_sources})

add_executable(sao_tool_launch_child_fixture
    tool_launch_child_fixture.cpp)
target_compile_features(sao_tool_launch_child_fixture PRIVATE cxx_std_20)
target_link_libraries(sao_tool_launch_child_fixture PRIVATE user32)

target_include_directories(sao_launcher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/platform/core/include
    ${CMAKE_SOURCE_DIR}/platform/ui/include
    ${CMAKE_SOURCE_DIR}/plugins/include
    ${CMAKE_SOURCE_DIR}/plugins/loader/include
    # SAO_ENC_STR() header-only compile-time string encryption; no link dep.
    ${CMAKE_SOURCE_DIR}/security/obfuscation/include
)

# We link the launcher's source files directly rather than the SaoAuto exe
# target so tests can call helpers without going through wWinMain.
target_sources(sao_launcher_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/args.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/single_instance.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/working_dir.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/crash_handler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/app.cpp
    # Headless init pipeline + shutdown so the
    # init pipeline tests can drive the full sequence with fail-closed stubs.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/init_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shutdown.cpp
    # Dual-run driver.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/dual_run.cpp
    # Rollout manager + dispatcher.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rollout_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rollout_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/provider_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/launcher_lifecycle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/user_menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/settings_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/settings_json_internal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/settings_owner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tool_launch_internal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/tool_launch_package_internal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/entity_builtin_action_internal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/entity_provider_publication_internal.cpp
)

target_compile_definitions(sao_launcher_tests PRIVATE
    SAO_LAUNCHER_COMPOSITION_TEST_PROVIDER=1)

# Cutover tests need the tools/cutover_diff exe path baked in so
# ``test_cutover_diff`` can spawn and diff two runs.  When the
# tools tree isn't built (SAO_BUILD_TOOLS=OFF), fall back to a name-only
# lookup — the cutover_diff test will WARN + skip its subprocess assertions.
target_compile_definitions(sao_launcher_tests
    PRIVATE
        SAO_PYTHON_ROOT_HINT="${CMAKE_CURRENT_SOURCE_DIR}/../../../python"
        SAO_CPP_ROOT_HINT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
        SAO_TOOL_LAUNCH_CHILD_FIXTURE="$<TARGET_FILE:sao_tool_launch_child_fixture>"
)

# Stealth-strings regression test needs the built launcher exe (and
# optionally sao_platform_net / sao_core shared libraries) to scan.
# Resolve lazily: SaoAuto target lives in the parent launcher/ scope which
# is add_subdirectory'd BEFORE tests/, so it is already defined here.  If
# a slim configure disables a given target the define stays empty and the
# corresponding case degrades to SUCCEED(... skipped).
if (TARGET SaoAuto)
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_STEALTH_SCAN_BINARY_PATH="$<TARGET_FILE:SaoAuto>"
    )
    add_dependencies(sao_launcher_tests SaoAuto)
else()
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_STEALTH_SCAN_BINARY_PATH=""
    )
endif()
if (TARGET sao_platform_net)
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_STEALTH_SCAN_PLATFORM_NET_PATH="$<TARGET_FILE:sao_platform_net>"
    )
    add_dependencies(sao_launcher_tests sao_platform_net)
else()
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_STEALTH_SCAN_PLATFORM_NET_PATH=""
    )
endif()
if (TARGET sao_core)
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_STEALTH_SCAN_CORE_PATH="$<TARGET_FILE:sao_core>"
    )
    add_dependencies(sao_launcher_tests sao_core)
else()
    target_compile_definitions(sao_launcher_tests
        PRIVATE
            SAO_STEALTH_SCAN_CORE_PATH=""
    )
endif()
add_dependencies(sao_launcher_tests sao_tool_launch_child_fixture)
if (TARGET sao::plugin_ai_editor)
    target_link_libraries(sao_launcher_tests PRIVATE sao::plugin_ai_editor)
    target_compile_definitions(sao_launcher_tests PRIVATE
        SAO_LAUNCHER_HAS_AI_EDITOR_ABI=1)
endif()
if (TARGET sao::sdk AND TARGET sao::ui)
    target_link_libraries(sao_launcher_tests PRIVATE sao::sdk sao::ui)
endif()
if (_sao_launcher_panel_test_libraries)
    target_link_libraries(sao_launcher_tests PRIVATE
        ${_sao_launcher_panel_test_libraries})
endif()
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

# sao_rollout_admin CLI target lookup.
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
        dwmapi
        shell32
        advapi32
        shlwapi
        dbghelp
        version
        bcrypt          # HMAC-SHA-256 for rollout bucketing
        crypt32         # current-user DPAPI for secure settings envelopes
        nlohmann_json::nlohmann_json
)
if (TARGET sao::core)
    target_link_libraries(sao_launcher_tests PRIVATE sao::core)
    target_compile_definitions(sao_launcher_tests PRIVATE
        SAO_LAUNCHER_CORE_LOG_PROVIDER=1)
endif()

# rollout_manager.cpp guards its telemetry_client
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
    PROPERTIES
        LABELS "launcher"
        RESOURCE_LOCK "sao_launcher_process_resources"
)

if (TARGET sao_launcher_plugins_provider)
    add_executable(sao_launcher_provider_tests
        test_provider_config_focused.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/provider_config.cpp)
    target_include_directories(sao_launcher_provider_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    target_compile_features(sao_launcher_provider_tests PRIVATE cxx_std_20)
    target_compile_definitions(sao_launcher_provider_tests PRIVATE
        SAO_PLUGINS_BUILDING_DLL)
    target_link_libraries(sao_launcher_provider_tests PRIVATE
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
        sao_launcher_plugins_provider)
    if (TARGET sao::sdk AND TARGET sao::ui)
        target_link_libraries(sao_launcher_provider_tests PRIVATE
            sao::sdk sao::ui)
        target_compile_definitions(sao_launcher_provider_tests PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_PLATFORM_SDK=1)
    endif()
    if (TARGET sao::plugins::python_host)
        target_link_libraries(sao_launcher_provider_tests PRIVATE
            sao::plugins::python_host)
        target_compile_definitions(sao_launcher_provider_tests PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_PYTHON=1)
    endif()
    if (TARGET sao::plugins::emma_host)
        target_link_libraries(sao_launcher_provider_tests PRIVATE
            sao::plugins::emma_host)
        target_compile_definitions(sao_launcher_provider_tests PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_EMMA=1)
    endif()
    if (TARGET sao::plugins::angel_host)
        target_link_libraries(sao_launcher_provider_tests PRIVATE
            sao::plugins::angel_host)
        target_compile_definitions(sao_launcher_provider_tests PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_ANGEL=1)
    endif()
    if (TARGET sao::plugins::lua_host)
        target_link_libraries(sao_launcher_provider_tests PRIVATE
            sao::plugins::lua_host)
        target_compile_definitions(sao_launcher_provider_tests PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_LUA=1)
    endif()
    if (TARGET sao::plugins::csharp_host)
        target_link_libraries(sao_launcher_provider_tests PRIVATE
            sao::plugins::csharp_host)
        target_compile_definitions(sao_launcher_provider_tests PRIVATE
            SAO_LAUNCHER_PROVIDER_HAS_CSHARP=1)
    endif()
    catch_discover_tests(sao_launcher_provider_tests
        TEST_PREFIX "launcher.provider."
        PROPERTIES LABELS "launcher;plugins;focused")
endif()

if (TARGET sao_launcher_plugins_provider AND
    TARGET sao::plugins::csharp_host AND
    TARGET sao::plugins::loader AND
    TARGET sao::ui AND
    TARGET sao::sdk AND
    TARGET sao_csharp_managed_entity_fixture AND
    TARGET Catch2::Catch2WithMain)
    set(_sao_csharp_entity_fixture_root
        "${CMAKE_BINARY_DIR}/plugins/csharp_host/tests/managed_entity_fixture")
    set(_sao_csharp_entity_fixture_a
        "${_sao_csharp_entity_fixture_root}/entity_a")
    set(_sao_csharp_entity_fixture_b
        "${_sao_csharp_entity_fixture_root}/entity_b")

    add_executable(sao_launcher_csharp_entity_tests
        test_csharp_entity_integration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/provider_config.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/entity_provider_publication_internal.cpp)
    add_dependencies(sao_launcher_csharp_entity_tests
        sao_csharp_managed_entity_fixture)
    target_include_directories(sao_launcher_csharp_entity_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_SOURCE_DIR}/platform/core/include
        ${CMAKE_SOURCE_DIR}/platform/ui/include
        ${CMAKE_SOURCE_DIR}/platform/sdk/include
        ${CMAKE_SOURCE_DIR}/plugins/include
        ${CMAKE_SOURCE_DIR}/plugins/loader/include
        ${CMAKE_SOURCE_DIR}/plugins/csharp_host/include)
    target_compile_features(sao_launcher_csharp_entity_tests PRIVATE cxx_std_20)
    target_compile_definitions(sao_launcher_csharp_entity_tests PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        SAO_CSHARP_ENTITY_A_DLL="${_sao_csharp_entity_fixture_a}/HelloPluginEntityA.dll"
        SAO_CSHARP_ENTITY_A_RUNTIMECONFIG="${_sao_csharp_entity_fixture_a}/HelloPluginEntityA.runtimeconfig.json"
        SAO_CSHARP_ENTITY_B_DLL="${_sao_csharp_entity_fixture_b}/HelloPluginEntityB.dll"
        SAO_CSHARP_ENTITY_B_RUNTIMECONFIG="${_sao_csharp_entity_fixture_b}/HelloPluginEntityB.runtimeconfig.json")
    if (MSVC)
        target_compile_options(sao_launcher_csharp_entity_tests PRIVATE /GR /EHsc)
    endif()
    target_link_libraries(sao_launcher_csharp_entity_tests PRIVATE
        Catch2::Catch2WithMain
        nlohmann_json::nlohmann_json
        sao_launcher_plugins_provider
        sao::plugins::csharp_host
        sao::plugins::loader
        sao::ui
        sao::sdk)

    catch_discover_tests(sao_launcher_csharp_entity_tests
        TEST_PREFIX "launcher.csharp_entity."
        PROPERTIES
            LABELS "launcher;plugins;csharp;entity;v2;headless"
            RESOURCE_LOCK "sao_launcher_process_resources")
endif()
