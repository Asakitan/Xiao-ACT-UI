// SAO Auto — platform/core ABI export macros.
//
// One header owns the Windows dllexport/dllimport wiring so every other
// header can just say `SAO_CORE_API` without knowing whether it's inside
// the module or outside.
//
// core/ is built STATIC (see CMakeLists.txt) — every other platform
// module links it directly, so SAO_CORE_API expands to nothing by
// default.  The macro remains in place so a later phase can flip core/
// to SHARED without touching every header.

#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SAO_CORE_BUILDING_DLL)
#    define SAO_CORE_API __declspec(dllexport)
#  elif defined(SAO_CORE_USING_DLL)
#    define SAO_CORE_API __declspec(dllimport)
#  else
#    define SAO_CORE_API
#  endif
#else
#  define SAO_CORE_API
#endif

#define SAO_CORE_CALL __cdecl

// core ABI version — 1.0.  Every public struct that grows a field must
// bump the minor; layout changes must bump the major.
#define SAO_CORE_ABI_VERSION_MAJOR 1u
#define SAO_CORE_ABI_VERSION_MINOR 0u
#define SAO_CORE_ABI_VERSION \
    ((SAO_CORE_ABI_VERSION_MAJOR << 16) | SAO_CORE_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

// Runtime probe — plugins/hosts call this and refuse to proceed on mismatch.
SAO_CORE_API uint32_t SAO_CORE_CALL sao_core_abi_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
