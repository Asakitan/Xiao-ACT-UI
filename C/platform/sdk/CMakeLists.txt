# platform/sdk — the plugin ABI (single-include header + dispatch DLL).
#
# SHARED because plugins LoadLibrary/GetProcAddress against sao_sdk.dll
# to discover the ABI probe function.

set(SAO_PLATFORM_SDK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_export.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_ui_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_event_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_hotkey_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_gpu_hunt_wire.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_provider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_net_provider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_process_provider.cpp
    # Phase 7 (Python parity closure): TTS SAPI backend.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_tts_wire.cpp
    # Phase 8 (Python parity closure): SDK sound (procedural / font).
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk_sound_wire.cpp
)

add_library(sao_platform_sdk SHARED ${SAO_PLATFORM_SDK_SOURCES})
add_library(sao::sdk ALIAS sao_platform_sdk)

# NOTE: SAO_SDK_BUILDING_DLL is #defined at the top of sdk_export.cpp
# so the header expands to __declspec(dllexport) *only* while compiling
# the platform side.  Consumers include the same header and see
# __declspec(dllimport) instead, which is exactly what we want.

target_include_directories(sao_platform_sdk
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(sao_platform_sdk PUBLIC cxx_std_20)

# The SDK dispatch DLL forwards into engine/core/ui but does NOT expose
# their headers to plugins — plugins only see the SDK's public include.
target_link_libraries(sao_platform_sdk
    PRIVATE
        sao::core
        sao::engine
        sao::ui
        sao::net
        sao::gpu_hunt
        nlohmann_json::nlohmann_json
)

if (SAO_BUILD_TESTS)
    target_compile_definitions(sao_platform_sdk PRIVATE
        SAO_SDK_TESTING=1
    )
    add_subdirectory(tests)
endif()
