// SAO Auto — TOPMOST / Z-order state machine.
//
// Python authoritative source: `render/overlay_host.py::_enforce_z_order`
// See handoff §3 (all-time most-changed function in the overlay subsystem).
//
// Single authority for the overlay host's topmost state.  Every other
// caller MUST go through this rather than issuing its own SetWindowPos
// — this is the memory note "统一DWM覆盖合成器" / handoff §3 rule.
//
// ── Priority chain ─────────────────────────────────────────────
//   1. Game attached + game topmost  → kernel path (tagWND exstyle
//      bit write; avoids user-mode API race), fallback to
//      SetWindowPos(HWND_TOPMOST)
//   2. Game attached, not topmost    → SetWindowPos(host, HWND_INSERT_
//      AFTER=game_hwnd) — sit exactly one slot above game
//   3. No game attached              → HWND_NOTOPMOST (clear style bit)
//      + HWND_TOP (assert front) — TWO STEPS, both required
//
// ── The HWND_TOP vs HWND_NOTOPMOST trap ──────────────────────
//   These are DIFFERENT requests.  HWND_TOP ("move to top of z-order")
//   does NOT clear a stuck WS_EX_TOPMOST bit.  If host is already
//   stuck-topmost (fisheye dismiss animation didn't run correctly),
//   HWND_TOP alone does NOTHING.  Must:  HWND_NOTOPMOST → clears the
//   style, THEN HWND_TOP → asserts front.  Both steps.
//
// ── Do NOT introduce "real topmost + compensating proxy lift" ─
//   Handoff §3 documents an attempted real-HWND_TOPMOST-plus-lifting-
//   all-Tk-input-proxies-back-above-host scheme.  Cross-thread
//   SetWindowPos requires target-thread message pumping to complete;
//   host and proxy threads then produce a topmost race that pins CPU
//   and GPU at ~100%.  DO NOT reintroduce.  The FIX WAS: keep the
//   weak HWND_TOP request but *poll it more frequently* — 250 ms
//   during idle-no-game, matches `_topmost_interval_idle`.
//
// ── The "越权拍板" lesson (handoff §3 verbatim) ────────────────
//   Verbatim from the Python maintainer note (do not delete):
//   "修 'A 状态卡死' 类 bug 时, 别把 '改成永远不进入 A 状态' 当解法
//    — A 可能是别处逻辑的合法目标状态, 正确解法是调用 '谁有权决定
//    当前该不该是 A 状态', 而不是自己越权拍板."
//   Translation: When fixing "stuck-in-state-A" bugs, don't make the
//   fix "never enter state A" — A may be a legitimate goal elsewhere;
//   the fix is to call the state authority (this module), not to
//   override its decision.

#pragma once

#include <cstddef>
#include <cstdint>

#include "sao/core/status.h"
#include "sao/ui/abi.h"
#include "sao/ui/overlay_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sao_ui_z_order_manager_s* sao_ui_z_order_manager_handle_t;

enum sao_ui_topmost_policy_e : int32_t {
    // Default: chase game topmost when attached, HWND_TOP-idle otherwise.
    // Matches the Python default.  Do NOT change without an owning
    // architectural decision — this is the compat-safe mode.
    SAO_UI_TOPMOST_FOLLOW_GAME = 0,

    // Force real WS_EX_TOPMOST always.  Only use for tests / diagnostic
    // sessions where anti-cheat compat is not a concern.  Documented
    // to break several games at runtime.
    SAO_UI_TOPMOST_ALWAYS      = 1,

    // Never touch topmost — leave host at whatever z-order it
    // organically settles at.  Used for embedded diagnostics only.
    SAO_UI_TOPMOST_NEVER       = 2,
};

// Ex-style bit constants — mirror Win32 defines locally so callers
// don't need to include <windows.h>.
enum sao_ui_exstyle_bits_e : uint32_t {
    SAO_UI_WS_EX_TOPMOST        = 0x00000008u,
    SAO_UI_WS_EX_TRANSPARENT    = 0x00000020u,
    SAO_UI_WS_EX_TOOLWINDOW     = 0x00000080u,
    SAO_UI_WS_EX_NOACTIVATE     = 0x08000000u,
    SAO_UI_WS_EX_LAYERED        = 0x00080000u,
    SAO_UI_WS_EX_NOREDIRBM      = 0x00200000u,
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_manager_create(
    sao_ui_overlay_host_handle_t host,
    sao_ui_z_order_manager_handle_t* out_handle);

SAO_UI_API void SAO_UI_CALL sao_ui_z_order_manager_destroy(
    sao_ui_z_order_manager_handle_t handle);

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_set_policy(
    sao_ui_z_order_manager_handle_t handle, int32_t policy);

// Tune poll intervals.  idle_ms is how often the "no game attached"
// branch reasserts HWND_TOP (default 250 ms — the value tuned in
// commit df6e6b2 after the failed real-topmost experiment).  active_ms
// is the interval when a game is attached and we chase its z-order
// (default 500 ms).  Never set below 100 ms — the reason more-
// frequent WEAK requests were chosen over LESS-frequent STRONG
// requests is precisely to avoid the topmost storm §3 documents.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_set_intervals(
    sao_ui_z_order_manager_handle_t handle,
    uint32_t idle_ms,
    uint32_t active_ms);

// Re-evaluate based on current game foreground/present state.  Called
// every frame by the compositor (cheap when nothing changed).  This
// is the SOLE entry point — any other module wanting to change host
// topmost must go through here.
//
// game_hwnd is the game's HWND (0 → no game attached).  game_topmost
// and game_present are snapshots from the game-window watcher.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_enforce(
    sao_ui_z_order_manager_handle_t handle,
    void* game_hwnd,
    bool game_is_topmost,
    bool game_present);

// Kernel-side ex-style bit hide (for tagWND writeout).  Sets a mask
// of exstyle bits that we want physically absent from the tagWND
// structure even though we may need them set at the user32 API
// level for functionality.  The DC-mutation coordinator handles
// this (see `render/dc_mutation_coordinator.py`).  Fails if the
// coordinator is not registered.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_hide_exstyle_mask(
    sao_ui_z_order_manager_handle_t handle,
    uint32_t mask);

// Current effective state (for the diagnostic status line).
struct SaoZOrderStatus {
    int32_t policy;                 // sao_ui_topmost_policy_e
    bool    is_topmost;
    bool    game_topmost_snapshot;
    bool    game_present_snapshot;
    uint8_t _pad;
    void*   game_hwnd_snapshot;
    uint64_t last_enforce_ns;
    // The three-count debug: number of `enforce()` calls per branch.
    // Useful for verifying that HWND_TOP-idle poll is actually
    // running vs. being blocked by an accidental override elsewhere.
    uint64_t branch_game_topmost_count;
    uint64_t branch_game_normal_count;
    uint64_t branch_idle_count;
};

SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_status(
    sao_ui_z_order_manager_handle_t handle,
    SaoZOrderStatus* out_status);

// Guard bit: were the three known-bad TOPMOST-leak commit patterns
// (a90adbf / c208715 / 6e1eb20 in the Python history) applied?  This
// runs at startup and refuses to boot if the exstyle mask has a
// suspicious combination that would reproduce one of the leaks.
// Compile-time helper for the memory constraint mentioned in the
// task brief.
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_z_order_check_leak_patterns(
    sao_ui_z_order_manager_handle_t handle,
    uint32_t current_exstyle);

#ifdef __cplusplus
}  // extern "C"
#endif
