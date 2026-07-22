// Compositor layer lifecycle, uniqueness, and z-order tests.
//
// Coverage:
//   * compositor_add_layer_returns_handle
//   * compositor_duplicate_name_rejected  (§7 layer name-reuse leak guard)
//   * compositor_remove_layer_decrements_count
//   * compositor_set_layer_z_reorders    (stable sort, higher z draws last)
//
// No overlay_host required -- the compositor tolerates a null host in
// these headless tests.  Other suites cover the real host, present, and RGN sync.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sao/ui/compositor.h"
#include "sao/core/status.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kLeftButtonDoubleClick = 0x0203;
constexpr uint32_t kMouseWheel = 0x020A;
constexpr uint32_t kCaptureChanged = 0x0215;
constexpr uint32_t kCancelMode = 0x001F;

struct InputLog {
    bool rebind_callbacks_on_cursor = false;
    sao_status_t callback_rebind_status = SAO_STATUS_OK;
    uint32_t cursor_calls = 0;
    uint32_t leave_calls = 0;
    uint32_t button_calls = 0;
    uint32_t scroll_calls = 0;
    float cursor_x = -1.0F;
    float cursor_y = -1.0F;
    float button_local_x = -1.0F;
    float button_local_y = -1.0F;
    uint32_t button = 0;
    bool pressed = false;
    float scroll_x = 0.0F;
    float scroll_y = 0.0F;
    sao_ui_layer_handle_t hide_on_cursor = nullptr;
    sao_status_t reentrant_status = SAO_STATUS_OK;
    sao_ui_compositor_handle_t destroy_on_cursor = nullptr;
    sao_status_t destroy_status = SAO_STATUS_OK;
};

struct ConcurrentRebindProbe {
    std::mutex mutex;
    std::condition_variable changed;
    sao_ui_layer_handle_t layer = nullptr;
    bool setup_complete = false;
    bool ready = false;
    bool cursor_entered = false;
    bool allow_cursor_return = false;
    bool leave_entered = false;
    bool mutation_complete = false;
    bool rebind_returned = false;
    std::thread::id owner_thread;
    std::thread::id leave_thread;
    sao_status_t setup_status = SAO_STATUS_ERR_UNKNOWN;
    sao_status_t dispatch_status = SAO_STATUS_ERR_UNKNOWN;
    sao_status_t rebind_status = SAO_STATUS_ERR_UNKNOWN;
        sao_status_t retry_status = SAO_STATUS_ERR_UNKNOWN;
        sao_status_t show_status = SAO_STATUS_ERR_UNKNOWN;
        sao_status_t replacement_dispatch_status = SAO_STATUS_ERR_UNKNOWN;
    sao_status_t mutation_status = SAO_STATUS_ERR_UNKNOWN;
    sao_status_t destroy_status = SAO_STATUS_ERR_UNKNOWN;
        uint32_t replacement_cursor_calls = 0;
};

struct SameBatchRebindProbe {
    sao_ui_layer_handle_t layer = nullptr;
    sao_status_t rebind_status = SAO_STATUS_OK;
    uint32_t old_button_calls = 0;
    uint32_t old_leave_calls = 0;
    uint32_t replacement_button_calls = 0;
    uint32_t replacement_leave_calls = 0;
};

void SAO_UI_CALL record_cursor(float x, float y, void* user_data);
void SAO_UI_CALL record_leave(void* user_data);
void SAO_UI_CALL record_button(int32_t button, int32_t action, int32_t, float, float,
                               void* user_data);
void SAO_UI_CALL record_scroll(float dx, float dy, void* user_data);
void SAO_UI_CALL concurrent_rebind_cursor(float, float, void* user_data);
void SAO_UI_CALL concurrent_rebind_leave(void* user_data);
void SAO_UI_CALL concurrent_replacement_cursor(float, float, void* user_data);
void SAO_UI_CALL same_batch_old_button(int32_t button, int32_t action, int32_t, float, float,
                                      void* user_data);
void SAO_UI_CALL same_batch_old_leave(void* user_data);
void SAO_UI_CALL same_batch_replacement_button(int32_t, int32_t, int32_t, float, float,
                                              void* user_data);
void SAO_UI_CALL same_batch_replacement_leave(void* user_data);

void SAO_UI_CALL concurrent_rebind_cursor(float, float, void* user_data) {
    auto& probe = *static_cast<ConcurrentRebindProbe*>(user_data);
    std::unique_lock lock(probe.mutex);
    probe.cursor_entered = true;
    probe.changed.notify_all();
    probe.changed.wait(lock, [&] { return probe.allow_cursor_return; });
}

void SAO_UI_CALL concurrent_rebind_leave(void* user_data) {
    auto& probe = *static_cast<ConcurrentRebindProbe*>(user_data);
    {
        std::lock_guard lock(probe.mutex);
        probe.leave_entered = true;
        probe.leave_thread = std::this_thread::get_id();
    }
    probe.changed.notify_all();
}

