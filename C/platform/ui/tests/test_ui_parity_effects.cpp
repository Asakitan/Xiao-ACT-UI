// Python-authority parity for Alert, BuffMon, and SkillFX.
// This intentionally has no CMake registration: integration owns target wiring.

#include <catch2/catch_test_macros.hpp>

#include "sao/core/status.h"
#include "sao/ui/compositor.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef SAO_UI_TEST_FIXTURE_DIR
#  define SAO_UI_TEST_FIXTURE_DIR ""
#endif

namespace {

using Json = nlohmann::json;

struct Fixture {
    std::string stem;
    Json metadata;
    std::vector<uint8_t> pixels;
};

SaoCompositorConfig compositor_config() {
    SaoCompositorConfig config{};
    config.target_hz = 60;
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    return config;
}

std::filesystem::path fixture_path(std::string_view name, std::string_view extension) {
    return std::filesystem::path(SAO_UI_TEST_FIXTURE_DIR) /
        (std::string(name) + std::string(extension));
}

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Fixture load_fixture(std::string_view stem) {
    const auto metadata_path = fixture_path(stem, ".json");
    std::ifstream metadata_input(metadata_path);
    REQUIRE(metadata_input.good());

    Fixture fixture{};
    fixture.stem = stem;
    metadata_input >> fixture.metadata;
    if (fixture.metadata.value("headless_status", "") == "skip") {
        return fixture;
    }
    fixture.pixels = read_bytes(fixture_path(stem, ".bgra"));
    REQUIRE(fixture.pixels.size() == fixture.metadata.at("bytes").get<size_t>());
    return fixture;
}

void verify_premultiplied_bgra(const Fixture& fixture) {
    REQUIRE(fixture.metadata.at("format") == "premultiplied-bgra");
    const uint32_t width = fixture.metadata.at("width").get<uint32_t>();
    const uint32_t height = fixture.metadata.at("height").get<uint32_t>();
    REQUIRE(fixture.metadata.at("stride").get<uint32_t>() == width * 4u);
    REQUIRE(fixture.pixels.size() == static_cast<size_t>(width) * height * 4u);
    for (size_t offset = 0; offset < fixture.pixels.size(); offset += 4u) {
        REQUIRE(fixture.pixels[offset] <= fixture.pixels[offset + 3]);
        REQUIRE(fixture.pixels[offset + 1] <= fixture.pixels[offset + 3]);
        REQUIRE(fixture.pixels[offset + 2] <= fixture.pixels[offset + 3]);
    }
}

Json alpha_bbox(const Fixture& fixture) {
    const uint32_t width = fixture.metadata.at("width").get<uint32_t>();
    const uint32_t height = fixture.metadata.at("height").get<uint32_t>();
    uint32_t min_x = width;
    uint32_t min_y = height;
    uint32_t max_x = 0;
    uint32_t max_y = 0;
    bool found = false;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t alpha = fixture.pixels[(static_cast<size_t>(y) * width + x) * 4u + 3u];
            if (alpha == 0) continue;
            found = true;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x + 1u);
            max_y = std::max(max_y, y + 1u);
        }
    }
    if (!found) return Json{{"x", 0}, {"y", 0}, {"width", 0}, {"height", 0}};
    return Json{{"x", min_x}, {"y", min_y},
                {"width", max_x - min_x}, {"height", max_y - min_y}};
}

uint8_t scale_alpha(uint8_t value, uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * alpha + 127u) / 255u);
}

