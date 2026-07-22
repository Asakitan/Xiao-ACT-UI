// SAO Auto — platform-side SDK dispatch table.
//
// This is the *platform* side of the plugin ABI.  It exports the single
// symbol plugins care about at load time (`sao_sdk_abi_version`) and
// hosts the vtable factory that produces a per-plugin SaoSdkContext.
//
// Skeleton phase: the factory is a stub — the real implementation lands
// when plugin loading is wired up.  Each vtable function
// pointer forwards to the corresponding platform module (sao::engine
// for event/UI, sao::core for mem/config, sao::ui for UI, etc.).

#define SAO_SDK_BUILDING_DLL 1

#include "sdk_internal.h"

#include <cstring>
#include <new>

extern "C" uint32_t SAO_SDK_CALL sao_sdk_abi_version(void) {
    return SAO_SDK_ABI_VERSION;
}

extern "C" SAO_SDK_API sao_sdk_status_t SAO_SDK_CALL sao_sdk_bind_context(
    const char* plugin_id_utf8, const char* plugin_version_utf8, struct SaoSdkContext* out_ctx) {
    if (out_ctx == nullptr)
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    std::memset(out_ctx, 0, sizeof(*out_ctx));
    if (plugin_id_utf8 == nullptr || plugin_id_utf8[0] == '\0') {
        return SAO_SDK_ERR_INVALID_ARGUMENT;
    }

    sao_sdk_internal::ContextState* state = nullptr;
    bool registered = false;
    bool runtime_acquired = false;
    try {
        const sao_sdk_status_t runtime_status =
            sao_sdk_internal::SharedRuntime::instance().acquire_context();
        if (runtime_status != SAO_SDK_OK)
            return runtime_status;
        runtime_acquired = true;
        state = new (std::nothrow) sao_sdk_internal::ContextState();
        if (state == nullptr) {
            sao_sdk_internal::SharedRuntime::instance().release_context();
            return SAO_SDK_ERR_NOT_INITIALIZED;
        }
        state->runtime_context_acquired = true;
        runtime_acquired = false;
        state->plugin_id = plugin_id_utf8;
        sao_sdk_internal::populate_context(state, out_ctx, plugin_version_utf8);
        sao_sdk_internal::register_context(state);
        registered = true;
        const auto provider_status = sao_sdk_internal::bind_process_providers(state);
        if (provider_status != SAO_SDK_OK) {
            if (sao_sdk_context_try_destroy(out_ctx) != SAO_SDK_OK)
                sao_sdk_internal::quarantine_context(state);
            return provider_status;
        }
        return SAO_SDK_OK;
    } catch (...) {
        if (state != nullptr) {
            if (registered) {
                if (sao_sdk_context_try_destroy(out_ctx) != SAO_SDK_OK)
                    sao_sdk_internal::quarantine_context(state);
            } else {
                if (state->runtime_context_acquired) {
                    sao_sdk_internal::SharedRuntime::instance().release_context();
                    state->runtime_context_acquired = false;
                }
                delete state;
                std::memset(out_ctx, 0, sizeof(*out_ctx));
            }
        } else if (runtime_acquired) {
            sao_sdk_internal::SharedRuntime::instance().release_context();
        }
        return SAO_SDK_ERR_INTERNAL;
    }
}