void SAO_UI_CALL concurrent_replacement_cursor(float, float, void* user_data) {
        ++static_cast<ConcurrentRebindProbe*>(user_data)->replacement_cursor_calls;
}

void SAO_UI_CALL same_batch_replacement_button(int32_t, int32_t, int32_t, float, float,
                                              void* user_data) {
    ++static_cast<SameBatchRebindProbe*>(user_data)->replacement_button_calls;
}

void SAO_UI_CALL same_batch_replacement_leave(void* user_data) {
    ++static_cast<SameBatchRebindProbe*>(user_data)->replacement_leave_calls;
}

void SAO_UI_CALL same_batch_old_button(int32_t, int32_t action, int32_t, float, float,
                                      void* user_data) {
    auto& probe = *static_cast<SameBatchRebindProbe*>(user_data);
    ++probe.old_button_calls;
    if (action == 0) {
        probe.rebind_status = sao_ui_layer_set_input_callbacks(
            probe.layer, nullptr, &same_batch_replacement_leave,
            &same_batch_replacement_button, nullptr, &probe);
    }
}

void SAO_UI_CALL same_batch_old_leave(void* user_data) {
    ++static_cast<SameBatchRebindProbe*>(user_data)->old_leave_calls;
}

void SAO_UI_CALL record_cursor(float x, float y, void* user_data) {
    auto& log = *static_cast<InputLog*>(user_data);
    ++log.cursor_calls;
    log.cursor_x = x;
    log.cursor_y = y;
    if (log.rebind_callbacks_on_cursor) {
        log.rebind_callbacks_on_cursor = false;
        log.callback_rebind_status = sao_ui_layer_set_input_callbacks(
            log.hide_on_cursor, &record_cursor, &record_leave, &record_button, &record_scroll,
            &log);
    }
    if (log.hide_on_cursor != nullptr) {
        log.reentrant_status = sao_ui_layer_set_visible(log.hide_on_cursor, false);
        log.hide_on_cursor = nullptr;
    }
    auto* destroy_target = std::exchange(log.destroy_on_cursor, nullptr);
    if (destroy_target != nullptr)
        log.destroy_status = sao_ui_compositor_try_destroy(destroy_target);
}

void SAO_UI_CALL record_leave(void* user_data) {
    ++static_cast<InputLog*>(user_data)->leave_calls;
}

void SAO_UI_CALL record_button(int32_t button, int32_t action, int32_t, float layer_x,
                               float layer_y,
                               void* user_data) {
    auto& log = *static_cast<InputLog*>(user_data);
    ++log.button_calls;
    log.button = static_cast<uint32_t>(button);
    log.pressed = action != 0;
    log.button_local_x = layer_x;
    log.button_local_y = layer_y;
}

void SAO_UI_CALL record_scroll(float dx, float dy, void* user_data) {
    auto& log = *static_cast<InputLog*>(user_data);
    ++log.scroll_calls;
    log.scroll_x = dx;
    log.scroll_y = dy;
}

SaoCompositorConfig make_compositor_config() {
    SaoCompositorConfig cfg{};
    cfg.target_hz              = 60;
    cfg.enable_temporal_union  = true;
    cfg.enable_rgn_cache       = true;
    return cfg;
}

SaoLayerConfig make_layer_config(const char* name, int32_t z = 0) {
    SaoLayerConfig cfg{};
    cfg.name_utf8       = name;
    cfg.x               = 0;
    cfg.y               = 0;
    cfg.width           = 320;
    cfg.height          = 240;
    cfg.z_order         = z;
    cfg.click_through   = true;
    cfg.rect_hit        = false;
    cfg.bgra_swizzle    = true;
    cfg.high_fps        = false;
    cfg.target_fps      = 0;
    return cfg;
}

// Convenience helper -- query the compositor for layer count only.
size_t layer_count(sao_ui_compositor_handle_t comp) {
    size_t n = 0;
    const sao_status_t rc =
        sao_ui_compositor_list_layers(comp, nullptr, 0, &n);
    if (rc != SAO_STATUS_OK) return static_cast<size_t>(-1);
    return n;
}

}  // namespace

TEST_CASE("compositor_add_layer_returns_handle",
          "[ui][compositor][host]") {
    SaoCompositorConfig cfg = make_compositor_config();
    sao_ui_compositor_handle_t comp = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &cfg, &comp) == SAO_STATUS_OK);
    REQUIRE(comp != nullptr);

    CHECK(layer_count(comp) == 0);

    SaoLayerConfig lc = make_layer_config("hud");
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(comp, &lc, &layer) == SAO_STATUS_OK);
    REQUIRE(layer != nullptr);

    CHECK(layer_count(comp) == 1);

    // list_layers with a small buffer should return the layer handle
    // and the correct count.
    sao_ui_layer_handle_t buf[4] = {};
    size_t n = 0;
    REQUIRE(sao_ui_compositor_list_layers(comp, buf, 4, &n)
            == SAO_STATUS_OK);
    CHECK(n == 1);
    CHECK(buf[0] == layer);

    sao_ui_layer_destroy(layer);
    CHECK(layer_count(comp) == 0);

    sao_ui_compositor_destroy(comp);
}

