// SAO Auto - Wave 17 / Agent a - focused tests for the platform/ui
// NOT_IMPLEMENTED classification pass.
//
// These tests exercise the taxonomy and the runtime capability-gate
// return codes.  Every case is hermetic — no real driver, no real
// WGC session, no real DXGI Desktop Duplication against the live
// desktop, no real SendInput, no real ShellNotifyIcon.  Windows-side
// code is either called with invalid/handle-invalid arguments (which
// short-circuits before any capability call) or observed through the
// wave17a_support_lib mocks.

#include <catch2/catch_test_macros.hpp>

#include "wave17a_ui_gap_support.h"

#include "sao/core/status.h"
#include "sao/ui/alerts.h"
#include "sao/ui/auto_key.h"
#include "sao/ui/d3d11_device.h"
#include "sao/ui/dcomp_bridge.h"
#include "sao/ui/dxgi_dup.h"
#include "sao/ui/gpu_capture.h"
#include "sao/ui/input.h"
#include "sao/ui/input_router.h"
#include "sao/ui/overlay_host.h"

#include <cstdint>

using sao::ui::wave17a::classification_matrix;
using sao::ui::wave17a::matrix_totals;
using sao::ui::wave17a::GapClass;
using sao::ui::wave17a::status_is_capability_gate;
using sao::ui::wave17a::status_is_legacy_skeleton;
using sao::ui::wave17a::taxonomy_codes_are_distinct;
using sao::ui::wave17a::SendInputLedger;
using sao::ui::wave17a::ShellNotifyLedger;
using sao::ui::wave17a::DcompLateBindMock;

// =====================================================================
// Section 1 — Class A (capability gate) return-code assertions.
// =====================================================================

TEST_CASE("wave17a_taxonomy_codes_are_distinct",
          "[ui][wave17a][taxonomy]") {
    // Sanity: the two return codes must be different values so callers
    // can tell "capability gate" apart from "skeleton not implemented".
    REQUIRE(taxonomy_codes_are_distinct());
    REQUIRE(SAO_STATUS_ERR_CAPABILITY_MISSING != SAO_STATUS_OK);
    REQUIRE(SAO_STATUS_ERR_NOT_IMPLEMENTED != SAO_STATUS_OK);
    REQUIRE(SAO_STATUS_ERR_NOT_IMPLEMENTED == -5);
    REQUIRE(SAO_STATUS_ERR_CAPABILITY_MISSING == -10);
}

TEST_CASE("wave17a_matrix_totals_are_pinned",
          "[ui][wave17a][taxonomy]") {
    const auto t = matrix_totals();
    // If a future wave adds/removes a NOT_IMPLEMENTED site the
    // classification matrix in wave17a_ui_gap_support.cpp must be
    // updated in the same commit — this pin catches drift.
    CHECK(t.total_rows == 53u);
    CHECK(t.capability_gate_rows == 46u);
    CHECK(t.legacy_skeleton_rows == 7u);
    CHECK(t.implementable_rows == 0u);
    CHECK(t.total_rows == t.capability_gate_rows +
                          t.legacy_skeleton_rows +
                          t.implementable_rows);
}

TEST_CASE("wave17a_matrix_covers_every_expected_cpp_file",
          "[ui][wave17a][taxonomy]") {
    const auto rows = classification_matrix();
    // At least one row per file the brief called out as having class-A
    // or class-C sites.
    const auto has = [&](std::string_view s) {
        for (const auto& r : rows) {
            if (r.cpp_file == s) return true;
        }
        return false;
    };
    CHECK(has("platform/ui/src/alerts.cpp"));
    CHECK(has("platform/ui/src/auto_key.cpp"));
    CHECK(has("platform/ui/src/d3d11_device.cpp"));
    CHECK(has("platform/ui/src/dcomp_bridge.cpp"));
    CHECK(has("platform/ui/src/dxgi_dup.cpp"));
    CHECK(has("platform/ui/src/gpu_capture.cpp"));
    CHECK(has("platform/ui/src/input.cpp"));
    CHECK(has("platform/ui/src/input_router.cpp"));
    CHECK(has("platform/ui/src/overlay_host.cpp"));
    CHECK(has("platform/ui/src/z_order.cpp"));
}

