#include "sao/scripting/script_engine.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <vector>

#include "script_internal.h"

struct sao_script_engine_s;
struct sao_script_instance_s;

struct sao_script_instance_s {
    sao_script_engine_s* engine = nullptr;
    void* impl = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    bool unloading = false;
    bool unloaded = false;
    bool finalizing = false;
    size_t active_calls = 0;
};

struct sao_script_engine_s {
    std::mutex mutex;
    std::condition_variable cv;
    SaoScriptEngineVTable vtable{};
    void* impl = nullptr;
    std::vector<std::shared_ptr<sao_script_instance_s>> instances;
    std::vector<std::shared_ptr<sao_script_instance_s>> pending_instances;
    SaoScriptError last_error{};
    bool destroying = false;
    bool cleanup_started = false;
    bool finalized = false;
    size_t active_calls = 0;
    size_t active_provider_calls = 0;
    size_t deferred_provider_releases = 0;
    bool deferred_provider_release_in_progress = false;
};

namespace {

std::mutex g_registry_mutex;
std::unordered_map<sao_script_engine_handle_t,
                   std::shared_ptr<sao_script_engine_s>> g_engines;
std::unordered_map<sao_script_engine_handle_t,
                   std::shared_ptr<sao_script_engine_s>> g_pending_engines;
std::unordered_map<sao_script_instance_handle_t,
                   std::shared_ptr<sao_script_instance_s>> g_instances;
std::unordered_map<sao_script_instance_handle_t,
                   std::shared_ptr<sao_script_instance_s>> g_pending_instances;
thread_local std::vector<const sao_script_instance_s*> g_call_stack;

struct ProviderCallbackFrame {
    sao_script_engine_s* engine = nullptr;
};

thread_local std::vector<ProviderCallbackFrame> g_provider_call_stack;

std::shared_ptr<sao_script_engine_s> acquire_engine(
    sao_script_engine_handle_t raw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        const auto found = g_engines.find(raw);
        if (found != g_engines.end()) return found->second;
        const auto pending = g_pending_engines.find(raw);
        return pending == g_pending_engines.end() ? nullptr : pending->second;
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<sao_script_instance_s> acquire_instance(
    sao_script_instance_handle_t raw) noexcept {
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        const auto found = g_instances.find(raw);
        if (found != g_instances.end()) return found->second;
        const auto pending = g_pending_instances.find(raw);
        return pending == g_pending_instances.end() ? nullptr : pending->second;
    } catch (...) {
        return nullptr;
    }
}

size_t owned_calls(const sao_script_instance_s* instance) {
    return static_cast<size_t>(std::count(g_call_stack.begin(),
                                          g_call_stack.end(), instance));
}

size_t owned_engine_calls(const sao_script_engine_s* engine) {
    return static_cast<size_t>(std::count_if(
        g_call_stack.begin(), g_call_stack.end(),
        [engine](const sao_script_instance_s* instance) {
            return instance != nullptr && instance->engine == engine;
        }));
}

bool provider_callback_active_for(const sao_script_engine_s* engine) noexcept {
    return std::any_of(
        g_provider_call_stack.begin(), g_provider_call_stack.end(),
        [engine](const ProviderCallbackFrame& frame) {
            return frame.engine == engine;
        });
}

void defer_provider_release(
    const std::shared_ptr<sao_script_engine_s>& engine) noexcept {
    if (!engine) return;
    try {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (engine->deferred_provider_releases !=
            std::numeric_limits<size_t>::max()) {
            ++engine->deferred_provider_releases;
        }
    } catch (...) {
    }
}

bool retry_one_deferred_provider_release(
    const std::shared_ptr<sao_script_engine_s>& engine) noexcept;

void maybe_finalize_engine(const std::shared_ptr<sao_script_engine_s>& engine,
                           bool wait_for_quiescence) noexcept;

void record_error(sao_script_engine_s* engine, sao_status_t status,
                  const char* message) noexcept {
    if (engine == nullptr) return;
    sao::scripting::internal::set_error(&engine->last_error, status, message);
}

void record_error_safely(
    const std::shared_ptr<sao_script_engine_s>& engine,
    sao_status_t status,
    const char* message) noexcept {
    if (!engine) return;
    try {
        std::lock_guard<std::mutex> lock(engine->mutex);
        record_error(engine.get(), status, message);
    } catch (...) {
    }
}

void clear_error(sao_script_engine_s* engine) noexcept {
    if (engine == nullptr) return;
    std::lock_guard<std::mutex> lock(engine->mutex);
    sao::scripting::internal::set_error(&engine->last_error, SAO_STATUS_OK,
                                        nullptr);
}

class ProviderCallbackLease final {
  public:
    explicit ProviderCallbackLease(
        std::shared_ptr<sao_script_engine_s> engine) noexcept
        : engine_(std::move(engine)) {
        if (!engine_) return;
        bool counted = false;
        try {
            {
                std::lock_guard<std::mutex> lock(engine_->mutex);
                ++engine_->active_provider_calls;
                counted = true;
            }
            g_provider_call_stack.push_back({engine_.get()});
            active_ = true;
        } catch (...) {
            if (counted) {
                try {
                    {
                        std::lock_guard<std::mutex> lock(engine_->mutex);
                        --engine_->active_provider_calls;
                    }
                    engine_->cv.notify_all();
                } catch (...) {
                }
            }
        }
    }

