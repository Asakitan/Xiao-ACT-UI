# sao_plugins_python_host is a STATIC lib, but the public header decorates
# every entry point with SAO_PLUGINS_API which expands to __declspec(dllimport)
# unless SAO_PLUGINS_BUILDING_DLL is defined at compile time. Test binaries
# that link the STATIC lib therefore need the same define to get plain
# (undecorated) prototypes and avoid __imp_* unresolved externals.

set(_SAO_TEST_PYTHON_HOME "")
if (Python3_FOUND)
    if (Python3_EXECUTABLE)
        get_filename_component(_SAO_TEST_PYTHON_HOME
            "${Python3_EXECUTABLE}" DIRECTORY)
    elseif (Python3_LIBRARIES)
        list(GET Python3_LIBRARIES 0 _SAO_TEST_PYTHON_LIBRARY)
        get_filename_component(_SAO_TEST_PYTHON_LIB_DIR
            "${_SAO_TEST_PYTHON_LIBRARY}" DIRECTORY)
        get_filename_component(_SAO_TEST_PYTHON_HOME
            "${_SAO_TEST_PYTHON_LIB_DIR}" DIRECTORY)
    endif()
    file(TO_CMAKE_PATH "${_SAO_TEST_PYTHON_HOME}" _SAO_TEST_PYTHON_HOME)
endif()

add_executable(test_plugins_python_host test_py_host.cpp)
target_link_libraries(test_plugins_python_host PRIVATE sao_plugins_python_host)
target_compile_features(test_plugins_python_host PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_python_host PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    "SAO_TEST_PYTHON_HOME=L\"${_SAO_TEST_PYTHON_HOME}\""
)
add_test(NAME plugins_python_host_smoke COMMAND test_plugins_python_host)
set_tests_properties(plugins_python_host_smoke PROPERTIES TIMEOUT 15)

# CPython init/finalize/version 真链路。未链接 Python3::Python 时，
# 可执行文件报告 capability skip 并返回成功。
add_executable(test_plugins_python_host_embed test_py_host_embed.cpp)
target_link_libraries(test_plugins_python_host_embed PRIVATE
    sao_plugins_python_host
)
target_compile_features(test_plugins_python_host_embed PRIVATE cxx_std_17)
target_compile_definitions(test_plugins_python_host_embed PRIVATE
    SAO_PLUGINS_BUILDING_DLL
    "SAO_TEST_PYTHON_HOME=L\"${_SAO_TEST_PYTHON_HOME}\""
)
add_test(NAME plugins_python_host_embed COMMAND test_plugins_python_host_embed)
set_tests_properties(plugins_python_host_embed PROPERTIES TIMEOUT 15)

# 真实老 Python 插件 (star_resonance / hide_seek / midi_piano) 加载
# —— 不改一个字, 用 Catch2. 缺 Catch2 或缺 Python embed 时不编 / SKIP.
find_package(Catch2 3 QUIET CONFIG)
if (Catch2_FOUND OR TARGET Catch2::Catch2WithMain)
    add_executable(test_plugins_python_host_real_plugins
        test_pyhost_real_plugins.cpp
    )
    target_link_libraries(test_plugins_python_host_real_plugins PRIVATE
        sao_plugins_python_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_python_host_real_plugins PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_python_host_real_plugins PRIVATE
        SAO_PLUGINS_BUILDING_DLL  # 让 STATIC lib 符号无 dllimport 前缀
        "SAO_TEST_PYTHON_HOME=L\"${_SAO_TEST_PYTHON_HOME}\""
    )
    if (MSVC)
        target_compile_options(test_plugins_python_host_real_plugins PRIVATE /GR /Zc:preprocessor)
    endif()

    # 真实老 Python 插件目录 (相对 repo 顶层). 定义为 macro 让测试代码用.
    set(_SAO_PY_PLUGINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../python/plugins")
    get_filename_component(_SAO_PY_PLUGINS_DIR "${_SAO_PY_PLUGINS_DIR}" ABSOLUTE)
    target_compile_definitions(test_plugins_python_host_real_plugins PRIVATE
        "SAO_TEST_PLUGIN_STAR_RESONANCE=\"${_SAO_PY_PLUGINS_DIR}/star_resonance_plugin\""
        "SAO_TEST_PLUGIN_HIDE_SEEK=\"${_SAO_PY_PLUGINS_DIR}/hide_seek_plugin\""
        "SAO_TEST_PLUGIN_MIDI_PIANO=\"${_SAO_PY_PLUGINS_DIR}/midi_piano_plugin\""
    )
    add_test(NAME plugins_python_host_real_plugins COMMAND test_plugins_python_host_real_plugins)
    set_tests_properties(plugins_python_host_real_plugins PROPERTIES TIMEOUT 30)

    # py_sandbox 真实装 (子解释器 + import 白名单/黑名单)。
    # 与 real-plugins 测试共用 Catch2 gate；无 Python embed 时内部自 SKIP。
    add_executable(test_plugins_python_host_sandbox
        test_py_sandbox.cpp
    )
    target_link_libraries(test_plugins_python_host_sandbox PRIVATE
        sao_plugins_python_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_python_host_sandbox PRIVATE cxx_std_17)
    target_compile_definitions(test_plugins_python_host_sandbox PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        "SAO_TEST_PYTHON_HOME=L\"${_SAO_TEST_PYTHON_HOME}\""
    )
    if (MSVC)
        target_compile_options(test_plugins_python_host_sandbox PRIVATE
            /GR /Zc:preprocessor)
    endif()
    add_test(NAME plugins_python_host_sandbox
             COMMAND test_plugins_python_host_sandbox)
    set_tests_properties(plugins_python_host_sandbox
        PROPERTIES LABELS "plugins;python;sandbox" TIMEOUT 30)

    add_executable(test_plugins_python_host_adapter
        test_py_loader_adapter.cpp
    )
    target_link_libraries(test_plugins_python_host_adapter PRIVATE
        sao_plugins_python_host
        Catch2::Catch2WithMain
    )
    target_compile_features(test_plugins_python_host_adapter PRIVATE cxx_std_20)
    target_compile_definitions(test_plugins_python_host_adapter PRIVATE
        SAO_PLUGINS_BUILDING_DLL
        "SAO_TEST_PYTHON_HOME=L\"${_SAO_TEST_PYTHON_HOME}\""
    )
    if (MSVC)
        target_compile_options(test_plugins_python_host_adapter PRIVATE
            /GR /Zc:preprocessor)
    endif()
    add_test(NAME plugins_python_host_adapter
             COMMAND test_plugins_python_host_adapter)
    set_tests_properties(plugins_python_host_adapter
        PROPERTIES LABELS "plugins;python;adapter;focused" TIMEOUT 30)
else()
    message(STATUS "plugins/python_host: Catch2 not found, skipping real_plugins / sandbox tests")
endif()
