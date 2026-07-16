// SAO Auto — platform/scripting ABI export macros.

#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SAO_SCRIPTING_BUILDING_DLL)
#    define SAO_SCRIPTING_API __declspec(dllexport)
#  elif defined(SAO_SCRIPTING_USING_DLL)
#    define SAO_SCRIPTING_API __declspec(dllimport)
#  else
#    define SAO_SCRIPTING_API
#  endif
#else
#  define SAO_SCRIPTING_API
#endif

#define SAO_SCRIPTING_CALL __cdecl

#define SAO_SCRIPTING_ABI_VERSION_MAJOR 1u
#define SAO_SCRIPTING_ABI_VERSION_MINOR 0u
#define SAO_SCRIPTING_ABI_VERSION \
    ((SAO_SCRIPTING_ABI_VERSION_MAJOR << 16) | SAO_SCRIPTING_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

SAO_SCRIPTING_API uint32_t SAO_SCRIPTING_CALL sao_scripting_abi_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
