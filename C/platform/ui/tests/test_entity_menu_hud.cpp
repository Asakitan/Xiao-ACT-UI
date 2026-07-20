#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "sao/core/status.h"
#include "sao/ui/entity_shell.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef SAO_UI_TEST_FIXTURE_DIR
#define SAO_UI_TEST_FIXTURE_DIR ""
#endif

#ifndef SAO_UI_TEST_ASSET_DIR
#define SAO_UI_TEST_ASSET_DIR ""
#endif

namespace {

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kMenuWidth = 442;
constexpr uint32_t kMenuHeight = 430;
constexpr uint32_t kMenuStride = 1768;
constexpr size_t kMenuBytes = 760240;
constexpr int32_t kMenuPad = 40;
constexpr int32_t kMenuSlot = 70;
constexpr int32_t kMenuColumnLeft = 40;
constexpr int32_t kMenuColumnRight = 110;
constexpr std::string_view kReviewedManifestSha256 =
    "fc9c69d9eab08b2e42ff929b0b513a86ed8d9cbfb3610f4672fd8eb02f34b003";

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::filesystem::path metadata_path() {
    return std::filesystem::path(SAO_UI_TEST_FIXTURE_DIR) / "entity_menu_hud.json";
}

nlohmann::json read_metadata() {
    const auto bytes = read_bytes(metadata_path());
    return nlohmann::json::parse(bytes.begin(), bytes.end());
}

std::vector<uint8_t> read_asset(const std::string& filename) {
    return read_bytes(std::filesystem::path(SAO_UI_TEST_ASSET_DIR) / filename);
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
    REQUIRE(BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()),
                           0) == 0);
    REQUIRE(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0);
    REQUIRE(BCryptDestroyHash(hash) == 0);
    REQUIRE(BCryptCloseAlgorithmProvider(algorithm, 0) == 0);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const uint8_t byte : digest) {
        output << std::setw(2) << static_cast<unsigned>(byte);
    }
    return output.str();
}

const nlohmann::json& find_frame(const nlohmann::json& metadata, std::string_view name) {
    const auto& frames = metadata.at("frames");
    const auto found =
        std::find_if(frames.begin(), frames.end(), [name](const nlohmann::json& frame) {
            return frame.at("name").get<std::string>() == name;
        });
    REQUIRE(found != frames.end());
    return *found;
}

uint64_t alpha_sum(const std::vector<uint8_t>& pixels) {
    uint64_t sum = 0;
    for (size_t offset = 3; offset < pixels.size(); offset += 4) {
        sum += pixels[offset];
    }
    return sum;
}

void require_premultiplied(const std::vector<uint8_t>& pixels) {
    REQUIRE(pixels.size() % 4U == 0U);
    bool valid = true;
    for (size_t offset = 0; offset < pixels.size(); offset += 4U) {
        const uint8_t alpha = pixels[offset + 3U];
        valid = valid && pixels[offset] <= alpha && pixels[offset + 1U] <= alpha &&
                pixels[offset + 2U] <= alpha;
    }
    CHECK(valid);
}

struct ShellFixture {
    sao_ui_entity_shell_handle_t shell{};
    SaoUiEntityShellSnapshot snapshot{};

    ShellFixture() {
        SaoUiEntityShellConfig config{};
        config.width = 640;
        config.height = 720;
        config.origin_x = 100;
        config.origin_y = 200;
        REQUIRE(sao_ui_entity_shell_create(nullptr, &config, &shell) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_bring_online(shell) == SAO_STATUS_OK);
        refresh();
    }

    ~ShellFixture() {
        if (shell != nullptr) {
            (void)sao_ui_entity_shell_take_offline(shell);
            sao_ui_entity_shell_destroy(shell);
        }
    }

    void refresh() {
        REQUIRE(sao_ui_entity_shell_get_snapshot(shell, &snapshot) == SAO_STATUS_OK);
    }

