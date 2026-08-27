# plugins/runtimes — build-time runtime schema cache
#
# This subdirectory does NOT build any C/C++ artefact. Its only job is to
# keep build-time runtime metadata available to configure-time tooling.
# The file is intentionally not installed: it is not a runtime-installer
# canonical asset, has no payload hashes, and carries no shipped binaries.
#
# The source filename remains manifest.json for compatibility with the
# repository layout, but every generated copy is named build-time-manifest.
set(SAO_PLUGINS_BUILD_TIME_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.json"
    CACHE FILEPATH
    "Absolute path to the build-time plugin runtime schema (JSON)")
mark_as_advanced(SAO_PLUGINS_BUILD_TIME_MANIFEST)

# Build-time cache is isolated inside each preset build tree. It is never
# shared with another preset and is never used as a shipped runtime root.
set(SAO_PLUGINS_RUNTIME_CACHE_DIR
    "${CMAKE_BINARY_DIR}/plugin_runtime_cache"
    CACHE PATH
    "Filesystem root used for build-time plugin runtime metadata.")
mark_as_advanced(SAO_PLUGINS_RUNTIME_CACHE_DIR)

file(MAKE_DIRECTORY "${SAO_PLUGINS_RUNTIME_CACHE_DIR}")

# Copy the schema under a non-canonical name so it cannot be mistaken for a
# runtime-installer manifest. Regenerate whenever the source changes.
configure_file(
    "${SAO_PLUGINS_BUILD_TIME_MANIFEST}"
    "${SAO_PLUGINS_RUNTIME_CACHE_DIR}/build-time-manifest.json"
    COPYONLY)
