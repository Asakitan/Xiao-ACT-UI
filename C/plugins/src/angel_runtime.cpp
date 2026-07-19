#include "sao_plugins/angel_runtime.h"

#include "logging.h"

#include <angelscript.h>

#include <new>
#include <string>

#include "sao_plugins/sao_status.h"

struct sao_plugins_angel_s {
    asIScriptEngine* engine = nullptr;
};

namespace {

constexpr char kComponent[] = "plugins.angel";

int32_t finish(int32_t status, const char* message) noexcept {
    sao::legacy_plugins::emit_log(status == SAO_OK ? sao::legacy_plugins::kLogLevelInfo
                                                   : sao::legacy_plugins::kLogLevelError,
                                  kComponent, status, message);
    return status;
}

int32_t fail(int32_t status, const char* message) noexcept {
    sao::legacy_plugins::emit_log(sao::legacy_plugins::kLogLevelError, kComponent, status, message);
    return status;
}

} // namespace

extern "C" int32_t SAO_PLUGINS_CALL
sao_plugins_angel_create(sao_plugins_angel_handle_t* out_handle) {
    if (out_handle == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "angel_create requires an output handle");
    }
    *out_handle = nullptr;
    asIScriptEngine* engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    if (engine == nullptr) {
        return fail(SAO_ERR_OS_CALL_FAILED, "angel_create failed to allocate an engine");
    }
    auto* handle = new (std::nothrow) sao_plugins_angel_s{engine};
    if (handle == nullptr) {
        engine->ShutDownAndRelease();
        return fail(SAO_ERR_OS_CALL_FAILED, "angel_create failed to allocate its facade handle");
    }
    *out_handle = handle;
    return finish(SAO_OK, "angel_create completed");
}

extern "C" void SAO_PLUGINS_CALL sao_plugins_angel_destroy(sao_plugins_angel_handle_t handle) {
    if (handle == nullptr)
        return;
    try {
        if (handle->engine != nullptr)
            handle->engine->ShutDownAndRelease();
        delete handle;
        (void)finish(SAO_OK, "angel_destroy completed");
    } catch (...) {
        (void)fail(SAO_ERR_OS_CALL_FAILED, "angel_destroy failed unexpectedly");
    }
}

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_angel_run_script(sao_plugins_angel_handle_t handle,
                                                                 const char* script_utf8,
                                                                 size_t script_len) {
    if (handle == nullptr || handle->engine == nullptr || script_utf8 == nullptr) {
        return fail(SAO_ERR_INVALID_ARGUMENT, "angel_run_script received invalid arguments");
    }
    try {
        asIScriptModule* module = handle->engine->GetModule("sao_plugin", asGM_ALWAYS_CREATE);
        if (module == nullptr) {
            return fail(SAO_ERR_OS_CALL_FAILED, "angel_run_script failed to create a module");
        }
        const std::string source(script_utf8, script_len);
        if (module->AddScriptSection("sao_plugin", source.c_str(), source.size()) < 0 ||
            module->Build() < 0) {
            return fail(SAO_ERR_INVALID_ARGUMENT, "angel_run_script failed to compile the script");
        }
        asIScriptFunction* entry = module->GetFunctionByName("__entry__");
        if (entry == nullptr)
            entry = module->GetFunctionByName("main");
        if (entry == nullptr)
            return SAO_OK;
        asIScriptContext* context = handle->engine->CreateContext();
        if (context == nullptr) {
            return fail(SAO_ERR_OS_CALL_FAILED, "angel_run_script failed to create a context");
        }
        const int prepare_status = context->Prepare(entry);
        const int execute_status = prepare_status >= 0 ? context->Execute() : prepare_status;
        context->Release();
        return execute_status == asEXECUTION_FINISHED
                   ? SAO_OK
                   : fail(SAO_ERR_OS_CALL_FAILED, "angel_run_script execution failed");
    } catch (...) {
        return fail(SAO_ERR_OS_CALL_FAILED, "angel_run_script failed unexpectedly");
    }
}
