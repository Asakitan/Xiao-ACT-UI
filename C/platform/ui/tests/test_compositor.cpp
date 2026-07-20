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

#include <cstring>
#include <string>

#include "sao/ui/compositor.h"
#include "sao/core/status.h"

namespace {

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
