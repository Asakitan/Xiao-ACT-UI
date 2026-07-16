#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "sao/core/status.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/menu.h"

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

extern "C" {
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_tick(sao_ui_menu_handle_t handle, int32_t dt_ms);
SAO_UI_API sao_status_t SAO_UI_CALL sao_ui_menu_get_transition_progress(sao_ui_menu_handle_t handle,
                                                                        float* out_progress);
}

namespace {

constexpr uint32_t kMouseMove = 0x0200;
constexpr uint32_t kLeftButtonDown = 0x0201;
constexpr uint32_t kLeftButtonUp = 0x0202;
constexpr uint32_t kMouseLeave = 0x02A3;

nlohmann::json read_metadata() {
    const std::filesystem::path path =
        std::filesystem::path(SAO_UI_TEST_FIXTURE_DIR) / "w19_entity_parity.json";
    std::ifstream input(path);
    REQUIRE(input.good());
    return nlohmann::json::parse(input);
}

std::vector<uint8_t> read_pixels(const std::string& filename) {
    const std::filesystem::path path = std::filesystem::path(SAO_UI_TEST_ASSET_DIR) / filename;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

    int32_t center_x() const {
        return snapshot.origin_x + snapshot.nervegear_x + SAO_UI_NERVEGEAR_SIZE / 2;
    }

    int32_t center_y() const {
        return snapshot.origin_y + snapshot.nervegear_y + SAO_UI_NERVEGEAR_SIZE / 2;
    }
};

void require_authority_crop(const ShellFixture& fixture, const nlohmann::json& frame) {
    const uint32_t frame_width = frame.at("width").get<uint32_t>();
    const uint32_t frame_height = frame.at("height").get<uint32_t>();
    const uint32_t frame_stride = frame.at("stride").get<uint32_t>();
    REQUIRE(frame_width == SAO_UI_NERVEGEAR_SIZE);
    REQUIRE(frame_height == SAO_UI_NERVEGEAR_SIZE);
    REQUIRE(frame_stride == frame_width * 4U);
    REQUIRE(frame.at("asset").get<std::string>() ==
            "assets/entity/" + frame.at("file").get<std::string>());

    const auto expected = read_pixels(frame.at("file").get<std::string>());
    REQUIRE(expected.size() == frame.at("bytes").get<size_t>());
    REQUIRE(expected.size() == static_cast<size_t>(frame_width) * frame_height * 4U);
    REQUIRE(sha256_hex(expected) == frame.at("sha256").get<std::string>());
    uint32_t composed_width = 0;
    const auto composed = fixture.pixels(&composed_width);
    for (uint32_t row = 0; row < frame_height; ++row) {
        const auto source = expected.begin() + static_cast<size_t>(row) * frame_stride;
        const auto target =
            composed.begin() +
            (static_cast<size_t>(fixture.snapshot.nervegear_y + row) * composed_width +
             static_cast<uint32_t>(fixture.snapshot.nervegear_x)) *
                4U;
        REQUIRE(std::equal(source, source + frame_stride, target));
    }
}

uint64_t menu_alpha_sum(const ShellFixture& fixture) {
    uint32_t composed_width = 0;
    uint32_t composed_height = 0;
    const auto composed = fixture.pixels(&composed_width, &composed_height);
    REQUIRE(fixture.snapshot.menu_x >= 0);
    REQUIRE(fixture.snapshot.menu_y >= 0);
    REQUIRE(static_cast<uint32_t>(fixture.snapshot.menu_x + fixture.snapshot.menu_width) <=
            composed_width);
    REQUIRE(static_cast<uint32_t>(fixture.snapshot.menu_y + fixture.snapshot.menu_height) <=
            composed_height);
    uint64_t total = 0;
    for (int32_t row = 0; row < fixture.snapshot.menu_height; ++row) {
        for (int32_t column = 0; column < fixture.snapshot.menu_width; ++column) {
            const size_t alpha_index =
                (static_cast<size_t>(fixture.snapshot.menu_y + row) * composed_width +
                 static_cast<uint32_t>(fixture.snapshot.menu_x + column)) *
                    4U +
                3U;
            total += composed[alpha_index];
        }
    }
    return total;
}

std::string phase_name(SaoUiMenuPhase phase) {
    switch (phase) {
    case SAO_UI_MENU_PHASE_CLOSED:
        return "closed";
    case SAO_UI_MENU_PHASE_OPENING:
        return "opening";
    case SAO_UI_MENU_PHASE_OPEN:
        return "open";
    case SAO_UI_MENU_PHASE_CHILD_OPENING:
        return "child_opening";
    case SAO_UI_MENU_PHASE_CHILD_OPEN:
        return "child_open";
    case SAO_UI_MENU_PHASE_CHILD_CLOSING:
        return "child_closing";
    case SAO_UI_MENU_PHASE_CLOSING:
        return "closing";
    }
    return "unknown";
}

std::string nervegear_state_name(SaoUiNerveGearState state) {
    switch (state) {
    case SAO_UI_NG_STATE_IDLE:
        return "idle";
    case SAO_UI_NG_STATE_HOVER:
        return "hover";
    case SAO_UI_NG_STATE_PRESSED:
        return "pressed";
    case SAO_UI_NG_STATE_DRAGGING:
        return "dragging";
    case SAO_UI_NG_STATE_LINKING:
        return "linking";
    case SAO_UI_NG_STATE_LINKED:
        return "linked";
    case SAO_UI_NG_STATE_LOGOUT:
        return "logout";
    }
    return "unknown";
}

sao_ui_menu_handle_t make_menu() {
    sao_ui_menu_handle_t menu = nullptr;
    REQUIRE(sao_ui_menu_create(nullptr, nullptr, SAO_UI_MENU_MODE_VERTICAL_STRIP, &menu) ==
            SAO_STATUS_OK);
    const SaoUiMenuItem item{"About", "?", 1, true, {false, false, false}};
    REQUIRE(sao_ui_menu_set_items(menu, &item, 1) == SAO_STATUS_OK);
    return menu;
}

void require_phase_checkpoints(sao_ui_menu_handle_t menu, const nlohmann::json& phase_fixture) {
    const auto& checkpoints = phase_fixture.at("checkpoints");
    const int32_t duration_ms = phase_fixture.at("duration_ms").get<int32_t>();
    REQUIRE(checkpoints.back().at("elapsed_ms").get<int32_t>() == duration_ms);
    int32_t elapsed_ms = 0;
    for (const auto& checkpoint : checkpoints) {
        const int32_t checkpoint_ms = checkpoint.at("elapsed_ms").get<int32_t>();
        REQUIRE(checkpoint_ms >= elapsed_ms);
        REQUIRE(sao_ui_menu_tick(menu, checkpoint_ms - elapsed_ms) == SAO_STATUS_OK);
        elapsed_ms = checkpoint_ms;

        SaoUiMenuPhase phase = SAO_UI_MENU_PHASE_CLOSED;
        REQUIRE(sao_ui_menu_get_phase(menu, &phase) == SAO_STATUS_OK);
        CHECK(phase_name(phase) == checkpoint.at("phase").get<std::string>());
        float progress = -1.0F;
        REQUIRE(sao_ui_menu_get_transition_progress(menu, &progress) == SAO_STATUS_OK);
        CHECK(progress == Catch::Approx(checkpoint.at("alpha").get<float>()).margin(0.00001F));
        CHECK(checkpoint.at("done").get<bool>() ==
              (phase == SAO_UI_MENU_PHASE_OPEN || phase == SAO_UI_MENU_PHASE_CLOSED));
    }
}

} // namespace