TEST_CASE("compositor_duplicate_name_rejected",
          "[ui][compositor][host]") {
    SaoCompositorConfig cfg = make_compositor_config();
    sao_ui_compositor_handle_t comp = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &cfg, &comp) == SAO_STATUS_OK);
    REQUIRE(comp != nullptr);

    SaoLayerConfig lc_a = make_layer_config("motion_blur");
    sao_ui_layer_handle_t la = nullptr;
    REQUIRE(sao_ui_layer_create(comp, &lc_a, &la) == SAO_STATUS_OK);

    // Same name -- must fail per §7 of the header banner.  The Python
    // authoritative source used to silently overwrite; the C++ port
    // makes callers destroy first.
    SaoLayerConfig lc_b = make_layer_config("motion_blur");
    sao_ui_layer_handle_t lb = nullptr;
    const sao_status_t rc = sao_ui_layer_create(comp, &lc_b, &lb);
    CHECK(rc == SAO_STATUS_ERR_ALREADY_EXISTS);
    CHECK(lb == nullptr);
    CHECK(layer_count(comp) == 1);

    // After destroying the first, the same name is reusable.
    sao_ui_layer_destroy(la);
    CHECK(layer_count(comp) == 0);

    sao_ui_layer_handle_t lc = nullptr;
    REQUIRE(sao_ui_layer_create(comp, &lc_b, &lc) == SAO_STATUS_OK);
    CHECK(lc != nullptr);
    CHECK(layer_count(comp) == 1);

    sao_ui_compositor_destroy(comp);
}

TEST_CASE("compositor_remove_layer_decrements_count",
          "[ui][compositor][host]") {
    SaoCompositorConfig cfg = make_compositor_config();
    sao_ui_compositor_handle_t comp = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &cfg, &comp) == SAO_STATUS_OK);
    REQUIRE(comp != nullptr);

    SaoLayerConfig la = make_layer_config("layer_a", 0);
    SaoLayerConfig lb = make_layer_config("layer_b", 0);
    SaoLayerConfig lc = make_layer_config("layer_c", 0);

    sao_ui_layer_handle_t ha = nullptr;
    sao_ui_layer_handle_t hb = nullptr;
    sao_ui_layer_handle_t hc = nullptr;
    REQUIRE(sao_ui_layer_create(comp, &la, &ha) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_create(comp, &lb, &hb) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_create(comp, &lc, &hc) == SAO_STATUS_OK);
    CHECK(layer_count(comp) == 3);

    sao_ui_layer_destroy(hb);
    CHECK(layer_count(comp) == 2);

    sao_ui_layer_destroy(ha);
    CHECK(layer_count(comp) == 1);

    sao_ui_layer_destroy(hc);
    CHECK(layer_count(comp) == 0);

    // Double-destroy is safe (header contract: "safe on null" + no-op
    // when the layer has already been removed).
    sao_ui_layer_destroy(nullptr);

    sao_ui_compositor_destroy(comp);
}

