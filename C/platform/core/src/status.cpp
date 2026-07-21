// TODO(status-table): replace with a table-driven implementation once the
// error taxonomy stabilises.

#include "sao/core/status.h"

extern "C" const char* SAO_CORE_CALL sao_status_str(sao_status_t status) {
    switch (status) {
        case SAO_STATUS_OK:                    return "ok";
        case SAO_STATUS_ERR_INVALID_ARGUMENT:  return "invalid_argument";
        case SAO_STATUS_ERR_NOT_INITIALIZED:   return "not_initialized";
        case SAO_STATUS_ERR_HANDLE_INVALID:    return "handle_invalid";
        case SAO_STATUS_ERR_BUFFER_TOO_SMALL:  return "buffer_too_small";
        case SAO_STATUS_ERR_NOT_IMPLEMENTED:   return "not_implemented";
        case SAO_STATUS_ERR_UNKNOWN:           return "unknown";
        case SAO_STATUS_ERR_TIMEOUT:           return "timeout";
        case SAO_STATUS_ERR_CANCELLED:         return "cancelled";
        case SAO_STATUS_ERR_ABI_MISMATCH:      return "abi_mismatch";
        case SAO_STATUS_ERR_CAPABILITY_MISSING: return "capability_missing";
        case SAO_STATUS_ERR_OS_CALL_FAILED:    return "os_call_failed";
        case SAO_STATUS_ERR_ACCESS_DENIED:     return "access_denied";
        case SAO_STATUS_ERR_NOT_FOUND:         return "not_found";
        case SAO_STATUS_ERR_ALREADY_EXISTS:    return "already_exists";
        case SAO_STATUS_ERR_PROCESS_GONE:      return "process_gone";
        case SAO_STATUS_ERR_READ_FAULT:        return "read_fault";
        case SAO_STATUS_ERR_MODULE_NOT_FOUND:  return "module_not_found";
        case SAO_STATUS_ERR_NET_DOWN:          return "net_down";
        case SAO_STATUS_ERR_NET_TLS:           return "net_tls";
        case SAO_STATUS_ERR_NET_HTTP_STATUS:   return "net_http_status";
        case SAO_STATUS_ERR_TOPIC_UNKNOWN:     return "topic_unknown";
        case SAO_STATUS_ERR_SUBSCRIPTION_GONE: return "subscription_gone";
        case SAO_STATUS_ERR_DEVICE_LOST:       return "device_lost";
        case SAO_STATUS_ERR_SURFACE_INVALID:   return "surface_invalid";
        case SAO_STATUS_ERR_SCRIPT_LOAD:       return "script_load";
        case SAO_STATUS_ERR_SCRIPT_RUNTIME:    return "script_runtime";
        default:                               return "unknown";
    }
}
