// SAO Auto — SDK ABI version.
//
// Plugins call sao_sdk_abi_version() during their init callback and
// refuse to proceed on mismatch.  The SDK is the *only* ABI seen by
// plugins — every other module is reached through the ctx vtable.

#pragma once

#include <cstdint>

#define SAO_SDK_ABI_VERSION_MAJOR 1u
#define SAO_SDK_ABI_VERSION_MINOR 11u
#define SAO_SDK_ABI_VERSION ((SAO_SDK_ABI_VERSION_MAJOR << 16) | SAO_SDK_ABI_VERSION_MINOR)

#if defined(_WIN32)
#if defined(SAO_SDK_BUILDING_DLL)
#define SAO_SDK_API __declspec(dllexport)
#elif defined(SAO_SDK_TESTING)
#define SAO_SDK_API
#else
#define SAO_SDK_API __declspec(dllimport)
#endif
#else
#define SAO_SDK_API
#endif

#define SAO_SDK_CALL __cdecl
