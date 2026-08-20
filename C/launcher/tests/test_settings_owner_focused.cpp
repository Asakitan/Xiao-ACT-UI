#include <catch2/catch_test_macros.hpp>



#include "settings_owner_internal.h"
#include "../src/hotkey_config_panel.h"
#include "../src/hotkey_manager.h"
#include "sao/ui/compositor.h"
#include "sao/ui/theme.h"
#include "../src/settings_config_panel.h"
#include "../src/settings_profiles.h"
#include "../src/settings_theme_internal.h"



#include <windows.h>



#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <ranges>

#include <fstream>

#include <memory>

#include <string>
#include <thread>
#include <utility>
#include <vector>



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

struct HotkeyGlobalsGuard {
    ~HotkeyGlobalsGuard() {
        (void)sao::launcher::hotkey::unregister_all();
        sao::launcher::hotkey::clear_callbacks();
        sao::launcher::hotkey::clear_native_hooks_for_testing();
        sao_launcher_hotkey_set_settings_owner(nullptr);
    }
};



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


namespace {

namespace Settings = sao::launcher::settings;

struct SettingsPanelFixture {
    std::filesystem::path root;
    std::filesystem::path settings_path;
    std::unique_ptr<SettingsOwner> owner;
    sao_ui_compositor_handle_t compositor{};

