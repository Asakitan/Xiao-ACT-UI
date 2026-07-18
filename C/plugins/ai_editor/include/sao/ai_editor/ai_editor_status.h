// Phase 10 — AI Editor subprocess plugin.
//
// Status codes shared across launcher / IPC / lifecycle entrypoints.
// Follows the abi-contract.md convention: 0 = ok, negative = error.
// The launcher starts a configured subprocess and communicates through a
// length-prefixed named-pipe channel.
//
// AI Editor is a SUBPROCESS by design (`AiEditor.exe`, mirrors the
// Python-side `subprocess.Popen`).  It does NOT share memory with the
// SAO main process, so C++ wraps it with a launcher + IPC channel.

#pragma once

#include <cstdint>

#if defined(_WIN32)
  #if defined(SAO_AI_EDITOR_BUILDING_DLL)
    #define SAO_AI_EDITOR_API __declspec(dllexport)
  #else
    #define SAO_AI_EDITOR_API __declspec(dllimport)
  #endif
#else
  #define SAO_AI_EDITOR_API
#endif

#define SAO_AI_EDITOR_CALL __cdecl

// Vendored per abi-contract.md "Module coupling": ai_editor is its
// own DLL, so it carries its own status enum rather than reach into
// sao_core across a target boundary.
enum SaoAiEditorStatus : int32_t {
    SAO_AI_EDITOR_OK                    = 0,
    SAO_AI_EDITOR_ERR_INVALID_ARGUMENT  = -1,
    SAO_AI_EDITOR_ERR_NOT_INITIALIZED   = -2,
    SAO_AI_EDITOR_ERR_HANDLE_INVALID    = -3,
    SAO_AI_EDITOR_ERR_BUFFER_TOO_SMALL  = -4,
    SAO_AI_EDITOR_ERR_OS_CALL_FAILED    = -5,
    SAO_AI_EDITOR_ERR_NOT_IMPLEMENTED   = -6,
    // Domain-specific:
    SAO_AI_EDITOR_ERR_LAUNCH_FAILED     = -100,
    SAO_AI_EDITOR_ERR_ALREADY_RUNNING   = -101,
    SAO_AI_EDITOR_ERR_NOT_RUNNING       = -102,
    SAO_AI_EDITOR_ERR_IPC_CONNECT_FAIL  = -110,
    SAO_AI_EDITOR_ERR_IPC_TIMEOUT       = -111,
    SAO_AI_EDITOR_ERR_IPC_CLOSED        = -112,
    SAO_AI_EDITOR_ERR_CONFIG_MISSING    = -120,
    // Native backend errors. Existing launcher/IPC values remain unchanged.
    SAO_AI_EDITOR_ERR_TIMEOUT               = -200,
    SAO_AI_EDITOR_ERR_NOT_FOUND             = -201,
    SAO_AI_EDITOR_ERR_PERMISSION_DENIED     = -202,
    SAO_AI_EDITOR_ERR_CONFIRMATION_REQUIRED = -203,
    SAO_AI_EDITOR_ERR_BOUNDARY_VIOLATION    = -204,
    SAO_AI_EDITOR_ERR_CANCELLED             = -205,
    SAO_AI_EDITOR_ERR_HTTP                  = -206,
    SAO_AI_EDITOR_ERR_PROTOCOL              = -207,
    SAO_AI_EDITOR_ERR_BUSY                  = -208,
};

// ABI version. Bump minor when adding new entrypoints; bump major
// when the launcher/ipc structs change layout.
#define SAO_AI_EDITOR_ABI_VERSION_MAJOR 1u
#define SAO_AI_EDITOR_ABI_VERSION_MINOR 1u
#define SAO_AI_EDITOR_ABI_VERSION \
    ((SAO_AI_EDITOR_ABI_VERSION_MAJOR << 16) | SAO_AI_EDITOR_ABI_VERSION_MINOR)

#ifdef __cplusplus
extern "C" {
#endif

SAO_AI_EDITOR_API uint32_t SAO_AI_EDITOR_CALL sao_ai_editor_abi_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif
