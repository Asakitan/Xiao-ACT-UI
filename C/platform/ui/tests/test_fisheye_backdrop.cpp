// SAO Auto - shared fisheye backdrop tests.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "sao/ui/compositor.h"
#include "sao/ui/fisheye_backdrop.h"

namespace {

constexpr uint32_t kBytesPerPixel = 4;

sao_ui_compositor_handle_t make_headless_compositor() {
    SaoCompositorConfig config{};
    config.target_hz = 60;
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    REQUIRE(compositor != nullptr);
    return compositor;
}

size_t layer_count(sao_ui_compositor_handle_t compositor) {
    size_t count = 0;
    REQUIRE(sao_ui_compositor_list_layers(compositor, nullptr, 0, &count) == SAO_STATUS_OK);
    return count;
}

std::vector<uint8_t> snapshot_compositor(sao_ui_compositor_handle_t compositor,
                                         uint32_t* out_width = nullptr,
                                         uint32_t* out_height = nullptr) {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t bytes = 0;
    const sao_status_t query_status =
        sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0, &width, &height, &bytes);
    REQUIRE((query_status == SAO_STATUS_OK || query_status == SAO_STATUS_ERR_BUFFER_TOO_SMALL));
    std::vector<uint8_t> frame(bytes);
    if (bytes != 0) {
        REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, frame.data(), frame.size(), &width,
                                                &height, &bytes) == SAO_STATUS_OK);
    }
    if (out_width != nullptr) {
        *out_width = width;
    }
    if (out_height != nullptr) {
        *out_height = height;
    }
    return frame;
}

bool has_visible_variation(const std::vector<uint8_t>& frame) {
    if (frame.size() < kBytesPerPixel * 2U) {
        return false;
    }
    std::unordered_set<uint32_t> colors;
    for (size_t offset = 0; offset + 3 < frame.size(); offset += kBytesPerPixel) {
        const uint32_t packed = static_cast<uint32_t>(frame[offset]) |
                                (static_cast<uint32_t>(frame[offset + 1]) << 8U) |
                                (static_cast<uint32_t>(frame[offset + 2]) << 16U) |
                                (static_cast<uint32_t>(frame[offset + 3]) << 24U);
        colors.insert(packed);
        if (colors.size() > 12U) {
            return true;
        }
    }
    return false;
}