TEST_CASE("compositor_set_layer_z_reorders",
          "[ui][compositor][host]") {
    SaoCompositorConfig cfg = make_compositor_config();
    sao_ui_compositor_handle_t comp = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &cfg, &comp) == SAO_STATUS_OK);
    REQUIRE(comp != nullptr);

    // Two layers, z=100 and z=0 respectively.  list_layers must return
    // [z0, z100] because "larger z draws on top" (SaoLayerConfig::z_order
    // docstring).
    SaoLayerConfig cfg_top = make_layer_config("top", /*z=*/100);
    SaoLayerConfig cfg_bot = make_layer_config("bot", /*z=*/0);

    sao_ui_layer_handle_t h_top = nullptr;
    sao_ui_layer_handle_t h_bot = nullptr;
    REQUIRE(sao_ui_layer_create(comp, &cfg_top, &h_top) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_create(comp, &cfg_bot, &h_bot) == SAO_STATUS_OK);
    CHECK(layer_count(comp) == 2);

    sao_ui_layer_handle_t buf[4] = {};
    size_t n = 0;
    REQUIRE(sao_ui_compositor_list_layers(comp, buf, 4, &n)
            == SAO_STATUS_OK);
    REQUIRE(n == 2);
    // Initial order: bot (z=0) first, top (z=100) last.
    CHECK(buf[0] == h_bot);
    CHECK(buf[1] == h_top);

    // Move "top" DOWN to z=0.  After a stable_sort, both share z=0 so
    // the tie-break falls back on creation_seq: top was created first
    // (creation_seq=1 vs bot=2), so the new order is [top, bot].
    REQUIRE(sao_ui_layer_set_z_order(h_top, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_list_layers(comp, buf, 4, &n)
            == SAO_STATUS_OK);
    REQUIRE(n == 2);
    CHECK(buf[0] == h_top);
    CHECK(buf[1] == h_bot);

    // Move "top" back UP to z=200 -- now it must be last again.
    REQUIRE(sao_ui_layer_set_z_order(h_top, 200) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_list_layers(comp, buf, 4, &n)
            == SAO_STATUS_OK);
    REQUIRE(n == 2);
    CHECK(buf[0] == h_bot);
    CHECK(buf[1] == h_top);

    // Also spot-check set_visible works (no visible-observation to
    // check without a real render pass; just a smoke test of the
    // status).
    REQUIRE(sao_ui_layer_set_visible(h_top, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_visible(h_top, true) == SAO_STATUS_OK);

    sao_ui_layer_destroy(h_top);
    sao_ui_layer_destroy(h_bot);
    sao_ui_compositor_destroy(comp);
}

TEST_CASE("compositor routes topmost input with local coordinates and filtering",
          "[ui][compositor][input]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    SaoLayerConfig lower_config = make_layer_config("input.lower", 10);
    lower_config.x = 10;
    lower_config.y = 20;
    lower_config.width = 16;
    lower_config.height = 16;
    lower_config.click_through = false;
    lower_config.rect_hit = true;
    SaoLayerConfig upper_config = lower_config;
    upper_config.name_utf8 = "input.upper";
    upper_config.x = 12;
    upper_config.y = 22;
    upper_config.z_order = 20;
    upper_config.rect_hit = false;

    sao_ui_layer_handle_t lower = nullptr;
    sao_ui_layer_handle_t upper = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &lower_config, &lower) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_create(compositor, &upper_config, &upper) == SAO_STATUS_OK);

    std::vector<uint8_t> upper_pixels(16U * 16U * 4U, 0);
    upper_pixels[(4U * 16U + 3U) * 4U + 3U] = 255;
    REQUIRE(sao_ui_layer_update_bgra(upper, upper_pixels.data(), 16, 16, 16U * 4U) ==
            SAO_STATUS_OK);

    InputLog lower_log;
    InputLog upper_log;
    REQUIRE(sao_ui_layer_set_input_callbacks(lower, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &lower_log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_callbacks(upper, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &upper_log) == SAO_STATUS_OK);

    bool hit = false;
    REQUIRE(sao_ui_compositor_hit_test(compositor, 15, 26, &hit) == SAO_STATUS_OK);
    REQUIRE(hit);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 15, 26, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(upper_log.cursor_calls == 1);
    CHECK(upper_log.cursor_x == 3.0F);
    CHECK(upper_log.cursor_y == 4.0F);
    CHECK(lower_log.cursor_calls == 0);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 15, 26, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(upper_log.button_calls == 1);
    CHECK(upper_log.button == 0);
    CHECK(upper_log.pressed);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 15, 26, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseWheel, 15, 26, -1, 120) ==
            SAO_STATUS_OK);
    CHECK(upper_log.scroll_calls == 1);
    CHECK(upper_log.scroll_x == 0.0F);
    CHECK(upper_log.scroll_y == 1.0F);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 13, 23, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(upper_log.leave_calls == 1);
    CHECK(lower_log.cursor_calls == 1);
    CHECK(lower_log.cursor_x == 3.0F);
    CHECK(lower_log.cursor_y == 3.0F);

    const SaoUiLayerInputRect logical_rect{1, 1, 2, 2};
    REQUIRE(sao_ui_layer_set_input_rects(upper, &logical_rect, 1) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 13, 23, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(upper_log.cursor_calls == 2);
    CHECK(upper_log.cursor_x == 1.0F);
    CHECK(upper_log.cursor_y == 1.0F);
    CHECK(lower_log.leave_calls == 1);

    REQUIRE(sao_ui_layer_set_visible(upper, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 13, 23, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(upper_log.leave_calls == 2);
    CHECK(lower_log.cursor_calls == 2);

    REQUIRE(sao_ui_layer_set_visible(upper, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_rects(upper, nullptr, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_alpha(upper, 0.0F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 15, 26, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(lower_log.cursor_calls == 3);
    REQUIRE(sao_ui_layer_set_alpha(upper, 1.0F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_enabled(upper, false) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 15, 26, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(lower_log.cursor_calls == 4);
    CHECK(upper_log.cursor_calls == 2);

    SaoLayerConfig pass_config = lower_config;
    pass_config.name_utf8 = "input.click-through";
    pass_config.z_order = 100;
    pass_config.click_through = true;
    sao_ui_layer_handle_t click_through = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &pass_config, &click_through) == SAO_STATUS_OK);
    InputLog click_through_log;
    REQUIRE(sao_ui_layer_set_input_callbacks(click_through, &record_cursor, &record_leave,
                                               &record_button, &record_scroll,
                                               &click_through_log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_enabled(upper, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 15, 26, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(upper_log.cursor_calls == 3);
    CHECK(click_through_log.cursor_calls == 0);

    sao_ui_layer_destroy(click_through);
    sao_ui_layer_destroy(upper);
    sao_ui_layer_destroy(lower);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("compositor invokes input callbacks outside its state lock",
          "[ui][compositor][input][reentrant]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("input.reentrant", 1);
    layer_config.width = 32;
    layer_config.height = 32;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);

    InputLog log;
    log.hide_on_cursor = layer;
    log.rebind_callbacks_on_cursor = true;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(log.cursor_calls == 1);
    CHECK(log.callback_rebind_status == SAO_STATUS_OK);
    CHECK(log.reentrant_status == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(log.leave_calls == 1);
    CHECK(log.cursor_calls == 1);

    sao_ui_layer_destroy(layer);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("compositor flushes worker leave callbacks on the owner thread",
          "[ui][compositor][input][generation][concurrent][owner]") {
    ConcurrentRebindProbe probe{};
    std::thread owner_thread([&] {
        probe.owner_thread = std::this_thread::get_id();
        const auto publish_setup = [&](sao_status_t status, sao_ui_layer_handle_t layer) {
            {
                std::lock_guard lock(probe.mutex);
                probe.layer = layer;
                probe.setup_status = status;
                probe.setup_complete = true;
                probe.ready = status == SAO_STATUS_OK;
            }
            probe.changed.notify_all();
        };

        SaoCompositorConfig config = make_compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        const sao_status_t compositor_status =
            sao_ui_compositor_create(nullptr, &config, &compositor);
        if (compositor_status != SAO_STATUS_OK) {
            publish_setup(compositor_status, nullptr);
            return;
        }

        SaoLayerConfig layer_config = make_layer_config("input.concurrent.rebind", 1);
        layer_config.width = 32;
        layer_config.height = 32;
        layer_config.click_through = false;
        layer_config.rect_hit = true;
        sao_ui_layer_handle_t layer = nullptr;
        const sao_status_t layer_status =
            sao_ui_layer_create(compositor, &layer_config, &layer);
        if (layer_status != SAO_STATUS_OK) {
            publish_setup(layer_status, nullptr);
            (void)sao_ui_compositor_try_destroy(compositor);
            return;
        }

        const sao_status_t callback_status = sao_ui_layer_set_input_callbacks(
            layer, &concurrent_rebind_cursor, &concurrent_rebind_leave, nullptr, nullptr, &probe);
        publish_setup(callback_status, layer);
        if (callback_status == SAO_STATUS_OK) {
            probe.dispatch_status =
                sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0);
                        probe.retry_status = sao_ui_layer_set_input_callbacks(
                                layer, &concurrent_replacement_cursor, nullptr, nullptr, nullptr, &probe);
                        probe.show_status = sao_ui_layer_set_visible(layer, true);
                        probe.replacement_dispatch_status =
                                sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0);
        }
        sao_ui_layer_destroy(layer);
        probe.destroy_status = sao_ui_compositor_try_destroy(compositor);
    });

    {
        std::unique_lock lock(probe.mutex);
        probe.changed.wait(lock, [&] { return probe.setup_complete; });
        if (probe.ready)
            probe.changed.wait(lock, [&] { return probe.cursor_entered; });
    }
    if (!probe.ready) {
        owner_thread.join();
        REQUIRE(probe.setup_status == SAO_STATUS_OK);
    }

    std::thread mutator_thread([&] {
                const sao_status_t mutation_status = sao_ui_layer_set_visible(probe.layer, false);
                const sao_status_t rebind_status = sao_ui_layer_set_input_callbacks(
                        probe.layer, &concurrent_replacement_cursor, nullptr, nullptr, nullptr, &probe);
        {
            std::lock_guard lock(probe.mutex);
                        probe.mutation_status = mutation_status;
                        probe.rebind_status = rebind_status;
                        probe.rebind_returned = true;
            probe.mutation_complete = true;
        }
        probe.changed.notify_all();
    });
    {
        std::unique_lock lock(probe.mutex);
        probe.changed.wait(lock, [&] { return probe.mutation_complete; });
        CHECK_FALSE(probe.leave_entered);
        CHECK(probe.rebind_returned);
        probe.allow_cursor_return = true;
    }
    probe.changed.notify_all();
    mutator_thread.join();
    owner_thread.join();

        CHECK(probe.rebind_status == SAO_STATUS_ERR_CANCELLED);
        CHECK(probe.retry_status == SAO_STATUS_OK);
        CHECK(probe.show_status == SAO_STATUS_OK);
        CHECK(probe.replacement_dispatch_status == SAO_STATUS_OK);
    CHECK(probe.mutation_status == SAO_STATUS_OK);
    CHECK(probe.dispatch_status == SAO_STATUS_OK);
    CHECK(probe.destroy_status == SAO_STATUS_OK);
    CHECK(probe.leave_entered);
    CHECK(probe.leave_thread == probe.owner_thread);
    CHECK(probe.replacement_cursor_calls == 1);
}

TEST_CASE("compositor owner rebind waits for an explicit pending leave drain",
          "[ui][compositor][input][generation][pending-owner][retry]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("input.pending-owner.rebind", 1);
    layer_config.width = 16;
    layer_config.height = 16;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);

    InputLog old_log;
    InputLog replacement_log;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &old_log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);

    sao_status_t worker_hide_status = SAO_STATUS_ERR_UNKNOWN;
    sao_status_t worker_rebind_status = SAO_STATUS_ERR_UNKNOWN;
    std::thread worker([&] {
        worker_hide_status = sao_ui_layer_set_visible(layer, false);
        worker_rebind_status = sao_ui_layer_set_input_callbacks(
            layer, &record_cursor, &record_leave, &record_button, &record_scroll,
            &replacement_log);
    });
    worker.join();
    CHECK(worker_hide_status == SAO_STATUS_OK);
    CHECK(worker_rebind_status == SAO_STATUS_ERR_CANCELLED);
    CHECK(old_log.leave_calls == 0);
    CHECK(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                            &record_scroll, &replacement_log) ==
          SAO_STATUS_ERR_CANCELLED);

    const sao_status_t tick_status = sao_ui_compositor_tick(compositor);
    CHECK((tick_status == SAO_STATUS_OK || tick_status == SAO_STATUS_ERR_NOT_INITIALIZED));
    CHECK(old_log.leave_calls == 1);
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &replacement_log) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_visible(layer, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(old_log.cursor_calls == 1);
    CHECK(replacement_log.cursor_calls == 1);

    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("compositor rejects rebind with a pending same-batch leave and preserves callbacks",
          "[ui][compositor][input][generation][same-batch]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("input.same-batch.rebind", 1);
    layer_config.width = 10;
    layer_config.height = 10;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);

    SameBatchRebindProbe probe{};
    probe.layer = layer;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, nullptr, &same_batch_old_leave,
                                               &same_batch_old_button, nullptr, &probe) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 2, 2, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 2, 2, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 20, 20, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(probe.rebind_status == SAO_STATUS_ERR_CANCELLED);
    CHECK(probe.old_button_calls == 2);
    CHECK(probe.old_leave_calls == 1);
    CHECK(probe.replacement_button_calls == 0);
    CHECK(probe.replacement_leave_calls == 0);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 2, 2, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 2, 2, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(probe.old_button_calls == 3);
    CHECK(probe.replacement_button_calls == 0);

    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("compositor captures pointer until button release and suppresses dblclick replay",
          "[ui][compositor][input][capture][dblclick]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("input.capture", 1);
    layer_config.width = 10;
    layer_config.height = 10;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    InputLog log;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &log) == SAO_STATUS_OK);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 2, 3, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 2, 3, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 40, 50, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(log.cursor_calls == 2);
    CHECK(log.cursor_x == 40.0F);
    CHECK(log.cursor_y == 50.0F);
    CHECK(log.leave_calls == 0);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 40, 50, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(log.button_calls == 2);
    CHECK_FALSE(log.pressed);
    CHECK(log.button_local_x == 40.0F);
    CHECK(log.button_local_y == 50.0F);
    CHECK(log.leave_calls == 1);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 2, 3, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 2, 3, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 2, 3, 0, 0) ==
            SAO_STATUS_OK);
    const uint32_t ordinary_button_calls = log.button_calls;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDoubleClick, 2, 3, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 2, 3, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(log.button_calls == ordinary_button_calls);

    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("capture invalidation suppresses the orphaned up before it reaches another layer",
          "[ui][compositor][input][capture][invalidation][suppression]") {
    enum class Invalidation {
        capture_changed,
        cancel_mode,
        hide,
        input_disable,
        alpha_zero,
        destroy,
    };
    constexpr std::array invalidations = {
        Invalidation::capture_changed, Invalidation::cancel_mode, Invalidation::hide,
        Invalidation::input_disable, Invalidation::alpha_zero, Invalidation::destroy};

    for (const auto invalidation : invalidations) {
        SaoCompositorConfig config = make_compositor_config();
        sao_ui_compositor_handle_t compositor = nullptr;
        REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

        SaoLayerConfig bottom_config = make_layer_config("input.capture.bottom", 1);
        bottom_config.width = 32;
        bottom_config.height = 32;
        bottom_config.click_through = false;
        bottom_config.rect_hit = true;
        sao_ui_layer_handle_t bottom = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &bottom_config, &bottom) == SAO_STATUS_OK);
        InputLog bottom_log;
        REQUIRE(sao_ui_layer_set_input_callbacks(bottom, &record_cursor, &record_leave,
                                                   &record_button, &record_scroll, &bottom_log) ==
                SAO_STATUS_OK);

        SaoLayerConfig top_config = make_layer_config("input.capture.top", 2);
        top_config.width = 16;
        top_config.height = 16;
        top_config.click_through = false;
        top_config.rect_hit = true;
        sao_ui_layer_handle_t top = nullptr;
        REQUIRE(sao_ui_layer_create(compositor, &top_config, &top) == SAO_STATUS_OK);
        InputLog top_log;
        REQUIRE(sao_ui_layer_set_input_callbacks(top, &record_cursor, &record_leave,
                                                   &record_button, &record_scroll, &top_log) ==
                SAO_STATUS_OK);

        REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
                SAO_STATUS_OK);
        REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 4, 5, 0, 0) ==
                SAO_STATUS_OK);
        bool top_destroyed = false;
        switch (invalidation) {
        case Invalidation::capture_changed:
            REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kCaptureChanged, 0, 0, -1, 0) ==
                    SAO_STATUS_OK);
            break;
        case Invalidation::cancel_mode:
            REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kCancelMode, 0, 0, -1, 0) ==
                    SAO_STATUS_OK);
            break;
        case Invalidation::hide:
            REQUIRE(sao_ui_layer_set_visible(top, false) == SAO_STATUS_OK);
            break;
        case Invalidation::input_disable:
            REQUIRE(sao_ui_layer_set_input_enabled(top, false) == SAO_STATUS_OK);
            break;
        case Invalidation::alpha_zero:
            REQUIRE(sao_ui_layer_set_alpha(top, 0.0F) == SAO_STATUS_OK);
            break;
        case Invalidation::destroy:
            sao_ui_layer_destroy(top);
            top_destroyed = true;
            break;
        }

        REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 20, 20, 0, 0) ==
                SAO_STATUS_OK);
        CHECK(top_log.button_calls == 1);
        CHECK(top_log.leave_calls == 1);
        CHECK(bottom_log.button_calls == 0);

        REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 20, 20, 0, 0) ==
                SAO_STATUS_OK);
        REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 20, 20, 0, 0) ==
                SAO_STATUS_OK);
        CHECK(bottom_log.button_calls == 2);

        if (!top_destroyed)
            sao_ui_layer_destroy(top);
        sao_ui_layer_destroy(bottom);
        REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    }
}

