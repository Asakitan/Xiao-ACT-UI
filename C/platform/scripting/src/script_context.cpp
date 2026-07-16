#include "sao/scripting/script_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sao/scripting/script_engine.h"
#include "sao/scripting/script_error.h"

#include "script_internal.h"

struct sao_script_context_s {
    std::mutex mutex;
    sao_script_engine_handle_t engine = nullptr;
    sao_script_instance_handle_t instance = nullptr;
    const SaoSdkContext* owner = nullptr;
    std::string source_dir;
    std::string entry_file;
    std::unordered_map<std::string, std::vector<uint8_t>> values;
    std::atomic<bool> cancelled{false};
    SaoScriptContextStats stats{};
    SaoScriptError last_error{};
};

namespace {

std::mutex g_contexts_mutex;
std::vector<sao_script_context_s*> g_contexts;

uint64_t elapsed_ns(std::chrono::steady_clock::time_point started) noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());
}

uint64_t unix_time_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void update_error(sao_script_context_s* context,
                  sao_status_t status,
                  const char* fallback) noexcept {
    SaoScriptError error{};
    const bool provider_diagnostic =
        status == SAO_STATUS_ERR_SCRIPT_LOAD ||
        status == SAO_STATUS_ERR_SCRIPT_RUNTIME ||
        status == SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    if (provider_diagnostic && context->engine != nullptr &&
        context->instance != nullptr &&
        sao_script_engine_last_error(context->engine, context->instance,
                                     &error) == SAO_STATUS_OK &&
        error.status != SAO_STATUS_OK) {
        std::lock_guard lock(context->mutex);
        context->last_error = error;
        context->stats.last_error_ts_ns = unix_time_ns();
        return;
    }
    sao::scripting::internal::set_error(&error, status, fallback);
    std::lock_guard lock(context->mutex);
    context->last_error = error;
    context->stats.last_error_ts_ns = unix_time_ns();
}

bool remove_context_registration(sao_script_context_s* context) noexcept {
    std::lock_guard lock(g_contexts_mutex);
    const auto found = std::find(g_contexts.begin(), g_contexts.end(), context);
    if (found == g_contexts.end()) return false;
    g_contexts.erase(found);
    return true;
}

void destroy_context(sao_script_context_s* context,
                     bool remove_registration) noexcept {
    if (context == nullptr) return;
                    if (remove_registration && !remove_context_registration(context)) return;
    sao_script_engine_unload(context->engine, context->instance);
    sao_script_engine_destroy(context->engine);
    delete context;
}