    std::vector<uint8_t> pixels(uint32_t* width_out = nullptr,
                                uint32_t* height_out = nullptr) const {
        uint32_t width = 0;
        uint32_t height = 0;
        size_t required = 0;
        REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, nullptr, 0, &width, &height, &required) ==
                SAO_STATUS_ERR_BUFFER_TOO_SMALL);
        std::vector<uint8_t> result(required);
        REQUIRE(sao_ui_entity_shell_snapshot_bgra(shell, result.data(), result.size(), &width,
                                                  &height, &required) == SAO_STATUS_OK);
        if (width_out != nullptr)
            *width_out = width;
        if (height_out != nullptr)
            *height_out = height;
        return result;
    }

    int32_t nervegear_center_x() const {
        return snapshot.origin_x + snapshot.nervegear_x + SAO_UI_NERVEGEAR_SIZE / 2;
    }

    int32_t nervegear_center_y() const {
        return snapshot.origin_y + snapshot.nervegear_y + SAO_UI_NERVEGEAR_SIZE / 2;
    }

    int32_t menu_screen_x(int32_t local_x) const {
        return snapshot.origin_x + snapshot.menu_x + local_x;
    }

    int32_t menu_screen_y(int32_t local_y) const {
        return snapshot.origin_y + snapshot.menu_y + local_y;
    }

    void open_menu() {
        REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, nervegear_center_x(),
                                                 nervegear_center_y(), -1, 0) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonDown, nervegear_center_x(),
                                                 nervegear_center_y(), 0, 0) == SAO_STATUS_OK);
        REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kLeftButtonUp, nervegear_center_x(),
                                                 nervegear_center_y(), 0, 0) == SAO_STATUS_OK);
        refresh();
        REQUIRE(snapshot.menu_visible);
    }

    void move_menu(int32_t local_x, int32_t local_y) {
        REQUIRE(sao_ui_entity_shell_handle_mouse(shell, kMouseMove, menu_screen_x(local_x),
                                                 menu_screen_y(local_y), -1, 0) == SAO_STATUS_OK);
        refresh();
    }
};

std::vector<uint8_t> menu_crop(const ShellFixture& fixture) {
    uint32_t composed_width = 0;
    uint32_t composed_height = 0;
    const auto composed = fixture.pixels(&composed_width, &composed_height);
    REQUIRE(fixture.snapshot.menu_width == static_cast<int32_t>(kMenuWidth));
    REQUIRE(fixture.snapshot.menu_height == static_cast<int32_t>(kMenuHeight));
    REQUIRE(fixture.snapshot.menu_x >= 0);
    REQUIRE(fixture.snapshot.menu_y >= 0);
    REQUIRE(static_cast<uint32_t>(fixture.snapshot.menu_x) + kMenuWidth <= composed_width);
    REQUIRE(static_cast<uint32_t>(fixture.snapshot.menu_y) + kMenuHeight <= composed_height);

    std::vector<uint8_t> crop(kMenuBytes);
    for (uint32_t row = 0; row < kMenuHeight; ++row) {
        const auto source =
            composed.begin() +
            (static_cast<size_t>(fixture.snapshot.menu_y + static_cast<int32_t>(row)) *
                 composed_width +
             static_cast<uint32_t>(fixture.snapshot.menu_x)) *
                4U;
        std::copy_n(source, kMenuStride, crop.begin() + static_cast<size_t>(row) * kMenuStride);
    }
    return crop;
}

void require_canonical_crop(const ShellFixture& fixture, const nlohmann::json& frame) {
    const auto expected = read_asset(frame.at("file").get<std::string>());
    const auto actual = menu_crop(fixture);
    REQUIRE(actual == expected);
}

} // namespace

TEST_CASE("Entity menu HUD resources retain reviewed Python authority",
          "[ui][entity_menu_hud][python_authority][resource_integrity]") {
    const auto manifest_bytes = read_bytes(metadata_path());
    REQUIRE(sha256_hex(manifest_bytes) == kReviewedManifestSha256);
    const auto metadata = nlohmann::json::parse(manifest_bytes.begin(), manifest_bytes.end());
    REQUIRE(metadata.at("schema") == "sao.ui.python-authority.w20.entity-menu-hud.v1");
    REQUIRE(metadata.at("format") == "premultiplied-bgra");
    CHECK(metadata.at("dimensions").at("width") == kMenuWidth);
    CHECK(metadata.at("dimensions").at("height") == kMenuHeight);
    CHECK(metadata.at("dimensions").at("stride") == kMenuStride);
    CHECK(metadata.at("dimensions").at("bytes") == kMenuBytes);
    CHECK(metadata.at("frozen_environment").at("datetime") == "2026-07-14T12:34:56");
    CHECK(metadata.at("frozen_environment").at("screen") == nlohmann::json::array({1920, 1080}));
    CHECK(metadata.at("frozen_environment").at("hud_phase") == 0.375);
    CHECK(metadata.at("frozen_environment").at("fade_alpha") == 1.0);
    CHECK(metadata.at("menu").at("max_slots") == 9);
    CHECK(metadata.at("menu").at("item_count") == 5);
    CHECK(metadata.at("menu").at("child_rows").empty());

    const auto& frames = metadata.at("frames");
    REQUIRE(frames.size() == 6U);
    for (const auto& frame : frames) {
        INFO("frame=" << frame.at("name").get<std::string>());
        CHECK(frame.at("width") == kMenuWidth);
        CHECK(frame.at("height") == kMenuHeight);
        CHECK(frame.at("stride") == kMenuStride);
        CHECK(frame.at("bytes") == kMenuBytes);
        CHECK(frame.at("asset").get<std::string>() ==
              "assets/entity/" + frame.at("file").get<std::string>());
        const auto pixels = read_asset(frame.at("file").get<std::string>());
        REQUIRE(pixels.size() == kMenuBytes);
        CHECK(sha256_hex(pixels) == frame.at("sha256").get<std::string>());
        require_premultiplied(pixels);
    }

    const auto& idle = find_frame(metadata, "idle").at("state");
    CHECK(idle.at("btn_size") == nlohmann::json::array({54.0, 54.0, 54.0, 54.0, 54.0}));
    CHECK(idle.at("btn_hover_t") == nlohmann::json::array({0.0, 0.0, 0.0, 0.0, 0.0}));
    CHECK(idle.at("hover").is_null());
    CHECK(idle.at("active").is_null());
    for (int32_t index = 0; index < 5; ++index) {
        const auto& state = find_frame(metadata, "hover_" + std::to_string(index)).at("state");
        CHECK(state.at("hover") == index);
        CHECK(state.at("active").is_null());
        for (int32_t button = 0; button < 5; ++button) {
            CHECK(state.at("btn_size").at(button) == (button == index ? 70.0 : 54.0));
            CHECK(state.at("btn_hover_t").at(button) == (button == index ? 1.0 : 0.0));
        }
    }
}

