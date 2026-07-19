if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_executable(sao_platform_scripting_tests
    test_scripting.cpp
)

target_link_libraries(sao_platform_scripting_tests PRIVATE
    sao::scripting
    Catch2::Catch2WithMain
)

target_compile_features(sao_platform_scripting_tests PRIVATE cxx_std_20)

include(CTest)
include(Catch)
catch_discover_tests(sao_platform_scripting_tests
    PROPERTIES LABELS scripting
)
