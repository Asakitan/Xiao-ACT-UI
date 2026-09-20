// py_error.cpp — Python host 状态码 → 异常名映射。
//
// sao_plugins_pyhost_take_error 的实现在 py_host.cpp: 复用该 TU 的
// capture_and_clear_pyerr + GIL 管理, 无法在本 TU 访问 (TU-local).
#include "sao/plugins/python_host/py_error.h"

namespace sao::plugins::python_host {

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
