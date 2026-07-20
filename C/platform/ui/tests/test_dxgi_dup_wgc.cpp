// DXGI duplication tests for dirty/move rects, cursor,
// staging copy).
//
// Coverage (4 test cases):
//   * dxgi_dup_dirty_rects_returns_ok_or_skip
//   * dxgi_dup_move_rects_returns_ok_or_skip
//   * dxgi_dup_cursor_info_populates_position
//   * dxgi_dup_copy_to_staging_produces_bgra
//
// All four require an active duplication with a held frame; sessions
// without a real desktop (session 0 / RDP / VM headless) SKIP rather
// than fail.  The base DXGI tests cover the create+acquire+release
// pipe on session-0 hardware, so if create succeeds and Acquire
// returns OK we know the fixture holds; these cases add
// coverage for the "we actually asked for data" paths.

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/dxgi_dup.h"
#include "sao/core/status.h"

#include <cstdint>
#include <cstring>
#include <vector>

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

SaoDxgiDupConfig make_cfg(uint32_t timeout_ms = 32) {
    SaoDxgiDupConfig cfg{};
    cfg.output_index       = 0;
    cfg.adapter_index      = 0;
    cfg.staging_width      = 0;
    cfg.staging_height     = 0;
    cfg.acquire_timeout_ms = timeout_ms;
    cfg.auto_recover       = true;
    return cfg;
}

bool is_environmental_skip(sao_status_t rc) {
    return rc == SAO_STATUS_ERR_DEVICE_LOST
        || rc == SAO_STATUS_ERR_ACCESS_DENIED
        || rc == SAO_STATUS_ERR_OS_CALL_FAILED;
}

// Retry Acquire until we get OK or exhaust the budget.  Returns the
// final status.
sao_status_t acquire_with_retry(sao_ui_dxgi_dup_handle_t d,
                                SaoDxgiDupFrame* frame,
                                int max_tries) {
    sao_status_t rc = SAO_STATUS_ERR_TIMEOUT;
    for (int i = 0; i < max_tries; ++i) {
        rc = sao_ui_dxgi_dup_acquire_frame(d, frame);
        if (rc == SAO_STATUS_OK) return rc;
        if (rc != SAO_STATUS_ERR_TIMEOUT &&
            rc != SAO_STATUS_ERR_SURFACE_INVALID) break;
    }
    return rc;
}

}  // namespace

#if defined(_WIN32)

