# platform/sdk — the plugin ABI (single-include header + dispatch DLL).
#
# SHARED because plugins LoadLibrary/GetProcAddress against sao_sdk.dll
# to discover the ABI probe function.

add_library(sao_platform_sdk SHARED
    src/sdk_export.cpp

    # Wave 7 — context lifecycle + capability wires.  Every source below
    # forwards from ``SaoSdkContext::*->*`` (or the free-function
    # wrappers in ``sao_sdk.h``) into the corresponding platform
    # module.  Nothing here imports plugins — plugins only see the SDK
    # umbrella header.
    src/sdk_context.cpp     # sao_sdk_context_create/destroy + accessors
    src/sdk_ui_wire.cpp     # panel register + widget CRUD + render hook + redraw
    src/sdk_event_wire.cpp  # event bus subscribe/unsubscribe/publish (wave5)
    src/sdk_hotkey_wire.cpp # input_router_deep hotkey register/unregister
    src/sdk_provider.cpp    # provider implementation
)
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
    PRIVATE sao::core sao::engine sao::ui sao::net
)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