    ~ProviderCallbackLease() {
        if (!active_) return;
        g_provider_call_stack.pop_back();
        {
            std::lock_guard<std::mutex> lock(engine_->mutex);
            --engine_->active_provider_calls;
        }
        engine_->cv.notify_all();
        sao::scripting::internal::retry_deferred_context_cleanup(
            engine_.get());
        maybe_finalize_engine(engine_, false);
    }

    ProviderCallbackLease(const ProviderCallbackLease&) = delete;
    ProviderCallbackLease& operator=(const ProviderCallbackLease&) = delete;
    explicit operator bool() const noexcept { return active_; }

  private:
    std::shared_ptr<sao_script_engine_s> engine_;
    bool active_ = false;
};

bool try_provider_release(
    const std::shared_ptr<sao_script_engine_s>& engine,
    const SaoScriptEngineVTable& vtable) noexcept {
    ProviderCallbackLease callback_lease(engine);
    if (!callback_lease) {
        record_error_safely(
            engine, SAO_STATUS_ERR_UNKNOWN,
            "language provider release callback lease failed");
        return false;
    }
    try {
        vtable.release(vtable.user_data);
        return true;
    } catch (const std::exception& error) {
        record_error_safely(engine, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                            error.what());
    } catch (...) {
        record_error_safely(
            engine, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
            "language provider release crossed exception barrier");
    }
    return false;
}

template <typename Callback>
sao_status_t provider_call(
    const std::shared_ptr<sao_script_engine_s>& engine,
                           Callback&& callback) noexcept {
    ProviderCallbackLease callback_lease(engine);
    if (!callback_lease) return SAO_STATUS_ERR_UNKNOWN;
    try {
        const auto status = callback();
        if (status != SAO_STATUS_OK) {
            std::lock_guard<std::mutex> lock(engine->mutex);
            record_error(engine.get(), status, "language provider call failed");
        }
        return status;
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(engine->mutex);
        record_error(engine.get(), SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     error.what());
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    } catch (...) {
        std::lock_guard<std::mutex> lock(engine->mutex);
        record_error(engine.get(), SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     "language provider crossed exception barrier");
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    }
}

template <typename Callback>
void provider_void_call(
    const std::shared_ptr<sao_script_engine_s>& engine,
    Callback&& callback) noexcept {
    ProviderCallbackLease callback_lease(engine);
    if (!callback_lease) {
        if (engine) {
            record_error(engine.get(), SAO_STATUS_ERR_UNKNOWN,
                         "language provider callback lease failed");
        }
        return;
    }
    try {
        callback();
    } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(engine->mutex);
        record_error(engine.get(), SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     error.what());
    } catch (...) {
        std::lock_guard<std::mutex> lock(engine->mutex);
        record_error(engine.get(), SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     "language provider crossed exception barrier");
    }
}

bool has_field(const SaoScriptEngineVTable& vtable, size_t offset,
               size_t size) noexcept {
    return sao::scripting::internal::has_provider_field(vtable, offset, size);
}

void unload_instance(const std::shared_ptr<sao_script_engine_s>& engine,
                     const std::shared_ptr<sao_script_instance_s>& instance) noexcept;
void maybe_finalize_engine(const std::shared_ptr<sao_script_engine_s>& engine,
                           bool wait_for_quiescence) noexcept;

