add_library(sao_core SHARED
    src/sao_core.cpp
    src/process.cpp
    src/class_index.cpp
    src/scan.cpp
    src/pixels.cpp
    src/window.cpp
)
add_library(sao::legacy_core ALIAS sao_core)

target_compile_definitions(sao_core PRIVATE SAO_CORE_BUILDING_DLL)

target_include_directories(sao_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(sao_core PRIVATE user32)

if (SAO_BUILD_TESTS)
    add_subdirectory(tests)
endif()