TEST_CASE("W19 Entity NerveGear frames match Python authority exactly",
          "[ui][w19][entity][parity][pixel]") {
    const auto metadata = read_metadata();
    REQUIRE(metadata.at("schema") == "sao.ui.python-authority.w19.entity.v1");
    REQUIRE(metadata.at("format") == "premultiplied-bgra");
    CHECK(find_frame(metadata, "idle").at("sha256") ==
          "4b881372a988999416307db106e2be0e4e42a0d8132eecb7b3f66a1e0eae90ff");
    CHECK(find_frame(metadata, "hover").at("sha256") ==
          "850ed70d8baa32f15acde8fb5ea36b928bc33e745d9e5cf7843518028d2ea925");
    CHECK(find_frame(metadata, "pressed").at("sha256") ==
          "8710a3b064c38bd7e5f6e1f3c443121a19dcad7049bdf37a1d5a770bf0688088");

    ShellFixture fixture;
    require_authority_crop(fixture, find_frame(metadata, "idle"));

    REQUIRE(sao_ui_entity_shell_handle_mouse(fixture.shell, kMouseMove, fixture.center_x(),
                                             fixture.center_y(), -1, 0) == SAO_STATUS_OK);
    fixture.refresh();
    require_authority_crop(fixture, find_frame(metadata, "hover"));

    REQUIRE(sao_ui_entity_shell_handle_mouse(fixture.shell, kLeftButtonDown, fixture.center_x(),
                                             fixture.center_y(), 0, 0) == SAO_STATUS_OK);
    fixture.refresh();
    require_authority_crop(fixture, find_frame(metadata, "pressed"));
}