bool hide_instance(const std::shared_ptr<sao_script_engine_s>& engine,
                   const std::shared_ptr<sao_script_instance_s>& instance) noexcept {
    if (!engine || !instance) return false;
    try {
        std::lock_guard<std::mutex> engine_lock(engine->mutex);
        const auto pending = std::find_if(
            engine->pending_instances.begin(), engine->pending_instances.end(),
            [&instance](const auto& entry) { return entry.get() == instance.get(); });
        const bool add_engine_pending = pending == engine->pending_instances.end();
        if (add_engine_pending) {
            if (engine->pending_instances.size() ==
                engine->pending_instances.max_size()) {
                return false;
            }
            engine->pending_instances.reserve(engine->pending_instances.size() + 1);
        }
        std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
        const auto pending_registry = g_pending_instances.find(instance.get());
        if (pending_registry != g_pending_instances.end() &&
            pending_registry->second.get() != instance.get()) {
            return false;
        }
        if (pending_registry == g_pending_instances.end()) {
            const auto [unused, inserted] =
                g_pending_instances.emplace(instance.get(), instance);
            (void)unused;
            if (!inserted) return false;
        }
        engine->instances.erase(
            std::remove_if(engine->instances.begin(), engine->instances.end(),
                           [&instance](const auto& entry) {
                               return entry.get() == instance.get();
                           }),
            engine->instances.end());
        if (add_engine_pending) engine->pending_instances.push_back(instance);
        const auto found = g_instances.find(instance.get());
        if (found != g_instances.end() && found->second.get() == instance.get()) {
            g_instances.erase(found);
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool mark_instance_unloading(
    const std::shared_ptr<sao_script_engine_s>& engine,
    const std::shared_ptr<sao_script_instance_s>& instance) noexcept {
    if (!engine || !instance) return false;
    {
        std::lock_guard<std::mutex> lock(instance->mutex);
        if (instance->unloaded) return false;
        if (instance->unloading) return false;
        instance->unloading = true;
    }
    try {
        if (!hide_instance(engine, instance)) {
            std::lock_guard<std::mutex> lock(instance->mutex);
            instance->unloading = false;
            return false;
        }
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(instance->mutex);
            instance->unloading = false;
        } catch (...) {
        }
        return false;
    }
    return true;
}

void destroy_instance_impl(const std::shared_ptr<sao_script_engine_s>& engine,
                           void* instance_impl) noexcept {
    if (!engine || instance_impl == nullptr) return;
    SaoScriptEngineVTable vtable{};
    void* engine_impl = nullptr;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        vtable = engine->vtable;
        engine_impl = engine->impl;
    }
    if (engine_impl == nullptr) return;
    provider_void_call(engine, [&] {
        if (has_field(vtable, offsetof(SaoScriptEngineVTable, unload),
                      sizeof(vtable.unload)) &&
            vtable.unload != nullptr) {
            vtable.unload(engine_impl, instance_impl, vtable.user_data);
        } else if (vtable.context_destroy != nullptr) {
            vtable.context_destroy(engine_impl, instance_impl);
        }
    });
}

bool finalize_instance(const std::shared_ptr<sao_script_engine_s>& engine,
                       const std::shared_ptr<sao_script_instance_s>& instance,
                       bool wait_for_quiescence) noexcept {
    if (!engine || !instance || provider_callback_active_for(engine.get())) {
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(engine->mutex);
        if (engine->active_provider_calls != 0) {
            if (!wait_for_quiescence) return false;
            engine->cv.wait(lock, [&] { return engine->active_provider_calls == 0; });
        }
    }
    void* instance_impl = nullptr;
    for (;;) {
        std::unique_lock<std::mutex> lock(instance->mutex);
        if (instance->unloaded) return true;
        if (instance->finalizing) {
            if (!wait_for_quiescence) return false;
            instance->cv.wait(lock, [&] {
                return instance->unloaded || !instance->finalizing;
            });
            continue;
        }
        if (instance->active_calls != 0) {
            if (!wait_for_quiescence || owned_calls(instance.get()) != 0) {
                return false;
            }
            instance->cv.wait(lock, [&] {
                return instance->unloaded || instance->active_calls == 0;
            });
            continue;
        }
        instance->finalizing = true;
        instance_impl = instance->impl;
        break;
    }
    destroy_instance_impl(engine, instance_impl);
    {
        std::lock_guard<std::mutex> lock(instance->mutex);
        instance->impl = nullptr;
        instance->unloaded = true;
        instance->finalizing = false;
    }
    instance->cv.notify_all();
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine->pending_instances.erase(
            std::remove_if(engine->pending_instances.begin(),
                           engine->pending_instances.end(),
                           [&instance](const auto& entry) {
                               return entry.get() == instance.get();
                           }),
            engine->pending_instances.end());
    }
    {
        std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
        const auto found = g_pending_instances.find(instance.get());
        if (found != g_pending_instances.end() &&
            found->second.get() == instance.get()) {
            g_pending_instances.erase(found);
        }
    }
    engine->cv.notify_all();
    return true;
}

