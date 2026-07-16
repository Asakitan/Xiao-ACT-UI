#pragma once

#if defined(_WIN32)
  #if defined(SAO_CORE_BUILDING_DLL)
    #define SAO_CORE_API __declspec(dllexport)
  #else
    #define SAO_CORE_API __declspec(dllimport)
  #endif
#else
  #define SAO_CORE_API
#endif

#define SAO_CORE_CALL __cdecl
