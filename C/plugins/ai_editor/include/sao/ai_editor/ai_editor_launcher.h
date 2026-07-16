// Phase 10 — AI Editor subprocess launcher.
//
// The launcher owns one child process and one named-pipe channel per handle.
// Concurrent launches use distinct handles and pipe names.
//
// Design intent:
//   * AI Editor is a fully independent process.  When it crashes, the
//     main SAO process must remain alive.  This is why the ABI is
//     handle-based rather than "SAO_AI_EDITOR_INSTANCE".
//   * No CoreWebView2 pulled into the SAO main process — the child
//     process owns its own WebView2 instance (matches
//     `AI Editor真PTY支持落地` memory: PTY + subprocess pattern).

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/ai_editor/ai_editor_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to a launched AI Editor subprocess.  Callers own the
// handle lifetime and MUST call sao_ai_editor_destroy() to release it.
typedef struct SaoAiEditorLauncher* sao_ai_editor_launcher_t;

// Launch configuration.  `base_dir_utf8` is the AI Editor working
// directory (matches Python-side `config.BASE_DIR` — the runtime looks
// for its own subprocess resources relative to this path).  Optional
// fields may be nullptr to accept defaults.
typedef struct SaoAiEditorLaunchConfig {
    // Absolute path to the AI Editor executable.  Required.  The launcher
    // fails closed when this field is null, empty, relative, or missing.
    const char* executable_utf8;

    // Absolute working directory for the child.  Required and must exist.
    const char* base_dir_utf8;

    // Optional command-line arguments parsed with CommandLineToArgvW and
    // re-quoted argument-by-argument.  The string is never passed to a shell.
    const char* extra_args_utf8;

    // Optional named-pipe channel name for the IPC bootstrap.  When
    // nullptr the launcher will pick a per-launch random name.  When
    // provided the child is expected to open the same pipe name.
    const char* ipc_pipe_name_utf8;

    // Whether to inherit the parent stdio (log tail).  false by
    // default — AI Editor logs via its own file handle.
    bool inherit_stdio;

    // Handshake deadline.  Zero selects the default of 5000 ms.
    uint32_t handshake_timeout_ms;

    // Default request/response deadline.  Zero selects 5000 ms.
    uint32_t request_timeout_ms;
} SaoAiEditorLaunchConfig;

// Query ABI capability without launching anything.
SAO_AI_EDITOR_API bool SAO_AI_EDITOR_CALL sao_ai_editor_launcher_available(void);

// Create a launcher handle bound to the supplied configuration.  The
// handle does NOT spawn the subprocess yet — call sao_ai_editor_launch()
// on the returned handle.  This split lets tests configure the handle
// and inspect defaults without a real Popen.
//
// `out_handle` must be non-null.  On success returns SAO_AI_EDITOR_OK
// and writes a non-null handle.  On failure returns a negative status
// and `*out_handle == nullptr`.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_create(
    const SaoAiEditorLaunchConfig* config,
    sao_ai_editor_launcher_t*      out_handle);

// Launch the subprocess with CreateProcessW, accept its named-pipe connection,
// and complete the protocol handshake before returning success.
//
// `out_exit_code` is populated only when the child terminates before
// we return control (matches Python's `Popen(...).wait(0)` semantics
// for a fast-fail launch).  For a normal async launch the value is
// left untouched and callers use sao_ai_editor_wait().
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_launch(
    sao_ai_editor_launcher_t handle,
    int32_t*                 out_exit_code);

// Non-blocking status query.  Sets `*out_is_running` to true when the
// child process is still alive.  When the child has already exited,
// `*out_exit_code` is populated with the observed exit code.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_status(
    sao_ai_editor_launcher_t handle,
    bool*                    out_is_running,
    int32_t*                 out_exit_code);

// Send one request and wait for one response.  The caller owns the response
// buffer.  On a short buffer, writes the required byte count to *out_len and
// returns SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL without consuming the response.
// timeout_ms == 0 uses the configured request timeout.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_request(
    sao_ai_editor_launcher_t handle,
    const void*              request,
    uint32_t                 request_len,
    void*                    response,
    uint32_t                 response_cap,
    uint32_t*                out_len,
    uint32_t                 timeout_ms);

// Graceful shutdown sends the IPC "shutdown" request and waits for process
// exit.  On timeout the child is terminated before resources are released.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_shutdown(
    sao_ai_editor_launcher_t handle,
    uint32_t                 timeout_ms,
    int32_t*                 out_exit_code);

// Release the handle.  Safe to call on a handle whose child has
// already exited or was never launched.
SAO_AI_EDITOR_API void SAO_AI_EDITOR_CALL sao_ai_editor_destroy(
    sao_ai_editor_launcher_t handle);

// Read back the resolved configuration.  Useful for tests to prove
// defaults are applied correctly.  Any output buffer that would
// overflow returns SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL.
SAO_AI_EDITOR_API int32_t SAO_AI_EDITOR_CALL sao_ai_editor_get_config(
    sao_ai_editor_launcher_t handle,
    char*                    executable_out,
    size_t                   executable_cap,
    char*                    base_dir_out,
    size_t                   base_dir_cap,
    char*                    ipc_pipe_name_out,
    size_t                   ipc_pipe_cap);

#ifdef __cplusplus
}  // extern "C"
#endif
