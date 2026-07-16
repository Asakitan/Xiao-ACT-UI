// Wave 15 combat-panel authority fixtures cross the production compositor.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "sao/core/status.h"
#include "sao/ui/compositor.h"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

#ifndef SAO_UI_TEST_FIXTURE_DIR
#  define SAO_UI_TEST_FIXTURE_DIR ""
#endif

namespace {

struct CombatFixture {
    std::string name;
    nlohmann::json metadata;
    std::vector<uint8_t> pixels;
};

SaoCompositorConfig compositor_config() {
    SaoCompositorConfig config{};
    config.target_hz = 60;
    config.enable_temporal_union = true;
    config.enable_rgn_cache = true;
    return config;
}

std::filesystem::path fixture_path(const std::string& name, const char* extension) {
    return std::filesystem::path(SAO_UI_TEST_FIXTURE_DIR) / "wave15_combat" /
        (name + extension);
}

CombatFixture read_fixture(const std::string& name) {
    std::ifstream metadata_input(fixture_path(name, ".json"));
    REQUIRE(metadata_input.good());

    CombatFixture fixture{};
    fixture.name = name;
    REQUIRE_NOTHROW(metadata_input >> fixture.metadata);

    std::ifstream pixel_input(fixture_path(name, ".bgra"), std::ios::binary);
    REQUIRE(pixel_input.good());
    fixture.pixels.assign(std::istreambuf_iterator<char>(pixel_input), {});
    return fixture;
}

std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    REQUIRE(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0);

    DWORD object_bytes = 0;
    DWORD result_bytes = 0;
    REQUIRE(BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes),
                              &result_bytes, 0) == 0);
    std::vector<uint8_t> hash_object(object_bytes);
    std::array<uint8_t, 32> digest{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    REQUIRE(BCryptCreateHash(algorithm, &hash, hash_object.data(),
                             static_cast<ULONG>(hash_object.size()), nullptr, 0, 0) == 0);
    REQUIRE(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()),
                           static_cast<ULONG>(bytes.size()), 0) == 0);
    REQUIRE(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0);
    REQUIRE(BCryptDestroyHash(hash) == 0);
    REQUIRE(BCryptCloseAlgorithmProvider(algorithm, 0) == 0);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (uint8_t byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

std::array<int, 4> alpha_bbox(const std::vector<uint8_t>& pixels, uint32_t width,
                              uint32_t height) {
    int left = static_cast<int>(width);
    int top = static_cast<int>(height);
    int right = 0;
    int bottom = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            if (pixels[(static_cast<size_t>(y) * width + x) * 4u + 3u] == 0u) {
                continue;
            }
            left = std::min(left, static_cast<int>(x));
            top = std::min(top, static_cast<int>(y));
            right = std::max(right, static_cast<int>(x) + 1);
            bottom = std::max(bottom, static_cast<int>(y) + 1);
        }
    }
    return right == 0 ? std::array<int, 4>{0, 0, 0, 0}
                      : std::array<int, 4>{left, top, right, bottom};
}

std::vector<uint8_t> snapshot(sao_ui_compositor_handle_t compositor, uint32_t* width,
                              uint32_t* height) {
    size_t bytes = 0;
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, nullptr, 0, width, height,
                                             &bytes) == SAO_STATUS_ERR_BUFFER_TOO_SMALL);
    std::vector<uint8_t> result(bytes);
    REQUIRE(sao_ui_compositor_snapshot_bgra(compositor, result.data(), result.size(), width,
                                             height, &bytes) == SAO_STATUS_OK);
    REQUIRE(result.size() == bytes);
    return result;
}

sao_ui_layer_handle_t upload_fixture(sao_ui_compositor_handle_t compositor,
                                     const CombatFixture& fixture) {
    const auto& layer_metadata = fixture.metadata.at("layers").at(0);
    SaoLayerConfig definition{};
    definition.name_utf8 = layer_metadata.at("id").get_ref<const std::string&>().c_str();
    definition.x = layer_metadata.at("x").get<int32_t>();
    definition.y = layer_metadata.at("y").get<int32_t>();
    definition.width = layer_metadata.at("width").get<int32_t>();
    definition.height = layer_metadata.at("height").get<int32_t>();
    definition.z_order = layer_metadata.at("z_order").get<int32_t>();
    definition.bgra_swizzle = true;
    definition.click_through = true;

    sao_ui_layer_handle_t layer = nullptr;
    REQUIRE(sao_ui_layer_create(compositor, &definition, &layer) == SAO_STATUS_OK);
    REQUIRE(sao_ui_layer_update_bgra(
                layer, fixture.pixels.data(), fixture.metadata.at("width").get<uint32_t>(),
                fixture.metadata.at("height").get<uint32_t>(),
                fixture.metadata.at("stride").get<uint32_t>()) == SAO_STATUS_OK);
    return layer;
}

