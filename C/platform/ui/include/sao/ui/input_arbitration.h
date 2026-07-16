// SAO Auto — game-agnostic input arbitration policy table.
//
// A per-arbiter table of (VK → policy).  Plugins register a source
// (their plugin_id) and consult the arbiter before emitting a
// synthetic key.  The arbiter tells them whether they're allowed to
// touch that key right now.
//
// ── Sources ─────────────────────────────────────────────────────
//   `user`          — the physical user's live input.  Always allowed;
//                     never blocked.
//   `plugin_<id>`   — an automation plugin.  Subject to policy.
//
// ── Policies (mirror `sao_ui_auto_key_policy_e`) ───────────────
//   SHARED       — every source may fire.  Default when no explicit
//                  entry exists for a VK.
//   GAME_ONLY    — only `user` may fire; plugins blocked.
//   PLUGIN_ONLY  — only automation may fire.  Rare — used by macro
//                  editors that need to suppress accidental physical
//                  presses.
//   BLOCKED      — nobody may fire.  Used for temporary lockout
//                  windows (e.g. dodge safety in bossraid).
//
// ── Reservation ────────────────────────────────────────────────
//   `reserve(vk, plugin_id)` grabs an exclusive slot for one plugin on
//   a specific VK.  While the reservation stands, any other plugin
//   sees `allowed=false`.  Reservations survive policy changes but are
//   released on `reserve(vk, "")` or when the arbiter is destroyed.
//
// ── Conflict detection ─────────────────────────────────────────
//   `get_conflicts` enumerates every VK that has more than one plugin
//   asking to fire it (via the observed-source ledger).  The settings
//   UI uses this to surface "plugin A and plugin B both want F5" so
//   the user can resolve.  Detection uses observation — the arbiter
//   records unique plugin IDs seen per VK during `check` — so the
//   ledger only reports real usage, never hypothetical clashes.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/auto_key.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_input_arb_s* sao_ui_input_arb_handle_t;

// A pre-baked (VK, policy) entry for `create`.  Pass NULL / count=0 to
// start with every VK == SHARED.
struct SaoUiInputArbConfigEntry {
    uint32_t virtual_key;
    int32_t  policy;                    // sao_ui_auto_key_policy_e
};

struct SaoUiInputArbConfig {
    const SaoUiInputArbConfigEntry* entries;
    size_t                          count;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_create(
    const SaoUiInputArbConfig*  config,
    sao_ui_input_arb_handle_t*  out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_input_arb_destroy(
    sao_ui_input_arb_handle_t handle);

// ── Query ───────────────────────────────────────────────────────

// Check whether `source_utf8` may fire `virtual_key` right now.
// `source_utf8` == "user" is always allowed regardless of policy;
// anything else is treated as a plugin identity.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_check(
    sao_ui_input_arb_handle_t handle,
    uint32_t                  virtual_key,
    const char*               source_utf8,
    bool*                     allowed_out);

// ── Mutation ────────────────────────────────────────────────────

// Set / overwrite the policy on a VK.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_register_policy(
    sao_ui_input_arb_handle_t handle,
    uint32_t                  virtual_key,
    int32_t                   policy);

// Grant an exclusive slot on `virtual_key` to `plugin_id_utf8`.  Pass
// `plugin_id_utf8 == nullptr` OR "" to release the reservation.
// Reservations trump policy — even a SHARED VK with a reservation
// only accepts calls from the reserved plugin.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_reserve(
    sao_ui_input_arb_handle_t handle,
    uint32_t                  virtual_key,
    const char*               plugin_id_utf8);

// ── Diagnostics ─────────────────────────────────────────────────

struct SaoUiInputArbConflict {
    uint32_t virtual_key;
    // Number of distinct plugin sources observed asking for this VK.
    uint32_t observed_source_count;
    // Current policy value.
    int32_t  policy;
    // Non-zero when `virtual_key` is currently reserved.
    uint32_t reserved;
};

// Enumerate VKs with `observed_source_count >= 2` (multi-source
// contention).  When `capacity == 0` writes the count into `count_out`
// (query mode).
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_input_arb_get_conflicts(
    sao_ui_input_arb_handle_t handle,
    SaoUiInputArbConflict*    conflicts_out,
    size_t                    capacity,
    size_t*                   count_out);

#ifdef __cplusplus
}  // extern "C"
#endif