SaoUiFisheyeBackdropState wait_for_worker_state(sao_ui_fisheye_backdrop_handle_t backdrop,
                                                bool running) {
    SaoUiFisheyeBackdropState state{};
    for (int attempt = 0; attempt < 100; ++attempt) {
        REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
        if (state.live_worker_running == running) {
            return state;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(state.live_worker_running == running);
    return state;
}

} // namespace

TEST_CASE("fisheye backdrop procedural pixels are styled premultiplied glass",
          "[ui][fisheye_backdrop][procedural]") {
    constexpr uint32_t width = 96;
    constexpr uint32_t height = 64;
    constexpr uint32_t stride = width * kBytesPerPixel + 8U;

    size_t required = 0;
    REQUIRE(sao_ui_fisheye_backdrop_render_procedural_bgra(width, height, stride, nullptr, 0,
                                                           &required) == SAO_STATUS_OK);
    REQUIRE(required == static_cast<size_t>(stride) * height);

    std::vector<uint8_t> too_small(required - 1U);
    size_t repeated_required = 0;
    REQUIRE(sao_ui_fisheye_backdrop_render_procedural_bgra(width, height, stride, too_small.data(),
                                                           too_small.size(), &repeated_required) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    REQUIRE(repeated_required == required);

    std::vector<uint8_t> frame(required, 0x7fU);
    REQUIRE(sao_ui_fisheye_backdrop_render_procedural_bgra(
                width, height, stride, frame.data(), frame.size(), &required) == SAO_STATUS_OK);

    std::unordered_set<uint32_t> colors;
    std::unordered_set<uint8_t> alphas;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = frame.data() + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* pixel = row + static_cast<size_t>(x) * kBytesPerPixel;
            CHECK(pixel[0] <= pixel[3]);
            CHECK(pixel[1] <= pixel[3]);
            CHECK(pixel[2] <= pixel[3]);
            colors.insert(static_cast<uint32_t>(pixel[0]) |
                          (static_cast<uint32_t>(pixel[1]) << 8U) |
                          (static_cast<uint32_t>(pixel[2]) << 16U) |
                          (static_cast<uint32_t>(pixel[3]) << 24U));
            alphas.insert(pixel[3]);
        }
        for (uint32_t x = width * kBytesPerPixel; x < stride; ++x) {
            CHECK(row[x] == 0U);
        }
    }
    CHECK(colors.size() > 32U);
    CHECK(alphas.size() > 4U);

    const uint8_t* cyan_corner = frame.data();
    const uint8_t* gold_corner = frame.data() + static_cast<size_t>(width - 1U) * 4U;
    const uint8_t* center = frame.data() + static_cast<size_t>(height / 2U) * stride +
                            static_cast<size_t>(width / 2U) * 4U;
    CHECK(cyan_corner[0] > cyan_corner[2]);
    CHECK(gold_corner[2] > gold_corner[0]);
    CHECK(cyan_corner[3] > 220U);
    CHECK(gold_corner[3] > 220U);
    CHECK(std::memcmp(cyan_corner, center, 4U) != 0);

    size_t invalid_required = 123U;
    CHECK(sao_ui_fisheye_backdrop_render_procedural_bgra(
              0, height, stride, nullptr, 0, &invalid_required) == SAO_STATUS_ERR_INVALID_ARGUMENT);
    CHECK(invalid_required == 0U);
    CHECK(sao_ui_fisheye_backdrop_render_procedural_bgra(UINT32_MAX, height, UINT32_MAX, nullptr, 0,
                                                         &invalid_required) ==
          SAO_STATUS_ERR_INVALID_ARGUMENT);

    std::vector<uint8_t> one_pixel(4U, 0U);
    REQUIRE(sao_ui_fisheye_backdrop_render_procedural_bgra(
                1, 1, 4, one_pixel.data(), one_pixel.size(), &invalid_required) == SAO_STATUS_OK);
    CHECK(invalid_required == one_pixel.size());
    CHECK(one_pixel[3] > 0U);
}