TEST_CASE("compositor immediately leaves inactive layers and excludes input proxies",
          "[ui][compositor][input][leave][proxy]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("input.transitions", 1);
    layer_config.width = 16;
    layer_config.height = 16;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    InputLog log;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &log) == SAO_STATUS_OK);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_visible(layer, false) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 1);
    REQUIRE(sao_ui_layer_set_visible(layer, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_input_enabled(layer, false) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 2);
    REQUIRE(sao_ui_layer_set_input_enabled(layer, true) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_alpha(layer, 0.0F) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 3);
    REQUIRE(sao_ui_layer_set_alpha(layer, 1.0F) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_enable_input_proxy(layer) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 4);
    bool hit = true;
    REQUIRE(sao_ui_compositor_hit_test(compositor, 4, 5, &hit) == SAO_STATUS_OK);
    CHECK_FALSE(hit);
    const uint32_t cursor_calls = log.cursor_calls;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 4, 5, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(log.cursor_calls == cursor_calls);

    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("compositor clears hover and capture when geometry or hit bounds move away",
          "[ui][compositor][input][leave][geometry]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("input.geometry.transitions", 1);
    layer_config.width = 16;
    layer_config.height = 16;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    InputLog log;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &log) == SAO_STATUS_OK);

    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 5, 5, -1, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonDown, 5, 5, 0, 0) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_position(layer, 20, 20) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 1);
    const uint32_t captured_button_calls = log.button_calls;
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kLeftButtonUp, 5, 5, 0, 0) ==
            SAO_STATUS_OK);
    CHECK(log.button_calls == captured_button_calls);

    REQUIRE(sao_ui_layer_set_position(layer, 0, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 10, 10, -1, 0) ==
            SAO_STATUS_OK);
    const std::vector<uint8_t> small_frame(4U * 4U * 4U, 0xff);
    REQUIRE(sao_ui_layer_update_bgra(layer, small_frame.data(), 4, 4, 16) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 2);

    const std::vector<uint8_t> restored_frame(16U * 16U * 4U, 0xff);
    REQUIRE(sao_ui_layer_update_bgra(layer, restored_frame.data(), 16, 16, 64) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 5, 5, -1, 0) ==
            SAO_STATUS_OK);
    const SaoUiLayerInputRect narrow_rect{0, 0, 2, 2};
    REQUIRE(sao_ui_layer_set_input_rects(layer, &narrow_rect, 1) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 3);

    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("compositor try destroy reports callback busy and succeeds on retry",
          "[ui][compositor][lifecycle][retry]") {
    SaoCompositorConfig config = make_compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    SaoLayerConfig layer_config = make_layer_config("destroy.retry", 1);
    layer_config.width = 8;
    layer_config.height = 8;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    InputLog log;
    log.destroy_on_cursor = compositor;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &log) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_dispatch_mouse(compositor, kMouseMove, 1, 1, -1, 0) ==
            SAO_STATUS_OK);
    CHECK(log.destroy_status == SAO_STATUS_ERR_CANCELLED);
    CHECK(layer_count(compositor) == 1);
    sao_ui_layer_destroy(layer);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

