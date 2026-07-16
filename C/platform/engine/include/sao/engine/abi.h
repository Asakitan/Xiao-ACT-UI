// SAO Auto — platform/engine ABI export macros.

#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SAO_ENGINE_BUILDING_DLL)
#    define SAO_ENGINE_API __declspec(dllexport)
#  elif defined(SAO_ENGINE_USING_DLL)
#    define SAO_ENGINE_API __declspec(dllimport)
#  else
#    define SAO_ENGINE_API
#  endif
#else
#  define SAO_ENGINE_API
#endif

#define SAO_ENGINE_CALL __cdecl

#define SAO_ENGINE_ABI_VERSION_MAJOR 1u
#define SAO_ENGINE_ABI_VERSION_MINOR 0u
#define SAO_ENGINE_ABI_VERSION \
    ((SAO_ENGINE_ABI_VERSION_MAJOR << 16) | SAO_ENGINE_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

SAO_ENGINE_API uint32_t SAO_ENGINE_CALL sao_engine_abi_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
