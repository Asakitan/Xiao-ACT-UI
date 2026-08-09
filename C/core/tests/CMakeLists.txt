if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()
list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})

set(sao_core_test_sources test_core.cpp)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_logging.cpp")
    list(APPEND sao_core_test_sources test_logging.cpp)
endif()
if(TARGET sao::core AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_combined_abi.cpp")
    list(APPEND sao_core_test_sources test_combined_abi.cpp)
endif()

add_executable(sao_core_tests ${sao_core_test_sources})

if(TARGET sao::core)
    target_link_libraries(sao_core_tests PRIVATE sao::core sao::legacy_core Catch2::Catch2WithMain)
else()
    target_link_libraries(sao_core_tests PRIVATE sao::legacy_core Catch2::Catch2WithMain)
endif()

include(CTest)
include(Catch)
add_test(NAME legacy_core COMMAND sao_core_tests)
set_tests_properties(legacy_core PROPERTIES LABELS "core;behavior")

if(TARGET sao::core AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/launcher_core_link_smoke.cpp")
    add_executable(sao_launcher_core_link_smoke launcher_core_link_smoke.cpp)
    target_link_libraries(sao_launcher_core_link_smoke PRIVATE sao::core sao::legacy_core)
    add_test(NAME legacy_core.launcher_core_link_smoke COMMAND sao_launcher_core_link_smoke)
    set_tests_properties(legacy_core.launcher_core_link_smoke PROPERTIES
        LABELS "core;launcher;link-smoke")
endif()
