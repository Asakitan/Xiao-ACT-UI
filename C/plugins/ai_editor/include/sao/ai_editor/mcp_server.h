#pragma once

#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#ifdef __cplusplus
extern "C" {
#endif

SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http(uint16_t port);

SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http_with_token(
    uint16_t port, const char* token_utf8);

SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_run_mcp_server_streamable_http_ex(
    uint16_t port, const char* workspace_utf8, const char* token_utf8);

SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL
sao_ai_editor_stop_mcp_server_streamable_http(void);

SAO_AI_EDITOR_API uint16_t SAO_AI_EDITOR_CALL
sao_ai_editor_mcp_http_port(void);

SAO_AI_EDITOR_API int SAO_AI_EDITOR_CALL sao_ai_editor_mcp_http_token(
    char* token_out, uint32_t token_capacity, uint32_t* token_size);

#ifdef __cplusplus
}
#endif
