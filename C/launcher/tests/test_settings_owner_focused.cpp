#include <catch2/catch_test_macros.hpp>

#include "settings_owner_internal.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

using Json = sao::launcher::settings_owner::Json;
using SettingsOwner = sao::launcher::settings_owner::SettingsOwner;

struct TempSettingsPath {
    std::filesystem::path root;
    std::filesystem::path settings;

    explicit TempSettingsPath(const wchar_t* leaf = L"settings.v1.dat") {
        root = std::filesystem::temp_directory_path() /
               (L"sao_nervgear_settings_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + std::to_wstring(GetTickCount64()));
        settings = root / leaf;
        std::error_code error;
        std::filesystem::remove_all(root, error);
        REQUIRE(std::filesystem::create_directories(root));
    }

    ~TempSettingsPath() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::unique_ptr<SettingsOwner> create_loaded_owner(
    const std::filesystem::path& path) {
    std::unique_ptr<SettingsOwner> owner;
    REQUIRE(SettingsOwner::create(path.wstring(), owner) == SAO_STATUS_OK);
    sao::launcher::settings_owner::LoadInfo info{};
    REQUIRE(owner->load(info) == SAO_STATUS_OK);
    return owner;
}

} // namespace

TEST_CASE("settings owner applies Python truthiness with a missing default",
          "[launcher][settings][nervgear_mode][focused]") {
    TempSettingsPath temp;
    auto owner = create_loaded_owner(temp.settings);

    bool value = false;
    REQUIRE(owner->get_truthy("nervgear_mode", true, value) == SAO_STATUS_OK);
    CHECK(value);
    CHECK(owner->get_truthy("", true, value) == SAO_STATUS_ERR_INVALID_ARGUMENT);

    const auto check = [&](Json input, bool expected) {
        REQUIRE(owner->set_value("nervgear_mode", std::move(input)) ==
                SAO_STATUS_OK);
        REQUIRE(owner->get_truthy("nervgear_mode", !expected, value) ==
                SAO_STATUS_OK);
        CHECK(value == expected);
    };

    check(nullptr, false);
    check(false, false);
    check(true, true);
    check(0, false);
    check(-3, true);
    check(0.0, false);
    check(0.25, true);
    check("", false);
    check("false", true);
    check(Json::array(), false);
    check(Json::array({1}), true);
    check(Json::object(), false);
    check(Json::object({{"enabled", false}}), true);
}

TEST_CASE("settings owner saves NerveGear mode immediately",
          "[launcher][settings][nervgear_mode][focused]") {
    TempSettingsPath temp;
    auto owner = create_loaded_owner(temp.settings);

    REQUIRE(owner->set_value_and_save("nervgear_mode", false) == SAO_STATUS_OK);
    CHECK_FALSE(owner->dirty());
    CHECK(std::filesystem::is_regular_file(temp.settings));

    owner.reset();
    auto reloaded = create_loaded_owner(temp.settings);
    bool value = true;
    REQUIRE(reloaded->get_truthy("nervgear_mode", true, value) == SAO_STATUS_OK);
    CHECK_FALSE(value);
    CHECK_FALSE(reloaded->dirty());
}

TEST_CASE("settings owner restores document and dirty state after save failure",
          "[launcher][settings][nervgear_mode][focused]") {
    TempSettingsPath temp(L"blocked\\settings.v1.dat");
    auto owner = create_loaded_owner(temp.settings);
    REQUIRE(owner->set_value_and_save("existing", 7) == SAO_STATUS_OK);
    REQUIRE_FALSE(owner->dirty());

    Json before;
    REQUIRE(owner->snapshot(before) == SAO_STATUS_OK);
    REQUIRE(before == Json::object({{"existing", 7}}));

    std::error_code error;
    REQUIRE(std::filesystem::remove(temp.settings, error));
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::remove(temp.settings.parent_path(), error));
    REQUIRE_FALSE(error);
    std::ofstream blocker(temp.settings.parent_path(), std::ios::binary);
    REQUIRE(blocker.good());
    blocker << "not-a-directory";
    blocker.close();
    REQUIRE(blocker.good());

    CHECK(owner->set_value_and_save("nervgear_mode", false) != SAO_STATUS_OK);
    Json after;
    REQUIRE(owner->snapshot(after) == SAO_STATUS_OK);
    CHECK(after == before);
    CHECK_FALSE(owner->dirty());
    Json absent;
    CHECK(owner->get_value("nervgear_mode", absent) == SAO_STATUS_ERR_NOT_FOUND);

    REQUIRE(owner->set_value("pending", 9) == SAO_STATUS_OK);
    REQUIRE(owner->dirty());
    Json dirty_before;
    REQUIRE(owner->snapshot(dirty_before) == SAO_STATUS_OK);
    CHECK(owner->set_value_and_save("nervgear_mode", true) != SAO_STATUS_OK);
    Json dirty_after;
    REQUIRE(owner->snapshot(dirty_after) == SAO_STATUS_OK);
    CHECK(dirty_after == dirty_before);
    CHECK(owner->dirty());
}