void destroy_engine_impl(const std::shared_ptr<sao_script_engine_s>& engine) noexcept {
    if (!engine) return;
    SaoScriptEngineVTable vtable{};
    void* engine_impl = nullptr;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (engine->impl == nullptr) return;
        vtable = engine->vtable;
        engine_impl = engine->impl;
        engine->impl = nullptr;
    }
    provider_void_call(engine, [&] {
        if (has_field(vtable, offsetof(SaoScriptEngineVTable, destroy),
                      sizeof(vtable.destroy)) &&
            vtable.destroy != nullptr) {
            vtable.destroy(engine_impl, vtable.user_data);
        } else if (vtable.engine_shutdown != nullptr) {
            vtable.engine_shutdown(engine_impl);
        }
    });
}

bool retry_one_deferred_provider_release(
    const std::shared_ptr<sao_script_engine_s>& engine) noexcept {
    if (!engine) return false;
    SaoScriptEngineVTable vtable{};
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (engine->deferred_provider_releases == 0 ||
            engine->deferred_provider_release_in_progress ||
            engine->active_provider_calls != 0) {
            return false;
        }
        --engine->deferred_provider_releases;
        engine->deferred_provider_release_in_progress = true;
        vtable = engine->vtable;
    }

    const bool released = try_provider_release(engine, vtable);
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (!released) ++engine->deferred_provider_releases;
        engine->deferred_provider_release_in_progress = false;
    }
    engine->cv.notify_all();
    if (released) {
        sao::scripting::internal::retry_deferred_context_cleanup(engine.get());
    }
    return released;
}

void maybe_finalize_engine(const std::shared_ptr<sao_script_engine_s>& engine,
                           bool wait_for_quiescence) noexcept {
    if (!engine) return;
    if (provider_callback_active_for(engine.get()) ||
        owned_engine_calls(engine.get()) != 0) {
        wait_for_quiescence = false;
    }
    for (;;) {
        while (retry_one_deferred_provider_release(engine)) {
        }
        std::vector<std::shared_ptr<sao_script_instance_s>> pending_instances;
        {
            std::unique_lock<std::mutex> lock(engine->mutex);
            if (engine->cleanup_started) {
                if (!wait_for_quiescence) return;
                engine->cv.wait(lock, [&] { return !engine->cleanup_started; });
                continue;
            }
            if (engine->active_calls != 0 || engine->active_provider_calls != 0) {
                if (!wait_for_quiescence) return;
                engine->cv.wait(lock, [&] {
                    return engine->active_calls == 0 &&
                           engine->active_provider_calls == 0;
                });
                continue;
            }
            try {
                pending_instances = engine->pending_instances;
            } catch (...) {
                return;
            }
            if (!pending_instances.empty()) engine->cleanup_started = true;
        }
        if (!pending_instances.empty()) {
            for (const auto& instance : pending_instances) {
                finalize_instance(engine, instance, wait_for_quiescence);
            }
            {
                std::lock_guard<std::mutex> lock(engine->mutex);
                engine->cleanup_started = false;
            }
            engine->cv.notify_all();
            if (!wait_for_quiescence) return;
            continue;
        }
        bool finalize_engine = false;
        {
            std::lock_guard<std::mutex> lock(engine->mutex);
            if (engine->destroying && !engine->finalized &&
                engine->active_calls == 0 && engine->active_provider_calls == 0 &&
                engine->deferred_provider_releases == 0 &&
                !engine->deferred_provider_release_in_progress &&
                engine->instances.empty() &&
                engine->pending_instances.empty()) {
                engine->finalized = true;
                finalize_engine = true;
            }
        }
        if (!finalize_engine) return;
        const auto vtable = engine->vtable;
        destroy_engine_impl(engine);
        sao::scripting::internal::release_provider(vtable);
        std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
        g_engines.erase(engine.get());
        const auto pending_engine = g_pending_engines.find(engine.get());
        if (pending_engine != g_pending_engines.end() &&
            pending_engine->second.get() == engine.get()) {
            g_pending_engines.erase(pending_engine);
        }
        return;
    }
}

void unload_instance(const std::shared_ptr<sao_script_engine_s>& engine,
                     const std::shared_ptr<sao_script_instance_s>& instance) noexcept {
    if (!mark_instance_unloading(engine, instance)) return;
    if (provider_callback_active_for(engine.get()) || owned_calls(instance.get()) != 0) {
        return;
    }
    finalize_instance(engine, instance, true);
    maybe_finalize_engine(engine, true);
}

void mark_engine_instances_unloading(
    const std::shared_ptr<sao_script_engine_s>& engine) noexcept {
    std::vector<std::shared_ptr<sao_script_instance_s>> instances;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        try {
            instances = engine->instances;
        } catch (...) {
            return;
        }
    }
    for (const auto& instance : instances) {
        mark_instance_unloading(engine, instance);
    }
}

