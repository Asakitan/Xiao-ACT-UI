// DXGI duplication lifecycle and frame-lease tests.
//
// Coverage:
//   * dxgi_dup_create_on_default_output_succeeds
//   * dxgi_dup_acquire_frame_timeout_returns_expected_status
//   * dxgi_dup_double_release_returns_error
//   * dxgi_dup_invalid_output_index_rejected
//
// Duplication requires an interactive desktop session with a real
// (or WARP-emulatable) monitor.  Session 0 / RDP / VM headless
// configurations reliably fail DuplicateOutput with
// DXGI_ERROR_UNSUPPORTED; we treat those as SKIP rather than failure
// because these tests validate protocol wiring, not the
// availability of a desktop.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/dxgi_dup.h"
#include "sao/core/status.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

SaoDxgiDupConfig make_dup_config(uint32_t output_index = 0,
                                 uint32_t timeout_ms = 16) {
    SaoDxgiDupConfig cfg{};
    cfg.output_index       = output_index;
    cfg.adapter_index      = 0;
    cfg.staging_width      = 0;
    cfg.staging_height     = 0;
    cfg.acquire_timeout_ms = timeout_ms;
    cfg.auto_recover       = true;
    return cfg;
}

// Environmental gate: creating a duplication in a session 0 / no-
// desktop / no-monitor host returns SAO_STATUS_ERR_ACCESS_DENIED
// (mapped from DXGI_ERROR_UNSUPPORTED), or the D3D11 device create
// itself fails and returns SAO_STATUS_ERR_DEVICE_LOST.  Callers use
// this helper to distinguish "test contract violated" from "runner
// physically can't duplicate".
bool is_environmental_skip(sao_status_t rc) {
    return rc == SAO_STATUS_ERR_DEVICE_LOST     // no HW + no WARP
        || rc == SAO_STATUS_ERR_ACCESS_DENIED   // no interactive desktop
        || rc == SAO_STATUS_ERR_OS_CALL_FAILED; // driver couldn't dup
}

}  // namespace

#if defined(_WIN32)

TEST_CASE("dxgi_dup_create_on_default_output_succeeds",
          "[ui][dxgi_dup][interpreter]") {
    const SaoDxgiDupConfig cfg = make_dup_config(0, 16);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t rc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (rc != SAO_STATUS_OK) {
        if (is_environmental_skip(rc)) {
            SKIP("Duplication unavailable in this session (rc=" << rc << ")");
        }
        FAIL("create returned unexpected status " << rc);
    }
    REQUIRE(d != nullptr);

    // get_desc must report a plausible mode.
    SaoDxgiDupDesc desc{};
    const sao_status_t drc = sao_ui_dxgi_dup_get_desc(d, &desc);
    CHECK(drc == SAO_STATUS_OK);
    CHECK(desc.width  > 0u);
    CHECK(desc.height > 0u);

    sao_ui_dxgi_dup_destroy(d);
}

TEST_CASE("dxgi_dup_acquire_frame_timeout_returns_expected_status",
          "[ui][dxgi_dup][interpreter]") {
    // timeout=0 -> non-blocking poll: either an ACCESS_LOST/timeout or
    // a first frame.  The contract is that the return is one of a
    // known-good set, never an unmapped OS_CALL_FAILED.
    const SaoDxgiDupConfig cfg = make_dup_config(0, 0);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t rc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (rc != SAO_STATUS_OK) {
        if (is_environmental_skip(rc)) SKIP("Duplication unavailable");
        FAIL("create returned unexpected status " << rc);
    }

    SaoDxgiDupFrame frame{};
    const sao_status_t frc = sao_ui_dxgi_dup_acquire_frame(d, &frame);
    // Legal outcomes on a fresh duplication with a 0ms timeout:
    //   OK              : the driver has a first frame queued
    //   ERR_TIMEOUT     : no new frame in the (0ms) window
    //   ERR_SURFACE_INVALID : ACCESS_LOST during warmup (rare)
    // ERR_DEVICE_LOST would also be legal on a truly dead device.
    const bool ok =
        frc == SAO_STATUS_OK
     || frc == SAO_STATUS_ERR_TIMEOUT
     || frc == SAO_STATUS_ERR_SURFACE_INVALID
     || frc == SAO_STATUS_ERR_DEVICE_LOST;
    CHECK(ok);

    // If we did acquire, release it so destroy stays clean.
    if (frc == SAO_STATUS_OK) {
        const sao_status_t rr = sao_ui_dxgi_dup_release_frame(d);
        CHECK(rr == SAO_STATUS_OK);
    }
    sao_ui_dxgi_dup_destroy(d);
}

