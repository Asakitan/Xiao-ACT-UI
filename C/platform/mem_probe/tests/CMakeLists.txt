find_package(Catch2 3 QUIET CONFIG)
if (NOT Catch2_FOUND AND NOT TARGET Catch2::Catch2WithMain)
    message(STATUS "mem_probe: Catch2 not found, skipping tests")
    return()
endif()

add_executable(test_mem_probe_scanner
    test_scanner.cpp
    test_pointer_chain.cpp
    test_anchor_store.cpp
    test_engine_detector.cpp
)
target_link_libraries(test_mem_probe_scanner PRIVATE
    sao::mem_probe
    sao::core
    Catch2::Catch2WithMain
)
target_compile_features(test_mem_probe_scanner PRIVATE cxx_std_20)
add_test(NAME mem_probe_scanner COMMAND test_mem_probe_scanner)
set_tests_properties(mem_probe_scanner
    PROPERTIES LABELS "mem_probe;focused")