bool valid_key(const char* key_utf8) noexcept {
    return key_utf8 != nullptr && key_utf8[0] != '\0';
}

}  // namespace

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_create(
    const SaoScriptContextConfig* config,
    sao_script_context_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config == nullptr || config->source_dir_utf8 == nullptr ||
        config->entry_file_utf8 == nullptr ||
        config->entry_file_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    auto context = std::unique_ptr<sao_script_context_s>(
        new (std::nothrow) sao_script_context_s());
    if (!context) return SAO_STATUS_ERR_UNKNOWN;
    try {
        context->owner = config->owner_sdk_ctx;
        context->source_dir = config->source_dir_utf8;
        context->entry_file = config->entry_file_utf8;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }

    sao_status_t status = sao_script_engine_create(config->language,
                                                    &context->engine);
    if (status != SAO_STATUS_OK) return status;

    SaoScriptContextConfig stable_config = *config;
    stable_config.source_dir_utf8 = context->source_dir.c_str();
    stable_config.entry_file_utf8 = context->entry_file.c_str();
    status = sao_script_engine_load(context->engine, &stable_config,
                                    &context->instance);
    if (status != SAO_STATUS_OK) {
        sao_script_engine_destroy(context->engine);
        context->engine = nullptr;
        return status;
    }
    try {
        std::lock_guard lock(g_contexts_mutex);
        g_contexts.push_back(context.get());
    } catch (...) {
        sao_script_engine_unload(context->engine, context->instance);
        sao_script_engine_destroy(context->engine);
        context->instance = nullptr;
        context->engine = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_handle = context.release();
    return SAO_STATUS_OK;
}

extern "C" void SAO_SCRIPTING_CALL sao_script_context_destroy(
    sao_script_context_handle_t handle) {
    destroy_context(handle, true);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_run(
    sao_script_context_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->cancelled.load(std::memory_order_acquire)) {
        update_error(handle, SAO_STATUS_ERR_CANCELLED,
                     "script context is cancelled");
        return SAO_STATUS_ERR_CANCELLED;
    }
    const auto started = std::chrono::steady_clock::now();
    const sao_status_t status =
        sao_script_engine_run(handle->engine, handle->instance);
    const uint64_t duration = elapsed_ns(started);
    {
        std::lock_guard lock(handle->mutex);
        handle->stats.total_run_time_ns += duration;
    }
    if (status != SAO_STATUS_OK) {
        update_error(handle, status, "script run failed");
    }
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_call(
    sao_script_context_handle_t handle,
    const char* function_name_utf8,
    const uint8_t* arg_json_utf8,
    size_t arg_len,
    uint8_t* out_result_json_utf8,
    size_t out_capacity,
    size_t* out_written) {
    if (out_written != nullptr) *out_written = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (handle->cancelled.load(std::memory_order_acquire)) {
        update_error(handle, SAO_STATUS_ERR_CANCELLED,
                     "script context is cancelled");
        return SAO_STATUS_ERR_CANCELLED;
    }
    const auto started = std::chrono::steady_clock::now();
    const sao_status_t status = sao_script_engine_invoke(
        handle->engine, handle->instance, function_name_utf8, arg_json_utf8,
        arg_len, out_result_json_utf8, out_capacity, out_written);
    const uint64_t duration = elapsed_ns(started);
    {
        std::lock_guard lock(handle->mutex);
        ++handle->stats.total_call_count;
        handle->stats.total_call_time_ns += duration;
    }
    if (status != SAO_STATUS_OK &&
        status != SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
        update_error(handle, status, "script invoke failed");
    }
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_stats(
    sao_script_context_handle_t handle, SaoScriptContextStats* out_stats) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_stats == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    *out_stats = handle->stats;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_set_value(
    sao_script_context_handle_t handle,
    const char* key_utf8,
    const uint8_t* value,
    size_t value_size) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(key_utf8) || (value == nullptr && value_size != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<uint8_t> copy;
        if (value_size != 0) copy.assign(value, value + value_size);
        std::lock_guard lock(handle->mutex);
        handle->values[std::string(key_utf8)] = std::move(copy);
        uint64_t total = 0;
        for (const auto& [key, stored] : handle->values) {
            total += key.size() + stored.size();
        }
        handle->stats.peak_memory_bytes =
            std::max(handle->stats.peak_memory_bytes, total);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_get_value(
    sao_script_context_handle_t handle,
    const char* key_utf8,
    uint8_t* out_value,
    size_t out_capacity,
    size_t* out_required) {
    if (out_required != nullptr) *out_required = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(key_utf8) || out_required == nullptr ||
        (out_value == nullptr && out_capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(handle->mutex);
    const auto found = handle->values.find(key_utf8);
    if (found == handle->values.end()) return SAO_STATUS_ERR_NOT_FOUND;
    *out_required = found->second.size();
    if (out_capacity < found->second.size() ||
        (out_value == nullptr && !found->second.empty())) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    if (!found->second.empty()) {
        std::memcpy(out_value, found->second.data(), found->second.size());
    }
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_remove_value(
    sao_script_context_handle_t handle, const char* key_utf8) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(key_utf8)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    return handle->values.erase(key_utf8) == 0 ? SAO_STATUS_ERR_NOT_FOUND
                                               : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL
sao_script_context_has_capability(
    sao_script_context_handle_t handle,
    const char* capability_utf8,
    int32_t* out_supported) {
    if (out_supported != nullptr) *out_supported = 0;
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(capability_utf8) || out_supported == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    if (std::strcmp(capability_utf8, SAO_SCRIPT_CAPABILITY_KEY_VALUE) == 0 ||
        std::strcmp(capability_utf8, SAO_SCRIPT_CAPABILITY_CANCELLATION) == 0 ||
        std::strcmp(capability_utf8, SAO_SCRIPT_CAPABILITY_ERROR) == 0) {
        *out_supported = 1;
        return SAO_STATUS_OK;
    }
    return sao_script_engine_has_capability(
        handle->engine, handle->instance, capability_utf8, out_supported);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL
sao_script_context_request_cancel(sao_script_context_handle_t handle) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    handle->cancelled.store(true, std::memory_order_release);
    const sao_status_t status =
        sao_script_engine_cancel(handle->engine, handle->instance);
    if (status == SAO_STATUS_ERR_SCRIPT_UNSUPPORTED) return SAO_STATUS_OK;
    if (status != SAO_STATUS_OK) {
        update_error(handle, status, "script cancellation failed");
    }
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_is_cancelled(
    sao_script_context_handle_t handle, int32_t* out_cancelled) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_cancelled == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_cancelled = handle->cancelled.load(std::memory_order_acquire) ? 1 : 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_last_error(
    sao_script_context_handle_t handle, SaoScriptError* out_error) {
    if (handle == nullptr) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (out_error == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(handle->mutex);
    *out_error = handle->last_error;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL
sao_script_context_release_owner(const SaoSdkContext* owner_sdk_ctx,
                                 size_t* out_released) {
    if (owner_sdk_ctx == nullptr || out_released == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_released = 0;
    std::vector<sao_script_context_s*> released;
    {
        std::lock_guard lock(g_contexts_mutex);
        for (auto iterator = g_contexts.begin(); iterator != g_contexts.end();) {
            if ((*iterator)->owner != owner_sdk_ctx) {
                ++iterator;
                continue;
            }
            released.push_back(*iterator);
            iterator = g_contexts.erase(iterator);
        }
    }
    for (auto* context : released) destroy_context(context, false);
    *out_released = released.size();
    return SAO_STATUS_OK;
}
