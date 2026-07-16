// SAO Auto — platform/ui ABI export macros.

#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SAO_UI_BUILDING_DLL)
#    define SAO_UI_API __declspec(dllexport)
#  elif defined(SAO_UI_USING_DLL)
#    define SAO_UI_API __declspec(dllimport)
#  else
#    define SAO_UI_API
#  endif
#else
#  define SAO_UI_API
#endif

#define SAO_UI_CALL __cdecl

#define SAO_UI_ABI_VERSION_MAJOR 1u
#define SAO_UI_ABI_VERSION_MINOR 2u
#define SAO_UI_ABI_VERSION \
    ((SAO_UI_ABI_VERSION_MAJOR << 16) | SAO_UI_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

SAO_UI_API uint32_t SAO_UI_CALL sao_ui_abi_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