uint8_t scale_alpha(uint8_t value, uint8_t alpha) {
    return static_cast<uint8_t>((static_cast<uint32_t>(value) * alpha + 127u) / 255u);
}

std::array<uint8_t, 4> source_over(const std::array<uint8_t, 4>& bottom,
                                   const std::array<uint8_t, 4>& top) {
    const uint8_t inverse_alpha = static_cast<uint8_t>(255u - top[3]);
    return {
        static_cast<uint8_t>(top[0] + scale_alpha(bottom[0], inverse_alpha)),
        static_cast<uint8_t>(top[1] + scale_alpha(bottom[1], inverse_alpha)),
        static_cast<uint8_t>(top[2] + scale_alpha(bottom[2], inverse_alpha)),
        static_cast<uint8_t>(top[3] + scale_alpha(bottom[3], inverse_alpha)),
    };
}

std::array<uint8_t, 4> pixel_at(const std::vector<uint8_t>& pixels, uint32_t width,
                                uint32_t x, uint32_t y) {
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2], pixels[offset + 3]};
}

void require_metadata_format_authority(const CombatFixture& fixture) {
    const auto& metadata = fixture.metadata;
    REQUIRE(metadata.at("schema") == "sao.ui.python-authority.wave15.combat.v1");
    REQUIRE(metadata.at("render_path") == "PIL.compose_frame");
    REQUIRE(metadata.at("pixel_format") == "premultiplied-bgra");
    REQUIRE(metadata.at("format_strings").is_object());
    REQUIRE_FALSE(metadata.at("format_strings").empty());

    const auto& boundaries = metadata.at("number_format_boundaries");
    REQUIRE(boundaries.is_array());
    REQUIRE(boundaries.size() >= 3u);
    double previous_input = -1.0;
    for (const auto& boundary : boundaries) {
        REQUIRE(boundary.at("id").is_string());
        REQUIRE(boundary.at("input").is_number());
        REQUIRE(boundary.at("output").is_string());
        REQUIRE_FALSE(boundary.at("output").get<std::string>().empty());
        REQUIRE(boundary.at("input").get<double>() >= previous_input);
        previous_input = boundary.at("input").get<double>();
    }
}

void require_exact_transit(const std::string& fixture_name) {
    const CombatFixture fixture = read_fixture(fixture_name);
    const auto& metadata = fixture.metadata;
    const auto& layer = metadata.at("layers").at(0);
    const uint32_t width = metadata.at("width").get<uint32_t>();
    const uint32_t height = metadata.at("height").get<uint32_t>();
    const uint32_t stride = metadata.at("stride").get<uint32_t>();
    const int32_t x = layer.at("x").get<int32_t>();
    const int32_t y = layer.at("y").get<int32_t>();

    require_metadata_format_authority(fixture);
    REQUIRE(stride == width * 4u);
    REQUIRE(fixture.pixels.size() == metadata.at("bytes").get<size_t>());
    REQUIRE(fixture.pixels.size() == static_cast<size_t>(stride) * height);
    REQUIRE(sha256_hex(fixture.pixels) == metadata.at("sha256").get<std::string>());

    const std::array<int, 4> expected_bbox = metadata.at("alpha_bbox").get<std::array<int, 4>>();
    REQUIRE(alpha_bbox(fixture.pixels, width, height) == expected_bbox);

    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    sao_ui_layer_handle_t layer_handle = upload_fixture(compositor, fixture);

    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    const std::vector<uint8_t> composed = snapshot(compositor, &snapshot_width, &snapshot_height);
    REQUIRE(snapshot_width == static_cast<uint32_t>(x) + width);
    REQUIRE(snapshot_height == static_cast<uint32_t>(y) + height);

    for (uint32_t row = 0; row < height; ++row) {
        const size_t source_offset = static_cast<size_t>(row) * stride;
        const size_t snapshot_offset =
            (static_cast<size_t>(row + y) * snapshot_width + x) * 4u;
        REQUIRE(std::equal(fixture.pixels.begin() + source_offset,
                           fixture.pixels.begin() + source_offset + stride,
                           composed.begin() + snapshot_offset));
    }

    std::array<int, 4> composed_bbox = alpha_bbox(composed, snapshot_width, snapshot_height);
    REQUIRE(composed_bbox == std::array<int, 4>{
                                 expected_bbox[0] + x, expected_bbox[1] + y,
                                 expected_bbox[2] + x, expected_bbox[3] + y});

    sao_ui_layer_destroy(layer_handle);
    sao_ui_compositor_destroy(compositor);
}

}  // namespace

