#pragma once

#if defined(_WIN32)
  #if defined(SAO_PLUGINS_BUILDING_DLL)
    #define SAO_PLUGINS_API __declspec(dllexport)
  #else
    #define SAO_PLUGINS_API __declspec(dllimport)
  #endif
#else
  #define SAO_PLUGINS_API
#endif

#define SAO_PLUGINS_CALL __cdecl

// ── legacy facade ABI version ────────────────────────────────────────
// Packed (major<<16)|minor reported by sao_plugins_abi_version(). The
// compat facade intentionally pins v1.0 while internal modules track
// their own ABI; plugin.json manifests instead gate on
// SAO_PLUGINS_ABI_VERSION (max supported plugin ABI, set by CMake).
#ifndef SAO_PLUGINS_ABI_VERSION_MAJOR
#define SAO_PLUGINS_ABI_VERSION_MAJOR 1
#endif
#ifndef SAO_PLUGINS_ABI_VERSION_MINOR
#define SAO_PLUGINS_ABI_VERSION_MINOR 0
#endif
