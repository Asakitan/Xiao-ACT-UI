#pragma once

#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaoAiEditorMcpDecoder* sao_ai_editor_mcp_decoder_t;

// Encode one JSON-RPC object as MCP stdio Content-Length framing.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_encode(
    const void* json_message,
    uint32_t json_length,
    void* frame_out,
    uint32_t frame_cap,
    uint32_t* out_len);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_decoder_create(
    uint32_t maximum_message_bytes,
    sao_ai_editor_mcp_decoder_t* out_handle);

// Incrementally decode Content-Length or newline-delimited MCP JSON-RPC.
// Output is a NUL-terminated JSON array; *out_len excludes the terminator.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_mcp_decoder_feed(
    sao_ai_editor_mcp_decoder_t handle,
    const void* bytes,
    uint32_t byte_count,
    char* messages_out,
    uint32_t messages_cap,
    uint32_t* out_len);

SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_mcp_decoder_destroy(
    sao_ai_editor_mcp_decoder_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
