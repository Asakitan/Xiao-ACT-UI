// py_error.cpp — stub
#include "sao/plugins/python_host/py_error.h"

namespace sao::plugins::python_host {

extern "C" SAO_PLUGINS_API int32_t SAO_PLUGINS_CALL
sao_plugins_pyhost_take_error(char** out_utf8) {
    if (out_utf8 != nullptr) *out_utf8 = nullptr;
    return SAO_ERR_NOT_IMPLEMENTED;
}

extern "C" SAO_PLUGINS_API const char* SAO_PLUGINS_CALL
sao_plugins_pyhost_status_to_python_exc(int32_t status) {
    switch (status) {
        case SAO_ERR_INVALID_ARGUMENT: return "ValueError";
        case SAO_ERR_NOT_INITIALIZED: return "RuntimeError";
        case SAO_ERR_HANDLE_INVALID: return "TypeError";
        case SAO_ERR_BUFFER_TOO_SMALL: return "MemoryError";
        case SAO_ERR_OS_CALL_FAILED: return "OSError";
        case SAO_ERR_NOT_IMPLEMENTED: return "NotImplementedError";
        default: return "Exception";
    }
}

} // namespace sao::plugins::python_host