#if defined(_WIN32)
TEST_CASE("host capture loss clears compositor capture and emits one leave",
          "[ui][compositor][host][input][capture][cancel]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 96;
    host_config.height = 96;
    host_config.origin_x = 40;
    host_config.origin_y = 40;
    host_config.title_utf16 = L"SAO compositor capture loss test";
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&host_config, &host) == SAO_STATUS_OK);
    sao_ui_compositor_handle_t compositor = nullptr;
    const sao_status_t create_status = sao_ui_compositor_create(host, nullptr, &compositor);
    if (create_status != SAO_STATUS_OK) {
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D/DComp compositor unavailable in this Windows session");
    }
    SaoLayerConfig layer_config = make_layer_config("host.capture.loss", 1);
    layer_config.width = 24;
    layer_config.height = 24;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    InputLog log;
    REQUIRE(sao_ui_layer_set_input_callbacks(layer, &record_cursor, &record_leave, &record_button,
                                               &record_scroll, &log) == SAO_STATUS_OK);
    const auto hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);
    const auto capture_layer = [&] {
        REQUIRE(::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(2, 3)) == 0);
        REQUIRE(::SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 3)) == 0);
        REQUIRE(::GetCapture() == hwnd);
    };

    capture_layer();
    HWND other = ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                   ::GetModuleHandleW(nullptr), nullptr);
    REQUIRE(other != nullptr);
    REQUIRE(::SetCapture(other) == hwnd);
    CHECK(log.leave_calls == 1);
    REQUIRE(::GetCapture() == other);
    REQUIRE(::ReleaseCapture());
    REQUIRE(::DestroyWindow(other));

    capture_layer();
    REQUIRE(::SendMessageW(hwnd, WM_CANCELMODE, 0, 0) == 0);
    CHECK(log.leave_calls == 2);
    CHECK(::GetCapture() == nullptr);

    capture_layer();
    REQUIRE(sao_ui_overlay_host_set_visible(host, false) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 3);
    CHECK(::GetCapture() == nullptr);

    capture_layer();
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    CHECK(log.leave_calls == 4);
    CHECK(::GetCapture() == nullptr);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}

