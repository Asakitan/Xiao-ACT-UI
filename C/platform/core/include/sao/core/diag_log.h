// SAO — platform/core/diag_log.h
//
// Shared diagnostic append channel for pp_diag.log across launcher, helper
// (LocalSystem), and the license client. Behaviour:
//   - Debug builds (SAO_RT_IO_ACTUAL_DEBUG=1): plaintext append; honours the
//     SAO_RT_IO_PP_DEBUG environment path override.
//   - Non-debug (RelWithDebInfo / Release / hardened): every line is wrapped
//     in an ECIES envelope (ephemeral P-256 ECDH -> HKDF-lite SHA256 ->
//     AES-256-GCM). Only the holder of the private key can decrypt; the
//     shipped binary carries just the public key.
//   - sao_diag_begin_session() truncates the log so each launch refreshes
//     the diagnostic file instead of letting it grow forever.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Truncate/create the diagnostic log for a fresh launch session and emit a
// begin-session sentinel line. Called once per launcher-side driver-chain
// attempt (helper/service/waiter processes must not call this).
void sao_diag_begin_session(void);

// Append one diagnostic line (LF is added by the writer if absent). In
// non-debug builds the line is encrypted before it reaches disk. `line` need
// not be NUL-terminated when `length` is provided.
void sao_diag_write(const char* line, size_t length);

#ifdef __cplusplus
}
#endif
