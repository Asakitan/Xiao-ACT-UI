find_package(Catch2 3 CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})

add_executable(sao_core_tests test_core.cpp)

target_link_libraries(sao_core_tests
    PRIVATE
        sao::legacy_core
        Catch2::Catch2WithMain
)

include(CTest)
include(Catch)
catch_discover_tests(sao_core_tests
    TEST_PREFIX "legacy_core."
    PROPERTIES LABELS "core;behavior"
)
