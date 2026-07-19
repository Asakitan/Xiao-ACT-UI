if (NOT TARGET Catch2::Catch2WithMain)
    find_package(Catch2 3 CONFIG REQUIRED)
endif()

add_library(sao_loader_native_fixture SHARED native_plugin_fixture.cpp)
target_include_directories(sao_loader_native_fixture PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../include
	${CMAKE_CURRENT_SOURCE_DIR}/../../include)
target_compile_features(sao_loader_native_fixture PRIVATE cxx_std_20)
target_compile_definitions(sao_loader_native_fixture PRIVATE SAO_PLUGINS_BUILDING_DLL)
set_target_properties(sao_loader_native_fixture PROPERTIES
	PREFIX ""
	OUTPUT_NAME "sao_loader_native_fixture")

set(sao_plugins_loader_test_sources test_loader.cpp)
foreach(local_test IN ITEMS
		plugin_context_capability_test.cpp
		plugin_context_compositor_test.cpp
		plugin_deps_session_test.cpp)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${local_test}")
		list(APPEND sao_plugins_loader_test_sources "${local_test}")
	endif()
endforeach()

add_executable(test_plugins_loader ${sao_plugins_loader_test_sources})
target_include_directories(test_plugins_loader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_plugins_loader PRIVATE
	sao_plugins_loader
	Catch2::Catch2WithMain)
target_compile_features(test_plugins_loader PRIVATE cxx_std_20)
# sao_plugins_loader is a STATIC lib, but the public header decorates every
# entry point with SAO_PLUGINS_API which expands to __declspec(dllimport)
# unless SAO_PLUGINS_BUILDING_DLL is defined at compile time.  Consumers
# that statically link the same TU must therefore see BUILDING_DLL to get
# the plain (undecorated) prototype and avoid __imp_* unresolved externals.
target_compile_definitions(test_plugins_loader PRIVATE
	SAO_PLUGINS_BUILDING_DLL
	SAO_PLUGINS_LOADER_TESTING
	SAO_TEST_NATIVE_PLUGIN_PATH=L"$<TARGET_FILE:sao_loader_native_fixture>")
add_dependencies(test_plugins_loader sao_loader_native_fixture)

include(Catch)
catch_discover_tests(test_plugins_loader
	TEST_PREFIX "plugins_loader."
	PROPERTIES LABELS "plugins;loader")
