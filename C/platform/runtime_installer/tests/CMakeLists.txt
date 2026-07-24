if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_executable(sao_runtime_installer_tests
    test_runtime_installer.cpp
)

target_link_libraries(sao_runtime_installer_tests PRIVATE
    sao::runtime_installer
    Catch2::Catch2WithMain
    bcrypt
    ole32
)

target_compile_features(sao_runtime_installer_tests PRIVATE cxx_std_20)

# The tests link against the runtime_installer DLL and exercise the test
# hooks (transport override + root override), so they must compile with
# both SAO_RUNTIME_INSTALLER_USING_DLL and SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS.
target_compile_definitions(sao_runtime_installer_tests PRIVATE
    SAO_RUNTIME_INSTALLER_USING_DLL
    SAO_RUNTIME_INSTALLER_ENABLE_TEST_HOOKS)

include(CTest)
include(Catch)
catch_discover_tests(sao_runtime_installer_tests
    TEST_PREFIX "runtime_installer."
    PROPERTIES LABELS "platform;runtime_installer")
