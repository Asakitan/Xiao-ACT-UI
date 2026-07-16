find_package(Catch2 3 CONFIG REQUIRED)

add_executable(sao_platform_sdk_tests
    test_sdk.cpp
    test_sdk_provider.cpp
    test_render_clock_integration.cpp
    test_sdk_register_demo_wave7.cpp
)

target_link_libraries(sao_platform_sdk_tests PRIVATE
    sao::sdk
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_sdk_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
catch_discover_tests(sao_platform_sdk_tests)
