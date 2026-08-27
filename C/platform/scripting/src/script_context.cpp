#include "sao/scripting/script_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
    std::condition_variable cv;
    sao_script_engine_handle_t engine = nullptr;
    sao_script_instance_handle_t instance = nullptr;
    const SaoSdkContext* owner = nullptr;
    std::string source_dir;
    std::string entry_file;
    std::unordered_map<std::string, std::vector<uint8_t>> values;
    std::atomic<bool> cancelled{false};
    SaoScriptContextStats stats{};
    SaoScriptError last_error{};
    bool destroying = false;
    std::atomic<bool> cleanup_started{false};
    bool cleanup_finished = false;
    size_t active_calls = 0;
};

namespace {

std::mutex g_context_registry_mutex;
std::unordered_map<sao_script_context_handle_t,
                   std::shared_ptr<sao_script_context_s>> g_contexts;
std::vector<std::shared_ptr<sao_script_context_s>> g_pending_contexts;
thread_local std::vector<const sao_script_context_s*> g_context_call_stack;

std::shared_ptr<sao_script_context_s> acquire_context(
    sao_script_context_handle_t raw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_context_registry_mutex);
        const auto found = g_contexts.find(raw);
        if (found == g_contexts.end()) return nullptr;
        auto context = found->second;
        {
            std::lock_guard<std::mutex> context_lock(context->mutex);
            if (context->destroying) return nullptr;
            ++context->active_calls;
        }
        try {
            g_context_call_stack.push_back(context.get());
        } catch (...) {
            std::lock_guard<std::mutex> context_lock(context->mutex);
            --context->active_calls;
            context->cv.notify_all();
            return nullptr;
        }
        return context;
    } catch (...) {
        return nullptr;
    }
}

size_t owned_context_calls(const sao_script_context_s* context) {
    return static_cast<size_t>(std::count(g_context_call_stack.begin(),
                                          g_context_call_stack.end(), context));
}

void forget_pending_context(
    const std::shared_ptr<sao_script_context_s>& context) noexcept {
    std::lock_guard<std::mutex> lock(g_context_registry_mutex);
    g_pending_contexts.erase(
        std::remove_if(g_pending_contexts.begin(), g_pending_contexts.end(),
                       [&context](const auto& entry) {
                           return entry.get() == context.get();
                       }),
        g_pending_contexts.end());
}

void finish_context(const std::shared_ptr<sao_script_context_s>& context,
                    bool wait_for_quiescence) noexcept;

void release_context_call(
    const std::shared_ptr<sao_script_context_s>& context) noexcept {
    g_context_call_stack.pop_back();
    sao_script_engine_handle_t engine = nullptr;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        --context->active_calls;
        engine = context->engine;
    }
    context->cv.notify_all();
    finish_context(context, true);
    sao::scripting::internal::retry_deferred_engine_cleanup(engine);
}

class ContextLease final {
  public:
    explicit ContextLease(std::shared_ptr<sao_script_context_s> context)
        : context_(std::move(context)) {}
    ~ContextLease() {
        if (context_) release_context_call(context_);
    }
    ContextLease(const ContextLease&) = delete;
    ContextLease& operator=(const ContextLease&) = delete;
    sao_script_context_s* get() const noexcept { return context_.get(); }
    explicit operator bool() const noexcept { return context_ != nullptr; }
  private:
    std::shared_ptr<sao_script_context_s> context_;
};

uint64_t elapsed_ns(std::chrono::steady_clock::time_point started) noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());
}

uint64_t unix_time_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void update_error(sao_script_context_s* context, sao_status_t status,
                  const char* fallback) noexcept {
    SaoScriptError error{};
    const bool provider_diagnostic =
        status == SAO_STATUS_ERR_SCRIPT_LOAD ||
        status == SAO_STATUS_ERR_SCRIPT_RUNTIME ||
        status == SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    if (provider_diagnostic && context->engine != nullptr &&
        context->instance != nullptr &&
        sao_script_engine_last_error(context->engine, context->instance, &error) ==
            SAO_STATUS_OK && error.status != SAO_STATUS_OK) {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->last_error = error;
        context->stats.last_error_ts_ns = unix_time_ns();
        return;
    }
    sao::scripting::internal::set_error(&error, status, fallback);
    std::lock_guard<std::mutex> lock(context->mutex);
    context->last_error = error;
    context->stats.last_error_ts_ns = unix_time_ns();
}

