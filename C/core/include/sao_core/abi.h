#pragma once

#if defined(_WIN32)
#if defined(SAO_LEGACY_CORE_BUILDING_DLL)
#define SAO_LEGACY_CORE_API __declspec(dllexport)
#else
#define SAO_LEGACY_CORE_API __declspec(dllimport)
#endif
#else
#define SAO_LEGACY_CORE_API
#endif

#define SAO_LEGACY_CORE_CALL __cdecl

#define SAO_LEGACY_CORE_ABI_VERSION_MAJOR 1u
#define SAO_LEGACY_CORE_ABI_VERSION_MINOR 1u
#define SAO_LEGACY_CORE_ABI_VERSION                                                                \
    ((SAO_LEGACY_CORE_ABI_VERSION_MAJOR << 16u) | SAO_LEGACY_CORE_ABI_VERSION_MINOR)
