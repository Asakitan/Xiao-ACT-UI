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

// Legacy core export ABI is currently 1.2. The class metadata provider table has an independent
// ABI version declared in class_index.h; this version must not be used to validate that table.
#define SAO_LEGACY_CORE_ABI_VERSION_MAJOR 1u
#define SAO_LEGACY_CORE_ABI_VERSION_MINOR 2u
#define SAO_LEGACY_CORE_ABI_VERSION                                                                \
    ((SAO_LEGACY_CORE_ABI_VERSION_MAJOR << 16u) | SAO_LEGACY_CORE_ABI_VERSION_MINOR)