TEST_CASE("ui parity combat wave15 DPS authority transit and formatting",
          "[ui][parity][combat][wave15]") {
    require_exact_transit("dps_idle");
    require_exact_transit("dps_active");
}

TEST_CASE("ui parity combat wave15 BossHP authority transit and formatting",
          "[ui][parity][combat][wave15]") {
    require_exact_transit("bosshp_idle");
    require_exact_transit("bosshp_active");
}

TEST_CASE("ui parity combat wave15 HP authority transit and formatting",
          "[ui][parity][combat][wave15]") {
    require_exact_transit("hp_idle");
    require_exact_transit("hp_active");
}

TEST_CASE("ui parity combat wave15 production compositor preserves metadata z-order",
          "[ui][parity][combat][wave15][z-order]") {
    const CombatFixture dps = read_fixture("dps_active");
    const CombatFixture hp = read_fixture("hp_active");
    require_metadata_format_authority(dps);
    require_metadata_format_authority(hp);

    SaoCompositorConfig config = compositor_config();
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, &config, &compositor) == SAO_STATUS_OK);
    sao_ui_layer_handle_t dps_layer = upload_fixture(compositor, dps);
    sao_ui_layer_handle_t hp_layer = upload_fixture(compositor, hp);

    std::array<sao_ui_layer_handle_t, 2> layers{};
    size_t layer_count = 0;
    REQUIRE(sao_ui_compositor_list_layers(compositor, layers.data(), layers.size(),
                                          &layer_count) == SAO_STATUS_OK);
    REQUIRE(layer_count == 2u);
    REQUIRE(layers[0] == dps_layer);
    REQUIRE(layers[1] == hp_layer);

    uint32_t snapshot_width = 0;
    uint32_t snapshot_height = 0;
    const std::vector<uint8_t> composed = snapshot(compositor, &snapshot_width, &snapshot_height);
    const auto& dps_layer_metadata = dps.metadata.at("layers").at(0);
    const auto& hp_layer_metadata = hp.metadata.at("layers").at(0);
    const int32_t dps_x = dps_layer_metadata.at("x").get<int32_t>();
    const int32_t dps_y = dps_layer_metadata.at("y").get<int32_t>();
    const int32_t hp_x = hp_layer_metadata.at("x").get<int32_t>();
    const int32_t hp_y = hp_layer_metadata.at("y").get<int32_t>();
    const uint32_t dps_width = dps.metadata.at("width").get<uint32_t>();
    const uint32_t dps_height = dps.metadata.at("height").get<uint32_t>();
    const uint32_t hp_width = hp.metadata.at("width").get<uint32_t>();
    const uint32_t hp_height = hp.metadata.at("height").get<uint32_t>();

    bool asserted_overlap = false;
    for (uint32_t y = 0; y < hp_height && !asserted_overlap; ++y) {
        for (uint32_t x = 0; x < hp_width; ++x) {
            const int32_t global_x = hp_x + static_cast<int32_t>(x);
            const int32_t global_y = hp_y + static_cast<int32_t>(y);
            const int32_t dps_source_x = global_x - dps_x;
            const int32_t dps_source_y = global_y - dps_y;
            if (dps_source_x < 0 || dps_source_y < 0 ||
                dps_source_x >= static_cast<int32_t>(dps_width) ||
                dps_source_y >= static_cast<int32_t>(dps_height)) {
                continue;
            }
            const auto bottom = pixel_at(dps.pixels, dps_width,
                                         static_cast<uint32_t>(dps_source_x),
                                         static_cast<uint32_t>(dps_source_y));
            const auto top = pixel_at(hp.pixels, hp_width, x, y);
            if (bottom[3] == 0u || top[3] == 0u) {
                continue;
            }
            REQUIRE(pixel_at(composed, snapshot_width, static_cast<uint32_t>(global_x),
                             static_cast<uint32_t>(global_y)) == source_over(bottom, top));
            asserted_overlap = true;
            break;
        }
    }
    REQUIRE(asserted_overlap);

    sao_ui_layer_destroy(hp_layer);
    sao_ui_layer_destroy(dps_layer);
    sao_ui_compositor_destroy(compositor);
}
