# sao_plugins_python_host is a STATIC lib, but the public header decorates
# every entry point with SAO_PLUGINS_API which expands to __declspec(dllimport)
# unless SAO_PLUGINS_BUILDING_DLL is defined at compile time. Test binaries
# that link the STATIC lib therefore need the same define to get plain
# (undecorated) prototypes and avoid __imp_* unresolved externals.

add_executable(test_plugins_python_host test_py_host.cpp)
target_link_libraries(test_plugins_python_host PRIVATE sao_plugins_python_host)
target_compile_features(test_plugins_python_host PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_python_host PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_python_host_smoke COMMAND test_plugins_python_host)

# Wave 2: CPython init/finalize/version 真链路 (Python3::Python link'd 才真跑,
# 否则 main 打印 skipped 退出 0).
add_executable(test_plugins_python_host_wave2 test_py_host_wave2.cpp)
target_link_libraries(test_plugins_python_host_wave2 PRIVATE sao_plugins_python_host)
target_compile_features(test_plugins_python_host_wave2 PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_python_host_wave2 PRIVATE SAO_PLUGINS_BUILDING_DLL)
add_test(NAME plugins_python_host_wave2 COMMAND test_plugins_python_host_wave2)

# Wave 7: 真实老 Python 插件 (star_resonance / hide_seek / midi_piano) 加载
# —— 不改一个字, 用 Catch2. 缺 Catch2 或缺 Python embed 时不编 / SKIP.
find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_python_host_wave7
        test_pyhost_real_plugins_wave7.cpp
    )
    target_link_libraries(test_plugins_python_host_wave7 PRIVATE
        sao_plugins_python_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_python_host_wave7 PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_python_host_wave7 PRIVATE
        SAO_PLUGINS_BUILDING_DLL  # 让 STATIC lib 符号无 dllimport 前缀
    )
    if (MSVC)
        target_compile_options(test_plugins_python_host_wave7 PRIVATE /GR /Zc:preprocessor)
    endif()

    # 真实老 Python 插件目录 (相对 repo 顶层). 定义为 macro 让测试代码用.
    set(_SAO_PY_PLUGINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../python/plugins")
    get_filename_component(_SAO_PY_PLUGINS_DIR "${_SAO_PY_PLUGINS_DIR}" ABSOLUTE)
    target_compile_definitions(test_plugins_python_host_wave7 PRIVATE
        "SAO_TEST_PLUGIN_STAR_RESONANCE=\"${_SAO_PY_PLUGINS_DIR}/star_resonance_plugin\""
        "SAO_TEST_PLUGIN_HIDE_SEEK=\"${_SAO_PY_PLUGINS_DIR}/hide_seek_plugin\""
        "SAO_TEST_PLUGIN_MIDI_PIANO=\"${_SAO_PY_PLUGINS_DIR}/midi_piano_plugin\""
    )
    add_test(NAME plugins_python_host_wave7 COMMAND test_plugins_python_host_wave7)
else()
    message(STATUS "plugins/python_host: Catch2 not found, skipping wave7 real-plugin tests")
endif()