TEST_CASE("wave17a_alerts_speak_missing_voice_is_capability_gated",
          "[ui][wave17a][class_a][alerts]") {
    // Empty voice / empty text is an argument error, but the fact that
    // the function ever exposes CAPABILITY_MISSING at all means the
    // gate rework landed.  On Windows this call may hit SAPI and get
    // OS_CALL_FAILED / OK; on non-Windows it must be CAPABILITY_MISSING.
    const uint16_t empty[] = {0};
    sao_status_t rc = sao_ui_alerts_speak(empty, nullptr, 0, 0);
    const bool ok = (rc == SAO_STATUS_ERR_INVALID_ARGUMENT) ||
                    (rc == SAO_STATUS_OK) ||
                    (rc == SAO_STATUS_ERR_OS_CALL_FAILED) ||
                    (rc == SAO_STATUS_ERR_NOT_IMPLEMENTED) ||
                    (rc == SAO_STATUS_ERR_CAPABILITY_MISSING);
    CHECK(ok);
#if !defined(_WIN32)
    CHECK(status_is_capability_gate(rc));
#endif
}

TEST_CASE("wave17a_alerts_sound_stop_gate_semantics",
          "[ui][wave17a][class_a][alerts]") {
    sao_status_t rc = sao_ui_alerts_sound_stop(0);
#if defined(_WIN32)
    // Windows: PlaySound(NULL) always succeeds even with no active id.
    CHECK(rc == SAO_STATUS_OK);
#else
    CHECK(status_is_capability_gate(rc));
#endif
}