TEST_CASE("dxgi_dup_double_release_returns_error",
          "[ui][dxgi_dup][interpreter]") {
    const SaoDxgiDupConfig cfg = make_dup_config(0, 16);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t rc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (rc != SAO_STATUS_OK) {
        if (is_environmental_skip(rc)) SKIP("Duplication unavailable");
        FAIL("create returned unexpected status " << rc);
    }

    // Release without an acquired frame -> INVALID_ARGUMENT (double
    // release / release-before-acquire is the same failure mode from
    // the caller's perspective).
    const sao_status_t r1 = sao_ui_dxgi_dup_release_frame(d);
    CHECK(r1 == SAO_STATUS_ERR_INVALID_ARGUMENT);

    // Acquire loop with modest retry.  On flaky environments the very
    // first AcquireNextFrame can return TIMEOUT; we allow up to ~500ms
    // total (32 * 16ms) before giving up and skipping.
    SaoDxgiDupFrame frame{};
    sao_status_t ar = SAO_STATUS_ERR_TIMEOUT;
    for (int i = 0; i < 32; ++i) {
        ar = sao_ui_dxgi_dup_acquire_frame(d, &frame);
        if (ar == SAO_STATUS_OK) break;
        if (ar != SAO_STATUS_ERR_TIMEOUT &&
            ar != SAO_STATUS_ERR_SURFACE_INVALID) {
            break;
        }
    }
    if (ar != SAO_STATUS_OK) {
        sao_ui_dxgi_dup_destroy(d);
        SKIP("no desktop frame available for acquire+release test (ar="
             << ar << ")");
    }

    const sao_status_t r2 = sao_ui_dxgi_dup_release_frame(d);
    CHECK(r2 == SAO_STATUS_OK);

    // Double-release the just-released frame -> INVALID_ARGUMENT.
    const sao_status_t r3 = sao_ui_dxgi_dup_release_frame(d);
    CHECK(r3 == SAO_STATUS_ERR_INVALID_ARGUMENT);

    sao_ui_dxgi_dup_destroy(d);
}

TEST_CASE("dxgi_dup_invalid_output_index_rejected",
          "[ui][dxgi_dup][interpreter]") {
    // Pick a wildly-out-of-range monitor index.  EnumOutputs returns
    // DXGI_ERROR_NOT_FOUND for any index >= adapter output count, which
    // the module maps to SAO_STATUS_ERR_NOT_FOUND.
    const SaoDxgiDupConfig cfg = make_dup_config(/*output_index*/ 999, 16);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t rc = sao_ui_dxgi_dup_create(&cfg, &d);

    if (rc == SAO_STATUS_OK) {
        // Impossible unless the machine has 1000+ monitors - be defensive.
        sao_ui_dxgi_dup_destroy(d);
        FAIL("output_index=999 unexpectedly succeeded");
    }

    // Accept either NOT_FOUND (the mapped happy path when the D3D11
    // device did come up) or an environmental skip (D3D11 device
    // itself couldn't come up on this host).
    if (is_environmental_skip(rc)) {
        SKIP("Duplication unavailable (rc=" << rc << ")");
    }
    CHECK(rc == SAO_STATUS_ERR_NOT_FOUND);
    CHECK(d == nullptr);
}

#else  // !_WIN32

TEST_CASE("dxgi_dup_only_on_windows", "[ui][dxgi_dup][interpreter]") {
    SUCCEED("dxgi_dup is Windows-only");
}

#endif  // _WIN32
