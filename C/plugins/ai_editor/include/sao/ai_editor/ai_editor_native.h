#pragma once

#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#define SAO_AI_EDITOR_NATIVE_ABI_VERSION_MAJOR 1u
#define SAO_AI_EDITOR_NATIVE_ABI_VERSION_MINOR 0u
#define SAO_AI_EDITOR_NATIVE_ABI_VERSION \
    ((SAO_AI_EDITOR_NATIVE_ABI_VERSION_MAJOR << 16) | \
     SAO_AI_EDITOR_NATIVE_ABI_VERSION_MINOR)
#define SAO_AI_EDITOR_PROTOCOL_VERSION 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaoAiEditorRuntime* sao_ai_editor_runtime_t;

typedef struct SaoAiEditorRuntimeConfig {
    uint32_t struct_size;
    const char* workspace_root_utf8;
    const char* system_root_utf8;
    const char* plugin_roots_json_utf8;
    uint32_t max_file_bytes;
    uint32_t max_search_results;
} SaoAiEditorRuntimeConfig;

SAO_AI_EDITOR_API uint32_t SAO_AI_EDITOR_CALL
sao_ai_editor_native_abi_version(void);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_create(
    const SaoAiEditorRuntimeConfig* config,
    sao_ai_editor_runtime_t* out_handle);

// Dispatch one versioned JSON-RPC 2.0 request. Requests carry
// {"sao":{"protocolVersion":1}}. The returned UTF-8 JSON is NUL-terminated;
// *out_len excludes the terminator. A short buffer retains the response, and
// a retry with request=nullptr/request_len=0 retrieves it without re-execution.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_dispatch(
    sao_ai_editor_runtime_t handle,
    const void* request,
    uint32_t request_len,
    char* response_out,
    uint32_t response_cap,
    uint32_t* out_len);

// Read the next sao.event JSON-RPC notification. No queued event returns OK
// with *out_len=0. A short buffer leaves the event queued.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_next_event(
    sao_ai_editor_runtime_t handle,
    char* event_out,
    uint32_t event_cap,
    uint32_t* out_len);

// Direct cancellation outlet for callers whose JSON-RPC dispatch thread is
// blocked in WinHTTP. The equivalent JSON-RPC method is run.cancel.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_runtime_cancel(
    sao_ai_editor_runtime_t handle,
    const char* run_id_utf8);

SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_runtime_destroy(
    sao_ai_editor_runtime_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