void blend_fixture(std::vector<uint8_t>& destination, uint32_t destination_width,
                   const Fixture& fixture, int32_t x, int32_t y, float layer_alpha) {
    const uint32_t width = fixture.metadata.at("width").get<uint32_t>();
    const uint32_t height = fixture.metadata.at("height").get<uint32_t>();
    const uint32_t destination_height = static_cast<uint32_t>(destination.size() / 4u / destination_width);
    const uint8_t alpha = static_cast<uint8_t>(std::clamp(layer_alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    for (uint32_t source_y = 0; source_y < height; ++source_y) {
        const int32_t target_y = y + static_cast<int32_t>(source_y);
        if (target_y < 0 || target_y >= static_cast<int32_t>(destination_height)) continue;
        for (uint32_t source_x = 0; source_x < width; ++source_x) {
            const int32_t target_x = x + static_cast<int32_t>(source_x);
            if (target_x < 0 || target_x >= static_cast<int32_t>(destination_width)) continue;
            const uint8_t* source = fixture.pixels.data() +
                (static_cast<size_t>(source_y) * width + source_x) * 4u;
            uint8_t* target = destination.data() +
                (static_cast<size_t>(target_y) * destination_width + target_x) * 4u;
            const uint8_t source_alpha = scale_alpha(source[3], alpha);
            const uint8_t inverse_alpha = static_cast<uint8_t>(255u - source_alpha);
            target[0] = static_cast<uint8_t>(scale_alpha(source[0], alpha) + scale_alpha(target[0], inverse_alpha));
            target[1] = static_cast<uint8_t>(scale_alpha(source[1], alpha) + scale_alpha(target[1], inverse_alpha));
            target[2] = static_cast<uint8_t>(scale_alpha(source[2], alpha) + scale_alpha(target[2], inverse_alpha));
            target[3] = static_cast<uint8_t>(source_alpha + scale_alpha(target[3], inverse_alpha));
        }
    }
}

std::vector<uint8_t> snapshot(sao_ui_compositor_handle_t compositor,
                              uint32_t* width, uint32_t* height) {
    size_t bytes = 0;
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0, width, height, &bytes) ==
            SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> pixels(bytes);
    REQUIRE(sao_ui_compositor_snapshot_bgra(
                compositor, pixels.data(), pixels.size(), width, height, &bytes) == SAO_STATUS_OK);
    REQUIRE(pixels.size() == bytes);
    return pixels;
}

struct Layer {
    Fixture* fixture;
    sao_ui_layer_handle_t handle = nullptr;
    int32_t z_order = 0;
    int32_t x = 0;
    int32_t y = 0;
    float alpha = 1.0f;
};

Layer create_layer(sao_ui_compositor_handle_t compositor, Fixture* fixture) {
    const Json& spec = fixture->metadata.at("layers").at(0);
    Layer layer{};
    layer.fixture = fixture;
    layer.z_order = spec.at("z_order").get<int32_t>();
    layer.x = spec.at("x").get<int32_t>();
    layer.y = spec.at("y").get<int32_t>();
    layer.alpha = spec.at("alpha").get<float>();

    SaoLayerConfig config{};
    config.name_utf8 = spec.at("name").get_ref<const std::string&>().c_str();
    config.x = layer.x;
    config.y = layer.y;
    config.width = fixture->metadata.at("width").get<int32_t>();
    config.height = fixture->metadata.at("height").get<int32_t>();
    config.z_order = layer.z_order;
    config.click_through = true;
    config.bgra_swizzle = true;
    REQUIRE(sao_ui_layer_create(compositor, &config, &layer.handle) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_update_bgra(
                layer.handle, fixture->pixels.data(),
                fixture->metadata.at("width").get<uint32_t>(),
                fixture->metadata.at("height").get<uint32_t>(),
                fixture->metadata.at("stride").get<uint32_t>()) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_set_alpha(layer.handle, layer.alpha) == SAO_STATUS_OK);
    return layer;
}

std::vector<uint8_t> expected_snapshot(std::vector<Layer> layers,
                                       uint32_t width, uint32_t height) {
    std::stable_sort(layers.begin(), layers.end(), [](const Layer& left, const Layer& right) {
        return left.z_order < right.z_order;
    });
    std::vector<uint8_t> expected(static_cast<size_t>(width) * height * 4u, 0u);
    for (const Layer& layer : layers) {
        blend_fixture(expected, width, *layer.fixture, layer.x, layer.y, layer.alpha);
    }
    return expected;
}

const Json kAlertEvents = Json::array({
    {{"event", "alert.render_frame"}, {"title", "MECHANIC ALERT"}},
    {{"event", "compositor.create_layer"}, {"layer", "alert"}, {"z_order", 20}},
    {{"event", "compositor.update_bgra"}, {"layer", "alert"}},
});

const Json kBuffMonEvents = Json::array({
    {{"event", "buffmon.render_base"}, {"row_count", 2}},
    {{"event", "compositor.create_layer"}, {"layer", "buffmon"}, {"z_order", 10}},
    {{"event", "compositor.update_bgra"}, {"layer", "buffmon"}},
});

const Json kSkillFXCaptionEvents = Json::array({
    {{"event", "skillfx.draw_caption"}, {"fixed_now", 1001.0}},
    {{"event", "compositor.create_layer"}, {"layer", "skillfx_caption"}, {"z_order", 30}},
    {{"event", "compositor.update_bgra"}, {"layer", "skillfx_caption"}},
});

const Json kSkillFXSdfEvents = Json::array({
    {{"event", "skillfx.compose_frame_gpu"}, {"fixed_now", 1001.0}},
    {{"event", "compositor.create_layer"}, {"layer", "skillfx_sdf_gl"}, {"z_order", 30}},
    {{"event", "compositor.update_bgra"}, {"layer", "skillfx_sdf_gl"}},
});

}  // namespace