TEST_CASE("fisheye backdrop reuses one compositor layer across show and hide",
          "[ui][fisheye_backdrop][lifecycle]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    const size_t baseline = layer_count(compositor);

    sao_ui_fisheye_backdrop_handle_t backdrop = nullptr;
    REQUIRE(sao_ui_fisheye_backdrop_create(compositor, &backdrop) == SAO_STATUS_OK);
    REQUIRE(backdrop != nullptr);

    const SaoUiFisheyeBackdropRect first{0, 0, 96, 64};
    REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &first, 120) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
    CHECK(layer_count(compositor) == baseline + 1U);

    SaoUiFisheyeBackdropState state{};
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    CHECK(state.visible);
    CHECK(state.layer_present);
    CHECK(state.mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL);
    CHECK(state.geometry.rect.width == first.width);
    CHECK(state.geometry.rect.height == first.height);
    CHECK(state.geometry.z_order == 120);
    CHECK(state.frame_generation >= 1U);
    const uint64_t first_generation = state.frame_generation;

    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    const std::vector<uint8_t> first_frame =
        snapshot_compositor(compositor, &snapshot_width, &snapshot_height);
    CHECK(snapshot_width == static_cast<uint32_t>(first.width));
    CHECK(snapshot_height == static_cast<uint32_t>(first.height));
    CHECK(has_visible_variation(first_frame));

    const SaoUiFisheyeBackdropRect second{8, 6, 72, 48};
    REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &second, 240) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_service(backdrop) == SAO_STATUS_OK);
    CHECK(layer_count(compositor) == baseline + 1U);
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    CHECK(state.visible);
    CHECK(state.geometry.rect.x == second.x);
    CHECK(state.geometry.rect.y == second.y);
    CHECK(state.geometry.rect.width == second.width);
    CHECK(state.geometry.rect.height == second.height);
    CHECK(state.geometry.z_order == 240);
    CHECK(state.frame_generation > first_generation);

    REQUIRE(sao_ui_fisheye_backdrop_hide(backdrop) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.visible);
    CHECK(state.layer_present);
    CHECK(layer_count(compositor) == baseline + 1U);

    REQUIRE(sao_ui_fisheye_backdrop_try_destroy(backdrop) == SAO_STATUS_OK);
    CHECK(layer_count(compositor) == baseline);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("fisheye backdrop rect uses compositor host-local coordinates",
          "[ui][fisheye_backdrop][coordinates]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    sao_ui_fisheye_backdrop_handle_t backdrop = nullptr;
    REQUIRE(sao_ui_fisheye_backdrop_create(compositor, &backdrop) == SAO_STATUS_OK);

    const SaoUiFisheyeBackdropRect rect{13, 9, 32, 20};
    REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &rect, 90) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);

    SaoUiFisheyeBackdropState state{};
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    CHECK(state.geometry.rect.x == rect.x);
    CHECK(state.geometry.rect.y == rect.y);

    uint32_t width = 0;
    uint32_t height = 0;
    const std::vector<uint8_t> frame = snapshot_compositor(compositor, &width, &height);
    REQUIRE(width == static_cast<uint32_t>(rect.x + rect.width));
    REQUIRE(height == static_cast<uint32_t>(rect.y + rect.height));
    REQUIRE(frame.size() == static_cast<size_t>(width) * height * kBytesPerPixel);
    const size_t outside_offset = 3U;
    const size_t local_origin_offset =
        (static_cast<size_t>(rect.y) * width + static_cast<size_t>(rect.x)) * kBytesPerPixel;
    CHECK(frame[outside_offset] == 0U);
    CHECK(frame[local_origin_offset + 3U] > 0U);

    REQUIRE(sao_ui_fisheye_backdrop_try_destroy(backdrop) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("fisheye backdrop live mode preserves procedural fallback",
          "[ui][fisheye_backdrop][live][fallback]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    sao_ui_fisheye_backdrop_handle_t backdrop = nullptr;
    REQUIRE(sao_ui_fisheye_backdrop_create(compositor, &backdrop) == SAO_STATUS_OK);

    const SaoUiFisheyeBackdropRect rect{0, 0, 80, 48};
    REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &rect, 180) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);

    SaoUiFisheyeBackdropState state{};
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    const uint64_t procedural_generation = state.frame_generation;
    REQUIRE(procedural_generation >= 1U);

    REQUIRE(sao_ui_fisheye_backdrop_set_mode(backdrop, SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);

    for (int attempt = 0; attempt < 30; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
        REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
        if (state.live_available || state.last_status != SAO_STATUS_ERR_NOT_INITIALIZED) {
            break;
        }
    }

    CHECK(state.visible);
    CHECK(state.layer_present);
    CHECK(state.mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE);
    CHECK(state.frame_generation > procedural_generation);
    if (!state.live_available) {
        CHECK(state.last_status != SAO_STATUS_OK);
    }

    const std::vector<uint8_t> frame = snapshot_compositor(compositor);
    CHECK(has_visible_variation(frame));
    CHECK(std::any_of(frame.begin(), frame.end(), [](uint8_t value) { return value != 0U; }));

    REQUIRE(sao_ui_fisheye_backdrop_try_destroy(backdrop) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("fisheye backdrop live worker releases on hide and procedural mode",
          "[ui][fisheye_backdrop][live][lifecycle]") {
    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    const size_t baseline = layer_count(compositor);
    sao_ui_fisheye_backdrop_handle_t backdrop = nullptr;
    REQUIRE(sao_ui_fisheye_backdrop_create(compositor, &backdrop) == SAO_STATUS_OK);

    const SaoUiFisheyeBackdropRect rect{0, 0, 64, 40};
    REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &rect, 180) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_set_mode(backdrop, SAO_UI_FISHEYE_BACKDROP_MODE_LIVE) ==
            SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
    SaoUiFisheyeBackdropState state = wait_for_worker_state(backdrop, true);
    CHECK(state.mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE);
    CHECK(state.visible);
    CHECK_FALSE((state.live_resources_active && !state.live_worker_running));
    CHECK(layer_count(compositor) == baseline + 1U);

    sao_status_t off_owner_tick = SAO_STATUS_OK;
    std::thread off_owner([&] { off_owner_tick = sao_ui_fisheye_backdrop_tick(backdrop); });
    off_owner.join();
    CHECK(off_owner_tick == SAO_STATUS_ERR_ACCESS_DENIED);
    state = wait_for_worker_state(backdrop, true);

    sao_status_t off_owner_destroy = SAO_STATUS_OK;
    std::thread off_owner_teardown(
        [&] { off_owner_destroy = sao_ui_fisheye_backdrop_try_destroy(backdrop); });
    off_owner_teardown.join();
    CHECK(off_owner_destroy == SAO_STATUS_ERR_ACCESS_DENIED);
    state = wait_for_worker_state(backdrop, true);

    REQUIRE(sao_ui_fisheye_backdrop_hide(backdrop) == SAO_STATUS_OK);
    state = wait_for_worker_state(backdrop, false);
    CHECK_FALSE(state.visible);
    CHECK_FALSE(state.live_resources_active);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    CHECK_FALSE(state.live_worker_running);
    CHECK_FALSE(state.live_resources_active);
    CHECK(state.layer_present);
    CHECK(layer_count(compositor) == baseline + 1U);

    REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &rect, 180) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
    state = wait_for_worker_state(backdrop, true);
    CHECK(state.visible);
    CHECK(state.mode == SAO_UI_FISHEYE_BACKDROP_MODE_LIVE);

    REQUIRE(sao_ui_fisheye_backdrop_set_mode(backdrop, SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL) ==
            SAO_STATUS_OK);
    state = wait_for_worker_state(backdrop, false);
    CHECK(state.mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL);
    CHECK_FALSE(state.live_resources_active);
    REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
    REQUIRE(sao_ui_fisheye_backdrop_get_state(backdrop, &state) == SAO_STATUS_OK);
    CHECK(state.visible);
    CHECK(state.mode == SAO_UI_FISHEYE_BACKDROP_MODE_PROCEDURAL);
    CHECK_FALSE(state.live_worker_running);
    CHECK_FALSE(state.live_resources_active);
    CHECK(state.last_status == SAO_STATUS_OK);
    CHECK(state.layer_present);
    CHECK(layer_count(compositor) == baseline + 1U);

    const std::vector<uint8_t> frame = snapshot_compositor(compositor);
    CHECK(has_visible_variation(frame));

    REQUIRE(sao_ui_fisheye_backdrop_try_destroy(backdrop) == SAO_STATUS_OK);
    CHECK(layer_count(compositor) == baseline);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}

