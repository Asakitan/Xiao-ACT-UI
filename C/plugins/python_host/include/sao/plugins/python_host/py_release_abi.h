#pragma once

// CPython's Windows headers select the debug ABI whenever MSVC defines
// _DEBUG. This host deliberately embeds a release Python DLL, so hide _DEBUG
// only while Python.h is parsed; the enclosing translation unit still uses
// the Debug CRT and its normal debug configuration afterward.
#if defined(_DEBUG)
#  define SAO_PYHOST_RESTORE_MSVC_DEBUG
#  undef _DEBUG
#endif

#include <Python.h>

#if defined(SAO_PYHOST_RESTORE_MSVC_DEBUG)
#  define _DEBUG 1
#  undef SAO_PYHOST_RESTORE_MSVC_DEBUG
#endif