bool valid_key(const char* key_utf8) noexcept {
    return key_utf8 != nullptr && key_utf8[0] != '\0';
}

void finish_context(const std::shared_ptr<sao_script_context_s>& context,
                    bool wait_for_quiescence) noexcept {
    if (!context) return;
    std::unique_lock<std::mutex> lock(context->mutex);
    if (sao::scripting::internal::provider_callback_active(context->engine)) {
        return;
    }
    if (!context->destroying) return;
    if (owned_context_calls(context.get()) != 0) return;
    bool expected = false;
    if (!context->cleanup_started.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        if (!wait_for_quiescence) return;
        context->cv.wait(lock, [&] {
            return context->cleanup_finished || !context->cleanup_started.load(
                                                       std::memory_order_acquire);
        });
        return;
    }
    if (context->active_calls != 0) {
        if (!wait_for_quiescence) {
            context->cleanup_started.store(false, std::memory_order_release);
            return;
        }
        context->cv.wait(lock, [&] { return context->active_calls == 0; });
    }
    const auto engine = context->engine;
    const auto instance = context->instance;
    lock.unlock();
    const bool cleanup_finished =
        sao::scripting::internal::cleanup_engine_instance(engine, instance);
    lock.lock();
    if (!cleanup_finished) {
        context->cleanup_started.store(false, std::memory_order_release);
        lock.unlock();
        context->cv.notify_all();
        return;
    }
    context->engine = nullptr;
    context->instance = nullptr;
    context->cleanup_finished = true;
    lock.unlock();
    context->cv.notify_all();
    forget_pending_context(context);
}

std::shared_ptr<sao_script_context_s> remove_context(
    sao_script_context_handle_t raw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_context_registry_mutex);
        const auto found = g_contexts.find(raw);
        if (found == g_contexts.end()) {
            const auto pending = std::find_if(
                g_pending_contexts.begin(), g_pending_contexts.end(),
                [raw](const auto& context) { return context.get() == raw; });
            return pending == g_pending_contexts.end() ? nullptr : *pending;
        }
        if (g_pending_contexts.size() == g_pending_contexts.max_size()) return nullptr;
        g_pending_contexts.reserve(g_pending_contexts.size() + 1);
        auto context = found->second;
        {
            std::lock_guard<std::mutex> context_lock(context->mutex);
            context->destroying = true;
        }
        g_pending_contexts.push_back(context);
        g_contexts.erase(found);
        return context;
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