TEST_CASE("fisheye backdrop headless creation and repeated destroy leave no layers",
          "[ui][fisheye_backdrop][headless][leak]") {
    sao_ui_fisheye_backdrop_handle_t headless = nullptr;
    REQUIRE(sao_ui_fisheye_backdrop_create(nullptr, &headless) == SAO_STATUS_OK);
    const SaoUiFisheyeBackdropRect rect{0, 0, 32, 24};
    REQUIRE(sao_ui_fisheye_backdrop_show(headless, &rect, 1) == SAO_STATUS_OK);
    CHECK(sao_ui_fisheye_backdrop_tick(headless) == SAO_STATUS_ERR_CAPABILITY_MISSING);
    SaoUiFisheyeBackdropState headless_state{};
    REQUIRE(sao_ui_fisheye_backdrop_get_state(headless, &headless_state) == SAO_STATUS_OK);
    CHECK(headless_state.visible);
    CHECK_FALSE(headless_state.layer_present);
    CHECK(headless_state.last_status == SAO_STATUS_ERR_CAPABILITY_MISSING);
    REQUIRE(sao_ui_fisheye_backdrop_try_destroy(headless) == SAO_STATUS_OK);

    sao_ui_compositor_handle_t compositor = make_headless_compositor();
    const size_t baseline = layer_count(compositor);
    for (int iteration = 0; iteration < 6; ++iteration) {
        sao_ui_fisheye_backdrop_handle_t backdrop = nullptr;
        REQUIRE(sao_ui_fisheye_backdrop_create(compositor, &backdrop) == SAO_STATUS_OK);
        REQUIRE(sao_ui_fisheye_backdrop_show(backdrop, &rect, iteration) == SAO_STATUS_OK);
        REQUIRE(sao_ui_fisheye_backdrop_tick(backdrop) == SAO_STATUS_OK);
        CHECK(layer_count(compositor) == baseline + 1U);
        REQUIRE(sao_ui_fisheye_backdrop_try_destroy(backdrop) == SAO_STATUS_OK);
        CHECK(layer_count(compositor) == baseline);
    }
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}