TEST_CASE("Entity public composition selects every canonical menu frame",
          "[ui][entity_menu_hud][python_authority][production_composition]") {
    const auto metadata = read_metadata();
    ShellFixture fixture;
    fixture.open_menu();

    const auto opening = menu_crop(fixture);
    const auto canonical_idle =
        read_asset(find_frame(metadata, "idle").at("file").get<std::string>());
    CHECK(alpha_sum(opening) > 0U);
    CHECK(alpha_sum(opening) < alpha_sum(canonical_idle));

    REQUIRE(sao_ui_entity_shell_tick(fixture.shell, 450) == SAO_STATUS_OK);
    fixture.refresh();
    CHECK(fixture.snapshot.menu_hover_index == -1);
    require_canonical_crop(fixture, find_frame(metadata, "idle"));

    for (int32_t index = 0; index < 5; ++index) {
        fixture.move_menu(75, 75 + index * kMenuSlot);
        INFO("hover index=" << index);
        CHECK(fixture.snapshot.menu_hover_index == index);
        require_canonical_crop(fixture, find_frame(metadata, "hover_" + std::to_string(index)));
    }

    REQUIRE(sao_ui_entity_shell_handle_mouse(
                fixture.shell, kLeftButtonDown, fixture.menu_screen_x(75),
                fixture.menu_screen_y(75 + 2 * kMenuSlot), 0, 0) == SAO_STATUS_OK);
    fixture.refresh();
    CHECK(fixture.snapshot.menu_pressed_index == 2);
    require_canonical_crop(fixture, find_frame(metadata, "hover_2"));
}

TEST_CASE("Entity root menu uses canonical 70px authority hit slots",
          "[ui][entity_menu_hud][python_authority][production_composition][geometry]") {
    const auto metadata = read_metadata();
    const auto& hit_rects = metadata.at("menu").at("geometry").at("hit_rects");
    REQUIRE(hit_rects.size() == 5U);

    ShellFixture fixture;
    fixture.open_menu();
    REQUIRE(sao_ui_entity_shell_tick(fixture.shell, 450) == SAO_STATUS_OK);
    fixture.refresh();

    for (int32_t index = 0; index < 5; ++index) {
        const int32_t top = kMenuPad + index * kMenuSlot;
        const int32_t bottom = top + kMenuSlot;
        CHECK(hit_rects.at(index).at("rect") ==
              nlohmann::json::array({kMenuColumnLeft, top, kMenuColumnRight, bottom}));
        for (const auto [x, y] : std::array<std::array<int32_t, 2>, 2>{
                 std::array<int32_t, 2>{kMenuColumnLeft, top},
                 std::array<int32_t, 2>{kMenuColumnRight - 1, bottom - 1}}) {
            fixture.move_menu(x, y);
            INFO("slot=" << index << " point=" << x << "," << y);
            CHECK(fixture.snapshot.menu_hover_index == index);
            bool hit = false;
            REQUIRE(sao_ui_entity_shell_hit_test(fixture.shell, fixture.menu_screen_x(x),
                                                 fixture.menu_screen_y(y), &hit) == SAO_STATUS_OK);
            CHECK(hit);
        }
    }

    for (const auto [x, y] : std::array<std::array<int32_t, 2>, 4>{
             std::array<int32_t, 2>{39, 75}, std::array<int32_t, 2>{110, 75},
             std::array<int32_t, 2>{75, 39}, std::array<int32_t, 2>{75, 390}}) {
        fixture.move_menu(x, y);
        INFO("outside point=" << x << "," << y);
        CHECK(fixture.snapshot.menu_hover_index == -1);
        bool hit = true;
        REQUIRE(sao_ui_entity_shell_hit_test(fixture.shell, fixture.menu_screen_x(x),
                                             fixture.menu_screen_y(y), &hit) == SAO_STATUS_OK);
        CHECK_FALSE(hit);
    }
}
