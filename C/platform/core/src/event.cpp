#include "sao/core/event.h"

#ifdef _WIN32
#include <windows.h>

#include <new>

struct sao_core_wait_event_s {
    HANDLE native_handle;
};
#endif

extern "C" sao_status_t SAO_CORE_CALL sao_core_wait_event_create(
    int32_t reset_mode,
    bool initial_signalled,
    sao_core_wait_event_handle_t* out_event) {
    if (out_event == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_event = nullptr;
#ifdef _WIN32
    if (reset_mode != SAO_WAIT_RESET_AUTO && reset_mode != SAO_WAIT_RESET_MANUAL) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    const HANDLE native_handle = CreateEventW(
        nullptr,
        reset_mode == SAO_WAIT_RESET_MANUAL ? TRUE : FALSE,
        initial_signalled ? TRUE : FALSE,
        nullptr);
    if (native_handle == nullptr) {
        return SAO_STATUS_ERR_OS_CALL_FAILED;
    }
    auto* event = new (std::nothrow) sao_core_wait_event_s{native_handle};
    if (event == nullptr) {
        CloseHandle(native_handle);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_event = event;
    return SAO_STATUS_OK;
#else
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" void SAO_CORE_CALL sao_core_wait_event_destroy(
    sao_core_wait_event_handle_t event) {
#ifdef _WIN32
    if (event != nullptr) {
        CloseHandle(event->native_handle);
        delete event;
    }
#else
    (void)event;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_wait_event_signal(
    sao_core_wait_event_handle_t event) {
#ifdef _WIN32
    if (event == nullptr || event->native_handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    return SetEvent(event->native_handle) != FALSE ? SAO_STATUS_OK
                                                   : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    (void)event;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_wait_event_reset(
    sao_core_wait_event_handle_t event) {
#ifdef _WIN32
    if (event == nullptr || event->native_handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    return ResetEvent(event->native_handle) != FALSE ? SAO_STATUS_OK
                                                     : SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    (void)event;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_wait_event_wait(
    sao_core_wait_event_handle_t event, uint32_t timeout_ms) {
#ifdef _WIN32
    if (event == nullptr || event->native_handle == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    const DWORD wait_result = WaitForSingleObject(event->native_handle, timeout_ms);
    if (wait_result == WAIT_OBJECT_0) {
        return SAO_STATUS_OK;
    }
    if (wait_result == WAIT_TIMEOUT) {
        return SAO_STATUS_ERR_TIMEOUT;
    }
    return SAO_STATUS_ERR_OS_CALL_FAILED;
#else
    (void)event;
    (void)timeout_ms;
    return SAO_STATUS_ERR_NOT_IMPLEMENTED;
#endif
}
