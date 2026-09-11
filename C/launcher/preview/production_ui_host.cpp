#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellapi.h>

#include "hotkey_config_panel.h"
#include "hotkey_manager.h"
#include "plugin_manager_panel_internal.h"
#include "process_selector_panel_internal.h"
#include "settings_config_panel.h"
#include "settings_owner_internal.h"
#include "settings_profiles.h"
#include "workshop_panel_internal.h"
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
#include "license_panel_internal.h"
#endif
#include "sao/ai_editor/ai_editor_launcher.h"
#include "sao/ai_editor/ai_editor_main_panel.h"
#include "sao/ai_editor/ai_editor_settings_panel.h"
#include "sao/launcher/user_menu.h"
#include "sao/sdk/sao_sdk_platform_internal.h"
#include "sao/server/freetier/workshop_client/workshop_client.h"
#include "sao/ui/entity_shell.h"
#include "sao/ui/input_router.h"
#include "sao/ui/linkstart_intro.h"
#include "sao/ui/overlay_host.h"
#include "sao/ui/sound.h"
#include "sao/ui/theme.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sao::launcher::settings {
// Production entry point with a status result; the public wrapper is intentionally void.
sao_status_t open_config_panel_status() noexcept;
} // namespace sao::launcher::settings

namespace {
constexpr UINT_PTR kUiService = 1;
constexpr int32_t kOpenLauncherSettings = 1000;
constexpr int32_t kOpenHotkeys = 1001;
constexpr int32_t kOpenAiMain = 1002;
constexpr int32_t kOpenAiSettings = 1003;
constexpr int32_t kOpenPluginManager = 1004;
constexpr int32_t kOpenWorkshop = 1005;
constexpr int32_t kOpenProcessSelector = 1006;
constexpr int32_t kOpenLicense = 1007;
constexpr int32_t kOpenUserMenu = 1008;
constexpr int32_t kOpenAboutGuide = 1009;

void require(sao_status_t status,
             const std::source_location source = std::source_location::current()) {
    if (status != SAO_STATUS_OK)
        throw std::runtime_error("Production UI API status " + std::to_string(status) + " at " +
                                 source.function_name() + ":" + std::to_string(source.line()));
}
void require_sdk(sao_sdk_status_t status) {
    if (status != SAO_SDK_OK)
        throw std::runtime_error("Production SDK API status " + std::to_string(status));
}
void require_active(sao_status_t status,
                    const std::source_location source = std::source_location::current()) {
    if (status != SAO_STATUS_OK && status != SAO_STATUS_ERR_NOT_INITIALIZED)
        require(status, source);
}

enum class InitialSurface {
    root,
    ai_main,
    ai_settings,
    settings,
    hotkeys,
    plugins,
    workshop,
    process,
    license,
    user,
    about
};
struct Options {
    InitialSurface initial_surface{InitialSurface::root};
    bool offline{};
    bool offline_explicit{};
    bool backend_explicit{};
    bool local_backend_fixture{};
    bool intro{};
    bool intro_audition{};
    std::filesystem::path workspace{std::filesystem::current_path()};
    std::filesystem::path backend;
    std::filesystem::path settings;
    std::filesystem::path frame_out;
    int32_t frame_ms{1000};
    bool frame_time_explicit{};
};

std::string utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= static_cast<DWORD>(path.size()))
        throw std::runtime_error("Preview executable path unavailable");
    return std::filesystem::path(std::wstring_view(path.data(), length)).parent_path();
}

std::filesystem::path sibling_executable(std::wstring_view name) {
    return executable_directory() / name;
}

// The Workshop owner remains the production page.  This provider only reads
// the real client configuration and deliberately exposes a detached backend
// state, so opening the page never schedules a network request in the shell.
sao_status_t detached_workshop_status() noexcept {
    char endpoint[512]{};
    size_t written = 0;
    (void)sao_workshop_client_get_base_url(endpoint, std::size(endpoint), &written);
    return SAO_STATUS_ERR_NOT_INITIALIZED;
}