class EngineOperationLease final {
  public:
    explicit EngineOperationLease(std::shared_ptr<sao_script_engine_s> engine)
        : engine_(std::move(engine)) {
        if (!engine_) return;
        std::lock_guard<std::mutex> lock(engine_->mutex);
        if (engine_->destroying || engine_->impl == nullptr) return;
        ++engine_->active_calls;
        vtable_ = engine_->vtable;
        engine_impl_ = engine_->impl;
        active_ = true;
    }

    ~EngineOperationLease() {
        if (!active_) return;
        {
            std::lock_guard<std::mutex> lock(engine_->mutex);
            --engine_->active_calls;
        }
        engine_->cv.notify_all();
        maybe_finalize_engine(engine_, false);
    }

    EngineOperationLease(const EngineOperationLease&) = delete;
    EngineOperationLease& operator=(const EngineOperationLease&) = delete;

    explicit operator bool() const noexcept { return active_; }
    void* engine_impl() const noexcept { return engine_impl_; }
    const SaoScriptEngineVTable& vtable() const noexcept { return vtable_; }

  private:
    std::shared_ptr<sao_script_engine_s> engine_;
    SaoScriptEngineVTable vtable_{};
    void* engine_impl_ = nullptr;
    bool active_ = false;
};

class InstanceLease final {
  public:
    InstanceLease(std::shared_ptr<sao_script_engine_s> engine,
                  std::shared_ptr<sao_script_instance_s> instance) noexcept {
        status_ = begin(engine, instance);
    }

    sao_status_t begin(const std::shared_ptr<sao_script_engine_s>& engine,
                       const std::shared_ptr<sao_script_instance_s>& instance) noexcept {
        engine_ = engine;
        instance_ = instance;
        if (!engine_ || !instance_) return SAO_STATUS_ERR_HANDLE_INVALID;
        try {
            bool stack_push_failed = false;
            {
                std::scoped_lock lock(engine_->mutex, instance_->mutex);
                if (engine_->destroying || instance_->unloading ||
                    instance_->engine != engine_.get()) {
                    return SAO_STATUS_ERR_HANDLE_INVALID;
                }
                ++engine_->active_calls;
                ++instance_->active_calls;
                call_counted_ = true;
                try {
                    g_call_stack.push_back(instance_.get());
                } catch (...) {
                    stack_push_failed = true;
                }
                if (!stack_push_failed) {
                    stack_pushed_ = true;
                    vtable_ = engine_->vtable;
                    engine_impl_ = engine_->impl;
                    ctx_impl_ = instance_->impl;
                }
            }
            if (stack_push_failed) {
                rollback_begin();
                return SAO_STATUS_ERR_UNKNOWN;
            }
            if (has_field(vtable_, offsetof(SaoScriptEngineVTable, retain),
                          sizeof(vtable_.retain)) &&
                vtable_.retain != nullptr) {
                ProviderCallbackLease callback_lease(engine_);
                if (!callback_lease) {
                    rollback_begin();
                    return SAO_STATUS_ERR_UNKNOWN;
                }
                vtable_.retain(vtable_.user_data);
            }
        } catch (...) {
            rollback_begin();
            return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
        }
        active_ = true;
        return SAO_STATUS_OK;
    }

    ~InstanceLease() {
        if (!active_) return;
        if (has_field(vtable_, offsetof(SaoScriptEngineVTable, release),
                      sizeof(vtable_.release)) &&
            vtable_.release != nullptr) {
            if (!try_provider_release(engine_, vtable_)) {
                defer_provider_release(engine_);
            }
        }
        g_call_stack.pop_back();
        stack_pushed_ = false;
        {
            std::lock_guard<std::mutex> lock(instance_->mutex);
            --instance_->active_calls;
        }
        {
            std::lock_guard<std::mutex> lock(engine_->mutex);
            --engine_->active_calls;
        }
        instance_->cv.notify_all();
        engine_->cv.notify_all();
        maybe_finalize_engine(engine_, false);
    }

    InstanceLease(const InstanceLease&) = delete;
    InstanceLease& operator=(const InstanceLease&) = delete;

    void* engine_impl() const noexcept { return engine_impl_; }
    void* context_impl() const noexcept { return ctx_impl_; }
    const SaoScriptEngineVTable& vtable() const noexcept { return vtable_; }
    sao_status_t status() const noexcept { return status_; }
    explicit operator bool() const noexcept { return active_; }

  private:
    void rollback_begin() noexcept {
        if (stack_pushed_) {
            g_call_stack.pop_back();
            stack_pushed_ = false;
        }
        if (call_counted_) {
            {
                std::lock_guard<std::mutex> lock(instance_->mutex);
                --instance_->active_calls;
            }
            {
                std::lock_guard<std::mutex> lock(engine_->mutex);
                --engine_->active_calls;
            }
            call_counted_ = false;
            instance_->cv.notify_all();
            engine_->cv.notify_all();
        }
    }

