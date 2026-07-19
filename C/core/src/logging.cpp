#include "logging.h"

#include <sao_core/sao_core.h>

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace {

struct CallbackState final {
    sao_legacy_core_log_callback_t legacy_callback{};
    sao_legacy_core_structured_log_callback_t structured_callback{};
    void* user_data{};
    std::atomic_bool retired{false};
    std::mutex active_mutex;
    std::condition_variable active_condition;
    uint32_t active_calls{};
};

std::atomic<std::shared_ptr<CallbackState>> g_callback_state;
std::atomic_bool g_logging_enabled{false};
std::mutex g_configuration_mutex;
thread_local CallbackState* g_current_callback_state = nullptr;

[[nodiscard]] uint32_t current_thread_calls(const CallbackState* state) noexcept {
    return g_current_callback_state == state ? 1U : 0U;
}

[[nodiscard]] bool acquire_callback(const std::shared_ptr<CallbackState>& state) noexcept {
    std::lock_guard lock(state->active_mutex);
    if (state->retired.load(std::memory_order_acquire)) {
        return false;
    }
    ++state->active_calls;
    return true;
}

void release_callback(const std::shared_ptr<CallbackState>& state) noexcept {
    {
        std::lock_guard lock(state->active_mutex);
        --state->active_calls;
    }
    state->active_condition.notify_all();
}

void retire_callback(const std::shared_ptr<CallbackState>& state) noexcept {
    if (!state) {
        return;
    }

    state->retired.store(true, std::memory_order_release);
    const uint32_t current_calls = current_thread_calls(state.get());
    std::unique_lock lock(state->active_mutex);
    state->active_condition.wait(
        lock, [&state, current_calls] { return state->active_calls <= current_calls; });
}

void invoke_cpp_callback(const CallbackState* state, int32_t level, const char* component,
                         int32_t status, const char* message) noexcept {
    try {
        if (state->structured_callback != nullptr) {
            state->structured_callback(level, component, status, message, state->user_data);
            return;
        }

        char structured_message[768]{};
        (void)std::snprintf(structured_message, sizeof(structured_message),
                            "component=%s status=%d message=%s", component, status, message);
        state->legacy_callback(level, structured_message);
    } catch (...) {
    }
}

void invoke_callback(const CallbackState* state, int32_t level, const char* component,
                     int32_t status, const char* message) noexcept {
#if defined(_MSC_VER)
    __try {
        invoke_cpp_callback(state, level, component, status, message);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    invoke_cpp_callback(state, level, component, status, message);
#endif
}

[[nodiscard]] int32_t set_callback(sao_legacy_core_log_callback_t legacy_callback,
                                   sao_legacy_core_structured_log_callback_t structured_callback,
                                   void* user_data) noexcept {
    try {
        std::shared_ptr<CallbackState> next;
        if (legacy_callback != nullptr || structured_callback != nullptr) {
            next = std::make_shared<CallbackState>();
            next->legacy_callback = legacy_callback;
            next->structured_callback = structured_callback;
            next->user_data = user_data;
        }

        std::shared_ptr<CallbackState> previous;
        {
            std::lock_guard lock(g_configuration_mutex);
            previous = g_callback_state.exchange(next, std::memory_order_acq_rel);
            g_logging_enabled.store(next != nullptr, std::memory_order_release);
        }
        retire_callback(previous);
        return SAO_OK;
    } catch (const std::bad_alloc&) {
        return SAO_ERR_UNKNOWN;
    } catch (...) {
        return SAO_ERR_UNKNOWN;
    }
}

} // namespace

namespace sao::legacy_core {

bool logging_enabled() noexcept {
    return g_logging_enabled.load(std::memory_order_acquire);
}

void emit_log(int32_t level, const char* utf8_component, int32_t status,
              const char* utf8_message) noexcept {
    if (!logging_enabled() || g_current_callback_state != nullptr) {
        return;
    }

    auto state = g_callback_state.load(std::memory_order_acquire);
    if (!state || !acquire_callback(state)) {
        return;
    }

    g_current_callback_state = state.get();
    invoke_callback(state.get(), level, utf8_component, status, utf8_message);
    g_current_callback_state = nullptr;
    release_callback(state);
}

} // namespace sao::legacy_core

extern "C" int32_t SAO_LEGACY_CORE_CALL sao_legacy_core_set_structured_log_callback(
    sao_legacy_core_structured_log_callback_t callback, void* user_data) {
    const int32_t status = set_callback(nullptr, callback, user_data);
    if (status != SAO_OK) {
        sao::legacy_core::emit_log(sao::legacy_core::kLogLevelError, "core.logging", status,
                                   "set_structured_log_callback failed");
    }
    return status;
}

extern "C" void SAO_LEGACY_CORE_CALL
sao_legacy_core_set_log_callback(sao_legacy_core_log_callback_t callback) {
    const int32_t status = set_callback(callback, nullptr, nullptr);
    if (status != SAO_OK) {
        sao::legacy_core::emit_log(sao::legacy_core::kLogLevelError, "core.logging", status,
                                   "set_log_callback failed");
    }
}
