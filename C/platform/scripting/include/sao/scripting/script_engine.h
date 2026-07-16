// SAO Auto — IScriptEngine abstract interface (C ABI).
//
// Five concrete engines back this header (Python / Emma / AngelScript
// / Lua / C#).  Their actual hosts live under `../plugins/*_host/`
// and are the responsibility of Agent 4.  The interface exposed here
// is the game-agnostic contract they must implement.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/scripting/abi.h"
#include "sao/scripting/script_context.h"
#include "sao/scripting/script_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_script_engine_s* sao_script_engine_handle_t;
typedef struct sao_script_instance_s* sao_script_instance_handle_t;

enum sao_script_provider_flag_e : uint32_t {
    SAO_SCRIPT_PROVIDER_FLAG_NONE = 0,
    SAO_SCRIPT_PROVIDER_FLAG_ISOLATED_RUNTIME = 1u << 0,
};

// Vtable an engine implementation fills in.  Register with the registry
// (script_registry.h) at plugin/host load time.
struct SaoScriptEngineVTable {
    // Metadata.
    const char* name_utf8;              // "cpython", "lua54", …
    int32_t     language;               // sao_script_language_e
    uint32_t    engine_version;         // (major<<16)|minor
    uint32_t    abi_version;            // must == SAO_SCRIPTING_ABI_VERSION

    // Lifecycle.
    sao_status_t (SAO_SCRIPTING_CALL* engine_init)(void** out_engine_impl);
    void         (SAO_SCRIPTING_CALL* engine_shutdown)(void* engine_impl);

    // Per-context lifecycle — the host wraps its own state inside
    // ctx_impl (opaque to the platform).
    sao_status_t (SAO_SCRIPTING_CALL* context_create)(
        void* engine_impl,
        const struct SaoScriptContextConfig* config,
        void** out_ctx_impl);

    void         (SAO_SCRIPTING_CALL* context_destroy)(
        void* engine_impl, void* ctx_impl);

    sao_status_t (SAO_SCRIPTING_CALL* context_run)(
        void* engine_impl, void* ctx_impl);

    sao_status_t (SAO_SCRIPTING_CALL* context_call)(
        void* engine_impl, void* ctx_impl,
        const char* function_name_utf8,
        const uint8_t* arg_json_utf8, size_t arg_len,
        uint8_t* out_result_json_utf8, size_t out_capacity,
        size_t* out_bytes_written);

    sao_status_t (SAO_SCRIPTING_CALL* context_last_error)(
        void* engine_impl, void* ctx_impl,
        struct SaoScriptError* out_error);

    // Optional provider extension.  A zero struct_size selects the original
    // v1.0 prefix ending at context_last_error.
    uint32_t struct_size;
    uint32_t flags;
    const void* owner;
    void* user_data;

    bool (SAO_SCRIPTING_CALL* available)(void* user_data);
    void (SAO_SCRIPTING_CALL* retain)(void* user_data);
    void (SAO_SCRIPTING_CALL* release)(void* user_data);

    sao_status_t (SAO_SCRIPTING_CALL* context_has_capability)(
        void* engine_impl, void* ctx_impl,
        const char* capability_utf8, int32_t* out_supported);

    sao_status_t (SAO_SCRIPTING_CALL* context_cancel)(
        void* engine_impl, void* ctx_impl);

    // Unified provider lifecycle.  New providers should fill these slots;
    // the original callbacks above remain accepted as a compatibility
    // prefix.  Every callback receives provider-owned user_data.
    sao_status_t (SAO_SCRIPTING_CALL* create)(
        void* user_data, void** out_engine_impl);
    void (SAO_SCRIPTING_CALL* destroy)(
        void* engine_impl, void* user_data);
    sao_status_t (SAO_SCRIPTING_CALL* load)(
        void* engine_impl,
        const struct SaoScriptContextConfig* config,
        void** out_ctx_impl,
        void* user_data);
    void (SAO_SCRIPTING_CALL* unload)(
        void* engine_impl, void* ctx_impl, void* user_data);
    sao_status_t (SAO_SCRIPTING_CALL* run)(
        void* engine_impl, void* ctx_impl, void* user_data);
    sao_status_t (SAO_SCRIPTING_CALL* invoke)(
        void* engine_impl, void* ctx_impl,
        const char* function_name_utf8,
        const uint8_t* arg_json_utf8, size_t arg_len,
        uint8_t* out_result_json_utf8, size_t out_capacity,
        size_t* out_required,
        void* user_data);
    sao_status_t (SAO_SCRIPTING_CALL* last_error)(
        void* engine_impl, void* ctx_impl,
        struct SaoScriptError* out_error,
        void* user_data);
};

// Retrieve an engine's vtable — usually called via the registry.
SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_engine_open(
    int32_t language,
    sao_script_engine_handle_t* out_engine);

SAO_SCRIPTING_API void SAO_SCRIPTING_CALL sao_script_engine_close(
    sao_script_engine_handle_t engine);

SAO_SCRIPTING_API const struct SaoScriptEngineVTable* SAO_SCRIPTING_CALL
    sao_script_engine_vtable(sao_script_engine_handle_t engine);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_engine_create(
    int32_t language,
    sao_script_engine_handle_t* out_engine);

SAO_SCRIPTING_API void SAO_SCRIPTING_CALL sao_script_engine_destroy(
    sao_script_engine_handle_t engine);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_engine_load(
    sao_script_engine_handle_t engine,
    const struct SaoScriptContextConfig* config,
    sao_script_instance_handle_t* out_instance);

SAO_SCRIPTING_API void SAO_SCRIPTING_CALL sao_script_engine_unload(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_engine_run(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_engine_invoke(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance,
    const char* function_name_utf8,
    const uint8_t* arg_json_utf8,
    size_t arg_len,
    uint8_t* out_result_json_utf8,
    size_t out_capacity,
    size_t* out_required);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_engine_has_capability(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance,
    const char* capability_utf8,
    int32_t* out_supported);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_engine_cancel(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_engine_last_error(
    sao_script_engine_handle_t engine,
    sao_script_instance_handle_t instance,
    struct SaoScriptError* out_error);

#ifdef __cplusplus
}  // extern "C"
#endif
