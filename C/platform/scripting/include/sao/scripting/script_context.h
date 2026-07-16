// SAO Auto — script execution context.
//
// A `SaoScriptContext` is what a script host hands to a script's entry
// function.  It's the scripting-layer analogue of `SaoSdkContext` — a
// vtable + opaque impl pointer — but scoped to the interpreter, not to
// a plugin binary.
//
// The relationship: one plugin binary can host N script contexts (one
// per script file it runs); each script context is bound to a single
// `SaoSdkContext` through its `owner_sdk_ctx` field.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/scripting/abi.h"

// Forward-declared SDK type so this header doesn't drag the SDK in.
struct SaoSdkContext;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_script_context_s* sao_script_context_handle_t;

#define SAO_SCRIPT_CAPABILITY_KEY_VALUE "context.key_value"
#define SAO_SCRIPT_CAPABILITY_CANCELLATION "context.cancellation"
#define SAO_SCRIPT_CAPABILITY_ERROR "context.error"

// Script language ids.  Fixed enum so scripting/registry.h can build
// its factory table.
enum sao_script_language_e : int32_t {
    SAO_SCRIPT_LANG_PYTHON      = 0,
    SAO_SCRIPT_LANG_EMMA        = 1,
    SAO_SCRIPT_LANG_ANGELSCRIPT = 2,
    SAO_SCRIPT_LANG_LUA         = 3,
    SAO_SCRIPT_LANG_CSHARP      = 4,
};

struct SaoScriptContextConfig {
    int32_t                language;         // sao_script_language_e
    const struct SaoSdkContext* owner_sdk_ctx;   // SDK ctx to expose to the script
    const char*            source_dir_utf8;  // resolves `require`/`import`
    const char*            entry_file_utf8;  // e.g. "main.lua"
    uint32_t               memory_cap_bytes; // 0 = language default
    uint32_t               time_slice_ms;    // co-op scheduling; 0 = uncapped
};

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_context_create(
    const struct SaoScriptContextConfig* config,
    sao_script_context_handle_t* out_handle);

SAO_SCRIPTING_API void SAO_SCRIPTING_CALL sao_script_context_destroy(
    sao_script_context_handle_t handle);

// Execute the entry file.  Blocking; time-sliced according to config.
SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_context_run(
    sao_script_context_handle_t handle);

// Call a named function inside the running script with a UTF-8 JSON
// argument.  Result JSON is written into the caller's buffer.
SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_context_call(
    sao_script_context_handle_t handle,
    const char* function_name_utf8,
    const uint8_t* arg_json_utf8,
    size_t arg_len,
    uint8_t* out_result_json_utf8,
    size_t out_capacity,
    size_t* out_bytes_written);

// Peek at diagnostic counters (time-slice budget used, allocations, …).
struct SaoScriptContextStats {
    uint64_t total_run_time_ns;
    uint64_t total_call_count;
    uint64_t total_call_time_ns;
    uint64_t peak_memory_bytes;
    uint64_t last_error_ts_ns;
};

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL sao_script_context_stats(
    sao_script_context_handle_t handle,
    struct SaoScriptContextStats* out_stats);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_set_value(
    sao_script_context_handle_t handle,
    const char* key_utf8,
    const uint8_t* value,
    size_t value_size);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_get_value(
    sao_script_context_handle_t handle,
    const char* key_utf8,
    uint8_t* out_value,
    size_t out_capacity,
    size_t* out_required);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_remove_value(
    sao_script_context_handle_t handle,
    const char* key_utf8);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_has_capability(
    sao_script_context_handle_t handle,
    const char* capability_utf8,
    int32_t* out_supported);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_request_cancel(sao_script_context_handle_t handle);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_is_cancelled(
    sao_script_context_handle_t handle,
    int32_t* out_cancelled);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_last_error(
    sao_script_context_handle_t handle,
    struct SaoScriptError* out_error);

SAO_SCRIPTING_API sao_status_t SAO_SCRIPTING_CALL
sao_script_context_release_owner(
    const struct SaoSdkContext* owner_sdk_ctx,
    size_t* out_released);

#ifdef __cplusplus
}  // extern "C"
#endif
