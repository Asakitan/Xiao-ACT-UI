add_library(sao_core SHARED
    src/sao_core.cpp
    src/legacy_binary_compat.cpp
    src/legacy_binary_compat.def
    src/logging.cpp
    src/process.cpp
    src/class_index.cpp
    src/scan.cpp
    src/pixels.cpp
    src/window.cpp
)
add_library(sao::legacy_core ALIAS sao_core)

target_compile_definitions(sao_core PRIVATE SAO_LEGACY_CORE_BUILDING_DLL)

target_include_directories(sao_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# SAO_ENC_STR() header-only compile-time string encryption; no link dep.
target_include_directories(sao_core PRIVATE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/security/obfuscation/include>)

target_link_libraries(sao_core PRIVATE user32)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