    std::shared_ptr<sao_script_engine_s> engine_;
    std::shared_ptr<sao_script_instance_s> instance_;
    SaoScriptEngineVTable vtable_{};
    void* engine_impl_ = nullptr;
    void* ctx_impl_ = nullptr;
    bool call_counted_ = false;
    bool stack_pushed_ = false;
    bool active_ = false;
    sao_status_t status_ = SAO_STATUS_ERR_HANDLE_INVALID;
};

}  // namespace

namespace sao::scripting::internal {

bool provider_callback_active(sao_script_engine_handle_t engine) noexcept {
    return provider_callback_active_for(engine);
}

void retry_deferred_engine_cleanup(
    sao_script_engine_handle_t raw_engine) noexcept {
    const auto engine = acquire_engine(raw_engine);
    if (engine) maybe_finalize_engine(engine, false);
}

bool cleanup_engine_instance(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance) noexcept {
    const auto engine = acquire_engine(raw_engine);
    const auto instance = acquire_instance(raw_instance);
    if (!engine || !instance || instance->engine != engine.get()) return false;
    unload_instance(engine, instance);
    sao_script_engine_destroy(engine.get());
    maybe_finalize_engine(engine, true);
    bool instance_done = false;
    {
        std::lock_guard<std::mutex> lock(instance->mutex);
        instance_done = instance->unloaded;
    }
    bool engine_done = false;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        engine_done = engine->finalized;
    }
    return instance_done && engine_done;
}

}  // namespace sao::scripting::internal

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_open(
    int32_t language, sao_script_engine_handle_t* out_engine) {
    return sao_script_engine_create(language, out_engine);
}

extern "C" void SAO_SCRIPTING_CALL sao_script_engine_close(
    sao_script_engine_handle_t engine) {
    sao_script_engine_destroy(engine);
}