TEST_CASE("W19 Entity menu phase follows Python fade authority",
          "[ui][w19][entity][parity][phase]") {
    const auto metadata = read_metadata();
    sao_ui_menu_handle_t menu = make_menu();
    REQUIRE(sao_ui_menu_show(menu, 40, 40) == SAO_STATUS_OK);
    require_phase_checkpoints(menu, metadata.at("phase").at("opening"));
    REQUIRE(sao_ui_menu_hide(menu) == SAO_STATUS_OK);
    require_phase_checkpoints(menu, metadata.at("phase").at("closing"));
    sao_ui_menu_destroy(menu);
}

TEST_CASE("W19 Entity click trace follows Python interaction authority",
          "[ui][w19][entity][parity][interaction]") {
    const auto metadata = read_metadata();
    const auto& interaction = metadata.at("interaction");
    const auto& events = interaction.at("events");
    const auto& steps = interaction.at("steps");
    REQUIRE(steps.size() == events.size() + 1U);

    ShellFixture fixture;
    uint64_t observed_clicks = 0;
    bool previous_menu_visible = fixture.snapshot.menu_visible;
    for (size_t index = 0; index < steps.size(); ++index) {
        if (index > 0) {
            const auto& event = events.at(index - 1U);
            const std::string op = event.at("op").get<std::string>();
            uint32_t message = 0;
            int32_t button = -1;
            if (op == "mouse_move") {
                message = kMouseMove;
            } else if (op == "left_down") {
                message = kLeftButtonDown;
                button = 0;
            } else if (op == "left_up") {
                message = kLeftButtonUp;
                button = 0;
            } else if (op == "mouse_leave") {
                message = kMouseLeave;
            } else {
                FAIL("unsupported interaction op: " << op);
            }
            INFO("interaction op=" << op);
            const int32_t screen_x = fixture.snapshot.origin_x + fixture.snapshot.nervegear_x +
                                     event.at("x").get<int32_t>();
            const int32_t screen_y = fixture.snapshot.origin_y + fixture.snapshot.nervegear_y +
                                     event.at("y").get<int32_t>();
            REQUIRE(sao_ui_entity_shell_handle_mouse(fixture.shell, message, screen_x, screen_y,
                                                     button, 0) == SAO_STATUS_OK);
            fixture.refresh();
            if (!previous_menu_visible && fixture.snapshot.menu_visible) {
                ++observed_clicks;
            }
            previous_menu_visible = fixture.snapshot.menu_visible;
        }

        const auto& expected = steps.at(index).at("expected");
        CHECK(nervegear_state_name(fixture.snapshot.nervegear_state) ==
              expected.at("nervegear_state").get<std::string>());
        CHECK(fixture.snapshot.menu_visible == expected.at("menu_visible").get<bool>());
        CHECK(observed_clicks == expected.at("click_count").get<uint64_t>());
    }
}

TEST_CASE("W19 Entity menu layer consumes Python fade progress",
          "[ui][w19][entity][parity][phase][integration]") {
    ShellFixture fixture;
    CHECK(menu_alpha_sum(fixture) == 0U);

    REQUIRE(sao_ui_entity_shell_handle_mouse(fixture.shell, kMouseMove, fixture.center_x(),
                                             fixture.center_y(), -1, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(fixture.shell, kLeftButtonDown, fixture.center_x(),
                                             fixture.center_y(), 0, 0) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_handle_mouse(fixture.shell, kLeftButtonUp, fixture.center_x(),
                                             fixture.center_y(), 0, 0) == SAO_STATUS_OK);
    fixture.refresh();
    const uint64_t opening_alpha = menu_alpha_sum(fixture);
    CHECK(opening_alpha > 0U);

    REQUIRE(sao_ui_entity_shell_tick(fixture.shell, 225) == SAO_STATUS_OK);
    fixture.refresh();
    const uint64_t midpoint_alpha = menu_alpha_sum(fixture);
    CHECK(midpoint_alpha > opening_alpha);

    REQUIRE(sao_ui_entity_shell_tick(fixture.shell, 225) == SAO_STATUS_OK);
    fixture.refresh();
    const uint64_t open_alpha = menu_alpha_sum(fixture);
    CHECK(open_alpha > midpoint_alpha);

    REQUIRE(sao_ui_entity_shell_home(fixture.shell) == SAO_STATUS_OK);
    REQUIRE(sao_ui_entity_shell_tick(fixture.shell, 150) == SAO_STATUS_OK);
    fixture.refresh();
    const uint64_t closing_alpha = menu_alpha_sum(fixture);
    CHECK(closing_alpha < open_alpha);
    CHECK(closing_alpha > 0U);

    REQUIRE(sao_ui_entity_shell_tick(fixture.shell, 150) == SAO_STATUS_OK);
    fixture.refresh();
    CHECK(menu_alpha_sum(fixture) == 0U);
}
