#include "sao/scripting/script_engine.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <new>
#include <vector>

#include "script_internal.h"

struct sao_script_engine_s;

struct sao_script_instance_s {
    sao_script_engine_s* engine = nullptr;
    void* impl = nullptr;
};

struct sao_script_engine_s {
    std::recursive_mutex mutex;
    SaoScriptEngineVTable vtable{};
    void* impl = nullptr;
    std::vector<sao_script_instance_s*> instances;
    SaoScriptError last_error{};
};

namespace {

void record_error(sao_script_engine_s* engine,
                  sao_status_t status,
                  const char* message) noexcept {
    sao::scripting::internal::set_error(&engine->last_error, status, message);
}

template <typename Callback>
sao_status_t provider_call(sao_script_engine_s* engine,
                           Callback&& callback) noexcept {
    try {
        const sao_status_t status = callback();
        if (status != SAO_STATUS_OK) {
            record_error(engine, status, "language provider call failed");
        }
        return status;
    } catch (const std::exception& error) {
        record_error(engine, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     error.what());
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    } catch (...) {
        record_error(engine, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     "language provider crossed exception barrier");
        return SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION;
    }
}

bool owns_instance(const sao_script_engine_s* engine,
                   const sao_script_instance_s* instance) noexcept {
    if (instance == nullptr ||
        std::find(engine->instances.begin(), engine->instances.end(),
                  instance) == engine->instances.end()) {
        return false;
    }
    return instance->engine == engine;
}

bool has_unified_field(const SaoScriptEngineVTable& vtable,
                       size_t field_offset,
                       size_t field_size) noexcept;

void unload_locked(sao_script_engine_s* engine,
                   sao_script_instance_s* instance) noexcept {
    const auto found = std::find(engine->instances.begin(),
                                 engine->instances.end(), instance);
    if (found == engine->instances.end()) return;
    try {
        if (has_unified_field(engine->vtable,
                              offsetof(SaoScriptEngineVTable, unload),
                              sizeof(engine->vtable.unload)) &&
            engine->vtable.unload != nullptr) {
            engine->vtable.unload(engine->impl, instance->impl,
                                  engine->vtable.user_data);
        } else {
            engine->vtable.context_destroy(engine->impl, instance->impl);
        }
    } catch (...) {
        record_error(engine, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                     "language provider context_destroy threw");
    }
    engine->instances.erase(found);
    delete instance;
}

bool has_capability_slot(const SaoScriptEngineVTable& vtable) noexcept {
    return sao::scripting::internal::has_provider_field(
               vtable,
               offsetof(SaoScriptEngineVTable, context_has_capability),
               sizeof(vtable.context_has_capability)) &&
           vtable.context_has_capability != nullptr;
}

bool has_cancel_slot(const SaoScriptEngineVTable& vtable) noexcept {
    return sao::scripting::internal::has_provider_field(
               vtable, offsetof(SaoScriptEngineVTable, context_cancel),
               sizeof(vtable.context_cancel)) &&
           vtable.context_cancel != nullptr;
}

bool has_unified_field(const SaoScriptEngineVTable& vtable,
                       size_t field_offset,
                       size_t field_size) noexcept {
    return sao::scripting::internal::has_provider_field(
        vtable, field_offset, field_size);
}

}  // namespace

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
    return engine == nullptr ? nullptr : &engine->vtable;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_create(
    int32_t language, sao_script_engine_handle_t* out_engine) {
    if (out_engine == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    *out_engine = nullptr;

    SaoScriptEngineVTable provider{};
    const sao_status_t provider_status =
        sao::scripting::internal::acquire_provider(language, &provider);
    if (provider_status != SAO_STATUS_OK) return provider_status;

    auto* engine = new (std::nothrow) sao_script_engine_s();
    if (engine == nullptr) {
        sao::scripting::internal::release_provider(provider);
        return SAO_STATUS_ERR_UNKNOWN;
    }
    engine->vtable = provider;
    const sao_status_t init_status = provider_call(engine, [&] {
        if (has_unified_field(engine->vtable,
                              offsetof(SaoScriptEngineVTable, create),
                              sizeof(engine->vtable.create)) &&
            engine->vtable.create != nullptr) {
            return engine->vtable.create(engine->vtable.user_data,
                                         &engine->impl);
        }
        return engine->vtable.engine_init(&engine->impl);
    });
    if (init_status != SAO_STATUS_OK) {
        if (engine->impl != nullptr) {
            try {
                if (has_unified_field(engine->vtable,
                                      offsetof(SaoScriptEngineVTable, destroy),
                                      sizeof(engine->vtable.destroy)) &&
                    engine->vtable.destroy != nullptr) {
                    engine->vtable.destroy(engine->impl,
                                           engine->vtable.user_data);
                } else {
                    engine->vtable.engine_shutdown(engine->impl);
                }
            } catch (...) {
            }
        }
        sao::scripting::internal::release_provider(engine->vtable);
        delete engine;
        return init_status;
    }
    *out_engine = engine;
    return SAO_STATUS_OK;
}

extern "C" void SAO_SCRIPTING_CALL sao_script_engine_destroy(
    sao_script_engine_handle_t engine) {
    if (engine == nullptr) return;
    {
        std::lock_guard lock(engine->mutex);
        while (!engine->instances.empty()) {
            unload_locked(engine, engine->instances.back());
        }
        try {
            if (has_unified_field(engine->vtable,
                                  offsetof(SaoScriptEngineVTable, destroy),
                                  sizeof(engine->vtable.destroy)) &&
                engine->vtable.destroy != nullptr) {
                engine->vtable.destroy(engine->impl,
                                       engine->vtable.user_data);
            } else {
                engine->vtable.engine_shutdown(engine->impl);
            }
        } catch (...) {
            record_error(engine, SAO_STATUS_ERR_SCRIPT_PROVIDER_EXCEPTION,
                         "language provider engine_shutdown threw");
        }
    }
    sao::scripting::internal::release_provider(engine->vtable);
    delete engine;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_load(
    sao_script_engine_handle_t engine,
    const SaoScriptContextConfig* config,
    sao_script_instance_handle_t* out_instance) {
    if (engine == nullptr || config == nullptr || out_instance == nullptr ||
        config->language != engine->vtable.language) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    *out_instance = nullptr;
    auto* instance = new (std::nothrow) sao_script_instance_s();
    if (instance == nullptr) return SAO_STATUS_ERR_UNKNOWN;
    instance->engine = engine;

    std::lock_guard lock(engine->mutex);
    const sao_status_t status = provider_call(engine, [&] {
        if (has_unified_field(engine->vtable,
                              offsetof(SaoScriptEngineVTable, load),
                              sizeof(engine->vtable.load)) &&
            engine->vtable.load != nullptr) {
            return engine->vtable.load(engine->impl, config, &instance->impl,
                                       engine->vtable.user_data);
        }
        return engine->vtable.context_create(engine->impl, config,
                                             &instance->impl);
    });
    if (status != SAO_STATUS_OK) {
        if (instance->impl != nullptr) {
            try {
                if (has_unified_field(engine->vtable,
                                      offsetof(SaoScriptEngineVTable, unload),
                                      sizeof(engine->vtable.unload)) &&
                    engine->vtable.unload != nullptr) {
                    engine->vtable.unload(engine->impl, instance->impl,
                                          engine->vtable.user_data);
                } else {
                    engine->vtable.context_destroy(engine->impl,
                                                   instance->impl);
                }
            } catch (...) {
            }
        }
        delete instance;
        return status;
    }
    try {
        engine->instances.push_back(instance);
    } catch (...) {
        try {
            if (has_unified_field(engine->vtable,
                                  offsetof(SaoScriptEngineVTable, unload),
                                  sizeof(engine->vtable.unload)) &&
                engine->vtable.unload != nullptr) {
                engine->vtable.unload(engine->impl, instance->impl,
                                      engine->vtable.user_data);
            } else {
                engine->vtable.context_destroy(engine->impl, instance->impl);
            }
        } catch (...) {
        }
        delete instance;
        return SAO_STATUS_ERR_UNKNOWN;
    }
    *out_instance = instance;
    return SAO_STATUS_OK;
}

extern "C" void SAO_SCRIPTING_CALL sao_script_engine_unload(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance) {
    if (engine == nullptr || instance == nullptr) return;
    std::lock_guard lock(engine->mutex);
    if (!owns_instance(engine, instance)) return;
    unload_locked(engine, instance);
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_run(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance) {
    if (engine == nullptr || instance == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    std::lock_guard lock(engine->mutex);
    if (!owns_instance(engine, instance)) return SAO_STATUS_ERR_HANDLE_INVALID;
    return provider_call(engine, [&] {
        if (has_unified_field(engine->vtable,
                              offsetof(SaoScriptEngineVTable, run),
                              sizeof(engine->vtable.run)) &&
            engine->vtable.run != nullptr) {
            return engine->vtable.run(engine->impl, instance->impl,
                                      engine->vtable.user_data);
        }
        return engine->vtable.context_run(engine->impl, instance->impl);
    });
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_invoke(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance,
    const char* function_name_utf8,
    const uint8_t* arg_json_utf8,
    size_t arg_len,
    uint8_t* out_result_json_utf8,
    size_t out_capacity,
    size_t* out_required) {
    if (out_required != nullptr) *out_required = 0;
    if (engine == nullptr || instance == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (function_name_utf8 == nullptr || function_name_utf8[0] == '\0' ||
        (arg_json_utf8 == nullptr && arg_len != 0) || out_required == nullptr ||
        (out_result_json_utf8 == nullptr && out_capacity != 0)) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }

    std::lock_guard lock(engine->mutex);
    if (!owns_instance(engine, instance)) return SAO_STATUS_ERR_HANDLE_INVALID;
    const sao_status_t status = provider_call(engine, [&] {
        if (has_unified_field(engine->vtable,
                              offsetof(SaoScriptEngineVTable, invoke),
                              sizeof(engine->vtable.invoke)) &&
            engine->vtable.invoke != nullptr) {
            return engine->vtable.invoke(
                engine->impl, instance->impl, function_name_utf8,
                arg_json_utf8, arg_len, out_result_json_utf8, out_capacity,
                out_required, engine->vtable.user_data);
        }
        return engine->vtable.context_call(
            engine->impl, instance->impl, function_name_utf8, arg_json_utf8,
            arg_len, out_result_json_utf8, out_capacity, out_required);
    });
    if (status == SAO_STATUS_OK &&
        (*out_required > out_capacity ||
         (out_result_json_utf8 == nullptr && *out_required != 0))) {
        return SAO_STATUS_ERR_BUFFER_TOO_SMALL;
    }
    return status;
}

extern "C" sao_status_t SAO_SCRIPTING_CALL
sao_script_engine_has_capability(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance,
    const char* capability_utf8,
    int32_t* out_supported) {
    if (out_supported != nullptr) *out_supported = 0;
    if (engine == nullptr || instance == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (capability_utf8 == nullptr || capability_utf8[0] == '\0' ||
        out_supported == nullptr) {
        return SAO_STATUS_ERR_INVALID_ARGUMENT;
    }
    std::lock_guard lock(engine->mutex);
    if (!owns_instance(engine, instance)) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!has_capability_slot(engine->vtable)) return SAO_STATUS_OK;
    return provider_call(engine, [&] {
        return engine->vtable.context_has_capability(
            engine->impl, instance->impl, capability_utf8, out_supported);
    });
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_cancel(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance) {
    if (engine == nullptr || instance == nullptr) {
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    std::lock_guard lock(engine->mutex);
    if (!owns_instance(engine, instance)) return SAO_STATUS_ERR_HANDLE_INVALID;
    if (!has_cancel_slot(engine->vtable)) {
        return SAO_STATUS_ERR_SCRIPT_UNSUPPORTED;
    }
    return provider_call(engine, [&] {
        return engine->vtable.context_cancel(engine->impl, instance->impl);
    });
}

extern "C" sao_status_t SAO_SCRIPTING_CALL sao_script_engine_last_error(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance,
    SaoScriptError* out_error) {
    if (out_error == nullptr) return SAO_STATUS_ERR_INVALID_ARGUMENT;
    sao_scripting_error_clear(out_error);
    if (engine == nullptr || instance == nullptr) {
        out_error->status = SAO_STATUS_ERR_HANDLE_INVALID;
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    std::lock_guard lock(engine->mutex);
    if (!owns_instance(engine, instance)) {
        out_error->status = SAO_STATUS_ERR_HANDLE_INVALID;
        return SAO_STATUS_ERR_HANDLE_INVALID;
    }
    if (has_unified_field(engine->vtable,
                          offsetof(SaoScriptEngineVTable, last_error),
                          sizeof(engine->vtable.last_error)) &&
        engine->vtable.last_error != nullptr) {
        const sao_status_t status = provider_call(engine, [&] {
            return engine->vtable.last_error(
                engine->impl, instance->impl, out_error,
                engine->vtable.user_data);
        });
        if (status == SAO_STATUS_OK && out_error->status != SAO_STATUS_OK) {
            return SAO_STATUS_OK;
        }
    } else if (engine->vtable.context_last_error != nullptr) {
        const sao_status_t status = provider_call(engine, [&] {
            return engine->vtable.context_last_error(
                engine->impl, instance->impl, out_error);
        });
        if (status == SAO_STATUS_OK && out_error->status != SAO_STATUS_OK) {
            return SAO_STATUS_OK;
        }
    }
    *out_error = engine->last_error;
    return SAO_STATUS_OK;
}