extern "C" const SaoScriptEngineVTable* SAO_SCRIPTING_CALL
sao_script_engine_vtable(sao_script_engine_handle_t engine) {
    auto held = acquire_engine(engine);
    return held ? &held->vtable : nullptr;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_create(
    int32_t language, sao_script_engine_handle_t* out_engine) {
    if (out_engine == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_engine = nullptr;
    SaoScriptEngineVTable provider{};
    const auto provider_status =
        sao::scripting::internal::acquire_provider(language, &provider);
    if (provider_status != SAO_STATUS_OK) return provider_status;
    std::shared_ptr<sao_script_engine_s> engine;
    try {
        engine = std::make_shared<sao_script_engine_s>();
    } catch (...) {
        sao::scripting::internal::release_provider(provider);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    engine->vtable = provider;
    sao_status_t status = SAO_STATUS_OK;
    if (has_field(engine->vtable, offsetof(SaoScriptEngineVTable, create),
                  sizeof(engine->vtable.create)) &&
        engine->vtable.create != nullptr) {
        status = provider_call(engine, [&] {
            return engine->vtable.create(engine->vtable.user_data, &engine->impl);
        });
    } else {
        status = provider_call(engine, [&] {
            return engine->vtable.engine_init(&engine->impl);
        });
    }
    if (status != SAO_STATUS_OK) {
        destroy_engine_impl(engine);
        sao::scripting::internal::release_provider(engine->vtable);
        return status;
    }
    bool inserted = false;
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        const auto [unused, result] = g_engines.emplace(engine.get(), engine);
        (void)unused;
        inserted = result;
    } catch (...) {
        destroy_engine_impl(engine);
        sao::scripting::internal::release_provider(engine->vtable);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    if (!inserted) {
        destroy_engine_impl(engine);
        sao::scripting::internal::release_provider(engine->vtable);
        return SAO_STATUS_ERR_ALREADY_EXISTS;
    }
    *out_engine = engine.get();
    return SAO_STATUS_OK;
}

extern "C" void SAO_SCRIPTING_CALL sao_script_engine_destroy(
    sao_script_engine_handle_t raw_engine) {
    auto engine = acquire_engine(raw_engine);
    if (!engine) return;
    {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (engine->finalized) return;
        engine->destroying = true;
    }
    bool pending_conflict = false;
    try {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        const auto [unused, inserted] =
            g_pending_engines.emplace(engine.get(), engine);
        (void)unused;
        if (!inserted) {
            const auto pending = g_pending_engines.find(engine.get());
            pending_conflict = pending == g_pending_engines.end() ||
                               pending->second.get() != engine.get();
        }
        if (!pending_conflict) g_engines.erase(engine.get());
    } catch (...) {
        try {
            std::lock_guard<std::mutex> lock(engine->mutex);
            if (!engine->finalized) engine->destroying = false;
        } catch (...) {
        }
        return;
    }
    if (pending_conflict) {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (!engine->finalized) engine->destroying = false;
        return;
    }
    mark_engine_instances_unloading(engine);
    const bool wait = !provider_callback_active_for(engine.get()) &&
                      owned_engine_calls(engine.get()) == 0;
    maybe_finalize_engine(engine, wait);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_load(
    sao_script_engine_handle_t raw_engine,
    const SaoScriptContextConfig* config,
    sao_script_instance_handle_t* out_instance) {
    auto engine = acquire_engine(raw_engine);
    if (!engine || config == nullptr || out_instance == nullptr ||
        config->language != engine->vtable.language) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_instance = nullptr;
    std::shared_ptr<sao_script_instance_s> instance;
    try {
        instance = std::make_shared<sao_script_instance_s>();
    } catch (...) {
        return SAO_STATUS_ERR_UNKNOWN;
    }
    instance->engine = engine.get();
    EngineOperationLease operation(engine);
    if (!operation) return SAO_STATUS_ERR_HANDLE_INVALID;
    const auto& vtable = operation.vtable();
    void* engine_impl = operation.engine_impl();
    sao_status_t status = provider_call(engine, [&] {
        if (has_field(vtable, offsetof(SaoScriptEngineVTable, load),
                      sizeof(vtable.load)) &&
            vtable.load != nullptr) {
            return vtable.load(engine_impl, config, &instance->impl,
                               vtable.user_data);
        }
        return vtable.context_create(engine_impl, config, &instance->impl);
    });
    bool registered = false;
    if (status == SAO_STATUS_OK) {
        std::lock_guard<std::mutex> lock(engine->mutex);
        if (!engine->destroying) {
            try {
                engine->instances.push_back(instance);
                std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
                const auto [unused, inserted] = g_instances.emplace(instance.get(), instance);
                (void)unused;
                if (inserted) {
                    registered = true;
                } else {
                    engine->instances.pop_back();
                    status = SAO_STATUS_ERR_ALREADY_EXISTS;
                }
            } catch (...) {
                if (!engine->instances.empty() &&
                    engine->instances.back().get() == instance.get()) {
                    engine->instances.pop_back();
                }
                status = SAO_STATUS_ERR_UNKNOWN;
            }
        } else {
            status = SAO_STATUS_ERR_HANDLE_INVALID;
        }
    }
    if (!registered) {
        destroy_instance_impl(engine, instance->impl);
        instance->impl = nullptr;
        maybe_finalize_engine(engine, false);
        return status;
    }
    *out_instance = instance.get();
    maybe_finalize_engine(engine, false);
    return SAO_STATUS_OK;
}

extern "C" void SAO_SCRIPTING_CALL sao_script_engine_unload(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance) {
    auto engine = acquire_engine(raw_engine);
    auto instance = acquire_instance(raw_instance);
    if (!engine || !instance || instance->engine != engine.get()) return;
    unload_instance(engine, instance);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_run(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance) {
    auto engine = acquire_engine(raw_engine);
    auto instance = acquire_instance(raw_instance);
    std::optional<InstanceLease> lease;
    lease.emplace(engine, instance);
    const auto begin_status = lease->status();
    if (begin_status != SAO_STATUS_OK) return begin_status;
    if (has_field(lease->vtable(), offsetof(SaoScriptEngineVTable, run),
                  sizeof(lease->vtable().run)) &&
        lease->vtable().run != nullptr) {
        const auto run_status = provider_call(engine, [&] {
            return lease->vtable().run(lease->engine_impl(), lease->context_impl(),
                                       lease->vtable().user_data);
        });
        if (run_status == SAO_STATUS_OK) clear_error(engine.get());
        return run_status;
    }
    const auto run_status = provider_call(engine, [&] {
        return lease->vtable().context_run(lease->engine_impl(), lease->context_impl());
    });
    if (run_status == SAO_STATUS_OK) clear_error(engine.get());
    return run_status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_invoke(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance,
    const char* function_name_utf8,
    const uint8_t* arg_json_utf8,
    size_t arg_len,
    uint8_t* out_result_json_utf8,
    size_t out_capacity,
    size_t* out_required) {
    if (out_required != nullptr) *out_required = 0;
    if (function_name_utf8 == nullptr || function_name_utf8[0] == '\0' ||
        (arg_json_utf8 == nullptr && arg_len != 0) || out_required == nullptr ||
        (out_result_json_utf8 == nullptr && out_capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto engine = acquire_engine(raw_engine);
    auto instance = acquire_instance(raw_instance);
    std::optional<InstanceLease> lease;
    lease.emplace(engine, instance);
    const auto begin_status = lease->status();
    if (begin_status != SAO_STATUS_OK) return begin_status;
    const auto status = provider_call(engine, [&] {
        if (has_field(lease->vtable(), offsetof(SaoScriptEngineVTable, invoke),
                      sizeof(lease->vtable().invoke)) &&
            lease->vtable().invoke != nullptr) {
            return lease->vtable().invoke(
                lease->engine_impl(), lease->context_impl(), function_name_utf8,
                arg_json_utf8, arg_len, out_result_json_utf8, out_capacity,
                out_required, lease->vtable().user_data);
        }
        return lease->vtable().context_call(
            lease->engine_impl(), lease->context_impl(), function_name_utf8,
            arg_json_utf8, arg_len, out_result_json_utf8, out_capacity,
            out_required);
    });
    if (status == SAO_STATUS_OK && *out_required > out_capacity) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    if (status == SAO_STATUS_OK) clear_error(engine.get());
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_has_capability(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance,
    const char* capability_utf8,
    int32_t* out_supported) {
    if (out_supported != nullptr) *out_supported = 0;
    if (capability_utf8 == nullptr || capability_utf8[0] == '\0' ||
        out_supported == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    auto engine = acquire_engine(raw_engine);
    auto instance = acquire_instance(raw_instance);
    std::optional<InstanceLease> lease;
    lease.emplace(engine, instance);
    const auto status = lease->status();
    if (status != SAO_STATUS_OK) return status;
    if (!has_field(lease->vtable(),
                   offsetof(SaoScriptEngineVTable, context_has_capability),
                   sizeof(lease->vtable().context_has_capability)) ||
        lease->vtable().context_has_capability == nullptr) {
        return SAO_STATUS_OK;
    }
    return provider_call(engine, [&] {
        return lease->vtable().context_has_capability(
            lease->engine_impl(), lease->context_impl(), capability_utf8,
            out_supported);
    });
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_cancel(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance) {
    auto engine = acquire_engine(raw_engine);
    auto instance = acquire_instance(raw_instance);
    std::optional<InstanceLease> lease;
    lease.emplace(engine, instance);
    const auto status = lease->status();
    if (status != SAO_STATUS_OK) return status;
    if (!has_field(lease->vtable(), offsetof(SaoScriptEngineVTable, context_cancel),
                   sizeof(lease->vtable().context_cancel)) ||
        lease->vtable().context_cancel == nullptr) {
        return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
    }
    return provider_call(engine, [&] {
        return lease->vtable().context_cancel(lease->engine_impl(),
                                             lease->context_impl());
    });
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_last_error(
    sao_script_engine_handle_t raw_engine,
    sao_script_instance_handle_t raw_instance,
    SaoScriptError* out_error) {
    if (out_error == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_scripting_error_clear(out_error);
    auto engine = acquire_engine(raw_engine);
    auto instance = acquire_instance(raw_instance);
    std::optional<InstanceLease> lease;
    lease.emplace(engine, instance);
    const auto status = lease->status();
    if (status != SAO_STATUS_OK) {
        out_error->status = status;
        return status;
    }
    auto copy_fallback = [&] {
        std::lock_guard<std::mutex> lock(engine->mutex);
        *out_error = engine->last_error;
        return SAO_STATUS_OK;
    };
    if (has_field(lease->vtable(), offsetof(SaoScriptEngineVTable, last_error),
                  sizeof(lease->vtable().last_error)) &&
        lease->vtable().last_error != nullptr) {
        const auto query_status = provider_call(engine, [&] {
            return lease->vtable().last_error(
                lease->engine_impl(), lease->context_impl(), out_error,
                lease->vtable().user_data);
        });
        if (query_status == SAO_STATUS_OK &&
            (out_error->status != SAO_STATUS_OK ||
             out_error->message_utf8[0] != '\0')) {
            return SAO_STATUS_OK;
        }
        return copy_fallback();
    }
    if (has_field(lease->vtable(), offsetof(SaoScriptEngineVTable, context_last_error),
                  sizeof(lease->vtable().context_last_error)) &&
        lease->vtable().context_last_error != nullptr) {
        const auto query_status = provider_call(engine, [&] {
            return lease->vtable().context_last_error(
                lease->engine_impl(), lease->context_impl(), out_error);
        });
        if (query_status == SAO_STATUS_OK &&
            (out_error->status != SAO_STATUS_OK ||
             out_error->message_utf8[0] != '\0')) {
            return SAO_STATUS_OK;
        }
        return copy_fallback();
    }
    return copy_fallback();
}