TEST_CASE("Python effects fixtures retain premultiplied geometry and event authority",
          "[ui][parity][effects][ui_parity]") {
    Fixture alert = load_fixture("alert");
    Fixture buffmon = load_fixture("buffmon_base");
    Fixture skillfx_caption = load_fixture("skillfx_caption");

    REQUIRE(alert.metadata.at("source") ==
            "plugins.star_resonance_plugin.panels.sao_gui_alert.AlertOverlay._render_frame");
    REQUIRE(buffmon.metadata.at("source") ==
            "plugins.star_resonance_plugin.panels.sao_gui_buffmon._BuffPanelBase._render_base");
    REQUIRE(skillfx_caption.metadata.at("source") ==
            "plugins.star_resonance_plugin.panels.sao_gui_skillfx.BurstReadyOverlay._draw_caption");
    REQUIRE(alert.metadata.at("event_order") == kAlertEvents);
    REQUIRE(buffmon.metadata.at("event_order") == kBuffMonEvents);
    REQUIRE(skillfx_caption.metadata.at("event_order") == kSkillFXCaptionEvents);

    for (const Fixture* fixture : {&alert, &buffmon, &skillfx_caption}) {
        verify_premultiplied_bgra(*fixture);
        REQUIRE(alpha_bbox(*fixture) == fixture->metadata.at("alpha_bbox"));
    }

    Fixture skillfx_sdf = load_fixture("skillfx_sdf_gl");
    if (skillfx_sdf.metadata.value("headless_status", "") == "skip") {
        REQUIRE_FALSE(std::filesystem::exists(fixture_path("skillfx_sdf_gl", ".bgra")));
        REQUIRE(skillfx_sdf.metadata.at("headless_skip").at("required_live_gate") ==
                "real D3D/GL presentation validation");
    } else {
        verify_premultiplied_bgra(skillfx_sdf);
        REQUIRE(alpha_bbox(skillfx_sdf) == skillfx_sdf.metadata.at("alpha_bbox"));
        REQUIRE(skillfx_sdf.metadata.at("source") ==
                "plugins.star_resonance_plugin.panels.sao_gui_skillfx.BurstReadyOverlay._compose_frame_gpu");
    }
}

