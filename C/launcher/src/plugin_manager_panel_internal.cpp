#include "plugin_manager_panel_internal.h"

#include "sao/ui/panel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#if (defined(SAO_LINKED_PLUGINS) || defined(SAO_LAUNCHER_PLUGIN_MANAGER_WITH_LOADER)) &&           \
    __has_include("sao/plugins/loader/plugin_lifecycle.h") &&                                      \
                  __has_include("sao/plugins/loader/plugin_registry.h")
#define SAO_LAUNCHER_PLUGIN_MANAGER_HAS_LOADER 1
#include "sao/plugins/loader/plugin_lifecycle.h"
#include "sao/plugins/loader/plugin_manifest.h"
#include "sao/plugins/loader/plugin_registry.h"
#else
#define SAO_LAUNCHER_PLUGIN_MANAGER_HAS_LOADER 0
#endif

namespace sao::launcher::plugin_manager_panel {
namespace {

using Json = nlohmann::json;

constexpr char kThemeOverride[] =
    R"({"colors":{"APP_BG":"#0b1018","APP_CARD":"#131b27","APP_BORDER":"#2a3850","APP_TEXT":"#eef4ff","APP_TEXT_2":"#c9d5e7","APP_TEXT_DIM":"#8292a8","APP_ACCENT":"#63b3ff","APP_BLUE":"#63b3ff","APP_GREEN":"#57d39b","APP_RED":"#ff6f7f","APP_ORANGE":"#f0a45d","APP_GOLD":"#e9ca72"}})";

constexpr std::string_view kActionRefresh = "plugin_manager.refresh";
constexpr std::string_view kActionReloadAll = "plugin_manager.reload_all";
constexpr std::string_view kActionEnable = "plugin_manager.enable";
constexpr std::string_view kActionDisable = "plugin_manager.disable";
constexpr std::string_view kActionReload = "plugin_manager.reload";

std::string clamp_utf8(std::string value, std::size_t maximum) {
    if (value.size() <= maximum)
        return value;
    if (maximum <= 3U)
        return value.substr(0, maximum);
    std::size_t end = maximum - 3U;
    while (end > 0U && (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
        --end;
    value.resize(end);
    value.append("...");
    return value;
}

Json text_node(std::string text, std::string_view style = "value", std::int32_t height = 22) {
    return Json{{"type", "text"},
                {"text", clamp_utf8(std::move(text), 4096U)},
                {"style", style},
                {"height", height}};
}

Json badge_node(std::string text, std::string_view style = "muted") {
    return Json{{"type", "badge"},
                {"text", clamp_utf8(std::move(text), 256U)},
                {"style", style},
                {"height", 22}};
}

Json button_node(std::string id, std::string label, std::string action, Json payload = Json(),
                 std::string_view style = "default", bool disabled = false) {
    Json node{{"type", "button"},
              {"id", std::move(id)},
              {"label", std::move(label)},
              {"action", std::move(action)},
              {"style", style},
              {"height", 28}};
    if (!payload.is_null())
        node["payload"] = std::move(payload);
    if (disabled)
        node["disabled"] = true;
    return node;
}

Json row_node(Json children) {
    return Json{{"type", "row"}, {"align", "left"}, {"children", std::move(children)}};
}

Json card_node(std::string title, Json children, std::string_view accent = "cyan") {
    return Json{{"type", "card"},
                {"title", clamp_utf8(std::move(title), 512U)},
                {"accent", accent},
                {"children", std::move(children)}};
}

std::string_view state_style(PluginState state) noexcept {
    static constexpr std::array<std::string_view, 13> styles{
        "accent", "accent", "warn",  "warn", "warn", "warn", "ok",
        "muted",  "warn",   "muted", "bad",  "warn", "warn",
    };
    const auto index = static_cast<std::size_t>(state);
    return index < styles.size() ? styles[index] : "muted";
}

std::string_view source_label(PluginSource source) noexcept {
    static constexpr std::array<std::string_view, 3> labels{
        "Unknown source / 来源未知",
        "Built-in / 内置",
        "User / 用户",
    };
    const auto index = static_cast<std::size_t>(source);
    return index < labels.size() ? labels[index] : labels.front();
}

std::string status_message(std::string_view operation, sao_status_t status) {
    return std::string(operation) + " failed (status " + std::to_string(status) + ").";
}

#if SAO_LAUNCHER_PLUGIN_MANAGER_HAS_LOADER

PluginState map_loader_state(sao::plugins::loader::lifecycle_state state) noexcept {
    static constexpr std::array<PluginState, 13> states{
        PluginState::unknown,       PluginState::discovered,
        PluginState::validating,    PluginState::resolving_dependencies,
        PluginState::bootstrapping, PluginState::loading,
        PluginState::loaded_active, PluginState::loaded_disabled,
        PluginState::unloading,     PluginState::unloaded,
        PluginState::failed,        PluginState::enabling,
        PluginState::disabling,
    };
    const auto index = static_cast<std::size_t>(state);
    return index < states.size() ? states[index] : PluginState::unknown;
}

Snapshot snapshot_loader() {
    namespace Loader = sao::plugins::loader;
    Snapshot snapshot;
    const Loader::registry_handle_t registry = Loader::sao_plugins_registry_instance();
    if (registry == nullptr) {
        snapshot.error_message = "Plugin loader registry unavailable / 插件加载器注册表不可用。";
        return snapshot;
    }

    snapshot.loader_available = true;
    const auto manifests = Loader::snapshot_manifests(registry);
    snapshot.plugins.reserve(manifests.size());
    for (const auto& manifest : manifests) {
        PluginSnapshot plugin;
        plugin.plugin_id = manifest.plugin_id;
        plugin.name = manifest.name.empty() ? manifest.plugin_id : manifest.name;
        plugin.version = manifest.version.empty() ? "unknown" : manifest.version;
        plugin.language = std::string(Loader::engine_kind_name(manifest.language));
        if (plugin.language.empty())
            plugin.language = "unknown";
        plugin.description = manifest.description;
        plugin.source_path = manifest.source_path;
        plugin.source = manifest.user_installed ? PluginSource::user : PluginSource::built_in;
        plugin.manifest_enabled = manifest.enabled;
        const Loader::plugin_handle_t handle =
            Loader::sao_plugins_registry_find(registry, manifest.plugin_id.c_str());
        plugin.state = handle == nullptr
                           ? PluginState::unknown
                           : map_loader_state(Loader::sao_plugins_lifecycle_state(handle));
        snapshot.plugins.push_back(std::move(plugin));
    }
    std::ranges::sort(snapshot.plugins,
                      [](const PluginSnapshot& left, const PluginSnapshot& right) {
                          if (left.name != right.name)
                              return left.name < right.name;
                          return left.plugin_id < right.plugin_id;
                      });
    return snapshot;
}

sao_status_t run_plugin_operation(std::string_view plugin_id, std::string_view action) {
    namespace Loader = sao::plugins::loader;
    const Loader::registry_handle_t registry = Loader::sao_plugins_registry_instance();
    if (registry == nullptr)
        return SAO_STATUS_ERR_CAPABILITY_MISSING;
    const std::string owned_id(plugin_id);
    const Loader::plugin_handle_t plugin =
        Loader::sao_plugins_registry_find(registry, owned_id.c_str());
    if (plugin == nullptr)
        return SAO_STATUS_ERR_NOT_FOUND;

    if (action == kActionEnable)
        return static_cast<sao_status_t>(Loader::sao_plugins_lifecycle_enable(plugin));
    if (action == kActionDisable)
        return static_cast<sao_status_t>(Loader::sao_plugins_lifecycle_disable(plugin));
    if (action == kActionReload)
        return static_cast<sao_status_t>(Loader::sao_plugins_lifecycle_reload(plugin));
    return SAO_STATUS_ERR_INVALID_ARGUMENT;
}

#else

Snapshot snapshot_loader() {
    Snapshot snapshot;
    snapshot.error_message =
        "Plugin loader unavailable / 插件加载器不可用。The panel remains available in this build.";
    return snapshot;
}

sao_status_t run_plugin_operation(std::string_view, std::string_view) {
    return SAO_STATUS_ERR_CAPABILITY_MISSING;
}

#endif

} // namespace

std::string_view plugin_state_label(PluginState state) noexcept {
    static constexpr std::array<std::string_view, 13> labels{
        "Unknown / 未知",
        "Discovered / 已发现",
        "Validating / 校验中",
        "Resolving dependencies / 解析依赖中",
        "Bootstrapping / 初始化依赖中",
        "Loading / 加载中",
        "Enabled / 已启用",
        "Disabled / 已禁用",
        "Unloading / 卸载中",
        "Unloaded / 已卸载",
        "Failed / 失败",
        "Enabling / 启用中",
        "Disabling / 禁用中",
    };
    const auto index = static_cast<std::size_t>(state);
    return index < labels.size() ? labels[index] : labels.front();
}

bool plugin_state_is_transitioning(PluginState state) noexcept {
    return state == PluginState::validating || state == PluginState::resolving_dependencies ||
           state == PluginState::bootstrapping || state == PluginState::loading ||
           state == PluginState::unloading || state == PluginState::enabling ||
           state == PluginState::disabling;
}

bool plugin_state_is_enabled(PluginState state) noexcept {
    return state == PluginState::loaded_active;
}

bool plugin_state_allows_enable(PluginState state) noexcept {
    return state == PluginState::discovered || state == PluginState::loaded_disabled ||
           state == PluginState::unloaded || state == PluginState::failed ||
           state == PluginState::loaded_active;
}

bool plugin_state_allows_reload(PluginState state) noexcept {
    return state != PluginState::unknown && !plugin_state_is_transitioning(state);
}

SaoPanelDescriptor descriptor_for_testing() noexcept {
    SaoPanelDescriptor descriptor{};
    descriptor.panel_id_utf8 = kPanelId.data();
    descriptor.title_utf8 = "Plugin Manager";
    descriptor.anchor = SAO_UI_PANEL_ANCHOR_CENTER;
    descriptor.default_width_px = 920;
    descriptor.default_height_px = 760;
    descriptor.min_width_px = 620;
    descriptor.min_height_px = 420;
    descriptor.movable = true;
    descriptor.resizable = true;
    descriptor.show_titlebar = true;
    descriptor.show_close_button = true;
    descriptor.visible = false;
    descriptor.remember_geometry = true;
    descriptor.modal = false;
    descriptor.overlay_style = false;
    descriptor.z_class = SAO_UI_PANEL_Z_NORMAL;
    descriptor.theme_override_json_utf8 = kThemeOverride;
    descriptor.initial_opacity = 1.0F;
    descriptor.auto_scroll = true;
    return descriptor;
}

std::string build_spec_for_testing(const Snapshot& snapshot) {
    Json nodes = Json::array();
    nodes.push_back(text_node("Plugin Manager", "title", 30));
    nodes.push_back(text_node(
        "Launcher-native plugin lifecycle control. All actions refresh the canonical loader state.",
        "muted", 36));

    Json status_badges = Json::array();
    status_badges.push_back(badge_node(snapshot.loader_available
                                           ? "Loader available / 加载器可用"
                                           : "Loader unavailable / 加载器不可用",
                                       snapshot.loader_available ? "ok" : "warn"));
    status_badges.push_back(
        badge_node(std::to_string(snapshot.plugins.size()) + " plugins / 个插件", "accent"));
    nodes.push_back(row_node(std::move(status_badges)));

    Json toolbar = Json::array();
    toolbar.push_back(button_node("plugin-manager.refresh", "Refresh / 刷新",
                                  std::string(kActionRefresh), Json(), "primary"));
    toolbar.push_back(button_node("plugin-manager.reload-all", "Reload All / 全部重载",
                                  std::string(kActionReloadAll), Json(), "default",
                                  !snapshot.reload_all_available));
    Json toolbar_card = Json::array();
    toolbar_card.push_back(row_node(std::move(toolbar)));
    if (!snapshot.reload_all_available) {
        toolbar_card.push_back(text_node(
            "Reload All awaits an injected launcher handler / Reload All 等待 launcher 注入回调。",
            "muted", 30));
    }
    nodes.push_back(card_node("Actions / 操作", std::move(toolbar_card), "gold"));

    if (!snapshot.error_message.empty()) {
        Json errors = Json::array();
        errors.push_back(text_node(snapshot.error_message, "bad", 42));
        nodes.push_back(card_node("Error / 错误", std::move(errors), "bad"));
    }

    if (!snapshot.loader_available) {
        Json unavailable = Json::array();
        unavailable.push_back(text_node("Plugin loader unavailable / 插件加载器不可用。This "
                                        "compositor panel can still be opened and closed.",
                                        "warn", 48));
        nodes.push_back(card_node("Unavailable / 不可用", std::move(unavailable), "warn"));
    } else if (snapshot.plugins.empty()) {
        Json empty = Json::array();
        empty.push_back(text_node("No plugins discovered / 未发现插件。", "muted", 34));
        nodes.push_back(card_node("Plugins / 插件", std::move(empty), "cyan"));
    } else {
        for (std::size_t index = 0; index < snapshot.plugins.size(); ++index) {
            const PluginSnapshot& plugin = snapshot.plugins[index];
            const bool transitioning = plugin_state_is_transitioning(plugin.state);
            const bool active = plugin_state_is_enabled(plugin.state);
            Json children = Json::array();

            Json badges = Json::array();
            badges.push_back(badge_node(std::string(plugin_state_label(plugin.state)),
                                        state_style(plugin.state)));
            badges.push_back(badge_node(std::string(source_label(plugin.source)), "muted"));
            badges.push_back(badge_node(plugin.manifest_enabled ? "Configured enabled / 配置启用"
                                                                : "Configured disabled / 配置禁用",
                                        plugin.manifest_enabled ? "accent" : "muted"));
            children.push_back(row_node(std::move(badges)));

            children.push_back(text_node("ID: " + plugin.plugin_id, "mono", 24));
            children.push_back(text_node(
                "Version: " + plugin.version + " · Language: " + plugin.language, "value", 26));
            children.push_back(text_node(plugin.description.empty() ? "No description / 无简介"
                                                                    : plugin.description,
                                         plugin.description.empty() ? "muted" : "value", 36));
            children.push_back(
                text_node("Source: " + (plugin.source_path.empty() ? std::string("unknown")
                                                                   : plugin.source_path),
                          "mono", 34));

            Json actions = Json::array();
            actions.push_back(
                button_node("plugin-manager.toggle." + std::to_string(index),
                            active ? "Disable / 禁用" : "Enable / 启用",
                            active ? std::string(kActionDisable) : std::string(kActionEnable),
                            Json(plugin.plugin_id), active ? "danger" : "primary",
                            transitioning || !plugin_state_allows_enable(plugin.state)));
            actions.push_back(button_node("plugin-manager.reload." + std::to_string(index),
                                          "Reload / 重载", std::string(kActionReload),
                                          Json(plugin.plugin_id), "default",
                                          !plugin_state_allows_reload(plugin.state)));
            children.push_back(row_node(std::move(actions)));

            nodes.push_back(card_node(plugin.name.empty() ? plugin.plugin_id : plugin.name,
                                      std::move(children),
                                      plugin.state == PluginState::failed ? "bad" : "cyan"));
        }
    }

    return Json{{"version", 1}, {"title", ""}, {"surface", "solid"}, {"nodes", std::move(nodes)}}
        .dump();
}

struct Owner::Impl {
    explicit Impl(sao_ui_compositor_handle_t borrowed_compositor) noexcept
        : compositor(borrowed_compositor) {}

    ~Impl() {
        shutdown_noexcept();
    }

    struct CallbackLease final {
        explicit CallbackLease(Impl* candidate) noexcept : state(candidate) {
            if (state == nullptr)
                return;
            std::lock_guard lock(state->mutex);
            if (!state->accepting || state->retiring)
                return;
            ++state->callbacks_in_flight;
            active = true;
        }

        ~CallbackLease() {
            if (!active)
                return;
            {
                std::lock_guard lock(state->mutex);
                --state->callbacks_in_flight;
            }
            state->cv.notify_all();
        }

        explicit operator bool() const noexcept {
            return active;
        }

        Impl* state{};
        bool active{};
    };

    static void SAO_UI_CALL action_callback(const char* action_id_utf8,
                                            const std::uint8_t* payload_json_utf8,
                                            std::size_t payload_len, void* user_data) {
        auto* state = static_cast<Impl*>(user_data);
        CallbackLease lease(state);
        if (!lease || action_id_utf8 == nullptr)
            return;
        const std::string_view payload(
            payload_json_utf8 == nullptr ? "" : reinterpret_cast<const char*>(payload_json_utf8),
            payload_json_utf8 == nullptr ? 0U : payload_len);
        (void)state->dispatch(action_id_utf8, payload);
    }

    sao_status_t set_operations(Operations replacement) noexcept {
        try {
            std::lock_guard lock(mutex);
            if (retiring)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            operations = std::move(replacement);
            return SAO_STATUS_OK;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t open() noexcept {
        if (compositor == nullptr)
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        try {
            sao_ui_panel_handle_t existing = nullptr;
            {
                std::lock_guard lock(mutex);
                if (creating || retiring)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                existing = panel;
                if (existing == nullptr) {
                    creating = true;
                    accepting = false;
                }
            }

            if (existing == nullptr) {
                const SaoPanelDescriptor descriptor = descriptor_for_testing();
                sao_ui_panel_handle_t created_panel = nullptr;
                sao_ui_panel_body_handle_t created_body = nullptr;
                sao_status_t status =
                    sao_ui_panel_register(compositor, &descriptor, &created_panel, &created_body);
                if (status != SAO_STATUS_OK) {
                    finish_create_failure();
                    return status;
                }
                {
                    std::lock_guard lock(mutex);
                    panel = created_panel;
                    body = created_body;
                }

                status = sao_ui_panel_set_action_handler(created_panel, &action_callback, this);
                if (status == SAO_STATUS_OK) {
                    std::lock_guard lock(mutex);
                    action_handler_attached = true;
                    accepting = true;
                }
                if (status == SAO_STATUS_OK)
                    status = refresh_now();
                if (status != SAO_STATUS_OK) {
                    cleanup_failed_create();
                    return status;
                }
                existing = created_panel;
            } else {
                const sao_status_t refresh_status = refresh_now();
                if (refresh_status != SAO_STATUS_OK)
                    return refresh_status;
            }

            sao_status_t status = sao_ui_panel_show(existing);
            if (status == SAO_STATUS_OK)
                status = sao_ui_panel_bring_to_front(existing);
            {
                std::lock_guard lock(mutex);
                creating = false;
            }
            return status;
        } catch (...) {
            finish_create_failure();
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t refresh() noexcept {
        try {
            {
                std::lock_guard lock(mutex);
                if (creating || retiring)
                    return SAO_UI_PANEL_STATUS_ERR_BUSY;
                if (panel == nullptr || body == nullptr)
                    return SAO_STATUS_ERR_NOT_INITIALIZED;
            }
            return refresh_now();
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t refresh_now() noexcept {
        try {
            Snapshot snapshot = snapshot_loader();
            sao_ui_panel_body_handle_t target_body = nullptr;
            {
                std::lock_guard lock(mutex);
                target_body = body;
                snapshot.reload_all_available = static_cast<bool>(operations.reload_all);
                if (!operation_error.empty()) {
                    if (!snapshot.error_message.empty())
                        snapshot.error_message.append(" ");
                    snapshot.error_message.append(operation_error);
                }
            }
            if (target_body == nullptr)
                return SAO_STATUS_ERR_NOT_INITIALIZED;
            const std::string spec = build_spec_for_testing(snapshot);
            const sao_status_t status = sao_ui_panel_body_set_spec(
                target_body, reinterpret_cast<const std::uint8_t*>(spec.data()), spec.size());
            return status;
        } catch (...) {
            return SAO_STATUS_ERR_UNKNOWN;
        }
    }

    sao_status_t dispatch(std::string_view action_id, std::string_view payload_json) noexcept {
        if (action_id == kActionRefresh) {
            clear_operation_message();
            return refresh();
        }

        if (action_id == kActionReloadAll) {
            std::function<sao_status_t()> handler;
            try {
                std::lock_guard lock(mutex);
                handler = operations.reload_all;
            } catch (...) {
                set_operation_error("Reload All handler copy failed.");
                (void)refresh();
                return SAO_STATUS_ERR_UNKNOWN;
            }
            sao_status_t operation = SAO_STATUS_ERR_CAPABILITY_MISSING;
            if (handler) {
                try {
                    operation = handler();
                } catch (...) {
                    operation = SAO_STATUS_ERR_UNKNOWN;
                }
            }
            if (operation == SAO_STATUS_OK)
                clear_operation_message();
            else
                set_operation_error(status_message("Reload All", operation));
            const sao_status_t refresh_status = refresh();
            return operation == SAO_STATUS_OK ? refresh_status : operation;
        }

        if (action_id != kActionEnable && action_id != kActionDisable &&
            action_id != kActionReload) {
            set_operation_error("Unknown Plugin Manager action: " + std::string(action_id));
            (void)refresh();
            return SAO_STATUS_ERR_NOT_FOUND;
        }

        const Json payload = Json::parse(payload_json.begin(), payload_json.end(), nullptr, false);
        if (!payload.is_string() || payload.get_ref<const std::string&>().empty()) {
            set_operation_error("Plugin action payload must be the plugin id JSON string.");
            (void)refresh();
            return SAO_STATUS_ERR_INVALID_ARGUMENT;
        }
        const std::string plugin_id = payload.get<std::string>();
        const sao_status_t operation = run_plugin_operation(plugin_id, action_id);
        if (operation == SAO_STATUS_OK)
            clear_operation_message();
        else
            set_operation_error(
                status_message(std::string(action_id) + " for " + plugin_id, operation));
        const sao_status_t refresh_status = refresh();
        return operation == SAO_STATUS_OK ? refresh_status : operation;
    }

    sao_status_t take_offline() noexcept {
        sao_ui_panel_handle_t target_panel = nullptr;
        bool had_action = false;
        {
            std::lock_guard lock(mutex);
            if (creating || retiring || callbacks_in_flight != 0U)
                return SAO_UI_PANEL_STATUS_ERR_BUSY;
            if (panel == nullptr) {
                return SAO_STATUS_OK;
            }
            retiring = true;
            accepting = false;
            target_panel = panel;
            had_action = action_handler_attached;
        }

        bool action_detached = !had_action;
        sao_status_t status = SAO_STATUS_OK;
        if (had_action) {
            status = sao_ui_panel_set_action_handler(target_panel, nullptr, nullptr);
            action_detached = status == SAO_STATUS_OK;
        }
        if (status == SAO_STATUS_OK)
            status = sao_ui_panel_unregister(target_panel);

        if (status == SAO_STATUS_OK) {
            std::lock_guard lock(mutex);
            panel = nullptr;
            body = nullptr;
            action_handler_attached = false;
            accepting = false;
            retiring = false;
            return SAO_STATUS_OK;
        }

        bool action_restored = !had_action;
        if (had_action && action_detached)
            action_restored = sao_ui_panel_set_action_handler(target_panel, &action_callback,
                                                              this) == SAO_STATUS_OK;
        else if (had_action)
            action_restored = true;

        {
            std::lock_guard lock(mutex);
            action_handler_attached = had_action && action_restored;
            accepting = action_restored;
            retiring = false;
        }
        return action_restored ? status : SAO_UI_PANEL_STATUS_ERR_ROLLBACK_FAILED;
    }

    bool is_registered() const noexcept {
        std::lock_guard lock(mutex);
        return panel != nullptr;
    }

    bool is_visible() const noexcept {
        const sao_ui_panel_handle_t target = panel_handle();
        SaoPanelState state{};
        return target != nullptr && sao_ui_panel_get_state(target, &state) == SAO_STATUS_OK &&
               state.visible;
    }

    sao_ui_panel_handle_t panel_handle() const noexcept {
        std::lock_guard lock(mutex);
        return panel;
    }

    void clear_operation_message() noexcept {
        std::lock_guard lock(mutex);
        operation_error.clear();
    }

    void set_operation_error(std::string message) noexcept {
        try {
            std::lock_guard lock(mutex);
            operation_error = std::move(message);
        } catch (...) {
        }
    }

    void finish_create_failure() noexcept {
        std::lock_guard lock(mutex);
        creating = false;
        accepting = panel != nullptr && action_handler_attached;
    }

    void cleanup_failed_create() noexcept {
        sao_ui_panel_handle_t failed_panel = nullptr;
        bool detach_action = false;
        {
            std::lock_guard lock(mutex);
            failed_panel = panel;
            detach_action = action_handler_attached;
            accepting = false;
        }
        if (failed_panel != nullptr && detach_action)
            (void)sao_ui_panel_set_action_handler(failed_panel, nullptr, nullptr);
        if (failed_panel != nullptr)
            (void)sao_ui_panel_unregister(failed_panel);
        std::lock_guard lock(mutex);
        panel = nullptr;
        body = nullptr;
        action_handler_attached = false;
        accepting = false;
        creating = false;
    }

    void shutdown_noexcept() noexcept {
        for (;;) {
            const sao_status_t status = take_offline();
            if (status != SAO_UI_PANEL_STATUS_ERR_BUSY)
                break;
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] { return callbacks_in_flight == 0U; });
        }

        sao_ui_panel_handle_t remaining = nullptr;
        {
            std::lock_guard lock(mutex);
            remaining = panel;
            accepting = false;
            retiring = true;
        }
        if (remaining != nullptr) {
            (void)sao_ui_panel_set_action_handler(remaining, nullptr, nullptr);
            (void)sao_ui_panel_unregister(remaining);
        }
        std::lock_guard lock(mutex);
        panel = nullptr;
        body = nullptr;
        action_handler_attached = false;
        retiring = false;
    }

    sao_ui_compositor_handle_t compositor{};
    mutable std::mutex mutex;
    std::condition_variable cv;
    sao_ui_panel_handle_t panel{};
    sao_ui_panel_body_handle_t body{};
    Operations operations;
    std::string operation_error;
    std::size_t callbacks_in_flight{};
    bool action_handler_attached{};
    bool accepting{};
    bool creating{};
    bool retiring{};
};

Owner::Owner(sao_ui_compositor_handle_t borrowed_compositor) noexcept
    : impl_(new (std::nothrow) Impl(borrowed_compositor)) {}

Owner::~Owner() = default;

Owner::Owner(Owner&& other) noexcept = default;

Owner& Owner::operator=(Owner&& other) noexcept = default;

sao_status_t Owner::set_operations(Operations operations) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                            : impl_->set_operations(std::move(operations));
}

sao_status_t Owner::open() noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->open();
}

sao_status_t Owner::refresh() noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED : impl_->refresh();
}

sao_status_t Owner::take_offline() noexcept {
    return impl_ == nullptr ? SAO_STATUS_OK : impl_->take_offline();
}

bool Owner::is_registered() const noexcept {
    return impl_ != nullptr && impl_->is_registered();
}

bool Owner::is_visible() const noexcept {
    return impl_ != nullptr && impl_->is_visible();
}

sao_ui_panel_handle_t Owner::panel_handle() const noexcept {
    return impl_ == nullptr ? nullptr : impl_->panel_handle();
}

sao_status_t Owner::dispatch_action_for_testing(std::string_view action_id,
                                                std::string_view payload_json) noexcept {
    return impl_ == nullptr ? SAO_STATUS_ERR_NOT_INITIALIZED
                            : impl_->dispatch(action_id, payload_json);
}

} // namespace sao::launcher::plugin_manager_panel