namespace sao::scripting::internal {

void retry_deferred_context_cleanup(
    sao_script_engine_handle_t engine) noexcept {
    if (engine == nullptr) return;
    std::vector<std::shared_ptr<sao_script_context_s>> pending;
    try {
        std::lock_guard<std::mutex> lock(g_context_registry_mutex);
        pending = g_pending_contexts;
    } catch (...) {
        return;
    }
    for (const auto& context : pending) {
        bool matches = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            matches = context->destroying && context->engine == engine;
        }
        if (matches) finish_context(context, false);
    }
}

}  // namespace sao::scripting::internal

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_create(
    const SaoScriptContextConfig* config, sao_script_context_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    if (config == nullptr || config->source_dir_utf8 == nullptr ||
        config->entry_file_utf8 == nullptr || config->entry_file_utf8[0] == '\0') {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::shared_ptr<sao_script_context_s> context;
    try {
        context = std::make_shared<sao_script_context_s>();

        context->owner = config->owner_sdk_ctx;
        context->source_dir = config->source_dir_utf8;
        context->entry_file = config->entry_file_utf8;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    sao_status_t status = sao_script_engine_create(config->language, &context->engine);
    if (status != SAO_STATUS_OK) return status;
    SaoScriptContextConfig stable_config = *config;
    stable_config.source_dir_utf8 = context->source_dir.c_str();
    stable_config.entry_file_utf8 = context->entry_file.c_str();
    status = sao_script_engine_load(context->engine, &stable_config, &context->instance);
    if (status != SAO_STATUS_OK) {
        sao_script_engine_destroy(context->engine);
        context->engine = nullptr;
        return status;
    }
    try {
        std::lock_guard<std::mutex> lock(g_context_registry_mutex);
        const auto [unused, inserted] = g_contexts.emplace(context.get(), context);
        (void)unused;
        if (!inserted) {
            sao_script_engine_unload(context->engine, context->instance);
            sao_script_engine_destroy(context->engine);
            context->engine = nullptr;
            context->instance = nullptr;
            return SAO_STATUS_ERR_ALREADY_EXISTS;
        }
    } catch (...) {
        sao_script_engine_unload(context->engine, context->instance);
        sao_script_engine_destroy(context->engine);
        context->engine = nullptr;
        context->instance = nullptr;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_handle = context.get();
    return SAO_STATUS_OK;
}

extern "C" void SAO_SCRIPTING_CALL sao_script_context_destroy(
    sao_script_context_handle_t raw) {
    auto context = remove_context(raw);
    if (!context) return;
    finish_context(context, true);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_run(
    sao_script_context_handle_t raw) {
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* context = lease.get();
    if (context->cancelled.load(std::memory_order_acquire)) {
        update_error(context, SAO_STATUS_ERR_CANCELLED, "script context is cancelled");
        return SAO_STATUS_ERR_CANCELLED;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto status = sao_script_engine_run(context->engine, context->instance);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->stats.total_run_time_ns += elapsed_ns(started);
    }
    if (status != SAO_STATUS_OK) update_error(context, status, "script run failed");
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_call(
    sao_script_context_handle_t raw, const char* function_name_utf8,
    const uint8_t* arg_json_utf8, size_t arg_len, uint8_t* out_result_json_utf8,
    size_t out_capacity, size_t* out_written) {
    if (out_written != nullptr) *out_written = 0;
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* context = lease.get();
    if (context->cancelled.load(std::memory_order_acquire)) {
        update_error(context, SAO_STATUS_ERR_CANCELLED, "script context is cancelled");
        return SAO_STATUS_ERR_CANCELLED;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto status = sao_script_engine_invoke(
        context->engine, context->instance, function_name_utf8, arg_json_utf8, arg_len,
        out_result_json_utf8, out_capacity, out_written);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        ++context->stats.total_call_count;
        context->stats.total_call_time_ns += elapsed_ns(started);
    }
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_BUFFER_TOO_SMALL) {
        update_error(context, status, "script invoke failed");
    }
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_stats(
    sao_script_context_handle_t raw, SaoScriptContextStats* out_stats) {
    if (out_stats == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(lease.get()->mutex);
    *out_stats = lease.get()->stats;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_set_value(
    sao_script_context_handle_t raw, const char* key_utf8, const uint8_t* value,
    size_t value_size) {
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(key_utf8) || (value == nullptr && value_size != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    try {
        std::vector<uint8_t> copy;
        if (value_size != 0) copy.assign(value, value + value_size);
        std::lock_guard<std::mutex> lock(lease.get()->mutex);
        lease.get()->values[std::string(key_utf8)] = std::move(copy);
        uint64_t total = 0;
        for (const auto& [key, stored] : lease.get()->values) total += key.size() + stored.size();
        lease.get()->stats.peak_memory_bytes =
            std::max(lease.get()->stats.peak_memory_bytes, total);
        return SAO_STATUS_OK;
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_get_value(
    sao_script_context_handle_t raw, const char* key_utf8, uint8_t* out_value,
    size_t out_capacity, size_t* out_required) {
    if (out_required != nullptr) *out_required = 0;
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(key_utf8) || out_required == nullptr ||
        (out_value == nullptr && out_capacity != 0)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(lease.get()->mutex);
    const auto found = lease.get()->values.find(key_utf8);
    if (found == lease.get()->values.end()) return SAO_STATUS_ERR_NOT_FOUND;
    *out_required = found->second.size();
    if (out_capacity < found->second.size() ||
        (out_value == nullptr && !found->second.empty())) return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    if (!found->second.empty()) std::memcpy(out_value, found->second.data(), found->second.size());
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_remove_value(
    sao_script_context_handle_t raw, const char* key_utf8) {
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(key_utf8)) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    std::lock_guard<std::mutex> lock(lease.get()->mutex);
    return lease.get()->values.erase(key_utf8) == 0 ? SAO_STATUS_ERR_NOT_FOUND : SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_has_capability(
    sao_script_context_handle_t raw, const char* capability_utf8, int32_t* out_supported) {
    if (out_supported != nullptr) *out_supported = 0;
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!valid_key(capability_utf8) || out_supported == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    if (std::strcmp(capability_utf8, SAO_SCRIPT_CAPABILITY_KEY_VALUE) == 0 ||
        std::strcmp(capability_utf8, SAO_SCRIPT_CAPABILITY_CANCELLATION) == 0 ||
        std::strcmp(capability_utf8, SAO_SCRIPT_CAPABILITY_ERROR) == 0) {
        *out_supported = 1;
        return SAO_STATUS_OK;
    }
    return sao_script_engine_has_capability(lease.get()->engine, lease.get()->instance,
                                            capability_utf8, out_supported);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_request_cancel(
    sao_script_context_handle_t raw) {
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    auto* context = lease.get();
    context->cancelled.store(true, std::memory_order_release);
    const auto status = sao_script_engine_cancel(context->engine, context->instance);
    if (status == SAO_STATUS_ERR_SCRIPT_UNSUPPORTED) return SAO_STATUS_OK;
    if (status != SAO_STATUS_OK) update_error(context, status, "script cancellation failed");
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_is_cancelled(
    sao_script_context_handle_t raw, int32_t* out_cancelled) {
    if (out_cancelled == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    *out_cancelled = lease.get()->cancelled.load(std::memory_order_acquire) ? 1 : 0;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_last_error(
    sao_script_context_handle_t raw, SaoScriptError* out_error) {
    if (out_error == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    ContextLease lease(acquire_context(raw));
    if (!lease) return SAO_STATUS_ERR_HANDLE_INVALID;
    std::lock_guard<std::mutex> lock(lease.get()->mutex);
    *out_error = lease.get()->last_error;
    return SAO_STATUS_OK;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_context_release_owner(
    const SaoSdkContext* owner_sdk_ctx, size_t* out_released) {
    if (owner_sdk_ctx == nullptr || out_released == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_released = 0;
    try {
        std::vector<std::shared_ptr<sao_script_context_s>> released;
        {
            std::lock_guard<std::mutex> lock(g_context_registry_mutex);
            size_t matching = 0;
            for (const auto& [_, context] : g_contexts) {
                if (context->owner == owner_sdk_ctx) ++matching;
            }
            if (matching > g_pending_contexts.max_size() -
                              g_pending_contexts.size()) {
                return SAO_STATUS_ERR_UNKNOWN;
            }
            released.reserve(matching);
            g_pending_contexts.reserve(g_pending_contexts.size() + matching);
            for (auto it = g_contexts.begin(); it != g_contexts.end();) {
                if (it->second->owner != owner_sdk_ctx) {
                    ++it;
                    continue;
                }
                auto context = it->second;
                {
                    std::lock_guard<std::mutex> context_lock(context->mutex);
                    context->destroying = true;
                }
                g_pending_contexts.push_back(context);
                released.push_back(std::move(context));
                it = g_contexts.erase(it);
            }
        }
        for (const auto& context : released) finish_context(context, true);
        *out_released = released.size();
        return SAO_STATUS_OK;
    } catch (...) {
        *out_released = 0;
        return SAO_STATUS_ERR_UNKNOWN;
    }
}
