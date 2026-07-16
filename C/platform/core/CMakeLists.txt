# platform/core — universal runtime primitives.
#
# Built STATIC on purpose: every other platform module links it directly,
# and its footprint (roughly the surface of `mem_probe/` + a couple of
# helpers) is small enough that duplicated symbols are not a concern.

add_library(sao_platform_core STATIC
    src/abi.cpp
    src/status.cpp
    src/logging.cpp
    src/config.cpp
    src/crypto.cpp
    src/memory.cpp
    src/process.cpp
    src/window.cpp
    src/time.cpp
    src/string.cpp
    src/path.cpp
    src/thread.cpp
    src/event.cpp
    src/error.cpp
    # Wave 6 (Phase 4) - game-agnostic engine detection + IL2CPP probe.
    src/engine_detector.cpp
    src/il2cpp_probe.cpp
)
add_library(sao::core ALIAS sao_platform_core)

target_include_directories(sao_platform_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(sao_platform_core PUBLIC cxx_std_20)

# Wave 5 / Phase 1 — settings envelope uses nlohmann::json for parse +
# shortest-round-trip float formatting.  The dependency is header-only,
# so we only need PRIVATE (its symbols never appear in sao/core/*.h).
find_package(nlohmann_json 3 CONFIG REQUIRED)
target_link_libraries(sao_platform_core PRIVATE nlohmann_json::nlohmann_json)

if (WIN32)
    target_link_libraries(sao_platform_core PUBLIC
        # Win32 primitives used by process/window/memory/time.
        kernel32
        user32
        advapi32
        bcrypt
    )
endif()

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
