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
