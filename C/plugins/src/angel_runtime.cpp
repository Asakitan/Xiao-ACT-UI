#include "sao_plugins/angel_runtime.h"

#include <angelscript.h>

#include <new>
#include <string>

#include "sao_plugins/sao_status.h"

struct sao_plugins_angel_s {
    asIScriptEngine* engine = nullptr;
};

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_angel_create(
    sao_plugins_angel_handle_t* out_handle) {
    if (out_handle == nullptr) return SAO_ERR_INVALID_ARGUMENT;
    *out_handle = nullptr;
    asIScriptEngine* engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    if (engine == nullptr) return SAO_ERR_OS_CALL_FAILED;
    auto* handle = new (std::nothrow) sao_plugins_angel_s{engine};
    if (handle == nullptr) {
        engine->ShutDownAndRelease();
        return SAO_ERR_OS_CALL_FAILED;
    }
    *out_handle = handle;
    return SAO_OK;
}

extern "C" void SAO_PLUGINS_CALL sao_plugins_angel_destroy(sao_plugins_angel_handle_t handle) {
    if (handle == nullptr) return;
    if (handle->engine != nullptr) handle->engine->ShutDownAndRelease();
    delete handle;
}

extern "C" int32_t SAO_PLUGINS_CALL sao_plugins_angel_run_script(
    sao_plugins_angel_handle_t handle,
    const char* script_utf8,
    size_t script_len) {
    if (handle == nullptr || handle->engine == nullptr || script_utf8 == nullptr) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    asIScriptModule* module = handle->engine->GetModule(
        "sao_plugin", asGM_ALWAYS_CREATE);
    if (module == nullptr) return SAO_ERR_OS_CALL_FAILED;
    const std::string source(script_utf8, script_len);
    if (module->AddScriptSection("sao_plugin", source.c_str(),
                                 source.size()) < 0 ||
        module->Build() < 0) {
        return SAO_ERR_INVALID_ARGUMENT;
    }
    asIScriptFunction* entry = module->GetFunctionByName("__entry__");
    if (entry == nullptr) entry = module->GetFunctionByName("main");
    if (entry == nullptr) return SAO_OK;
    asIScriptContext* context = handle->engine->CreateContext();
    if (context == nullptr) return SAO_ERR_OS_CALL_FAILED;
    const int prepare_status = context->Prepare(entry);
    const int execute_status = prepare_status >= 0 ? context->Execute()
                                                    : prepare_status;
    context->Release();
    return execute_status == asEXECUTION_FINISHED ? SAO_OK
                                                   : SAO_ERR_OS_CALL_FAILED;
}
