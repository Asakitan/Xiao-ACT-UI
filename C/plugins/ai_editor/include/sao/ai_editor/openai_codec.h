#pragma once

#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaoAiEditorOpenAiSseDecoder*
    sao_ai_editor_openai_sse_decoder_t;

// Normalize one OpenAI-compatible non-streaming chat-completions response.
// Output is NUL-terminated JSON; *out_len excludes the terminator.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_openai_decode_response(
    const void* response_json,
    uint32_t response_len,
    char* normalized_out,
    uint32_t normalized_cap,
    uint32_t* out_len);

SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_openai_sse_decoder_create(
    sao_ai_editor_openai_sse_decoder_t* out_handle);

// Incrementally decode SSE bytes. Output is a JSON array of normalized
// delta/done/error records. Empty input is valid and flushes no partial event.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL
sao_ai_editor_openai_sse_decoder_feed(
    sao_ai_editor_openai_sse_decoder_t handle,
    const void* bytes,
    uint32_t byte_count,
    char* events_out,
    uint32_t events_cap,
    uint32_t* out_len);

SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL
sao_ai_editor_openai_sse_decoder_destroy(
    sao_ai_editor_openai_sse_decoder_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
