// SAO Auto — launcher boot-residency UX.
//
// Boot residency state machine (W6.9): after the helper promotes the R1/R3/R5
// services from demand-start to boot-start, one real restart is required for
// DriverEntry to re-run under SCM.  The helper stores a self-expiring
// restart-required latch (registry value pair under each service key) that
// survives exactly until the running boot id differs from the promotion boot
// id — a fast-startup shutdown keeps the boot id and keeps the latch armed,
// so the prompt correctly insists on "Restart" rather than "Shut down".
//
// This module owns the launcher side of that contract: environment gates
// (safe mode / RDP / fast startup / UAC / pending OS update reboots), the
// latch scan, the user prompt, and the restart executor.

#pragma once

#include <cstdint>

namespace sao::launcher {

// Static environment gates that shape the restart prompt.  All are
// informational except `safe_mode`, which suppresses the prompt entirely
// (boot-start drivers do not load in safe mode, so a restart there is
// pointless).
struct BootEnvironmentGates {
    uint32_t struct_size = 0u;
    uint32_t safe_mode = 0u;             // GetSystemMetrics(SM_CLEANBOOT) != 0
    uint32_t remote_session = 0u;        // GetSystemMetrics(SM_REMOTESESSION) != 0
    uint32_t fast_startup = 0u;          // HiberbootEnabled != 0 (hybrid shutdown)
    uint32_t uac_enabled = 0u;           // EnableLUA != 0 (absent = enabled default)
    uint32_t update_pending_reboot = 0u; // WU/CBS/PendingFileRenameOperations pending
};

// Probe the reboot-relevant environment.  Individual probe failures leave
// the corresponding gate at 0 (unknown/disabled) — the prompt degrades, it
// never blocks on probe errors.
void boot_environment_probe(BootEnvironmentGates* out_gates) noexcept;

// Scan the service registry for an unexpired restart-required latch.
// Mirrors the helper-side self-expiry: only a latch whose stored boot id
// equals the running boot id counts.  Never fails on registry errors —
// a probe failure reports 0 (no prompt) because prompting is a UX action,
// not a safety action.
void boot_residency_restart_required(uint32_t* out_required) noexcept;

// Restart executor.  Enables SE_SHUTDOWN_NAME, then schedules a reboot in
// `timeout_seconds` (no force; apps get the normal close path).  Returns
// SAO_STATUS_OK (0) when the shutdown was scheduled (abortable via
// AbortSystemShutdownW while the countdown runs).
int request_machine_restart(const wchar_t* message_utf16, uint32_t timeout_seconds) noexcept;

// Cancels a pending restart scheduled by the executor (no-op when none).
void abort_machine_restart() noexcept;

// Prompt result codes.
enum BootResidencyPromptResult : int32_t {
    BOOT_RESIDENCY_NOT_REQUIRED = 0,
    BOOT_RESIDENCY_RESTART_ACCEPTED = 1,
    BOOT_RESIDENCY_RESTART_DECLINED = 2,
    BOOT_RESIDENCY_PROMPT_PENDING = 3,
    BOOT_RESIDENCY_ERROR = -1,
};

// Presentation is asynchronous on the compositor owner thread.
int32_t boot_residency_prompt_if_required(void* owner_hwnd) noexcept;
int32_t boot_residency_take_prompt_result() noexcept;
bool boot_residency_close_prompt() noexcept;

} // namespace sao::launcher