sao::launcher::workshop_panel::Operations detached_workshop_operations() {
    using namespace sao::launcher::workshop_panel;
    Operations operations;
    operations.list = [](std::stop_token stop, std::uint32_t, std::uint32_t, PluginPage& output) {
        output = {};
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.detail = [](std::stop_token stop, std::string_view, PluginDetail& output) {
        output = {};
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.download = [](std::stop_token stop, std::string_view, const std::filesystem::path&,
                             std::filesystem::path& output) {
        output.clear();
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.verify = [](std::stop_token stop, const std::filesystem::path&, std::string_view) {
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.install = [](std::stop_token stop, const std::filesystem::path&,
                            const std::filesystem::path&) {
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    operations.uninstall = [](std::stop_token stop, std::string_view, const std::filesystem::path&,
                              bool) {
        return stop.stop_requested() ? SAO_STATUS_ERR_CANCELLED : detached_workshop_status();
    };
    return operations;
}

Options options() {
    Options value;
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (args == nullptr)
        throw std::runtime_error("Arguments unavailable");
    struct ArgsRelease {
        LPWSTR* value;
        ~ArgsRelease() {
            LocalFree(value);
        }
    } release{args};
    for (int index = 1; index < count; ++index) {
        const std::wstring arg = args[index];
        if (arg == L"--offline") {
            value.offline = true;
            value.offline_explicit = true;
        } else if (arg == L"--ai-main" || arg == L"--main")
            value.initial_surface = InitialSurface::ai_main;
        else if (arg == L"--ai-settings")
            value.initial_surface = InitialSurface::ai_settings;
        else if (arg == L"--page" && index + 1 < count) {
            const std::wstring page = args[++index];
            if (page == L"root")
                value.initial_surface = InitialSurface::root;
            else if (page == L"settings")
                value.initial_surface = InitialSurface::settings;
            else if (page == L"hotkeys")
                value.initial_surface = InitialSurface::hotkeys;
            else if (page == L"plugins")
                value.initial_surface = InitialSurface::plugins;
            else if (page == L"workshop")
                value.initial_surface = InitialSurface::workshop;
            else if (page == L"process")
                value.initial_surface = InitialSurface::process;
            else if (page == L"license")
                value.initial_surface = InitialSurface::license;
            else if (page == L"user")
                value.initial_surface = InitialSurface::user;
            else if (page == L"about" || page == L"guide")
                value.initial_surface = InitialSurface::about;
            else if (page == L"ai-main")
                value.initial_surface = InitialSurface::ai_main;
            else if (page == L"ai-settings")
                value.initial_surface = InitialSurface::ai_settings;
            else if (page == L"link-start") {
                value.initial_surface = InitialSurface::root;
                value.intro = true;
            } else
                throw std::runtime_error("Unknown production UI page");
        } else if (arg == L"--intro")
            value.intro = true;
        else if (arg == L"--intro-audition") {
            value.intro = true;
            value.intro_audition = true;
            value.offline = true;
            value.offline_explicit = true;
        }
        else if (arg == L"--workspace" && index + 1 < count)
            value.workspace = std::filesystem::absolute(args[++index]);
        else if (arg == L"--settings" && index + 1 < count)
            value.settings = std::filesystem::absolute(args[++index]);
        else if (arg == L"--frame-out" && index + 1 < count)
            value.frame_out = std::filesystem::absolute(args[++index]);
        else if (arg == L"--frame-ms" && index + 1 < count) {
            const std::wstring input = args[++index];
            size_t consumed = 0;
            const long long milliseconds = std::stoll(input, &consumed);
            if (consumed != input.size() || milliseconds < 0 || milliseconds > 60000)
                throw std::runtime_error("Frame time must be 0..60000 milliseconds");
            value.frame_ms = static_cast<int32_t>(milliseconds);
            value.frame_time_explicit = true;
        }
        else if (arg == L"--backend" && index + 1 < count) {
            if (value.local_backend_fixture)
                throw std::runtime_error("Preview backend selection is ambiguous");
            value.backend = std::filesystem::absolute(args[++index]);
            value.backend_explicit = true;
        } else if (arg == L"--local-backend") {
            if (value.backend_explicit)
                throw std::runtime_error("Preview backend selection is ambiguous");
            value.backend_explicit = true;
            value.local_backend_fixture = true;
        } else
            throw std::runtime_error(
                "Usage: sao_ui_preview [--main|--ai-main|--ai-settings] [--page "
                "root|settings|hotkeys|plugins|workshop|process|license|user|about|link-start] "
                "[--offline] [--backend EXE|--local-backend] [--intro|--intro-audition] [--workspace PATH] "
                "[--settings PATH] [--frame-out BMP_PATH --frame-ms MILLISECONDS --offline]");
    }
    if (value.backend_explicit && !value.offline_explicit)
        value.offline = false;
    if ((!value.frame_out.empty() && !value.offline) ||
        (value.frame_time_explicit && value.frame_out.empty()))
        throw std::runtime_error("Frame export requires --offline and --frame-out");
    if (value.intro_audition && !value.frame_out.empty())
        throw std::runtime_error("Audition and muted frame export are separate modes");
    return value;
}

// One production DComp/compositor owner. The shell intentionally does not run
// the launcher pipeline, driver, or plugin discovery; AI backend launch occurs
// only when the caller explicitly supplies --backend without --offline.
struct Host {
    Options config;
    std::filesystem::path base_dir;
    HWND window{};
    sao_ui_overlay_host_handle_t overlay{};
    sao_ui_compositor_handle_t compositor{};
    sao_ui_entity_shell_handle_t entity{};
    sao_ui_input_router_deep_handle_t keyboard{};
    sao_ui_linkstart_handle_t intro{};
    sao_ai_editor_settings_panel_t ai_settings{};
    sao_ai_editor_main_panel_t ai_main{};
    sao_ai_editor_launcher_t backend{};
    std::unique_ptr<sao::launcher::settings_owner::SettingsOwner> settings_owner;
    std::unique_ptr<sao::launcher::hotkey::Owner> hotkey_owner;
    std::unique_ptr<sao::launcher::plugin_manager_panel::Owner> plugin_manager;
    std::unique_ptr<sao::launcher::process_selector_panel::Owner> process_selector;
    std::unique_ptr<sao::launcher::workshop_panel::Owner> workshop;
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
    std::unique_ptr<sao::launcher::license_panel::Owner> license;
#endif
    sao::launcher::UserMenu user_menu;
    bool user_menu_created{};
    ULONGLONG last_tick{};
    ULONGLONG last_service{};
    ULONGLONG close_started{};
    ULONGLONG intro_started{};
    int32_t audition_phase{-1};
    bool audition_flight_reported{};
    bool sdk_bound{};
    bool closing{};

    explicit Host(Options value)
        : config(std::move(value)), base_dir(executable_directory()) {}
    ~Host() {
        (void)close();
        (void)sao_ui_sound_shutdown();
    }

    static sao_status_t SAO_UI_CALL entity_action(SaoUiEntityAction action, void* user_data) {
        auto* host = static_cast<Host*>(user_data);
        if (host == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        switch (static_cast<int32_t>(action)) {
        case kOpenLauncherSettings:
            return host->open_launcher_settings();
        case kOpenHotkeys:
            return host->open_hotkeys();
        case kOpenAiMain:
            return host->open_ai_main();
        case kOpenAiSettings:
            return host->open_ai_settings();
        case kOpenPluginManager:
            return host->open_plugin_manager();
        case kOpenWorkshop:
            return host->open_workshop();
        case kOpenProcessSelector:
            return host->open_process_selector();
        case kOpenLicense:
            return host->open_license();
        case kOpenUserMenu:
            return host->open_user_menu();
        case kOpenAboutGuide:
            return host->open_about_guide();
        case SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT:
            return sao_ui_theme_set_active_id(SAO_UI_THEME_LIGHT);
        case SAO_UI_ENTITY_ACTION_SET_ALL_DARK:
            return sao_ui_theme_set_active_id(SAO_UI_THEME_DARK);
        case SAO_UI_ENTITY_ACTION_SAVE_SETTINGS:
            return host->settings_owner ? host->settings_owner->save()
                                        : SAO_STATUS_ERR_NOT_INITIALIZED;
        default:
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
    }

    sao_status_t open_launcher_settings() noexcept {
        return sao::launcher::settings::open_config_panel_status();
    }
    sao_status_t open_hotkeys() noexcept {
        return hotkey_owner ? hotkey_owner->open() : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_ai_settings() noexcept {
        if (ai_settings == nullptr) {
            const sao_status_t status =
                sao_ai_editor_settings_panel_create(compositor, backend, &ai_settings);
            if (status != SAO_STATUS_OK)
                return status;
        }
        sao_status_t status = sao_ai_editor_settings_panel_show(ai_settings);
        if (status == SAO_STATUS_OK)
            status = sao_ai_editor_settings_panel_tick(ai_settings);
        return status;
    }
    sao_status_t open_ai_main() noexcept {
        if (ai_main == nullptr) {
            const sao_status_t status =
                sao_ai_editor_main_panel_create(compositor, backend, &ai_main);
            if (status != SAO_STATUS_OK)
                return status;
        }
        sao_status_t status = sao_ai_editor_main_panel_show(ai_main);
        if (status == SAO_STATUS_OK)
            status = sao_ai_editor_main_panel_tick(ai_main);
        return status;
    }
    sao_status_t open_plugin_manager() noexcept {
        return plugin_manager ? plugin_manager->open() : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_workshop() noexcept {
        return workshop ? workshop->open() : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_process_selector() noexcept {
        return process_selector ? process_selector->open() : SAO_STATUS_ERR_NOT_INITIALIZED;
    }
    sao_status_t open_license() noexcept {
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        return license ? license->open() : SAO_STATUS_ERR_NOT_INITIALIZED;
#else
        return SAO_STATUS_ERR_NOT_INITIALIZED;
#endif
    }
    sao_status_t open_user_menu() noexcept {
        if (!user_menu_created)
            return SAO_STATUS_ERR_NOT_INITIALIZED;
        user_menu.processCommandLine(L"", true);
        return SAO_STATUS_OK;
    }
    sao_status_t open_about_guide() noexcept {
        return sao::launcher::openUserDocsIndex(base_dir.c_str(), window)
                   ? SAO_STATUS_OK
                   : SAO_STATUS_ERR_NOT_FOUND;
    }

    int32_t close() noexcept {
        if (settings_owner != nullptr && settings_owner->dirty()) {
            const sao_status_t status = settings_owner->save();
            if (status != SAO_STATUS_OK)
                return status;
        }
        if (intro != nullptr) {
            sao_ui_linkstart_destroy(intro);
            intro = nullptr;
        }
        if (ai_main != nullptr) {
            const sao_status_t status = sao_ai_editor_main_panel_try_destroy(ai_main);
            if (status != SAO_STATUS_OK)
                return status;
            ai_main = nullptr;
        }
        if (ai_settings != nullptr) {
            const sao_status_t status = sao_ai_editor_settings_panel_try_destroy(ai_settings);
            if (status != SAO_STATUS_OK)
                return status;
            ai_settings = nullptr;
        }
        if (user_menu_created) {
            user_menu.unbind_hotkey_owner(hotkey_owner.get());
            user_menu.destroy();
            user_menu_created = false;
        }
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        if (license != nullptr) {
            const sao_status_t status = license->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            license.reset();
        }
#endif
        if (workshop != nullptr) {
            const sao_status_t status = workshop->try_take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            workshop.reset();
        }
        if (process_selector != nullptr) {
            const sao_status_t status = process_selector->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            process_selector.reset();
        }
        if (plugin_manager != nullptr) {
            const sao_status_t status = plugin_manager->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            plugin_manager.reset();
        }
        if (hotkey_owner != nullptr) {
            const sao_status_t status = hotkey_owner->take_offline();
            if (status != SAO_STATUS_OK)
                return status;
            hotkey_owner.reset();
        }
        const sao_status_t settings_panel_status =
            sao::launcher::settings::take_offline_for_testing();
        if (settings_panel_status != SAO_STATUS_OK)
            return settings_panel_status;
        (void)sao::launcher::hotkey::unregister_all();
        sao::launcher::hotkey::clear_callbacks();
        (void)sao_launcher_hotkey_set_settings_owner(nullptr);
        (void)sao::launcher::settings::settings_profiles_unbind_owner(nullptr);
        (void)sao::launcher::settings::settings_panel_unbind_owner(nullptr);
        if (settings_owner != nullptr) {
            const sao_status_t status = settings_owner->save();
            if (status != SAO_STATUS_OK)
                return status;
            settings_owner.reset();
        }
        if (entity != nullptr) {
            const sao_status_t status = sao_ui_entity_shell_try_destroy(entity);
            if (status != SAO_STATUS_OK)
                return status;
            entity = nullptr;
        }
        if (keyboard != nullptr) {
            const sao_status_t status = sao_ui_input_router_deep_try_destroy(keyboard);
            if (status != SAO_STATUS_OK)
                return status;
            keyboard = nullptr;
        }
        if (sdk_bound) {
            if (sao_sdk_platform_unbind_ui_compositor() != SAO_SDK_OK)
                return SAO_STATUS_ERR_UNKNOWN;
            sdk_bound = false;
        }
        if (compositor != nullptr) {
            const sao_status_t status = sao_ui_compositor_try_destroy(compositor);
            if (status != SAO_STATUS_OK)
                return status;
            compositor = nullptr;
        }
        if (overlay != nullptr) {
            if (!sao_ui_overlay_host_destroy(overlay))
                return SAO_UI_STATUS_ERR_BUSY;
            overlay = nullptr;
        }
        if (backend != nullptr) {
            int32_t exit_code = 0;
            (void)sao_ai_editor_shutdown(backend, 2000, &exit_code);
            sao_ai_editor_destroy(backend);
            backend = nullptr;
        }
        return SAO_STATUS_OK;
    }

    void resize() {
        if (overlay == nullptr)
            return;
        if (IsIconic(window)) {
            require(sao_ui_overlay_host_set_visible(overlay, false));
            return;
        }
        RECT rect{};
        POINT origin{};
        if (!GetClientRect(window, &rect) || !ClientToScreen(window, &origin) || rect.right < 1 ||
            rect.bottom < 1)
            return;
        require(
            sao_ui_overlay_host_set_bounds(overlay, origin.x, origin.y, rect.right, rect.bottom));
        if (intro != nullptr) {
            require(sao_ui_linkstart_resize(intro, static_cast<uint32_t>(rect.right),
                                            static_cast<uint32_t>(rect.bottom),
                                            sao_ui_overlay_host_current_dpi(overlay)));
        }
        require(sao_ui_overlay_host_set_visible(overlay, true));
    }

    void initialize_settings() {
        // A visual preview must not save over the production workspace's
        // settings on close. Seed its independent profile once from real data.
        if (config.settings.empty()) {
            config.settings = config.workspace / L".sao" / L"ui-preview" / L"settings.json";
            std::filesystem::create_directories(config.settings.parent_path());
            const auto source = config.workspace / L"settings.json";
            if (!std::filesystem::exists(config.settings) &&
                std::filesystem::is_regular_file(source))
                std::filesystem::copy_file(source, config.settings);
        }
        const auto settings_path = config.settings.wstring();
        const auto profiles_path = (config.settings.parent_path() / L"profiles").wstring();
        sao::launcher::settings::set_profiles_directory_for_testing(profiles_path);
        require(
            sao::launcher::settings_owner::SettingsOwner::create(settings_path, settings_owner));
        sao::launcher::settings_owner::LoadInfo load_info{};
        require(settings_owner->load(load_info));
        require(sao::launcher::settings::settings_panel_bind_owner(settings_owner.get()));
        require(sao::launcher::settings::settings_profiles_bind_owner(settings_owner.get()));
        require(sao_launcher_hotkey_set_settings_owner(settings_owner.get()));
    }

    void initialize_entity() {
        static constexpr SaoUiMenuItem control[] = {
            {"设置", "sao:settings", kOpenLauncherSettings, true, {false, false, false}},
            {"快捷键", "sao:keyboard", kOpenHotkeys, true, {false, false, false}},
            {"保存设置",
             "sao:settings",
             SAO_UI_ENTITY_ACTION_SAVE_SETTINGS,
             true,
             {false, false, false}},
        };
        static constexpr SaoUiMenuItem tools[] = {
            {"AI 工作台", "sao:chat", kOpenAiMain, true, {false, false, false}},
            {"AI 设置", "sao:settings", kOpenAiSettings, true, {false, false, false}},
            {"创意工坊", "sao:workshop", kOpenWorkshop, true, {false, false, false}},
            {"进程选择", "sao:process", kOpenProcessSelector, true, {false, false, false}},
        };
        static constexpr SaoUiMenuItem plugins[] = {
            {"插件管理", "sao:plugins", kOpenPluginManager, true, {false, false, false}},
            {"许可", "sao:license", kOpenLicense, true, {false, false, false}},
        };
        static constexpr SaoUiMenuItem appearance[] = {
            {"经典浅色",
             "sao:home",
             SAO_UI_ENTITY_ACTION_SET_ALL_LIGHT,
             true,
             {false, false, false}},
            {"经典深色",
             "sao:lock",
             SAO_UI_ENTITY_ACTION_SET_ALL_DARK,
             true,
             {false, false, false}},
        };
        static constexpr SaoUiMenuItem user[] = {
            {"用户菜单", "sao:user", kOpenUserMenu, true, {false, false, false}},
            {"关于与使用指南", "sao:info", kOpenAboutGuide, true, {false, false, false}},
        };
        static constexpr SaoUiEntityRootItem roots[] = {
            {sizeof(SaoUiEntityRootItem),
             "Control",
             "设置",
             "sao:settings",
             10,
             true,
             {0, 0, 0},
             control,
             std::size(control)},
            {sizeof(SaoUiEntityRootItem),
             "Tools",
             "工具",
             "sao:tools",
             11,
             true,
             {0, 0, 0},
             tools,
             std::size(tools)},
            {sizeof(SaoUiEntityRootItem),
             "Plugins",
             "插件",
             "sao:plugins",
             12,
             true,
             {0, 0, 0},
             plugins,
             std::size(plugins)},
            {sizeof(SaoUiEntityRootItem),
             "Skins",
             "外观",
             "sao:home",
             13,
             true,
             {0, 0, 0},
             appearance,
             std::size(appearance)},
            {sizeof(SaoUiEntityRootItem),
             "User",
             "用户",
             "sao:user",
             14,
             true,
             {0, 0, 0},
             user,
             std::size(user)},
        };
        SaoUiEntityShellConfig entity_config{};
        entity_config.action_fn = &Host::entity_action;
        entity_config.action_user_data = this;
        require(sao_ui_entity_shell_create_on_compositor(compositor, &entity_config, &entity));
        bool nervgear_enabled = true;
        require(settings_owner->get_truthy("nervgear_mode", true, nervgear_enabled));
        require(sao_ui_entity_shell_set_nervgear_mode(entity, nervgear_enabled));
        require(sao_ui_entity_shell_set_roots(entity, roots, std::size(roots)));
    }

    void initialize() {
        if (config.backend_explicit && !config.offline) {
            std::filesystem::path backend_workspace = config.workspace;
            if (config.local_backend_fixture) {
                config.backend = sibling_executable(L"SaoAiEditor.exe");
                backend_workspace = config.workspace / L".sao" / L"ui-preview" / L"ai-backend";
                std::error_code directory_error;
                std::filesystem::create_directories(backend_workspace, directory_error);
                if (directory_error || !std::filesystem::is_directory(backend_workspace))
                    throw std::runtime_error("Preview backend workspace unavailable");
            }
            if (!std::filesystem::is_regular_file(config.backend))
                throw std::runtime_error("Preview backend executable unavailable");
            const std::string executable = utf8(config.backend);
            const std::string workspace = utf8(backend_workspace);
            const std::string extra = "--headless --workspace \"" + workspace + "\"";
            SaoAiEditorLaunchConfig launch{};
            launch.executable_utf8 = executable.c_str();
            launch.base_dir_utf8 = workspace.c_str();
            launch.extra_args_utf8 = extra.c_str();
            launch.handshake_timeout_ms = 30000;
            require(sao_ai_editor_create(&launch, &backend));
            int32_t exit_code = 0;
            require(sao_ai_editor_launch(backend, &exit_code));
        }
        SaoOverlayHostConfig host{};
        host.width = 1280;
        host.height = 820;
        host.title_utf16 = L"SAO Classic — production UI shell";
        require(sao_ui_overlay_host_create(&host, &overlay));
        require(sao_ui_compositor_create(overlay, nullptr, &compositor));
        require_sdk(sao_sdk_platform_bind_ui_compositor(compositor));
        sdk_bound = true;
        initialize_settings();
        hotkey_owner = std::make_unique<sao::launcher::hotkey::Owner>(compositor);
        require(hotkey_owner->set_owner_wake_window(window));
        plugin_manager = std::make_unique<sao::launcher::plugin_manager_panel::Owner>(compositor);
        process_selector =
            std::make_unique<sao::launcher::process_selector_panel::Owner>(compositor, nullptr);
        workshop = std::make_unique<sao::launcher::workshop_panel::Owner>(
            compositor, config.workspace, detached_workshop_operations());
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
        license = std::make_unique<sao::launcher::license_panel::Owner>(compositor);
#endif
        if (!user_menu.create(base_dir.c_str()))
            throw std::runtime_error("Production user menu unavailable");
        user_menu.bind_hotkey_owner(hotkey_owner.get());
        user_menu_created = true;
        initialize_entity();
        require(sao_ui_entity_shell_bring_online(entity));
        sao::launcher::hotkey::clear_callbacks();
        sao::launcher::hotkey::set_callback("toggle_sao_menu", [this] {
            if (sao_ui_entity_shell_home(entity) == SAO_STATUS_OK)
                (void)sao_ui_compositor_tick(compositor);
        });
        sao::launcher::hotkey::set_callback("toggle_float_button", [this] {
            if (sao_ui_entity_shell_insert(entity) == SAO_STATUS_OK)
                (void)sao_ui_compositor_tick(compositor);
        });
        sao::launcher::hotkey::load_or_default({
            {"toggle_sao_menu", "Home", VK_HOME, MOD_NOREPEAT},
            {"toggle_float_button", "Insert", VK_INSERT, MOD_NOREPEAT},
        });
        if (config.frame_out.empty() && !config.intro_audition &&
            !sao::launcher::hotkey::register_all().empty())
            throw std::runtime_error("Production hotkey registration unavailable");
        require(sao_ui_input_router_deep_create(compositor, &keyboard));
        if (!config.frame_out.empty())
            require(sao_ui_sound_set_enabled(false));
        if (config.initial_surface == InitialSurface::ai_main)
            require(open_ai_main());
        if (config.initial_surface == InitialSurface::ai_settings)
            require(open_ai_settings());
        if (config.initial_surface == InitialSurface::settings)
            require(open_launcher_settings());
        if (config.initial_surface == InitialSurface::hotkeys)
            require(open_hotkeys());
        if (config.initial_surface == InitialSurface::plugins)
            require(open_plugin_manager());
        if (config.initial_surface == InitialSurface::workshop)
            require(open_workshop());
        if (config.initial_surface == InitialSurface::process)
            require(open_process_selector());
        if (config.initial_surface == InitialSurface::license)
            require(open_license());
        if (config.initial_surface == InitialSurface::user)
            require(open_user_menu());
        if (config.initial_surface == InitialSurface::about)
            require(open_about_guide());
        if (config.initial_surface == InitialSurface::root)
            require(sao_ui_entity_shell_home(entity));
        resize();
        if (config.intro) {
            if (!config.frame_out.empty())
                require(sao_ui_sound_set_enabled(false));
            else if (config.intro_audition) {
                require(sao_ui_sound_set_enabled(true));
                require(sao_ui_sound_set_volume(55));
            }
            RECT rect{};
            GetClientRect(window, &rect);
            SaoUiLinkStartConfig start{sizeof(SaoUiLinkStartConfig),
                                       static_cast<uint32_t>(rect.right),
                                       static_cast<uint32_t>(rect.bottom), 0, nullptr};
            require(sao_ui_linkstart_create(compositor, nullptr, &start, &intro));
            require(sao_ui_linkstart_resize(intro, static_cast<uint32_t>(rect.right),
                                            static_cast<uint32_t>(rect.bottom),
                                            sao_ui_overlay_host_current_dpi(overlay)));
            require(sao_ui_linkstart_show(intro));
            intro_started = GetTickCount64();
        }
        last_tick = GetTickCount64();
        if (!SetTimer(window, kUiService, 16, nullptr))
            throw std::runtime_error("UI timer unavailable");
    }

    void export_frame() {
        for (int32_t remaining = config.frame_ms; remaining > 0;) {
            const int32_t step = std::min(remaining, 1000);
            require(sao_ui_entity_shell_tick(entity, static_cast<uint32_t>(step)));
            remaining -= step;
        }
        if (intro != nullptr && config.frame_ms > 0)
            require(sao_ui_linkstart_tick(intro, config.frame_ms));
        uint32_t width = 0, height = 0;
        size_t bytes = 0;
        const auto query = sao_ui_compositor_snapshot_bgra(
            compositor, nullptr, 0, &width, &height, &bytes);
        if (query != SAO_STATUS_ERR_BUFFER_TOO_SMALL)
            require(query);
        if (bytes == 0 || bytes > UINT32_MAX - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER))
            throw std::runtime_error("Invalid native frame dimensions");
        std::vector<uint8_t> pixels(bytes);
        require(sao_ui_compositor_snapshot_bgra(compositor, pixels.data(), pixels.size(),
                                                 &width, &height, &bytes));
        BITMAPFILEHEADER file{};
        file.bfType = 0x4d42;
        file.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        file.bfSize = file.bfOffBits + static_cast<DWORD>(bytes);
        BITMAPINFOHEADER image{};
        image.biSize = sizeof(image);
        image.biWidth = static_cast<LONG>(width);
        image.biHeight = -static_cast<LONG>(height);
        image.biPlanes = 1;
        image.biBitCount = 32;
        image.biCompression = BI_RGB;
        image.biSizeImage = static_cast<DWORD>(bytes);
        std::filesystem::create_directories(config.frame_out.parent_path());
        std::ofstream output(config.frame_out, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(&file), sizeof(file));
        output.write(reinterpret_cast<const char*>(&image), sizeof(image));
        output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(bytes));
        output.close();
        if (!output)
            throw std::runtime_error("Native frame export failed");
    }

    void service() {
        const ULONGLONG now = GetTickCount64();
        if (closing) {
            if (close_started == 0)
                close_started = now;
            if (close() == SAO_STATUS_OK) {
                DestroyWindow(window);
                return;
            }
            if (now - close_started > 5000) {
                closing = false;
                close_started = 0;
                SetWindowTextW(window, L"SAO Classic · 页面仍在收口，请完成当前操作后再关闭");
            }
            return;
        }
        const uint32_t elapsed = static_cast<uint32_t>(std::min<ULONGLONG>(now - last_tick, 1000U));
        require(sao_ui_entity_shell_tick(entity, elapsed));
        if (now - last_service >= 50) {
            if (ai_settings != nullptr)
                require(sao_ai_editor_settings_panel_tick(ai_settings));
            if (ai_main != nullptr)
                require(sao_ai_editor_main_panel_tick(ai_main));
            if (plugin_manager != nullptr)
                require_active(plugin_manager->service_ui());
            if (process_selector != nullptr)
                require_active(process_selector->service_ui());
            if (workshop != nullptr)
                require_active(workshop->service_ui());
#if defined(SAO_LAUNCHER_LICENSE_PANEL)
            if (license != nullptr)
                require_active(license->service_ui());
#endif
            last_service = now;
        }
        if (intro != nullptr) {
            bool active = false;
            require(sao_ui_linkstart_is_active(intro, &active));
            const auto intro_elapsed = static_cast<int32_t>(
                std::min<ULONGLONG>(now - last_tick, static_cast<ULONGLONG>(INT32_MAX)));
            if (active &&
                sao_ui_linkstart_tick(intro, intro_elapsed) != SAO_STATUS_OK)
                (void)sao_ui_linkstart_dismiss(intro);
            if (config.intro_audition) {
                SaoUiLinkStartPhase phase = SAO_UI_LINKSTART_PHASE_HIDDEN;
                float progress = 0.0F;
                require(sao_ui_linkstart_get_phase(intro, &phase, &progress));
                SaoUiLinkStartAudioState audio = SAO_UI_LINKSTART_AUDIO_READY;
                sao_status_t audio_status = SAO_STATUS_OK;
                require(sao_ui_linkstart_get_audio_state(intro, &audio, &audio_status));
                if (audition_phase != phase) {
                    std::fprintf(stderr, "intro phase=%d wall_ms=%llu audio=%d status=%d\n",
                                 static_cast<int>(phase), GetTickCount64() - intro_started,
                                 static_cast<int>(audio), audio_status);
                    audition_phase = phase;
                }
                if (!audition_flight_reported && phase == SAO_UI_LINKSTART_PHASE_PARTICLE_TUNNEL &&
                    progress > 0.0F) {
                    std::fprintf(stderr, "intro first_flight wall_ms=%llu\n",
                                 GetTickCount64() - intro_started);
                    audition_flight_reported = true;
                }
                require(sao_ui_linkstart_is_active(intro, &active));
                if (!active)
                    closing = true;
            }
        }
        last_tick = now;
        if (!IsIconic(window)) {
            sao_status_t status = sao_ui_compositor_tick(compositor);
            if (status != SAO_STATUS_OK && intro != nullptr) {
                (void)sao_ui_linkstart_dismiss(intro);
                status = sao_ui_compositor_tick(compositor);
            }
            if (status != SAO_STATUS_ERR_DEVICE_LOST)
                require(status);
        }
    }

    void mouse(UINT message, WPARAM wp, LPARAM lp) noexcept {
        if (compositor == nullptr)
            return;
        POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (message != WM_MOUSEWHEEL)
            ClientToScreen(window, &point);
        int32_t button = -1;
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP)
            button = 0;
        else if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
            button = 1;
        else if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            button = 2;
        const int32_t wheel = message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(wp) : 0;
        (void)sao_ui_compositor_dispatch_mouse(compositor, message, point.x, point.y, button, wheel);
    }

    bool key(const MSG& message) {
        if (intro != nullptr &&
            (message.message == WM_KEYDOWN || message.message == WM_KEYUP ||
             message.message == WM_CHAR || message.message == WM_HOTKEY ||
             message.message == SAO_UI_NATIVE_TEXT_TAB_MESSAGE)) {
            bool intro_active = false;
            require(sao_ui_linkstart_is_active(intro, &intro_active));
            if (intro_active)
                return true;
        }
        if (message.message == WM_HOTKEY)
            return sao::launcher::hotkey::dispatch_by_native_id(static_cast<int>(message.wParam));
        if (message.message == sao::launcher::hotkey::kCaptureCompletionMessage &&
            hotkey_owner != nullptr) {
            require(hotkey_owner->drain_capture_for_owner());
            return true;
        }
        if (message.message != WM_KEYDOWN && message.message != WM_KEYUP &&
            message.message != WM_CHAR && message.message != SAO_UI_NATIVE_TEXT_TAB_MESSAGE)
            return false;
        const HWND render = static_cast<HWND>(sao_ui_compositor_host_hwnd(compositor));
        if (GetFocus() != render && GetFocus() != window)
            return false;
        bool consumed = false;
        const sao_status_t status = sao_ui_input_router_feed_raw_win32(
            keyboard, message.message, message.wParam, message.lParam, &consumed);
        if (status != SAO_STATUS_ERR_NOT_FOUND)
            require(status);
        if (!consumed && message.message == WM_KEYDOWN && entity != nullptr) {
            const sao_status_t shell_status =
                sao_ui_entity_shell_handle_key(entity, static_cast<uint32_t>(message.wParam), 0);
            if (shell_status != SAO_STATUS_ERR_NOT_FOUND)
                require(shell_status);
            consumed = shell_status == SAO_STATUS_OK;
        }
        return consumed;
    }
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* host = reinterpret_cast<Host*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<Host*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (host == nullptr)
        return DefWindowProcW(window, message, wp, lp);
    try {
        switch (message) {
        case WM_SIZE:
        case WM_MOVE:
            host->resize();
            return 0;
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            host->mouse(message, wp, lp);
            return 0;
        case WM_CANCELMODE:
        case WM_CAPTURECHANGED:
            host->mouse(message, 0, 0);
            return DefWindowProcW(window, message, wp, lp);
        case WM_TIMER:
            if (wp == kUiService)
                host->service();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {820, 620};
            return 0;
        case WM_CLOSE:
            host->closing = true;
            return 0;
        case WM_DESTROY:
            KillTimer(window, kUiService);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Production UI message %u: %s\n", message, error.what());
        std::fflush(stderr);
        SetWindowTextA(window, error.what());
        host->closing = true;
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    bool frame_export = false;
    int argument_count = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        for (int index = 1; index < argument_count; ++index) {
            const std::wstring_view argument(arguments[index]);
            frame_export = frame_export || argument == L"--frame-out" || argument == L"--frame-ms";
        }
        LocalFree(arguments);
    }
    const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_status)) {
        std::fprintf(stderr, "STA COM initialization failed: %ld\n", static_cast<long>(com_status));
        std::fflush(stderr);
        if (!frame_export) {
            const std::wstring message =
                L"STA COM initialization failed: " + std::to_wstring(com_status);
            MessageBoxW(nullptr, message.c_str(), L"Production UI host", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
    int code = 1;
    try {
        Host host(options());
        frame_export = !host.config.frame_out.empty();
        WNDCLASSW cls{};
        cls.lpfnWndProc = window_proc;
        cls.hInstance = instance;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        cls.lpszClassName = L"SaoProductionUiHost";
        if (!RegisterClassW(&cls))
            throw std::runtime_error("Window class unavailable");
        HWND window = CreateWindowExW(0, cls.lpszClassName, L"SAO Classic · production UI shell",
                                      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 860,
                                      nullptr, nullptr, instance, &host);
        if (window == nullptr)
            throw std::runtime_error("Window creation failed");
        try {
            host.initialize();
            if (frame_export) {
                host.export_frame();
                require(host.close());
                code = 0;
            } else {
                ShowWindow(window, show);
                MSG message{};
                BOOL received = 0;
                while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                    if (!host.key(message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                code = received == 0 ? 0 : 1;
            }
            if (IsWindow(window))
                DestroyWindow(window);
        } catch (...) {
            (void)host.close();
            DestroyWindow(window);
            throw;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Production UI startup: %s\n", error.what());
        std::fflush(stderr);
        if (!frame_export)
            MessageBoxA(nullptr, error.what(), "Production UI host", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return code;
}
