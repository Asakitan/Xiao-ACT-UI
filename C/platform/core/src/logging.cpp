// The sink configuration is snapshotted under a mutex. User callbacks run
// outside that mutex so they may safely reconfigure logging or emit again.

#include "sao/core/logging.h"

#include <cstdio>
#include <mutex>

namespace {
struct LogState {
    std::recursive_mutex callback_mutex;
    std::mutex mutex;
    sao_log_callback_t callback = nullptr;
    void* user_data = nullptr;
    int32_t min_level = SAO_LOG_INFO;
};

struct LogSnapshot {
    sao_log_callback_t callback;
    void* user_data;
    int32_t min_level;
};

LogState& log_state() {
    static LogState state;
    return state;
}

LogSnapshot log_snapshot() {
    auto& state = log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    return {state.callback, state.user_data, state.min_level};
}

thread_local uint32_t tls_last_os_error = 0;
}  // namespace

void sao_core_set_last_os_error_internal(uint32_t os_error) {
    tls_last_os_error = os_error;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_set_log_callback(
    sao_log_callback_t callback, void* user_data) {
    auto& state = log_state();
    std::lock_guard<std::recursive_mutex> callback_lock(state.callback_mutex);
    std::lock_guard<std::mutex> lock(state.mutex);
    state.callback = callback;
    state.user_data = callback != nullptr ? user_data : nullptr;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_set_log_level(int32_t min_level) {
    auto& state = log_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.min_level = min_level;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_log(
    int32_t level,
    const char* category_utf8,
    const char* message_utf8) {
    auto& state = log_state();
    std::lock_guard<std::recursive_mutex> callback_lock(state.callback_mutex);
    const auto snapshot = log_snapshot();
    if (level < snapshot.min_level || snapshot.callback == nullptr) {
        return SAO_STATUS_OK;
    }
    snapshot.callback(level, category_utf8, message_utf8, snapshot.user_data);
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_CORE_CALL sao_core_logf(
    int32_t level, const char* category_utf8, const char* fmt_utf8, ...) {
    auto& state = log_state();
    std::lock_guard<std::recursive_mutex> callback_lock(state.callback_mutex);
    const auto snapshot = log_snapshot();
    if (level < snapshot.min_level || snapshot.callback == nullptr) {
        return SAO_STATUS_OK;
    }
    char buffer[2048];
    va_list ap;
    va_start(ap, fmt_utf8);
    vsnprintf(buffer, sizeof(buffer), fmt_utf8, ap);
    va_end(ap);
    buffer[sizeof(buffer) - 1] = '\0';
    snapshot.callback(level, category_utf8, buffer, snapshot.user_data);
    return SAO_STATUS_OK;
}

extern "C" uint32_t SAO_CORE_CALL sao_core_last_os_error(void) {
    return tls_last_os_error;
}
