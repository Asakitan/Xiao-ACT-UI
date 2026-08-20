if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_executable(sao_platform_sdk_tests
    test_sdk.cpp
    test_sdk_memory_provider.cpp
    test_sdk_net_provider.cpp
    test_sdk_provider.cpp
    test_render_clock_integration.cpp
    test_sdk_register_demo.cpp
)

target_sources(sao_platform_sdk_tests PRIVATE ${SAO_PLATFORM_SDK_SOURCES})

target_include_directories(sao_platform_sdk_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_definitions(sao_platform_sdk_tests PRIVATE
    SAO_SDK_TESTING=1
)

target_link_libraries(sao_platform_sdk_tests PRIVATE
    nlohmann_json::nlohmann_json
    sao::core
    sao::engine
    sao::ui
    sao::net
    sao::gpu_hunt
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_sdk_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
add_test(NAME sao_platform_sdk_tests COMMAND sao_platform_sdk_tests)
set_tests_properties(sao_platform_sdk_tests PROPERTIES TIMEOUT 15)
