# plugins/runtimes — shared runtime manifest + build-time fetch cache
#
# This subdirectory does NOT build any C/C++ artefact.  Its single job is to
# expose the shared runtime manifest to both the build tree (so per-host
# CMakeLists can align FetchContent URLs with the launcher-side installer)
# and the install tree (so the launcher's runtime_installer can locate the
# canonical manifest at runtime).
#
# The manifest documents downloadable payloads for every plugin runtime that
# cannot ship as source (currently: .NET, Lua, AngelScript, Python).  Emma is
# zero-dependency.  Python is resolved by find_package(Python3
# Development.Embed) when a system / vcpkg copy is available; otherwise the
# python_host CMakeLists (opt-in via SAO_PLUGINS_PYTHON_AUTO_FETCH) fetches
# the python3_embed + python3_headers entries listed below.
set(SAO_PLUGINS_RUNTIME_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    CACHE FILEPATH
    "Absolute path to the shared plugin runtime manifest (JSON)")
mark_as_advanced(SAO_PLUGINS_RUNTIME_MANIFEST)

# Cache directory the launcher-side runtime_installer and the build-time
# FetchContent gates both share.  Defaults to a build-tree subfolder so that
# clean-checkouts do not fight with system caches.  A shipping build can
# override this to point at the packaged runtimes directory.
set(SAO_PLUGINS_RUNTIME_CACHE_DIR
    "${CMAKE_BINARY_DIR}/plugin_runtime_cache"
    CACHE PATH
    "Filesystem root used to cache fetched plugin runtimes (both at build time and at launcher runtime).")
mark_as_advanced(SAO_PLUGINS_RUNTIME_CACHE_DIR)

file(MAKE_DIRECTORY "${SAO_PLUGINS_RUNTIME_CACHE_DIR}")

# Copy the manifest next to the build tree so tests can locate it via a
# stable path (matches how the launcher will bundle it into the install
# image).  Regenerate whenever the source manifest changes.
configure_file(
    "${SAO_PLUGINS_RUNTIME_MANIFEST}"
    "${SAO_PLUGINS_RUNTIME_CACHE_DIR}/manifest.json"
    COPYONLY)

# Install rule.  The launcher reads this from
# <install>/share/sao/plugin_runtimes/manifest.json.  Guarded so the file is
# only staged when the caller has enabled installation.
if (NOT DEFINED SAO_PLUGINS_RUNTIME_INSTALL_DIR)
    set(SAO_PLUGINS_RUNTIME_INSTALL_DIR "share/sao/plugin_runtimes")
endif()
install(FILES "${SAO_PLUGINS_RUNTIME_MANIFEST}"
        DESTINATION "${SAO_PLUGINS_RUNTIME_INSTALL_DIR}"
        COMPONENT sao_plugins_runtime_manifest
        OPTIONAL)
