// Phase 10 — AI Editor IPC channel.
//
// First-slice selection: **Windows named pipe** (\\.\pipe\sao_ai_editor_<id>).
// Rationale:
//   * stdio inherits complicate the child's own PTY handling (AI Editor
//     memory `AI Editor真PTY支持落地` proved pywinpty is subtle).  A
//     dedicated named pipe keeps the parent<->child channel free of
//     the child's own terminal traffic.
//   * WebSocket would require an HTTP server inside the parent, which
//     collides with the freetier public API listener.
//   * Named pipes are ACL'able (allow only the parent's owner SID) and
//     survive across CRT boundaries.
//
// The parent binds the server end; the child connects as client with
// the pipe name passed via SaoAiEditorLaunchConfig.ipc_pipe_name_utf8.
// Message framing = length-prefixed UTF-8 JSON (uint32_t LE length +
// payload), matching the Python-side helper protocol convention.
//
// The implementation binds a real Windows named-pipe server and performs
// length-prefixed request/response IO.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque IPC channel handle (server end owned by parent).
typedef struct SaoAiEditorIpc* sao_ai_editor_ipc_t;

// Enumerate the supported IPC transports.  The launcher currently
// only accepts NAMED_PIPE; future transports may include UDS or WebSocket.
enum SaoAiEditorIpcTransport : int32_t {
    SAO_AI_EDITOR_IPC_NAMED_PIPE = 1,
    SAO_AI_EDITOR_IPC_STDIO      = 2,  // reserved
    SAO_AI_EDITOR_IPC_WEBSOCKET  = 3,  // reserved
};

// Create the parent-side server for the given pipe name.  When
// `pipe_name_utf8` is nullptr the implementation generates a random
// name (must be readable via sao_ai_editor_ipc_get_pipe_name()).
//
// The pipe is created immediately and fails closed on invalid names or OS
// errors.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_create(
    SaoAiEditorIpcTransport transport,
    const char*             pipe_name_utf8,
    sao_ai_editor_ipc_t*    out_handle);

// Query the actual pipe name in use (either the caller-supplied value
// or the launcher-generated random name).  Returns
// SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL when the buffer is short.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_get_pipe_name(
    sao_ai_editor_ipc_t handle,
    char*               name_out,
    size_t              name_cap);

// Blocking accept — server waits for the child to connect.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_accept(
    sao_ai_editor_ipc_t handle,
    uint32_t            timeout_ms);

// Non-blocking connect state query.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_is_connected(
    sao_ai_editor_ipc_t handle,
    bool*               out_connected);

// Send a length-prefixed payload.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_send(
    sao_ai_editor_ipc_t handle,
    const void*         payload,
    uint32_t            payload_len);

// Non-blocking receive.  When no message is queued, returns
// SAO_AI_EDITOR_OK and writes 0 into *out_len; when the buffer is too
// small returns SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL and *out_len
// contains the required length.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_recv(
    sao_ai_editor_ipc_t handle,
    void*               buffer,
    uint32_t            buffer_cap,
    uint32_t*           out_len);

// Close and release the channel handle.  Safe on a nullptr handle.
SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_ipc_destroy(
    sao_ai_editor_ipc_t handle);

// Retained for ABI compatibility.  Real IPC builds return
// SAO_AI_EDITOR_ERR_NOT_IMPLEMENTED.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_ipc_test_connect_mock(
    sao_ai_editor_ipc_t handle);

#ifdef __cplusplus
}  // extern "C"
#endif