TEST_CASE("dxgi_dup_dirty_rects_returns_ok_or_skip",
          "[ui][dxgi_dup][real_plugins]") {
    const SaoDxgiDupConfig cfg = make_cfg(64);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t crc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (crc != SAO_STATUS_OK) {
        if (is_environmental_skip(crc)) SKIP("Duplication unavailable");
        FAIL("create returned unexpected status " << crc);
    }

    // Before any frame is held, get_dirty_rects must return
    // NOT_INITIALIZED — the API contract.
    uint32_t precount = 42;
    const sao_status_t pre = sao_ui_dxgi_dup_get_dirty_rects(
        d, nullptr, 0, &precount);
    CHECK(pre == SAO_STATUS_ERR_NOT_INITIALIZED);
    CHECK(precount == 0u);

    SaoDxgiDupFrame frame{};
    const sao_status_t arc = acquire_with_retry(d, &frame, 32);
    if (arc != SAO_STATUS_OK) {
        sao_ui_dxgi_dup_destroy(d);
        SKIP("no desktop frame available (arc=" << arc << ")");
    }

    // Count-only probe with rects_out=NULL.  Legal per header contract.
    uint32_t count = 999999;
    sao_status_t rc = sao_ui_dxgi_dup_get_dirty_rects(
        d, nullptr, 0, &count);
    CHECK(rc == SAO_STATUS_OK);
    // count may be 0 (idle desktop) or non-zero.  Either is acceptable;
    // 999999 sentinel must be overwritten.
    CHECK(count < 100000u);

    // Sized read.  When there are dirty rects the values must fit
    // within the monitor rect.
    if (count > 0) {
        std::vector<SaoDxgiDupRect> rects(count);
        uint32_t got = 0;
        rc = sao_ui_dxgi_dup_get_dirty_rects(
            d, rects.data(), count, &got);
        CHECK(rc == SAO_STATUS_OK);
        CHECK(got == count);
        for (const auto& r : rects) {
            CHECK(r.left  >= 0);
            CHECK(r.top   >= 0);
            CHECK(r.right  >= r.left);
            CHECK(r.bottom >= r.top);
        }
    }

    // Undersized buffer must return BUFFER_TOO_SMALL when count > 0.
    if (count > 1) {
        std::vector<SaoDxgiDupRect> half(count - 1);
        uint32_t got = 0;
        rc = sao_ui_dxgi_dup_get_dirty_rects(
            d, half.data(), static_cast<uint32_t>(half.size()), &got);
        CHECK(rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
        CHECK(got == count);
    }

    CHECK(sao_ui_dxgi_dup_release_frame(d) == SAO_STATUS_OK);
    sao_ui_dxgi_dup_destroy(d);
}

TEST_CASE("dxgi_dup_move_rects_returns_ok_or_skip",
          "[ui][dxgi_dup][real_plugins]") {
    const SaoDxgiDupConfig cfg = make_cfg(64);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t crc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (crc != SAO_STATUS_OK) {
        if (is_environmental_skip(crc)) SKIP("Duplication unavailable");
        FAIL("create returned unexpected status " << crc);
    }

    SaoDxgiDupFrame frame{};
    const sao_status_t arc = acquire_with_retry(d, &frame, 32);
    if (arc != SAO_STATUS_OK) {
        sao_ui_dxgi_dup_destroy(d);
        SKIP("no desktop frame available (arc=" << arc << ")");
    }

    // Move rects are rarer than dirty rects (only DWM window drags
    // populate them).  Empty is the common case; the API must still
    // return OK for a probe.
    uint32_t count = 999u;
    sao_status_t rc = sao_ui_dxgi_dup_get_move_rects(
        d, nullptr, 0, &count);
    CHECK(rc == SAO_STATUS_OK);
    CHECK(count < 100000u);

    if (count > 0) {
        std::vector<SaoDxgiDupMoveRect> moves(count);
        uint32_t got = 0;
        rc = sao_ui_dxgi_dup_get_move_rects(
            d, moves.data(), count, &got);
        CHECK(rc == SAO_STATUS_OK);
        CHECK(got == count);
        for (const auto& m : moves) {
            CHECK(m.src_x >= 0);
            CHECK(m.src_y >= 0);
            CHECK(m.dst.right  >= m.dst.left);
            CHECK(m.dst.bottom >= m.dst.top);
        }
    }

    CHECK(sao_ui_dxgi_dup_release_frame(d) == SAO_STATUS_OK);
    sao_ui_dxgi_dup_destroy(d);
}

TEST_CASE("dxgi_dup_cursor_info_populates_position",
          "[ui][dxgi_dup][real_plugins]") {
    const SaoDxgiDupConfig cfg = make_cfg(64);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t crc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (crc != SAO_STATUS_OK) {
        if (is_environmental_skip(crc)) SKIP("Duplication unavailable");
        FAIL("create returned unexpected status " << crc);
    }

    SaoDxgiDupFrame frame{};
    const sao_status_t arc = acquire_with_retry(d, &frame, 32);
    if (arc != SAO_STATUS_OK) {
        sao_ui_dxgi_dup_destroy(d);
        SKIP("no desktop frame available (arc=" << arc << ")");
    }

    SaoDxgiDupCursorInfo info{};
    uint32_t shape_bytes = 0;
    // First probe with shape_out=NULL — legal per header when caller
    // only wants position (shape_capacity=0).
    sao_status_t rc = sao_ui_dxgi_dup_get_cursor_info(
        d, &info, nullptr, 0, &shape_bytes);
    CHECK(rc == SAO_STATUS_OK);
    // Position may be anywhere on desktop; sanity-bound only.
    CHECK(info.position_x >= -100000);
    CHECK(info.position_y >= -100000);
    CHECK(info.position_x <=  100000);
    CHECK(info.position_y <=  100000);

    // If a shape was updated we can pull it into a large buffer.
    if (info.shape_updated) {
        const uint32_t expected = info.shape_pitch * info.shape_height;
        std::vector<uint8_t> shape(expected + 64u, 0u);
        uint32_t got = 0;
        rc = sao_ui_dxgi_dup_get_cursor_info(
            d, &info, shape.data(),
            static_cast<uint32_t>(shape.size()), &got);
        CHECK(rc == SAO_STATUS_OK);
        // Second call typically won't re-update (DXGI only reports
        // shape when it changes); relaxed check.
    }

    // Undersized shape buffer must error when a shape is available.
    if (info.shape_width > 0 && info.shape_height > 0) {
        uint8_t tiny[4];
        uint32_t got = 0;
        rc = sao_ui_dxgi_dup_get_cursor_info(
            d, &info, tiny, sizeof(tiny), &got);
        // Either BUFFER_TOO_SMALL (shape cached, we asked for it) or
        // OK (no cached shape, tiny buffer sufficient for 0 bytes).
        CHECK((rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL ||
               rc == SAO_STATUS_OK));
    }

    CHECK(sao_ui_dxgi_dup_release_frame(d) == SAO_STATUS_OK);
    sao_ui_dxgi_dup_destroy(d);
}

TEST_CASE("dxgi_dup_copy_to_staging_produces_bgra",
          "[ui][dxgi_dup][real_plugins]") {
    const SaoDxgiDupConfig cfg = make_cfg(64);
    sao_ui_dxgi_dup_handle_t d = nullptr;
    const sao_status_t crc = sao_ui_dxgi_dup_create(&cfg, &d);
    if (crc != SAO_STATUS_OK) {
        if (is_environmental_skip(crc)) SKIP("Duplication unavailable");
        FAIL("create returned unexpected status " << crc);
    }

    SaoDxgiDupDesc desc{};
    REQUIRE(sao_ui_dxgi_dup_get_desc(d, &desc) == SAO_STATUS_OK);
    REQUIRE(desc.width > 0u);
    REQUIRE(desc.height > 0u);

    SaoDxgiDupFrame frame{};
    const sao_status_t arc = acquire_with_retry(d, &frame, 32);
    if (arc != SAO_STATUS_OK) {
        sao_ui_dxgi_dup_destroy(d);
        SKIP("no desktop frame available (arc=" << arc << ")");
    }

    // Undersized buffer must return BUFFER_TOO_SMALL and populate
    // stride_out for the caller to size-up.
    uint8_t tiny_buf[16];
    uint32_t stride_probe = 0;
    uint32_t written_probe = 0;
    const sao_status_t small_rc = sao_ui_dxgi_dup_copy_to_staging(
        d, tiny_buf, sizeof(tiny_buf), &stride_probe, &written_probe);
    CHECK(small_rc == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    CHECK(stride_probe > 0u);
    CHECK(stride_probe >= desc.width * 4u);
    CHECK(written_probe >= stride_probe * desc.height);

    // Full-size read.
    const uint32_t needed = stride_probe * desc.height;
    std::vector<uint8_t> buf(needed + 64u, 0u);
    uint32_t stride = 0;
    uint32_t written = 0;
    const sao_status_t rc = sao_ui_dxgi_dup_copy_to_staging(
        d, buf.data(), static_cast<uint32_t>(buf.size()),
        &stride, &written);
    CHECK(rc == SAO_STATUS_OK);
    CHECK(stride == stride_probe);
    CHECK(written == needed);

    // BGRA alpha channel is opaque-desktop for the visible surface;
    // check that SOME alpha=255 pixels exist (sampling a few rows).
    // Session-locked or all-transparent surfaces are still legal
    // according to the API (we just need a valid copy).
    size_t opaque_seen = 0;
    for (uint32_t y = 0; y < desc.height; y += 64) {
        for (uint32_t x = 0; x < desc.width; x += 64) {
            const uint32_t idx = y * stride + x * 4;
            if (buf[idx + 3] == 0xFF) opaque_seen += 1;
        }
    }
    // We don't assert this — a screen-locked session can produce
    // a valid staging read with 0 opaque samples.  Just check the
    // read completed sanely.
    (void)opaque_seen;

    // Post-release, copy_to_staging must fail (no held frame).
    CHECK(sao_ui_dxgi_dup_release_frame(d) == SAO_STATUS_OK);
    uint32_t stride_after = 0;
    const sao_status_t after = sao_ui_dxgi_dup_copy_to_staging(
        d, buf.data(), static_cast<uint32_t>(buf.size()),
        &stride_after, nullptr);
    CHECK(after == SAO_STATUS_ERR_NOT_INITIALIZED);

    sao_ui_dxgi_dup_destroy(d);
}

#else  // !_WIN32

TEST_CASE("dxgi_dup_wgc_windows_only", "[ui][dxgi_dup][real_plugins]") {
    SUCCEED("dxgi_dup WGC tests are Windows-only");
}

#endif  // _WIN32