    SettingsPanelFixture() {
        root = std::filesystem::temp_directory_path() /
               (L"sao_settings_panel_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + std::to_wstring(GetTickCount64()));
        settings_path = root / L"settings.v1.dat";
        REQUIRE(std::filesystem::create_directories(root));
        REQUIRE(SettingsOwner::create(settings_path.wstring(), owner) == SAO_STATUS_OK);
        sao::launcher::settings_owner::LoadInfo info{};
        REQUIRE(owner->load(info) == SAO_STATUS_OK);
        REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
        REQUIRE(Settings::set_compositor_for_testing(compositor) == SAO_STATUS_OK);
        REQUIRE(Settings::settings_panel_bind_owner(owner.get()) == SAO_STATUS_OK);
        REQUIRE(Settings::settings_profiles_bind_owner(owner.get()) == SAO_STATUS_OK);
        Settings::set_profiles_directory_for_testing((root / L"profiles").wstring());
    }

    ~SettingsPanelFixture() {
        (void)Settings::take_offline_for_testing();
        (void)Settings::settings_panel_unbind_owner(nullptr);
        (void)Settings::settings_profiles_unbind_owner(nullptr);
        Settings::set_profiles_directory_for_testing(L"");
        if (compositor != nullptr)
            (void)sao_ui_compositor_try_destroy(compositor);
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

} // namespace

TEST_CASE("native settings panel save failure leaves toggle rolled back",
          "[launcher][settings][panel][rollback][focused]") {
    SettingsPanelFixture fixture;
    REQUIRE(fixture.owner->set_value_and_save("streaming_mode", false) == SAO_STATUS_OK);
    Settings::open_config_panel();

    std::error_code error;
    REQUIRE(std::filesystem::remove(fixture.settings_path, error));
    REQUIRE(std::filesystem::remove(fixture.settings_path.parent_path(), error));
    std::ofstream blocker(fixture.settings_path.parent_path(), std::ios::binary);
    REQUIRE(blocker.good());
    blocker << "not-a-directory";
    blocker.close();

    CHECK(Settings::dispatch_action_for_testing(Settings::kSettingsActionToggle,
                                                R"({"key":"streaming_mode"})") != SAO_STATUS_OK);
    bool value = true;
    REQUIRE(fixture.owner->get_truthy("streaming_mode", true, value) == SAO_STATUS_OK);
    CHECK_FALSE(value);
}
TEST_CASE("hotkey manager performs conflict, OS failure, and save rollback transactionally",
          "[launcher][hotkey][transaction][focused]") {
    using sao::launcher::hotkey::HotkeyBinding;
    using sao::launcher::hotkey::NativeHooks;
    using sao::launcher::hotkey::RebindResult;

    TempSettingsPath temp;
    auto owner = create_loaded_owner(temp.settings);
    sao_launcher_hotkey_set_settings_owner(owner.get());
    const std::vector<HotkeyBinding> defaults{
        {"hotkey.one", "One", 'A', MOD_CONTROL},
        {"hotkey.two", "Two", 'B', MOD_CONTROL},
    };
    bool fail_next_register = false;
    int last_error = ERROR_HOTKEY_ALREADY_REGISTERED;
    HotkeyGlobalsGuard globals_guard;
    (void)globals_guard;
    NativeHooks hooks;
    hooks.register_hotkey = [&](int, uint32_t, uint32_t) {
        if (fail_next_register) {
            fail_next_register = false;
            return false;
        }
        return true;
    };
    hooks.unregister_hotkey = [](int) { return true; };
    hooks.last_error = [&] { return static_cast<uint32_t>(last_error); };
    sao::launcher::hotkey::set_native_hooks_for_testing(std::move(hooks));
    sao::launcher::hotkey::load_or_default(defaults);
    CHECK(sao::launcher::hotkey::register_all().empty());

    std::string reason;
    CHECK(sao::launcher::hotkey::rebind_live("hotkey.one", 'B', MOD_CONTROL, &reason) ==
          RebindResult::conflict);
    CHECK(reason == "hotkey.two");

    CHECK(sao::launcher::hotkey::rebind_live("hotkey.one", 'C', MOD_CONTROL, &reason) ==
          RebindResult::success);
    REQUIRE(sao::launcher::hotkey::query_binding("hotkey.one").has_value());
    CHECK(sao::launcher::hotkey::query_binding("hotkey.one")->vk == 'C');

    fail_next_register = true;
    CHECK(sao::launcher::hotkey::rebind_live("hotkey.one", 'D', MOD_CONTROL, &reason) ==
          RebindResult::system_error);
    CHECK(sao::launcher::hotkey::query_binding("hotkey.one")->vk == 'C');
    CHECK(reason.find("RegisterHotKey failed") != std::string::npos);

    TempSettingsPath blocked(L"blocked\\settings.v1.dat");
    auto blocked_owner = create_loaded_owner(blocked.settings);
    std::error_code error;
    std::filesystem::remove_all(blocked.settings.parent_path(), error);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(std::filesystem::exists(blocked.settings.parent_path()));
    std::ofstream blocker(blocked.settings.parent_path(), std::ios::binary);
    REQUIRE(blocker.good());
    blocker << "not-a-directory";
    blocker.close();
    sao_launcher_hotkey_set_settings_owner(blocked_owner.get());
    CHECK(sao::launcher::hotkey::rebind_live("hotkey.one", 'E', MOD_CONTROL, &reason) ==
          RebindResult::save_error);
    CHECK(sao::launcher::hotkey::query_binding("hotkey.one")->vk == 'C');
    CHECK(reason.find("settings save failed") != std::string::npos);

    sao::launcher::hotkey::unregister_all();
    sao::launcher::hotkey::clear_native_hooks_for_testing();
    sao_launcher_hotkey_set_settings_owner(nullptr);
}

TEST_CASE("hotkey manager reads object schema and dispatches registered native ids",
          "[launcher][hotkey][schema][dispatch][focused]") {
    using sao::launcher::hotkey::HotkeyBinding;
    using sao::launcher::hotkey::NativeHooks;
    TempSettingsPath temp;
    auto owner = create_loaded_owner(temp.settings);
    sao_launcher_hotkey_set_settings_owner(owner.get());
    REQUIRE(owner->set_value("hotkeys", Json{{"hotkey.one", Json{{"vk", 'Q'}, {"mods", MOD_SHIFT}}}}) == SAO_STATUS_OK);
    std::vector<int> native_ids;
    HotkeyGlobalsGuard globals_guard;
    (void)globals_guard;
    sao::launcher::hotkey::set_native_hooks_for_testing({
        [&](int id, uint32_t, uint32_t) { native_ids.push_back(id); return true; },
        [](int) { return true; },
        [] { return 0u; },
    });
    sao::launcher::hotkey::load_or_default({{"hotkey.one", "One", 'A', 0}});
    REQUIRE(sao::launcher::hotkey::query_binding("hotkey.one").has_value());
    CHECK(sao::launcher::hotkey::query_binding("hotkey.one")->vk == 'Q');
    REQUIRE(sao::launcher::hotkey::register_all().empty());
    bool called = false;
    sao::launcher::hotkey::set_callback("hotkey.one", [&] { called = true; });
    REQUIRE_FALSE(native_ids.empty());
    CHECK(sao::launcher::hotkey::dispatch_by_native_id(native_ids.front()));
    CHECK(called);
    Json saved;
    REQUIRE(owner->get_value("hotkeys", saved) == SAO_STATUS_OK);
    CHECK(saved.is_object());
    CHECK(saved["hotkey.one"]["vk"] == 'Q');
    CHECK(saved["hotkey.one"]["mods"] == MOD_SHIFT);
    CHECK(sao::launcher::hotkey::unregister_all().empty());
    sao::launcher::hotkey::clear_callbacks();
    sao::launcher::hotkey::clear_native_hooks_for_testing();
    sao_launcher_hotkey_set_settings_owner(nullptr);
}

TEST_CASE("hotkey manager preserves native maps when unregister fails",
          "[launcher][hotkey][unregister][focused]") {
    using sao::launcher::hotkey::NativeHooks;
    TempSettingsPath temp;
    auto owner = create_loaded_owner(temp.settings);
    sao_launcher_hotkey_set_settings_owner(owner.get());
    std::vector<int> native_ids;
    bool fail_unregister = false;
    HotkeyGlobalsGuard globals_guard;
    (void)globals_guard;
    sao::launcher::hotkey::set_native_hooks_for_testing({
        [&](int id, uint32_t, uint32_t) { native_ids.push_back(id); return true; },
        [&](int id) { return !fail_unregister || native_ids.empty() || id != native_ids.front(); },
        [] { return ERROR_HOTKEY_NOT_REGISTERED; },
    });
    sao::launcher::hotkey::load_or_default({{"hotkey.one", "One", 'A', 0}, {"hotkey.two", "Two", 'B', 0}});
    REQUIRE(sao::launcher::hotkey::register_all().empty());
    bool called = false;
    sao::launcher::hotkey::set_callback("hotkey.one", [&] { called = true; });
    fail_unregister = true;
    CHECK_FALSE(sao::launcher::hotkey::unregister_all().empty());
    CHECK(sao::launcher::hotkey::dispatch_by_native_id(native_ids.front()));
    CHECK(called);
    fail_unregister = false;
    CHECK(sao::launcher::hotkey::unregister_all().empty());
    sao::launcher::hotkey::clear_callbacks();
    sao::launcher::hotkey::clear_native_hooks_for_testing();
    sao_launcher_hotkey_set_settings_owner(nullptr);
}

TEST_CASE("settings panel and profile bindings teardown before owner release",
          "[launcher][settings][panel][profiles][teardown][focused]") {
    SettingsPanelFixture fixture;
    Settings::open_config_panel();
    REQUIRE(Settings::take_offline_for_testing() == SAO_STATUS_OK);
    REQUIRE(Settings::settings_panel_unbind_owner(fixture.owner.get()) == SAO_STATUS_OK);
    REQUIRE(Settings::settings_profiles_unbind_owner(fixture.owner.get()) == SAO_STATUS_OK);
}

TEST_CASE("hotkey panel handler restore failure remains retryable", "[launcher][hotkey][teardown][rollback][focused]") {
    sao_ui_compositor_handle_t compositor = nullptr;
    REQUIRE(sao_ui_compositor_create(nullptr, nullptr, &compositor) == SAO_STATUS_OK);
    sao::launcher::hotkey::Owner owner(compositor);
    REQUIRE(owner.open() == SAO_STATUS_OK);
    owner.fail_next_unregister_for_testing(SAO_STATUS_ERR_OS_CALL_FAILED);
    owner.fail_next_handler_restore_for_testing(SAO_STATUS_ERR_OS_CALL_FAILED, SAO_STATUS_OK);
    CHECK(owner.take_offline() != SAO_STATUS_OK);
    REQUIRE(owner.take_offline() == SAO_STATUS_OK);
    REQUIRE(sao_ui_compositor_try_destroy(compositor) == SAO_STATUS_OK);
}


