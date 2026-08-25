# platform/mem_probe — user-mode memory scan / pointer-chain / engine probes.
#
# Phase 4-6 (Python parity closure):
#   scanner.cpp       — first-scan + narrow + predicate (CE-style multi-frame)
#   pointer_chain.cpp — BFS backtrace to module static section + resolve
#   engine/*.cpp      — il2cpp / mono / unreal / native adapter
#
# Depends only on platform/core (process/memory/module iter) — no rt_io kernel
# path required (all user-mode ReadProcessMemory / VirtualQueryEx).

add_library(sao_platform_mem_probe STATIC
    src/scanner.cpp
    src/pointer_chain.cpp
    src/anchor_store.cpp
    src/engine/detector.cpp
    src/engine/il2cpp_adapter.cpp
    src/engine/mono_adapter.cpp
    src/engine/unreal_adapter.cpp
    src/engine/native_adapter.cpp
    # Phase 14 utility parity closure.
    src/cpu_tracker.cpp
    src/mem_viewer.cpp
)
add_library(sao::mem_probe ALIAS sao_platform_mem_probe)

target_include_directories(sao_platform_mem_probe
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(sao_platform_mem_probe PUBLIC cxx_std_20)

target_link_libraries(sao_platform_mem_probe
    PUBLIC sao::core
)

find_package(nlohmann_json 3 CONFIG REQUIRED)
target_link_libraries(sao_platform_mem_probe PRIVATE nlohmann_json::nlohmann_json)

if (WIN32)
    target_link_libraries(sao_platform_mem_probe PUBLIC
        kernel32
        psapi
        dbghelp
    )
endif()

# AVX2 SIMD scan kernel — auto-detected at runtime; compile with /arch:AVX2
# on MSVC so intrinsics are available. Fallback scalar path always compiled.
if (MSVC)
    set_source_files_properties(src/scanner.cpp
        PROPERTIES COMPILE_FLAGS "/arch:AVX2")
endif()