TEST_CASE("host-bound compositor validates owner and restores input state before destroy",
          "[ui][compositor][host][owner][rgn][teardown]") {
    SaoOverlayHostConfig host_config{};
    host_config.width = 96;
    host_config.height = 96;
    host_config.origin_x = 40;
    host_config.origin_y = 40;
    host_config.title_utf16 = L"SAO compositor teardown test";
    sao_ui_overlay_host_handle_t host = nullptr;
    REQUIRE(sao_ui_overlay_host_create(&host_config, &host) == SAO_STATUS_OK);

    sao_status_t foreign_create_status = SAO_STATUS_OK;
    sao_ui_compositor_handle_t foreign_compositor = reinterpret_cast<sao_ui_compositor_handle_t>(1);
    std::thread foreign_create([&] {
        foreign_create_status =
            sao_ui_compositor_create(host, nullptr, &foreign_compositor);
    });
    foreign_create.join();
    CHECK(foreign_create_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(foreign_compositor == nullptr);

    sao_ui_compositor_handle_t compositor = nullptr;
    const sao_status_t create_status = sao_ui_compositor_create(host, nullptr, &compositor);
    if (create_status != SAO_STATUS_OK) {
        REQUIRE(sao_ui_overlay_host_destroy(host));
        SKIP("D3D/DComp compositor unavailable in this Windows session");
    }
    SaoLayerConfig layer_config = make_layer_config("host.input", 1);
    layer_config.width = 24;
    layer_config.height = 24;
    layer_config.click_through = false;
    layer_config.rect_hit = true;
    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &layer_config, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_sync_host_rgn(compositor) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_sync_host_input_mode(compositor) == SAO_STATUS_OK);
    CHECK_FALSE(sao_ui_overlay_host_input_passthrough(host));

    sao_status_t foreign_destroy_status = SAO_STATUS_OK;
    std::thread foreign_destroy([&] {
        foreign_destroy_status = sao_ui_compositor_try_destroy(compositor);
    });
    foreign_destroy.join();
    CHECK(foreign_destroy_status == SAO_STATUS_ERR_ACCESS_DENIED);
    CHECK(layer_count(compositor) == 1);

    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
    CHECK(sao_ui_overlay_host_input_passthrough(host));
    const auto hwnd = static_cast<HWND>(sao_ui_overlay_host_hwnd(host));
    REQUIRE(hwnd != nullptr);
    const LPARAM point = MAKELPARAM(host_config.origin_x + 2, host_config.origin_y + 2);
    CHECK(::SendMessageW(hwnd, WM_NCHITTEST, 0, point) == HTTRANSPARENT);
    REQUIRE(sao_ui_overlay_host_destroy(host));
}
#endif