TEST_CASE("effects cross the production compositor with exact alpha z order and transit",
          "[ui][parity][effects][ui_parity][compositor]") {
    Fixture alert = load_fixture("alert");
    Fixture buffmon = load_fixture("buffmon_base");
    Fixture skillfx_caption = load_fixture("skillfx_caption");

    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    Layer buffmon_layer = create_layer(compositor, &buffmon);
    Layer alert_layer = create_layer(compositor, &alert);
    Layer skillfx_layer = create_layer(compositor, &skillfx_caption);

    std::array<sao_ui_layer_handle_t, 3> listed{};
    size_t listed_count = 0;
    REQUIRE(sao_ui_compositor_list_layers(compositor, listed.data(), listed.size(), &listed_count) == SAO_STATUS_OK);
    REQUIRE(listed_count == 3);
    REQUIRE(listed[0] == buffmon_layer.handle);
    REQUIRE(listed[1] == alert_layer.handle);
    REQUIRE(listed[2] == skillfx_layer.handle);

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> actual = snapshot(compositor, &width, &height);
    REQUIRE(width == 800);
    REQUIRE(height == 450);
    REQUIRE(actual == expected_snapshot(
        {buffmon_layer, alert_layer, skillfx_layer}, width, height));

    alert_layer.alpha = 0.5f;
    REQUIRE(sao_ui_layer_set_alpha(alert_layer.handle, alert_layer.alpha) == SAO_STATUS_OK);
    actual = snapshot(compositor, &width, &height);
    REQUIRE(actual == expected_snapshot(
        {buffmon_layer, alert_layer, skillfx_layer}, width, height));

    buffmon_layer.z_order = 40;
    REQUIRE(sao_ui_layer_set_z_order(buffmon_layer.handle, buffmon_layer.z_order) == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_list_layers(compositor, listed.data(), listed.size(), &listed_count) == SAO_STATUS_OK);
    REQUIRE(listed[0] == alert_layer.handle);
    REQUIRE(listed[1] == skillfx_layer.handle);
    REQUIRE(listed[2] == buffmon_layer.handle);
    actual = snapshot(compositor, &width, &height);
    REQUIRE(actual == expected_snapshot(
        {buffmon_layer, alert_layer, skillfx_layer}, width, height));

    sao_ui_layer_destroy(skillfx_layer.handle);
    sao_ui_layer_destroy(alert_layer.handle);
    sao_ui_layer_destroy(buffmon_layer.handle);
    sao_ui_compositor_destroy(compositor);
}

TEST_CASE("SkillFX SDF GL authority crosses the production compositor or declares its live gate",
          "[ui][parity][effects][ui_parity][skillfx][compositor]") {
    Fixture skillfx_sdf = load_fixture("skillfx_sdf_gl");
    if (skillfx_sdf.metadata.value("headless_status", "") == "skip") {
        REQUIRE(skillfx_sdf.metadata.at("headless_skip").at("required_live_gate") ==
                "real D3D/GL presentation validation");
        SKIP(skillfx_sdf.metadata.at("headless_skip").at("reason").get<std::string>());
    }

    REQUIRE(skillfx_sdf.metadata.at("source") ==
            "plugins.star_resonance_plugin.panels.sao_gui_skillfx.BurstReadyOverlay._compose_frame_gpu");
    REQUIRE(skillfx_sdf.metadata.at("event_order") == kSkillFXSdfEvents);
    verify_premultiplied_bgra(skillfx_sdf);
    REQUIRE(alpha_bbox(skillfx_sdf) == skillfx_sdf.metadata.at("alpha_bbox"));

    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);

    Layer layer = create_layer(compositor, &skillfx_sdf);
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> actual = snapshot(compositor, &width, &height);
    REQUIRE(width == skillfx_sdf.metadata.at("width").get<uint32_t>());
    REQUIRE(height == skillfx_sdf.metadata.at("height").get<uint32_t>());
    REQUIRE(actual == skillfx_sdf.pixels);

    layer.alpha = 0.5f;
    REQUIRE(sao_ui_layer_set_alpha(layer.handle, layer.alpha) == SAO_STATUS_OK);
    actual = snapshot(compositor, &width, &height);
    REQUIRE(actual == expected_snapshot({layer}, width, height));

    sao_ui_layer_destroy(layer.handle);
    sao_ui_compositor_destroy(compositor);
}