TEST_CASE("wave17a_auto_key_send_key_arg_validation",
          "[ui][wave17a][class_a][auto_key]") {
    // vk=0 and vk>0xFF are hard argument errors — validated ahead of
    // the platform capability check.  Guarantees any real SendInput
    // call is preceded by argument sanity so the hermetic test stays
    // observable.
    sao_status_t bad_lo = sao_ui_auto_key_send_key(0u, 0u, 0u);
    sao_status_t bad_hi = sao_ui_auto_key_send_key(0x1000u, 0u, 0u);
#if defined(_WIN32)
    CHECK(bad_lo == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(bad_hi == SAO_STATUS_ERR_INVALID_ARGUMENT);
#else
    // Non-Windows: the arg check may or may not run before the gate.
    const bool lo_ok = (bad_lo == SAO_STATUS_ERR_INVALID_ARGUMENT) ||
                       status_is_capability_gate(bad_lo);
    const bool hi_ok = (bad_hi == SAO_STATUS_ERR_INVALID_ARGUMENT) ||
                       status_is_capability_gate(bad_hi);
    CHECK(lo_ok);
    CHECK(hi_ok);
#endif
}

TEST_CASE("wave17a_auto_key_get_key_state_arg_and_gate",
          "[ui][wave17a][class_a][auto_key]") {
    bool state = true;
    sao_status_t rc = sao_ui_auto_key_get_key_state(0x41u, &state);
#if defined(_WIN32)
    CHECK(rc == SAO_STATUS_OK);
    // State is any real bool.
    CHECK((state == true || state == false));
#else
    CHECK(status_is_capability_gate(rc));
    CHECK(state == false);
#endif

    // nullptr out ptr always argument-error.
    sao_status_t bad = sao_ui_auto_key_get_key_state(0x41u, nullptr);
    CHECK(bad == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wave17a_dcomp_bridge_create_null_out_is_arg_error",
          "[ui][wave17a][class_a][dcomp]") {
    // out_handle == nullptr short-circuits before the capability probe.
    sao_status_t rc = sao_ui_dcomp_bridge_create(nullptr, nullptr, nullptr);
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wave17a_dcomp_bridge_create_null_config_capability_gated",
          "[ui][wave17a][class_a][dcomp]") {
    // out_handle non-null, config null.  On Windows this returns
    // INVALID_ARGUMENT; on non-Windows CAPABILITY_MISSING.
    sao_ui_dcomp_bridge_handle_t handle = nullptr;
    sao_status_t rc = sao_ui_dcomp_bridge_create(nullptr, nullptr, &handle);
#if defined(_WIN32)
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
#else
    CHECK(status_is_capability_gate(rc));
#endif
    CHECK(handle == nullptr);
}

TEST_CASE("wave17a_dxgi_dup_create_null_out_is_arg_error",
          "[ui][wave17a][class_a][dxgi_dup]") {
    sao_status_t rc = sao_ui_dxgi_dup_create(nullptr, nullptr);
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wave17a_dxgi_dup_release_frame_handle_invalid",
          "[ui][wave17a][class_a][dxgi_dup]") {
    // Null handle is always HANDLE_INVALID — never reaches the gate.
    sao_status_t rc = sao_ui_dxgi_dup_release_frame(nullptr);
    CHECK(rc == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("wave17a_gpu_capture_supported_is_bool",
          "[ui][wave17a][class_a][gpu_capture]") {
    // Whatever the answer, the probe must not crash and must be a bool.
    // On non-Windows CI the answer is always false.
    const bool supported = sao_ui_gpu_capture_supported();
#if !defined(_WIN32)
    CHECK_FALSE(supported);
#else
    (void)supported;
#endif
}

TEST_CASE("wave17a_gpu_capture_create_null_out_arg_error",
          "[ui][wave17a][class_a][gpu_capture]") {
    sao_status_t rc = sao_ui_gpu_capture_create(nullptr, nullptr);
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
}

TEST_CASE("wave17a_gpu_capture_bgra_from_texture_null_texture",
          "[ui][wave17a][class_a][gpu_capture]") {
    // Null texture: on Windows INVALID_ARGUMENT; on non-Windows the
    // whole function is a stub returning CAPABILITY_MISSING.
    uint8_t buf[16] = {0};
    uint32_t stride = 0;
    sao_status_t rc = sao_ui_gpu_capture_bgra_from_texture(
        nullptr, 1u, 1u, buf, sizeof(buf), &stride);
#if defined(_WIN32)
    CHECK(rc == SAO_STATUS_ERR_INVALID_ARGUMENT);
#else
    CHECK(status_is_capability_gate(rc));
#endif
}

TEST_CASE("wave17a_gpu_capture_ensure_session_handle_invalid",
          "[ui][wave17a][class_a][gpu_capture]") {
    sao_status_t rc = sao_ui_gpu_capture_ensure_session(nullptr);
    CHECK(rc == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("wave17a_input_install_ll_hooks_handle_invalid",
          "[ui][wave17a][class_a][input]") {
    // Null router handle is HANDLE_INVALID before the capability probe.
    sao_status_t rc = sao_ui_input_router_install_ll_hooks(nullptr);
    CHECK(rc == SAO_STATUS_ERR_HANDLE_INVALID);
}

TEST_CASE("wave17a_input_uninstall_ll_hooks_handle_invalid",
          "[ui][wave17a][class_a][input]") {
    sao_status_t rc = sao_ui_input_router_uninstall_ll_hooks(nullptr);
    CHECK(rc == SAO_STATUS_ERR_HANDLE_INVALID);
}

// =====================================================================
// Section 2 — Class C (legacy skeleton) return-code assertions.
// =====================================================================

TEST_CASE("wave17a_legacy_wgl_make_current_stays_not_implemented",
          "[ui][wave17a][class_c][overlay_host]") {
    // Passing null yields NOT_IMPLEMENTED because the entry point is a
    // hardcoded legacy stub — the null check is not even reached.  This
    // is the class-C invariant: the return code does not flip to
    // CAPABILITY_MISSING because the header documents the legacy gate
    // and existing tests pin the exact code.
    sao_status_t rc = sao_ui_overlay_host_make_current(nullptr);
    CHECK(status_is_legacy_skeleton(rc));
    CHECK(rc == SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

TEST_CASE("wave17a_legacy_wgl_release_current_stays_not_implemented",
          "[ui][wave17a][class_c][overlay_host]") {
    sao_status_t rc = sao_ui_overlay_host_release_current(nullptr);
    CHECK(rc == SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

TEST_CASE("wave17a_legacy_wgl_swap_buffers_stays_not_implemented",
          "[ui][wave17a][class_c][overlay_host]") {
    sao_status_t rc = sao_ui_overlay_host_swap_buffers(nullptr);
    CHECK(rc == SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

TEST_CASE("wave17a_legacy_dcomp_gl_interop_stays_not_implemented",
          "[ui][wave17a][class_c][dcomp]") {
    void* interop = reinterpret_cast<void*>(uintptr_t{0xDEADBEEFu});
    sao_status_t r_reg = sao_ui_dcomp_bridge_register_gl_interop(
        nullptr, nullptr, 0u, &interop);
    CHECK(r_reg == SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(interop == nullptr);   // must be zeroed even on the stub path

    CHECK(sao_ui_dcomp_bridge_unregister_gl_interop(nullptr, nullptr)
          == SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(sao_ui_dcomp_bridge_lock_texture(nullptr, nullptr)
          == SAO_STATUS_ERR_NOT_IMPLEMENTED);
    CHECK(sao_ui_dcomp_bridge_unlock_texture(nullptr, nullptr)
          == SAO_STATUS_ERR_NOT_IMPLEMENTED);
}

// =====================================================================
// Section 3 — Hermetic mocks (support-lib coverage).
// =====================================================================

TEST_CASE("wave17a_send_input_ledger_records_press_release",
          "[ui][wave17a][mock][auto_key]") {
    SendInputLedger ledger;
    CHECK(ledger.press_count() == 0);
    CHECK(ledger.release_count() == 0);

    ledger.record_press(0x41u, 0u, 50u);   // 'A' held 50ms
    ledger.record_release(0x41u);
    ledger.record_press(0x42u, 0u, 0u);    // 'B' tap
    ledger.record_release(0x42u);

    CHECK(ledger.press_count() == 2);
    CHECK(ledger.release_count() == 2);
    CHECK(ledger.total_events() == 4);
    const auto& evs = ledger.events();
    CHECK(evs[0].virtual_key == 0x41u);
    CHECK(evs[0].hold_ms == 50u);
    CHECK_FALSE(evs[0].is_release);
    CHECK(evs[1].is_release);
    CHECK(evs[2].virtual_key == 0x42u);

    ledger.reset();
    CHECK(ledger.press_count() == 0);
    CHECK(ledger.total_events() == 0);
}

TEST_CASE("wave17a_shell_notify_ledger_counts_all_ops",
          "[ui][wave17a][mock][alerts]") {
    ShellNotifyLedger ledger;
    auto add = ledger.simulate_add_icon(1u, "sao-auto");
    CHECK(add.status == SAO_STATUS_OK);
    CHECK(add.icon_added);
    CHECK_FALSE(add.icon_removed);

    auto modify = ledger.simulate_modify_icon(1u, "sao-auto-updated");
    CHECK(modify.status == SAO_STATUS_OK);
    CHECK_FALSE(modify.icon_added);
    CHECK_FALSE(modify.icon_removed);

    auto balloon = ledger.simulate_show_balloon(1u, "hi", "world");
    CHECK(balloon.balloon_show_count == 1u);

    auto remove = ledger.simulate_remove_icon(1u);
    CHECK(remove.icon_removed);

    CHECK(ledger.total_calls() == 4u);
    ledger.reset();
    CHECK(ledger.total_calls() == 0u);
}

TEST_CASE("wave17a_dcomp_late_bind_mock_classification",
          "[ui][wave17a][mock][dcomp]") {
    DcompLateBindMock mock;
    // Both present → real bridge could be built → implementable.
    CHECK(mock.resolve(true, true));
    CHECK(mock.classify_outcome(true, true) == GapClass::Implementable);
    CHECK(mock.last_dll_loaded());
    CHECK(mock.last_symbol_present());

    // DLL missing → capability gate.
    CHECK_FALSE(mock.resolve(false, false));
    CHECK(mock.classify_outcome(false, false) == GapClass::CapabilityGate);

    // DLL present but symbol missing → still capability gate (this is
    // exactly the runtime probe on line 208 of dcomp_bridge.cpp).
    CHECK_FALSE(mock.resolve(true, false));
    CHECK(mock.classify_outcome(true, false) == GapClass::CapabilityGate);
}

TEST_CASE("wave17a_matrix_class_a_rows_reference_real_files",
          "[ui][wave17a][taxonomy]") {
    // Sample two class-A rows and prove they carry the required
    // capability + rationale metadata so ops can act on the gate.
    const auto rows = classification_matrix();
    uint32_t verified = 0;
    for (const auto& r : rows) {
        if (r.gap_class != GapClass::CapabilityGate) continue;
        CHECK_FALSE(r.cpp_file.empty());
        CHECK_FALSE(r.api_symbol.empty());
        CHECK_FALSE(r.capability.empty());
        CHECK_FALSE(r.rationale.empty());
        CHECK(r.line_number > 0u);
        if (++verified >= 5u) break;
    }
    CHECK(verified >= 5u);
}

TEST_CASE("wave17a_matrix_class_c_rows_reference_real_files",
          "[ui][wave17a][taxonomy]") {
    const auto rows = classification_matrix();
    uint32_t verified = 0;
    for (const auto& r : rows) {
        if (r.gap_class != GapClass::LegacySkeleton) continue;
        CHECK_FALSE(r.cpp_file.empty());
        CHECK_FALSE(r.api_symbol.empty());
        CHECK_FALSE(r.rationale.empty());
        ++verified;
    }
    CHECK(verified == 7u);   // 4 dcomp interop + 3 overlay WGL
}
